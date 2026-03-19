#include "shift_io_internal.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* --------------------------------------------------------------------------
 * Module-static context pointer used by the read_buf destructor.
 * A single sio_context per process is the expected usage.
 * -------------------------------------------------------------------------- */

static sio_context_t *g_sio_ctx_for_destructor = NULL;

/* --------------------------------------------------------------------------
 * read_buf component destructor
 * Returns any live io_uring provided buffer back to the kernel ring so that
 * buffers are never leaked when a connection entity is destroyed (e.g. on
 * sio_disconnect while the entity is in read_out).
 * -------------------------------------------------------------------------- */

static void read_buf_destructor(void *data, uint32_t count) {
  sio_context_t *ctx = g_sio_ctx_for_destructor;
  if (!ctx)
    return;

  sio_read_buf_t *bufs = data;
  int             n    = 0;

  for (uint32_t i = 0; i < count; i++) {
    if (bufs[i].data == NULL)
      continue;
    void *buf_ptr =
        (char *)ctx->buf_base + (size_t)bufs[i].buf_id * ctx->buf_size;
    io_uring_buf_ring_add(ctx->buf_ring, buf_ptr, ctx->buf_size, bufs[i].buf_id,
                          io_uring_buf_ring_mask(ctx->buf_count), n++);
  }

  if (n > 0)
    io_uring_buf_ring_advance(ctx->buf_ring, n);
}

/* --------------------------------------------------------------------------
 * Forward declarations of static helpers
 * -------------------------------------------------------------------------- */

static sio_result_t sio_arm_accept(sio_context_t *ctx);
static sio_result_t sio_arm_recv(sio_context_t *ctx, int slot);
static void         sio_release_slot(sio_context_t *ctx, int slot);
static void         sio_flush_releases(sio_context_t *ctx);
static sio_result_t sio_handle_accept_cqe(sio_context_t       *ctx,
                                          struct io_uring_cqe *cqe);
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
  if (cfg->buf_size == 0 || cfg->max_connections == 0 || cfg->ring_entries == 0)
    return sio_error_invalid;

  sio_context_t *ctx = calloc(1, sizeof(*ctx));
  if (!ctx)
    return sio_error_oom;

  ctx->shift           = cfg->shift;
  ctx->buf_count       = cfg->buf_count;
  ctx->buf_size        = cfg->buf_size;
  ctx->max_connections = cfg->max_connections;
  ctx->listen_fd       = -1;
  sio_result_t err     = sio_error_oom; /* updated before each goto */

  /* Allocate fd table */
  ctx->fd_table = calloc(cfg->max_connections, sizeof(sio_fd_slot_t));
  if (!ctx->fd_table)
    goto cleanup_ctx;

  /* Allocate batched-release buffers */
  ctx->pending_releases = malloc(cfg->max_connections * sizeof(uint32_t));
  if (!ctx->pending_releases)
    goto cleanup_fd_table;

  ctx->release_minus_one = malloc(cfg->max_connections * sizeof(int));
  if (!ctx->release_minus_one)
    goto cleanup_pending;
  for (uint32_t i = 0; i < cfg->max_connections; i++)
    ctx->release_minus_one[i] = -1;

  /* Register components */
  shift_component_info_t fd_info = {
      .element_size = sizeof(sio_fd_t),
      .constructor  = NULL,
      .destructor   = NULL,
  };
  if (shift_component_register(ctx->shift, &fd_info, &ctx->comp_ids.fd) !=
      shift_ok)
    goto cleanup_pending;

  /* read_buf destructor returns io_uring provided buffers to the kernel when
   * a connection entity is destroyed while holding a live buffer. */
  shift_component_info_t rb_info = {
      .element_size = sizeof(sio_read_buf_t),
      .constructor  = NULL,
      .destructor   = read_buf_destructor,
  };
  if (shift_component_register(ctx->shift, &rb_info, &ctx->comp_ids.read_buf) !=
      shift_ok)
    goto cleanup_pending;

  shift_component_info_t wb_info = {
      .element_size = sizeof(sio_write_buf_t),
      .constructor  = NULL,
      .destructor   = NULL,
  };
  if (shift_component_register(ctx->shift, &wb_info,
                               &ctx->comp_ids.write_buf) != shift_ok)
    goto cleanup_pending;

  shift_component_info_t ir_info = {
      .element_size = sizeof(sio_io_result_t),
      .constructor  = NULL,
      .destructor   = NULL,
  };
  if (shift_component_register(ctx->shift, &ir_info,
                               &ctx->comp_ids.io_result) != shift_ok)
    goto cleanup_pending;

  /* Register read collections — all carry {fd, read_buf, io_result} */
  shift_component_id_t read_pending_comps[] = {
      ctx->comp_ids.fd, ctx->comp_ids.read_buf, ctx->comp_ids.io_result};
  shift_collection_info_t read_pending_info = {
      .comp_ids     = read_pending_comps,
      .comp_count   = 3,
      .max_capacity = 0,
      .on_enter     = NULL,
      .on_leave     = NULL,
  };
  if (shift_collection_register(ctx->shift, &read_pending_info,
                                &ctx->coll_read_pending) != shift_ok)
    goto cleanup_pending;

  shift_component_id_t read_result_out_comps[] = {
      ctx->comp_ids.fd, ctx->comp_ids.read_buf, ctx->comp_ids.io_result};
  shift_collection_info_t read_result_out_info = {
      .comp_ids     = read_result_out_comps,
      .comp_count   = 3,
      .max_capacity = 0,
      .on_enter     = NULL,
      .on_leave     = NULL,
  };
  if (shift_collection_register(ctx->shift, &read_result_out_info,
                                &ctx->coll_ids.read_result_out) != shift_ok)
    goto cleanup_pending;

  shift_component_id_t read_in_comps[] = {
      ctx->comp_ids.fd, ctx->comp_ids.read_buf, ctx->comp_ids.io_result};
  shift_collection_info_t read_in_info = {
      .comp_ids     = read_in_comps,
      .comp_count   = 3,
      .max_capacity = 0,
      .on_enter     = NULL,
      .on_leave     = NULL,
  };
  if (shift_collection_register(ctx->shift, &read_in_info,
                                &ctx->coll_ids.read_in) != shift_ok)
    goto cleanup_pending;

  shift_component_id_t close_in_comps[] = {
      ctx->comp_ids.fd, ctx->comp_ids.read_buf, ctx->comp_ids.io_result};
  shift_collection_info_t close_in_info = {
      .comp_ids     = close_in_comps,
      .comp_count   = 3,
      .max_capacity = 0,
      .on_enter     = NULL,
      .on_leave     = NULL,
  };
  if (shift_collection_register(ctx->shift, &close_in_info,
                                &ctx->coll_ids.close_in) != shift_ok)
    goto cleanup_pending;

  /* Register write collections — all carry {fd, write_buf, io_result}.
   * write_in:          app deposits write jobs here; sio_poll arms the send.
   * write_pending:     send SQE submitted; waiting for CQE.
   * write_result_out:  send done or failed; app checks io_result.error,
   *                    frees write_buf.data, and destroys entity.
   * write_retry:       partial send; library retries on next poll (internal).
   */
  shift_component_id_t write_comps[] = {
      ctx->comp_ids.fd, ctx->comp_ids.write_buf, ctx->comp_ids.io_result};

  shift_collection_info_t write_in_info = {
      .comp_ids     = write_comps,
      .comp_count   = 3,
      .max_capacity = 0,
      .on_enter     = NULL,
      .on_leave     = NULL,
  };
  if (shift_collection_register(ctx->shift, &write_in_info,
                                &ctx->coll_ids.write_in) != shift_ok)
    goto cleanup_pending;

  shift_collection_info_t write_pending_info = {
      .comp_ids     = write_comps,
      .comp_count   = 3,
      .max_capacity = 0,
      .on_enter     = NULL,
      .on_leave     = NULL,
  };
  if (shift_collection_register(ctx->shift, &write_pending_info,
                                &ctx->coll_write_pending) != shift_ok)
    goto cleanup_pending;

  shift_collection_info_t write_result_out_info = {
      .comp_ids     = write_comps,
      .comp_count   = 3,
      .max_capacity = 0,
      .on_enter     = NULL,
      .on_leave     = NULL,
  };
  if (shift_collection_register(ctx->shift, &write_result_out_info,
                                &ctx->coll_ids.write_result_out) != shift_ok)
    goto cleanup_pending;

  shift_collection_info_t write_retry_info = {
      .comp_ids     = write_comps,
      .comp_count   = 3,
      .max_capacity = 0,
      .on_enter     = NULL,
      .on_leave     = NULL,
  };
  if (shift_collection_register(ctx->shift, &write_retry_info,
                                &ctx->coll_write_retry) != shift_ok)
    goto cleanup_pending;

  /* Initialise io_uring */
  if (io_uring_queue_init(cfg->ring_entries, &ctx->ring, 0) < 0) {
    err = sio_error_io;
    goto cleanup_pending;
  }
  ctx->ring_initialized = true;

  /* Register a fixed-file pool of max_connections slots (all -1 / empty).
   * Accepted connections are installed directly into this table by
   * io_uring_prep_multishot_accept_direct; the CQE result is a slot
   * index rather than a real fd, eliminating per-SQE fd-table lookups. */
  {
    int *reg_fds = malloc(cfg->max_connections * sizeof(int));
    if (!reg_fds)
      goto cleanup_ring;
    for (uint32_t i = 0; i < cfg->max_connections; i++)
      reg_fds[i] = -1;
    int reg_ret =
        io_uring_register_files(&ctx->ring, reg_fds, (int)cfg->max_connections);
    free(reg_fds);
    if (reg_ret < 0) {
      err = sio_error_io;
      goto cleanup_ring;
    }
  }

  /* Allocate buffer backing memory */
  ctx->buf_base = malloc((size_t)cfg->buf_count * cfg->buf_size);
  if (!ctx->buf_base)
    goto cleanup_ring;

  /* Set up provided buffer ring */
  int br_ret;
  ctx->buf_ring = io_uring_setup_buf_ring(&ctx->ring, cfg->buf_count,
                                          SIO_BUF_GROUP_ID, 0, &br_ret);
  if (!ctx->buf_ring) {
    err = sio_error_io;
    goto cleanup_buf_base;
  }

  /* Populate ring with all buffers in one shot */
  for (uint32_t i = 0; i < cfg->buf_count; i++) {
    void *buf = (char *)ctx->buf_base + (size_t)i * cfg->buf_size;
    io_uring_buf_ring_add(ctx->buf_ring, buf, cfg->buf_size, (uint16_t)i,
                          io_uring_buf_ring_mask(cfg->buf_count), (int)i);
  }
  io_uring_buf_ring_advance(ctx->buf_ring, (int)cfg->buf_count);

  /* Expose context to the read_buf destructor */
  g_sio_ctx_for_destructor = ctx;

  *out = ctx;
  return sio_ok;

cleanup_buf_base:
  free(ctx->buf_base);
  ctx->buf_base = NULL;
cleanup_ring:
  io_uring_queue_exit(&ctx->ring);
  ctx->ring_initialized = false;
cleanup_pending:
  free(ctx->release_minus_one);
  ctx->release_minus_one = NULL;
  free(ctx->pending_releases);
  ctx->pending_releases = NULL;
cleanup_fd_table:
  free(ctx->fd_table);
cleanup_ctx:
  free(ctx);
  return err;
}

/* --------------------------------------------------------------------------
 * sio_context_destroy
 * -------------------------------------------------------------------------- */

void sio_context_destroy(sio_context_t *ctx) {
  if (!ctx)
    return;

  if (g_sio_ctx_for_destructor == ctx)
    g_sio_ctx_for_destructor = NULL;

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
  free(ctx->pending_releases);
  free(ctx->release_minus_one);
  free(ctx);
}

/* --------------------------------------------------------------------------
 * sio_arm_accept (static)
 * -------------------------------------------------------------------------- */

static sio_result_t sio_arm_accept(sio_context_t *ctx) {
  struct io_uring_sqe *sqe = io_uring_get_sqe(&ctx->ring);
  if (!sqe)
    return sio_error_no_sqe;

  io_uring_prep_multishot_accept_direct(sqe, ctx->listen_fd, NULL, NULL, 0);
  io_uring_sqe_set_data64(sqe, SIO_UD_ACCEPT);
  return sio_ok;
}

/* --------------------------------------------------------------------------
 * sio_arm_recv (static)
 * -------------------------------------------------------------------------- */

static sio_result_t sio_arm_recv(sio_context_t *ctx, int slot) {
  if (slot < 0 || (uint32_t)slot >= ctx->max_connections)
    return sio_error_invalid;

  sio_fd_slot_t *s = &ctx->fd_table[slot];
  if (s->recv_armed)
    return sio_ok; /* already armed */

  struct io_uring_sqe *sqe = io_uring_get_sqe(&ctx->ring);
  if (!sqe)
    return sio_error_no_sqe;

  io_uring_prep_recv(sqe, slot, NULL, 0, 0);
  sqe->flags |= IOSQE_FIXED_FILE | IOSQE_BUFFER_SELECT;
  sqe->buf_group = SIO_BUF_GROUP_ID;
  io_uring_sqe_set_data64(sqe, sio_encode_ud_recv(s->entity));
  s->recv_armed = true;
  return sio_ok;
}

/* --------------------------------------------------------------------------
 * sio_release_slot (static)
 * Queues a fixed-file slot for release. Actual io_uring_register_files_update
 * calls are batched by sio_flush_releases at the top of each sio_poll tick,
 * avoiding one blocking syscall per close inside the CQE drain loop.
 * -------------------------------------------------------------------------- */

static void sio_release_slot(sio_context_t *ctx, int slot) {
  if (slot >= 0 && (uint32_t)slot < ctx->max_connections)
    ctx->pending_releases[ctx->pending_release_count++] = (uint32_t)slot;
}

/* --------------------------------------------------------------------------
 * sio_flush_releases (static)
 * Sorts pending slot indices, finds contiguous runs, and issues one
 * io_uring_register_files_update call per run to minimise syscall count.
 * -------------------------------------------------------------------------- */

static int sio_cmp_uint32(const void *a, const void *b) {
  uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
  return (x > y) - (x < y);
}

static void sio_flush_releases(sio_context_t *ctx) {
  uint32_t count = ctx->pending_release_count;
  if (count == 0)
    return;

  qsort(ctx->pending_releases, count, sizeof(uint32_t), sio_cmp_uint32);

  uint32_t i = 0;
  while (i < count) {
    uint32_t start = ctx->pending_releases[i];
    uint32_t run   = 1;
    while (i + run < count && ctx->pending_releases[i + run] == start + run)
      run++;
    /* release_minus_one is all -1s (size max_connections) — pass the right
     * subrange */
    io_uring_register_files_update(&ctx->ring, start, ctx->release_minus_one,
                                   (int)run);
    i += run;
  }

  ctx->pending_release_count = 0;
}

/* --------------------------------------------------------------------------
 * sio_listen
 * -------------------------------------------------------------------------- */

sio_result_t sio_listen(sio_context_t *ctx, uint16_t port, int backlog) {
  if (!ctx)
    return sio_error_null;
  if (ctx->listen_fd >= 0)
    return sio_error_invalid; /* already listening */

  int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (fd < 0)
    return sio_error_io;

  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

  struct sockaddr_in addr = {
      .sin_family      = AF_INET,
      .sin_port        = htons(port),
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

static sio_result_t sio_handle_accept_cqe(sio_context_t       *ctx,
                                          struct io_uring_cqe *cqe) {
  uint32_t     flags        = cqe->flags;
  int          new_slot     = cqe->res;
  sio_result_t rearm_result = sio_ok;

  if (new_slot < 0) {
    /* Error on accept — re-arm if multishot cancelled */
    if (!(flags & IORING_CQE_F_MORE))
      rearm_result = sio_arm_accept(ctx);
    return rearm_result;
  }

  if ((uint32_t)new_slot >= ctx->max_connections) {
    sio_release_slot(ctx, new_slot);
    if (!(flags & IORING_CQE_F_MORE))
      rearm_result = sio_arm_accept(ctx);
    return rearm_result;
  }

  /* Multishot exhausted — re-arm */
  if (!(flags & IORING_CQE_F_MORE))
    rearm_result = sio_arm_accept(ctx);

  /* Create entity in read_pending */
  shift_entity_t entity;
  if (shift_entity_create_one(ctx->shift, ctx->coll_read_pending, &entity) !=
      shift_ok) {
    sio_release_slot(ctx, new_slot);
    return rearm_result;
  }

  /* Set fd component (create is eager, so the entity is already placed) */
  sio_fd_t *fd_comp = NULL;
  if (shift_entity_get_component(ctx->shift, entity, ctx->comp_ids.fd,
                                 (void **)&fd_comp) != shift_ok) {
    sio_release_slot(ctx, new_slot);
    shift_entity_destroy_one(ctx->shift, entity);
    return rearm_result;
  }
  fd_comp->fd = new_slot;

  /* Update fd_table and arm recv immediately — no need to wait for next tick */
  sio_fd_slot_t *fd_slot = &ctx->fd_table[new_slot];
  fd_slot->entity        = entity;
  fd_slot->valid         = true;
  fd_slot->recv_armed    = false;
  sio_arm_recv(ctx, new_slot);
  return rearm_result;
}

/* --------------------------------------------------------------------------
 * sio_handle_recv_cqe (static)
 * -------------------------------------------------------------------------- */

static void sio_handle_recv_cqe(sio_context_t *ctx, struct io_uring_cqe *cqe) {
  shift_entity_t entity = sio_ud_entity(io_uring_cqe_get_data64(cqe));

  /* Stale check: if the entity's generation no longer matches, this CQE
   * arrived after the connection was closed and the slot was reused. */
  if (shift_entity_is_stale(ctx->shift, entity))
    return;

  /* Recover fd from the entity's fd component */
  sio_fd_t *fd_comp = NULL;
  if (shift_entity_get_component(ctx->shift, entity, ctx->comp_ids.fd,
                                 (void **)&fd_comp) != shift_ok)
    return;

  int            fd   = fd_comp->fd;
  sio_fd_slot_t *slot = &ctx->fd_table[fd];
  slot->recv_armed    = false; /* clear before any early return */

  int res = cqe->res;

  if (res > 0) {
    /* Verify the kernel selected a buffer — absence indicates a kernel bug or
     * misconfiguration; treat it as a fatal connection error. */
    if (!(cqe->flags & IORING_CQE_F_BUFFER)) {
      sio_release_slot(ctx, fd);
      slot->valid = false;
      shift_entity_destroy_one(ctx->shift, entity);
      return;
    }

    uint16_t buf_id = (uint16_t)(cqe->flags >> IORING_CQE_BUFFER_SHIFT);
    void    *data   = (char *)ctx->buf_base + (size_t)buf_id * ctx->buf_size;

    /* Entity is in read_pending — read_buf component is accessible */
    sio_read_buf_t *rb = NULL;
    if (shift_entity_get_component(ctx->shift, entity, ctx->comp_ids.read_buf,
                                   (void **)&rb) != shift_ok) {
      /* Unexpected — return buffer and close */
      io_uring_buf_ring_add(ctx->buf_ring, data, ctx->buf_size, buf_id,
                            io_uring_buf_ring_mask(ctx->buf_count), 0);
      io_uring_buf_ring_advance(ctx->buf_ring, 1);
      sio_release_slot(ctx, fd);
      slot->valid = false;
      shift_entity_destroy_one(ctx->shift, entity);
      return;
    }
    rb->data   = data;
    rb->len    = (uint32_t)res;
    rb->buf_id = buf_id;
    /* io_result.error stays 0 (zero-initialised); move to read_result_out */
    shift_entity_move_one(ctx->shift, entity, ctx->coll_ids.read_result_out);

  } else if (res == 0 || (res != -ENOBUFS && res != -EINTR && res != -EAGAIN)) {
    /* EOF (res==0) or non-transient error — surface to app via read_result_out.
     * The slot is already released; app must destroy the entity. */
    sio_io_result_t *ir = NULL;
    shift_entity_get_component(ctx->shift, entity, ctx->comp_ids.io_result,
                               (void **)&ir);
    sio_release_slot(ctx, fd);
    slot->valid = false;
    if (ir) {
      ir->error = res; /* 0 = EOF, negative = errno */
      shift_entity_move_one(ctx->shift, entity, ctx->coll_ids.read_result_out);
    } else {
      shift_entity_destroy_one(ctx->shift, entity); /* unexpected fallback */
    }
  }
  /* -ENOBUFS/-EINTR/-EAGAIN: transient; recv_armed cleared, re-arms next tick
   */
}

/* --------------------------------------------------------------------------
 * sio_handle_send_cqe (static)
 *
 * The SQE user_data "fd" field carries the write entity's index so we can
 * locate it without a separate fd→entity lookup.  On success the entity moves
 * to write_out for the application to clean up.  On error the write entity is
 * destroyed; the connection entity's recv cycle handles its own teardown.
 * -------------------------------------------------------------------------- */

static void sio_handle_send_cqe(sio_context_t *ctx, struct io_uring_cqe *cqe) {
  /* Entity handle was captured at SQE submission time — generation reflects
   * the write entity as it was when the send was armed. */
  shift_entity_t entity = sio_ud_entity(io_uring_cqe_get_data64(cqe));

  if (shift_entity_is_stale(ctx->shift, entity))
    return; /* write entity already gone */

  sio_io_result_t *ir = NULL;
  shift_entity_get_component(ctx->shift, entity, ctx->comp_ids.io_result,
                             (void **)&ir);

  if (cqe->res < 0) {
    /* Send failed — record error and surface to app via write_result_out */
    if (ir)
      ir->error = cqe->res;
    shift_entity_move_one(ctx->shift, entity, ctx->coll_ids.write_result_out);
    return;
  }

  /* Update offset; if not all bytes were sent, queue for retry */
  sio_write_buf_t *wb = NULL;
  if (shift_entity_get_component(ctx->shift, entity, ctx->comp_ids.write_buf,
                                 (void **)&wb) != shift_ok) {
    if (ir)
      ir->error = -EIO;
    shift_entity_move_one(ctx->shift, entity, ctx->coll_ids.write_result_out);
    return;
  }

  wb->offset += (uint32_t)cqe->res;
  if (wb->offset < wb->len) {
    /* Partial send — library retries on the next sio_poll tick */
    shift_entity_move_one(ctx->shift, entity, ctx->coll_write_retry);
    return;
  }

  /* Full send complete — io_result.error stays 0 */
  shift_entity_move_one(ctx->shift, entity, ctx->coll_ids.write_result_out);
}

/* --------------------------------------------------------------------------
 * sio_poll
 * -------------------------------------------------------------------------- */

sio_result_t sio_poll(sio_context_t *ctx, uint32_t min_complete) {
  if (!ctx)
    return sio_error_null;

  /* Step 0a: flush batched slot releases from the previous tick.
   * Done before close_in drain so that any slots released by sio_disconnect
   * between ticks are flushed before we queue new ones from close_in. */
  sio_flush_releases(ctx);

  /* Step 0b: drain close_in — close fd and destroy entity for each connection
   * the app has flagged for teardown. Done before read_in drain and recv-arm
   * so we never try to re-arm a recv for a fd we are about to close. */
  {
    shift_entity_t *cl_entities = NULL;
    sio_fd_t       *cl_fds      = NULL;
    size_t          cl_count    = 0;

    shift_collection_get_entities(ctx->shift, ctx->coll_ids.close_in,
                                  &cl_entities, &cl_count);
    shift_collection_get_component_array(ctx->shift, ctx->coll_ids.close_in,
                                         ctx->comp_ids.fd, (void **)&cl_fds,
                                         NULL);

    for (size_t i = 0; i < cl_count; i++) {
      int fd = cl_fds[i].fd;
      sio_release_slot(ctx, fd);
      if (fd >= 0 && (uint32_t)fd < ctx->max_connections) {
        ctx->fd_table[fd].valid      = false;
        ctx->fd_table[fd].recv_armed = false;
      }
      /* read_buf destructor returns any live io_uring buffer automatically */
      shift_entity_destroy_one(ctx->shift, cl_entities[i]);
    }

    /* Flush releases queued by close_in drain before arming new recvs */
    sio_flush_releases(ctx);
  }

  /* Step 1: drain read_in — return buffers to ring, move back to read_pending,
   * and re-arm recv immediately (avoids a full read_pending scan each tick). */
  {
    shift_entity_t *centities = NULL;
    sio_fd_t       *cfds      = NULL;
    sio_read_buf_t *crbufs    = NULL;
    size_t          ccount    = 0;

    shift_collection_get_entities(ctx->shift, ctx->coll_ids.read_in, &centities,
                                  &ccount);
    shift_collection_get_component_array(ctx->shift, ctx->coll_ids.read_in,
                                         ctx->comp_ids.fd, (void **)&cfds,
                                         NULL);
    shift_collection_get_component_array(ctx->shift, ctx->coll_ids.read_in,
                                         ctx->comp_ids.read_buf,
                                         (void **)&crbufs, NULL);

    for (size_t i = 0; i < ccount; i++) {
      uint16_t buf_id = crbufs[i].buf_id;
      void    *data   = (char *)ctx->buf_base + (size_t)buf_id * ctx->buf_size;

      /* Zero the component so the read_buf destructor skips it if the entity
       * is later destroyed in read_pending (e.g. on disconnect). */
      crbufs[i].data   = NULL;
      crbufs[i].len    = 0;
      crbufs[i].buf_id = 0;

      io_uring_buf_ring_add(ctx->buf_ring, data, ctx->buf_size, buf_id,
                            io_uring_buf_ring_mask(ctx->buf_count), (int)i);
      shift_entity_move_one(ctx->shift, centities[i], ctx->coll_read_pending);
      sio_arm_recv(ctx, cfds[i].fd);
    }
    if (ccount > 0)
      io_uring_buf_ring_advance(ctx->buf_ring, (int)ccount);
  }

  /* Step 2 eliminated: recv is armed on accept and on read_in drain above,
   * so there is no need to scan read_pending every tick. */

  /* Step 3: arm sends — drain write_retry first (partial sends from last tick),
   * then write_in (new sends from the app).  Retry entities already have offset
   * set; new entities have offset == 0.  Both use data+offset / len-offset.
   * If the SQ ring is full, remaining entities stay in their collection. */
  {
    /* Helper lambda via compound-literal: arm one collection of write entities
     */
#define SIO_ARM_SENDS(coll_id)                                               \
  do {                                                                       \
    shift_entity_t  *_ents  = NULL;                                          \
    sio_fd_t        *_fds   = NULL;                                          \
    sio_write_buf_t *_wbufs = NULL;                                          \
    size_t           _count = 0;                                             \
    shift_collection_get_entities(ctx->shift, (coll_id), &_ents, &_count);   \
    shift_collection_get_component_array(                                    \
        ctx->shift, (coll_id), ctx->comp_ids.fd, (void **)&_fds, NULL);      \
    shift_collection_get_component_array(ctx->shift, (coll_id),              \
                                         ctx->comp_ids.write_buf,            \
                                         (void **)&_wbufs, NULL);            \
    for (size_t _i = 0; _i < _count; _i++) {                                 \
      struct io_uring_sqe *_sqe = io_uring_get_sqe(&ctx->ring);              \
      if (!_sqe)                                                             \
        goto send_ring_full;                                                 \
      io_uring_prep_send(_sqe, _fds[_i].fd,                                  \
                         (const char *)_wbufs[_i].data + _wbufs[_i].offset,  \
                         _wbufs[_i].len - _wbufs[_i].offset, MSG_NOSIGNAL);  \
      _sqe->flags |= IOSQE_FIXED_FILE;                                       \
      io_uring_sqe_set_data64(_sqe, sio_encode_ud_send(_ents[_i]));          \
      shift_entity_move_one(ctx->shift, _ents[_i], ctx->coll_write_pending); \
    }                                                                        \
  } while (0)

    SIO_ARM_SENDS(ctx->coll_write_retry);
    SIO_ARM_SENDS(ctx->coll_ids.write_in);
  send_ring_full:;
#undef SIO_ARM_SENDS
  }

  /* Step 4: submit and wait */
  int ret = io_uring_submit_and_wait(&ctx->ring, (unsigned)min_complete);
  if (ret < 0 && ret != -EINTR)
    return sio_error_io;

  /* Step 5: drain CQEs */
  struct io_uring_cqe *cqe;
  unsigned             head;
  uint32_t             cqe_count   = 0;
  sio_result_t         poll_result = sio_ok;

  io_uring_for_each_cqe(&ctx->ring, head, cqe) {
    uint64_t ud = io_uring_cqe_get_data64(cqe);
    if (sio_ud_is_accept(ud)) {
      if (sio_handle_accept_cqe(ctx, cqe) != sio_ok)
        poll_result = sio_error_io;
    } else if (sio_ud_is_send(ud))
      sio_handle_send_cqe(ctx, cqe);
    else
      sio_handle_recv_cqe(ctx, cqe);
    cqe_count++;
  }

  /* Step 6: advance CQ head */
  io_uring_cq_advance(&ctx->ring, cqe_count);

  /* Step 7: commit all deferred entity moves queued during this tick.
   * The app receives a consistent view of collections on return. */
  shift_flush(ctx->shift);

  return poll_result;
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

  sio_release_slot(ctx, fd);

  if (fd >= 0 && (uint32_t)fd < ctx->max_connections) {
    ctx->fd_table[fd].valid = false;
  }

  /* Destroying the entity triggers read_buf_destructor, which returns any live
   * io_uring provided buffer back to the kernel ring automatically. */
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
