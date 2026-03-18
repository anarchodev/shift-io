#include "shift_io_internal.h"

#include <errno.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* --------------------------------------------------------------------------
 * Forward declarations of static helpers
 * -------------------------------------------------------------------------- */

static sio_result_t sio_arm_accept(sio_context_t *ctx);
static sio_result_t sio_arm_recv(sio_context_t *ctx, int fd);
static void sio_handle_accept_cqe(sio_context_t *ctx, struct io_uring_cqe *cqe);
static void sio_handle_recv_cqe(sio_context_t *ctx, struct io_uring_cqe *cqe);
static void sio_handle_send_cqe(sio_context_t *ctx, struct io_uring_cqe *cqe);

/* --------------------------------------------------------------------------
 * sio_context_create
 * -------------------------------------------------------------------------- */

sio_result_t sio_context_create(const sio_config_t *cfg, sio_context_t **out) {
  if (!cfg || !out)
    return sio_error_null;
  if (!cfg->shift)
    return sio_error_null;
  if (cfg->buf_count == 0 || (cfg->buf_count & (cfg->buf_count - 1)) != 0)
    return sio_error_invalid; /* must be power of two */
  if (cfg->buf_size == 0 || cfg->max_fds == 0 || cfg->ring_entries == 0)
    return sio_error_invalid;

  sio_context_t *ctx = calloc(1, sizeof(*ctx));
  if (!ctx)
    return sio_error_oom;

  ctx->shift     = cfg->shift;
  ctx->buf_count = cfg->buf_count;
  ctx->buf_size  = cfg->buf_size;
  ctx->max_fds   = cfg->max_fds;
  ctx->listen_fd = -1;

  /* Allocate fd table */
  ctx->fd_table = calloc(cfg->max_fds, sizeof(sio_fd_slot_t));
  if (!ctx->fd_table)
    goto cleanup_ctx;

  /* Register components */
  shift_component_info_t fd_info = {
      .element_size = sizeof(sio_fd_t),
      .constructor  = NULL,
      .destructor   = NULL,
  };
  if (shift_component_register(ctx->shift, &fd_info, &ctx->comp_ids.fd) !=
      shift_ok)
    goto cleanup_fd_table;

  shift_component_info_t rb_info = {
      .element_size = sizeof(sio_read_buf_t),
      .constructor  = NULL,
      .destructor   = NULL,
  };
  if (shift_component_register(ctx->shift, &rb_info, &ctx->comp_ids.read_buf) !=
      shift_ok)
    goto cleanup_fd_table;

  shift_component_info_t wb_info = {
      .element_size = sizeof(sio_write_buf_t),
      .constructor  = NULL,
      .destructor   = NULL,
  };
  if (shift_component_register(ctx->shift, &wb_info,
                               &ctx->comp_ids.write_buf) != shift_ok)
    goto cleanup_fd_table;

  /* Register collections */
  shift_component_id_t    connected_comps[] = {ctx->comp_ids.fd};
  shift_collection_info_t connected_info    = {
         .comp_ids     = connected_comps,
         .comp_count   = 1,
         .max_capacity = 0,
         .on_enter     = NULL,
         .on_leave     = NULL,
  };
  if (shift_collection_register(ctx->shift, &connected_info,
                                &ctx->coll_ids.connected) != shift_ok)
    goto cleanup_fd_table;

  shift_component_id_t    reading_comps[] = {ctx->comp_ids.fd,
                                             ctx->comp_ids.read_buf};
  shift_collection_info_t reading_info    = {
         .comp_ids     = reading_comps,
         .comp_count   = 2,
         .max_capacity = 0,
         .on_enter     = NULL,
         .on_leave     = NULL,
  };
  if (shift_collection_register(ctx->shift, &reading_info,
                                &ctx->coll_ids.reading) != shift_ok)
    goto cleanup_fd_table;

  shift_component_id_t    writing_comps[] = {ctx->comp_ids.fd,
                                             ctx->comp_ids.write_buf};
  shift_collection_info_t writing_info    = {
         .comp_ids     = writing_comps,
         .comp_count   = 2,
         .max_capacity = 0,
         .on_enter     = NULL,
         .on_leave     = NULL,
  };
  if (shift_collection_register(ctx->shift, &writing_info,
                                &ctx->coll_ids.writing) != shift_ok)
    goto cleanup_fd_table;

  /* Initialise io_uring */
  if (io_uring_queue_init(cfg->ring_entries, &ctx->ring, 0) < 0)
    goto cleanup_fd_table;
  ctx->ring_initialized = true;

  /* Allocate buffer backing memory */
  ctx->buf_base = malloc((size_t)cfg->buf_count * cfg->buf_size);
  if (!ctx->buf_base)
    goto cleanup_ring;

  /* Set up provided buffer ring */
  int br_ret;
  ctx->buf_ring = io_uring_setup_buf_ring(&ctx->ring, cfg->buf_count,
                                          SIO_BUF_GROUP_ID, 0, &br_ret);
  if (!ctx->buf_ring)
    goto cleanup_buf_base;

  /* Populate ring with all buffers in one shot */
  for (uint32_t i = 0; i < cfg->buf_count; i++) {
    void *buf = (char *)ctx->buf_base + (size_t)i * cfg->buf_size;
    io_uring_buf_ring_add(ctx->buf_ring, buf, cfg->buf_size, (uint16_t)i,
                          io_uring_buf_ring_mask(cfg->buf_count), (int)i);
  }
  io_uring_buf_ring_advance(ctx->buf_ring, (int)cfg->buf_count);

  *out = ctx;
  return sio_ok;

cleanup_buf_base:
  free(ctx->buf_base);
  ctx->buf_base = NULL;
cleanup_ring:
  io_uring_queue_exit(&ctx->ring);
  ctx->ring_initialized = false;
cleanup_fd_table:
  free(ctx->fd_table);
cleanup_ctx:
  free(ctx);
  return sio_error_oom;
}

/* --------------------------------------------------------------------------
 * sio_context_destroy
 * -------------------------------------------------------------------------- */

void sio_context_destroy(sio_context_t *ctx) {
  if (!ctx)
    return;

  if (ctx->listen_fd >= 0) {
    close(ctx->listen_fd);
    ctx->listen_fd = -1;
  }

  if (ctx->buf_ring) {
    io_uring_free_buf_ring(&ctx->ring, ctx->buf_ring, ctx->buf_count,
                           SIO_BUF_GROUP_ID);
    ctx->buf_ring = NULL;
  }

  free(ctx->buf_base);
  ctx->buf_base = NULL;

  if (ctx->ring_initialized) {
    io_uring_queue_exit(&ctx->ring);
    ctx->ring_initialized = false;
  }

  free(ctx->fd_table);
  free(ctx);
}

/* --------------------------------------------------------------------------
 * sio_arm_accept (static)
 * -------------------------------------------------------------------------- */

static sio_result_t sio_arm_accept(sio_context_t *ctx) {
  struct io_uring_sqe *sqe = io_uring_get_sqe(&ctx->ring);
  if (!sqe)
    return sio_error_no_sqe;

  io_uring_prep_multishot_accept(sqe, ctx->listen_fd, NULL, NULL, 0);
  io_uring_sqe_set_data64(sqe, sio_encode_ud(SIO_OP_ACCEPT, ctx->listen_fd));
  return sio_ok;
}

/* --------------------------------------------------------------------------
 * sio_arm_recv (static)
 * -------------------------------------------------------------------------- */

static sio_result_t sio_arm_recv(sio_context_t *ctx, int fd) {
  if (fd < 0 || (uint32_t)fd >= ctx->max_fds)
    return sio_error_invalid;

  sio_fd_slot_t *slot = &ctx->fd_table[fd];
  if (slot->recv_armed)
    return sio_ok; /* already armed */

  struct io_uring_sqe *sqe = io_uring_get_sqe(&ctx->ring);
  if (!sqe)
    return sio_error_no_sqe;

  io_uring_prep_recv(sqe, fd, NULL, 0, 0);
  sqe->flags |= IOSQE_BUFFER_SELECT;
  sqe->buf_group = SIO_BUF_GROUP_ID;
  io_uring_sqe_set_data64(sqe, sio_encode_ud(SIO_OP_RECV, fd));
  slot->recv_armed = true;
  return sio_ok;
}

/* --------------------------------------------------------------------------
 * sio_listen
 * -------------------------------------------------------------------------- */

sio_result_t sio_listen(sio_context_t *ctx, uint16_t port, int backlog) {
  if (!ctx)
    return sio_error_null;

  int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (fd < 0)
    return sio_error_io;

  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

  struct sockaddr_in addr = {
      .sin_family      = AF_INET,
      .sin_port        = __builtin_bswap16(port),
      .sin_addr.s_addr = INADDR_ANY,
  };

  if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    close(fd);
    return sio_error_io;
  }

  if (listen(fd, backlog) < 0) {
    close(fd);
    return sio_error_io;
  }

  ctx->listen_fd = fd;
  return sio_arm_accept(ctx);
}

/* --------------------------------------------------------------------------
 * sio_handle_accept_cqe (static)
 * -------------------------------------------------------------------------- */

static void sio_handle_accept_cqe(sio_context_t       *ctx,
                                  struct io_uring_cqe *cqe) {
  uint32_t flags  = cqe->flags;
  int      new_fd = cqe->res;

  if (new_fd < 0) {
    /* Error on accept — re-arm if multishot cancelled */
    if (!(flags & IORING_CQE_F_MORE))
      sio_arm_accept(ctx);
    return;
  }

  if ((uint32_t)new_fd >= ctx->max_fds) {
    close(new_fd);
    if (!(flags & IORING_CQE_F_MORE))
      sio_arm_accept(ctx);
    return;
  }

  /* Multishot exhausted — re-arm */
  if (!(flags & IORING_CQE_F_MORE))
    sio_arm_accept(ctx);

  /* Create entity in connected collection */
  shift_entity_t entity;
  if (shift_entity_create_one(ctx->shift, ctx->coll_ids.connected, &entity) !=
      shift_ok) {
    close(new_fd);
    return;
  }

  /* Set fd component */
  sio_fd_t *fd_comp = NULL;
  if (shift_entity_get_component(ctx->shift, entity, ctx->comp_ids.fd,
                                 (void **)&fd_comp) != shift_ok) {
    close(new_fd);
    shift_entity_destroy_one(ctx->shift, entity);
    return;
  }
  fd_comp->fd = new_fd;

  /* Update fd_table */
  sio_fd_slot_t *slot = &ctx->fd_table[new_fd];
  slot->entity        = entity;
  slot->valid         = true;
  slot->recv_armed    = false;

  sio_arm_recv(ctx, new_fd);
}

/* --------------------------------------------------------------------------
 * sio_handle_recv_cqe (static)
 * -------------------------------------------------------------------------- */

static void sio_handle_recv_cqe(sio_context_t *ctx, struct io_uring_cqe *cqe) {
  int fd = sio_ud_fd(io_uring_cqe_get_data64(cqe));

  if (fd < 0 || (uint32_t)fd >= ctx->max_fds)
    return;

  sio_fd_slot_t *slot = &ctx->fd_table[fd];
  slot->recv_armed    = false; /* clear before any early return */

  if (!slot->valid)
    return; /* stale CQE for a closed fd */

  int res = cqe->res;

  if (res > 0) {
    /* Data received — buffer selected by the kernel */
    uint16_t buf_id = (uint16_t)(cqe->flags >> IORING_CQE_BUFFER_SHIFT);
    void    *data   = (char *)ctx->buf_base + (size_t)buf_id * ctx->buf_size;

    sio_read_buf_t *rb = NULL;
    if (shift_entity_get_component(ctx->shift, slot->entity,
                                   ctx->comp_ids.read_buf,
                                   (void **)&rb) != shift_ok) {
      /* Can't set component — return buffer and close */
      io_uring_buf_ring_add(ctx->buf_ring, data, ctx->buf_size, buf_id,
                            io_uring_buf_ring_mask(ctx->buf_count), 0);
      io_uring_buf_ring_advance(ctx->buf_ring, 1);
      close(fd);
      slot->valid = false;
      shift_entity_destroy_one(ctx->shift, slot->entity);
      return;
    }
    rb->data   = data;
    rb->len    = (uint32_t)res;
    rb->buf_id = buf_id;

    shift_entity_move_one(ctx->shift, slot->entity, ctx->coll_ids.reading);
    /* Do NOT re-arm recv — user must call sio_read_consume first */

  } else if (res == 0 || res == -ECONNRESET || res == -EPIPE) {
    /* EOF or connection reset */
    close(fd);
    slot->valid = false;
    shift_entity_destroy_one(ctx->shift, slot->entity);

  } else if (res == -ENOBUFS) {
    /* No buffer available in the ring — re-arm; will succeed once a
     * consumer returns a buffer via sio_read_consume */
    sio_arm_recv(ctx, fd);

  } else if (res == -EINTR || res == -EAGAIN) {
    sio_arm_recv(ctx, fd);
  }
  /* All other errors are silently dropped */
}

/* --------------------------------------------------------------------------
 * sio_handle_send_cqe (static)
 * -------------------------------------------------------------------------- */

static void sio_handle_send_cqe(sio_context_t *ctx, struct io_uring_cqe *cqe) {
  int fd = sio_ud_fd(io_uring_cqe_get_data64(cqe));

  if (fd < 0 || (uint32_t)fd >= ctx->max_fds)
    return;

  sio_fd_slot_t *slot = &ctx->fd_table[fd];
  if (!slot->valid)
    return;

  if (cqe->res < 0) {
    /* Send error — close and destroy */
    close(fd);
    slot->valid = false;
    shift_entity_destroy_one(ctx->shift, slot->entity);
    return;
  }

  /*
   * NOTE (v0 known limitation): partial sends are not retried.
   * The full send is assumed to have completed.
   */

  /* Move back to connected and re-arm recv */
  shift_entity_move_one(ctx->shift, slot->entity, ctx->coll_ids.connected);
  sio_arm_recv(ctx, fd);
}

/* --------------------------------------------------------------------------
 * sio_poll
 * -------------------------------------------------------------------------- */

sio_result_t sio_poll(sio_context_t *ctx, uint32_t min_complete) {
  if (!ctx)
    return sio_error_null;

  /* Step 1: scan writing collection and submit pending sends */
  sio_fd_t        *fds    = NULL;
  sio_write_buf_t *wbufs  = NULL;
  size_t           wcount = 0;

  shift_collection_get_component_array(ctx->shift, ctx->coll_ids.writing,
                                       ctx->comp_ids.fd, (void **)&fds,
                                       &wcount);
  shift_collection_get_component_array(ctx->shift, ctx->coll_ids.writing,
                                       ctx->comp_ids.write_buf, (void **)&wbufs,
                                       NULL);

  for (size_t i = 0; i < wcount; i++) {
    /* Activate a pending sio_write reply that arrived before this flush */
    if (wbufs[i].data == NULL) {
      int fd = fds[i].fd;
      if (fd >= 0 && (uint32_t)fd < ctx->max_fds) {
        sio_fd_slot_t *slot = &ctx->fd_table[fd];
        if (slot->pending_write_data) {
          wbufs[i].data            = slot->pending_write_data;
          wbufs[i].len             = slot->pending_write_len;
          slot->pending_write_data = NULL;
          slot->pending_write_len  = 0;
        }
      }
    }

    if (wbufs[i].data == NULL)
      continue; /* already submitted */

    struct io_uring_sqe *sqe = io_uring_get_sqe(&ctx->ring);
    if (!sqe)
      break; /* ring full — will pick up remaining on next poll */

    io_uring_prep_send(sqe, fds[i].fd, wbufs[i].data, wbufs[i].len,
                       MSG_NOSIGNAL);
    io_uring_sqe_set_data64(sqe, sio_encode_ud(SIO_OP_SEND, fds[i].fd));
    wbufs[i].data = NULL; /* mark as submitted */
  }

  /* Step 2: submit and wait */
  int ret = io_uring_submit_and_wait(&ctx->ring, (unsigned)min_complete);
  if (ret < 0 && ret != -EINTR)
    return sio_error_io;

  /* Step 3: drain CQEs */
  struct io_uring_cqe *cqe;
  unsigned             head;
  uint32_t             cqe_count = 0;

  io_uring_for_each_cqe(&ctx->ring, head, cqe) {
    uint8_t op = sio_ud_op(io_uring_cqe_get_data64(cqe));
    switch (op) {
    case SIO_OP_ACCEPT:
      sio_handle_accept_cqe(ctx, cqe);
      break;
    case SIO_OP_RECV:
      sio_handle_recv_cqe(ctx, cqe);
      break;
    case SIO_OP_SEND:
      sio_handle_send_cqe(ctx, cqe);
      break;
    default:
      break;
    }
    cqe_count++;
  }

  /* Step 4: advance CQ head */
  io_uring_cq_advance(&ctx->ring, cqe_count);

  return sio_ok;
}

/* --------------------------------------------------------------------------
 * sio_read_consume
 * -------------------------------------------------------------------------- */

sio_result_t sio_read_consume(sio_context_t *ctx, shift_entity_t entity) {
  if (!ctx)
    return sio_error_null;

  if (shift_entity_is_stale(ctx->shift, entity))
    return sio_error_stale;

  sio_read_buf_t *rb = NULL;
  if (shift_entity_get_component(ctx->shift, entity, ctx->comp_ids.read_buf,
                                 (void **)&rb) != shift_ok)
    return sio_error_invalid;

  sio_fd_t *fd_comp = NULL;
  if (shift_entity_get_component(ctx->shift, entity, ctx->comp_ids.fd,
                                 (void **)&fd_comp) != shift_ok)
    return sio_error_invalid;

  int      fd     = fd_comp->fd;
  uint16_t buf_id = rb->buf_id;
  void    *data   = (char *)ctx->buf_base + (size_t)buf_id * ctx->buf_size;

  /* Return buffer to ring BEFORE arming recv (avoids ENOBUFS) */
  io_uring_buf_ring_add(ctx->buf_ring, data, ctx->buf_size, buf_id,
                        io_uring_buf_ring_mask(ctx->buf_count), 0);
  io_uring_buf_ring_advance(ctx->buf_ring, 1);

  /* Clear read_buf fields */
  rb->data   = NULL;
  rb->len    = 0;
  rb->buf_id = 0;

  /* Move entity back to connected */
  shift_entity_move_one(ctx->shift, entity, ctx->coll_ids.connected);

  /* Re-arm recv */
  sio_arm_recv(ctx, fd);

  return sio_ok;
}

/* --------------------------------------------------------------------------
 * sio_write
 * -------------------------------------------------------------------------- */

sio_result_t sio_write(sio_context_t *ctx, shift_entity_t entity,
                       const void *data, uint32_t len) {
  if (!ctx)
    return sio_error_null;

  if (shift_entity_is_stale(ctx->shift, entity))
    return sio_error_stale;

  /* Entity must be in reading (has read_buf component) */
  sio_read_buf_t *rb = NULL;
  if (shift_entity_get_component(ctx->shift, entity, ctx->comp_ids.read_buf,
                                 (void **)&rb) != shift_ok)
    return sio_error_invalid;

  sio_fd_t *fd_comp = NULL;
  if (shift_entity_get_component(ctx->shift, entity, ctx->comp_ids.fd,
                                 (void **)&fd_comp) != shift_ok)
    return sio_error_invalid;

  int      fd     = fd_comp->fd;
  uint16_t buf_id = rb->buf_id;
  void    *buf    = (char *)ctx->buf_base + (size_t)buf_id * ctx->buf_size;

  /* Return io_uring receive buffer to the ring */
  io_uring_buf_ring_add(ctx->buf_ring, buf, ctx->buf_size, buf_id,
                        io_uring_buf_ring_mask(ctx->buf_count), 0);
  io_uring_buf_ring_advance(ctx->buf_ring, 1);

  /* Clear read_buf fields */
  rb->data   = NULL;
  rb->len    = 0;
  rb->buf_id = 0;

  /* Stash write data in the fd slot — sio_poll will pick it up */
  if (fd >= 0 && (uint32_t)fd < ctx->max_fds) {
    ctx->fd_table[fd].pending_write_data = data;
    ctx->fd_table[fd].pending_write_len  = len;
  }

  /* Move entity to writing (NOT connected — no recv re-arm) */
  shift_entity_move_one(ctx->shift, entity, ctx->coll_ids.writing);

  return sio_ok;
}

/* --------------------------------------------------------------------------
 * sio_disconnect
 * -------------------------------------------------------------------------- */

sio_result_t sio_disconnect(sio_context_t *ctx, shift_entity_t entity) {
  if (!ctx)
    return sio_error_null;

  if (shift_entity_is_stale(ctx->shift, entity))
    return sio_error_stale;

  sio_fd_t *fd_comp = NULL;
  if (shift_entity_get_component(ctx->shift, entity, ctx->comp_ids.fd,
                                 (void **)&fd_comp) != shift_ok)
    return sio_error_invalid;

  int fd = fd_comp->fd;

  close(fd);

  if (fd >= 0 && (uint32_t)fd < ctx->max_fds) {
    ctx->fd_table[fd].valid = false;
  }

  shift_entity_destroy_one(ctx->shift, entity);
  return sio_ok;
}

/* --------------------------------------------------------------------------
 * Accessors
 * -------------------------------------------------------------------------- */

const sio_component_ids_t *sio_get_component_ids(const sio_context_t *ctx) {
  return ctx ? &ctx->comp_ids : NULL;
}

const sio_collection_ids_t *sio_get_collection_ids(const sio_context_t *ctx) {
  return ctx ? &ctx->coll_ids : NULL;
}
