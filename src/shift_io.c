#include "shift_io_internal.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static void sio_release_slot(sio_context_t *ctx, int slot);

/* --------------------------------------------------------------------------
 * fd component destructor
 * Only releases fixed-file slots for entities in the connections collection.
 * Other collections carry fd as a copied value but do not own the slot.
 * -------------------------------------------------------------------------- */

static void fd_destructor(shift_t *sh, shift_collection_id_t col_id,
                          const shift_entity_t *entities, void *data,
                          uint32_t offset, uint32_t count,
                          void *user_data) {
  (void)sh; (void)entities;
  sio_context_t *ctx = user_data;
  if (!ctx)
    return;

  /* Only the connections collection owns the slot lifetime */
  if (col_id != ctx->coll_ids.connections)
    return;

  sio_fd_t *fds = (sio_fd_t *)data + offset;
  for (uint32_t i = 0; i < count; i++) {
    int fd = fds[i].fd;
    if (fd < 0 || (uint32_t)fd >= ctx->max_connections)
      continue;
    sio_release_slot(ctx, fd);
  }
}

/* --------------------------------------------------------------------------
 * read_buf component destructor
 * Returns any live io_uring provided buffer back to the kernel ring so that
 * buffers are never leaked when a read-cycle entity is destroyed.
 * -------------------------------------------------------------------------- */

static void read_buf_destructor(shift_t *sh, shift_collection_id_t col_id,
                                const shift_entity_t *entities, void *data,
                                uint32_t offset, uint32_t count,
                                void *user_data) {
  (void)sh; (void)col_id; (void)entities;
  sio_context_t *ctx = user_data;
  if (!ctx)
    return;

  sio_read_buf_t *bufs = (sio_read_buf_t *)data + offset;
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
 * on_leave callback for the connections collection.
 * When a connections entity is destroyed (user closes the connection),
 * this callback destroys the associated read-cycle entity and optionally
 * the user's connection_results entity.
 * -------------------------------------------------------------------------- */

static void connections_on_leave(shift_t *sh, shift_collection_id_t col_id,
                                 const shift_entity_t *entities,
                                 uint32_t offset, uint32_t count,
                                 void *user_ctx) {
  (void)col_id;
  sio_context_t *ctx = (sio_context_t *)user_ctx;

  for (uint32_t i = 0; i < count; i++) {
    shift_entity_t conn_entity = entities[offset + i];

    /* Destroy the read-cycle entity if it's still alive */
    sio_read_cycle_entity_t *rce = NULL;
    if (shift_entity_get_component(sh, conn_entity,
                                   ctx->comp_read_cycle_entity,
                                   (void **)&rce) == shift_ok) {
      if (!shift_entity_is_stale(sh, rce->entity))
        shift_entity_destroy_one(sh, rce->entity);
    }

    /* Optionally destroy the user's connection entity */
    if (ctx->auto_destroy_user_entity) {
      sio_user_conn_entity_t *uce = NULL;
      if (shift_entity_get_component(sh, conn_entity,
                                     ctx->comp_ids.user_conn_entity,
                                     (void **)&uce) == shift_ok) {
        if (!shift_entity_is_stale(sh, uce->entity))
          shift_entity_destroy_one(sh, uce->entity);
      }
    }
  }
}

/* --------------------------------------------------------------------------
 * Forward declarations of static helpers
 * -------------------------------------------------------------------------- */

static sio_result_t sio_arm_accept(sio_context_t *ctx);
static sio_result_t sio_arm_recv(sio_context_t *ctx, int slot, shift_entity_t entity);
static bool         sio_arm_sends(sio_context_t *ctx, shift_collection_id_t coll_id);
static void         sio_drain_read_in(sio_context_t *ctx);
static void         sio_flush_releases(sio_context_t *ctx);
static sio_result_t sio_submit_and_drain(sio_context_t *ctx, uint32_t min_complete);
static sio_result_t sio_handle_accept_cqe(sio_context_t       *ctx,
                                          struct io_uring_cqe *cqe);
static void sio_handle_recv_cqe(sio_context_t *ctx, struct io_uring_cqe *cqe);
static void sio_handle_send_cqe(sio_context_t *ctx, struct io_uring_cqe *cqe);

/* --------------------------------------------------------------------------
 * sio_collection_has_components (static)
 * Validates that a user-provided collection contains all required component
 * IDs.  Uses shift_collection_get_components for introspection.
 * -------------------------------------------------------------------------- */

static bool sio_collection_has_components(shift_t *sh,
                                          shift_collection_id_t col_id,
                                          const shift_component_id_t *required,
                                          uint32_t required_count) {
  const shift_component_id_t *col_comps = NULL;
  uint32_t                    col_count = 0;

  if (shift_collection_get_components(sh, col_id, &col_comps, &col_count) !=
      shift_ok)
    return false;

  /* Both lists are sorted (shift guarantees this).  Walk with two pointers. */
  uint32_t ci = 0;
  for (uint32_t ri = 0; ri < required_count; ri++) {
    while (ci < col_count && col_comps[ci] < required[ri])
      ci++;
    if (ci >= col_count || col_comps[ci] != required[ri])
      return false;
  }
  return true;
}

/* --------------------------------------------------------------------------
 * sio_register_components
 * Registers all sio component types on the given shift context.  Must be
 * called before creating user collections so the returned IDs can be used
 * in collection registration.
 * -------------------------------------------------------------------------- */

sio_result_t sio_register_components(shift_t *sh, sio_component_ids_t *out) {
  if (!sh || !out)
    return sio_error_null;

  shift_component_info_t rb_info = {
      .element_size = sizeof(sio_read_buf_t),
      .constructor  = NULL,
      .destructor   = read_buf_destructor,
  };
  if (shift_component_register(sh, &rb_info, &out->read_buf) != shift_ok)
    return sio_error_invalid;

  shift_component_info_t wb_info = {
      .element_size = sizeof(sio_write_buf_t),
  };
  if (shift_component_register(sh, &wb_info, &out->write_buf) != shift_ok)
    return sio_error_invalid;

  shift_component_info_t ir_info = {
      .element_size = sizeof(sio_io_result_t),
  };
  if (shift_component_register(sh, &ir_info, &out->io_result) != shift_ok)
    return sio_error_invalid;

  shift_component_info_t ce_info = {
      .element_size = sizeof(sio_conn_entity_t),
  };
  if (shift_component_register(sh, &ce_info, &out->conn_entity) != shift_ok)
    return sio_error_invalid;

  shift_component_info_t uce_info = {
      .element_size = sizeof(sio_user_conn_entity_t),
  };
  if (shift_component_register(sh, &uce_info, &out->user_conn_entity) !=
      shift_ok)
    return sio_error_invalid;

  return sio_ok;
}

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
  ctx->auto_destroy_user_entity = cfg->auto_destroy_user_entity;
  ctx->comp_ids        = cfg->comp_ids; /* pre-registered by sio_register_components */
  sio_result_t err     = sio_error_oom; /* updated before each goto */

  /* Allocate batched-release buffers */
  ctx->pending_releases = malloc(cfg->max_connections * sizeof(uint32_t));
  if (!ctx->pending_releases)
    goto cleanup_ctx;

  ctx->release_minus_one = malloc(cfg->max_connections * sizeof(int));
  if (!ctx->release_minus_one)
    goto cleanup_pending;
  for (uint32_t i = 0; i < cfg->max_connections; i++)
    ctx->release_minus_one[i] = -1;

  /* Register internal-only components */
  {
    shift_component_info_t fd_info = {
        .element_size = sizeof(sio_fd_t),
        .destructor   = fd_destructor,
    };
    if (shift_component_register(ctx->shift, &fd_info,
                                 &ctx->comp_fd) != shift_ok)
      goto cleanup_pending;
  }
  {
    shift_component_info_t rce_info = {
        .element_size = sizeof(sio_read_cycle_entity_t),
    };
    if (shift_component_register(ctx->shift, &rce_info,
                                 &ctx->comp_read_cycle_entity) != shift_ok)
      goto cleanup_pending;
  }

  /* Validate user-provided collections have required components.
   * Component IDs must be sorted for the two-pointer check. */
  err = sio_error_invalid;

  {
    /* connection_results must have >= {conn_entity} */
    shift_component_id_t conn_req[1] = {ctx->comp_ids.conn_entity};
    if (!sio_collection_has_components(ctx->shift, cfg->connection_results,
                                      conn_req, 1))
      goto cleanup_pending;
  }

  {
    /* read_results must have >= {read_buf, io_result,
     *                            conn_entity, user_conn_entity} */
    shift_component_id_t read_req[4] = {
        ctx->comp_ids.read_buf, ctx->comp_ids.io_result,
        ctx->comp_ids.conn_entity, ctx->comp_ids.user_conn_entity};
    for (int i = 1; i < 4; i++) {
      shift_component_id_t key = read_req[i];
      int j = i - 1;
      while (j >= 0 && read_req[j] > key) {
        read_req[j + 1] = read_req[j];
        j--;
      }
      read_req[j + 1] = key;
    }
    if (!sio_collection_has_components(ctx->shift, cfg->read_results,
                                      read_req, 4))
      goto cleanup_pending;
  }

  {
    /* write_results must have >= {write_buf, io_result,
     *                             conn_entity, user_conn_entity} */
    shift_component_id_t write_req[4] = {
        ctx->comp_ids.write_buf, ctx->comp_ids.io_result,
        ctx->comp_ids.conn_entity, ctx->comp_ids.user_conn_entity};
    for (int i = 1; i < 4; i++) {
      shift_component_id_t key = write_req[i];
      int j = i - 1;
      while (j >= 0 && write_req[j] > key) {
        write_req[j + 1] = write_req[j];
        j--;
      }
      write_req[j + 1] = key;
    }
    if (!sio_collection_has_components(ctx->shift, cfg->write_results,
                                      write_req, 4))
      goto cleanup_pending;
  }

  /* Store user-provided collection IDs */
  ctx->coll_connection_results = cfg->connection_results;
  ctx->coll_read_results       = cfg->read_results;
  ctx->coll_write_results      = cfg->write_results;

  err = sio_error_oom;

  /* Register connections collection: {fd, user_conn_entity, read_cycle_entity} */
  {
    shift_component_id_t conn_comps[] = {ctx->comp_fd,
                                         ctx->comp_ids.user_conn_entity,
                                         ctx->comp_read_cycle_entity};
    shift_collection_info_t conn_info = {
        .comp_ids   = conn_comps,
        .comp_count = 3,
        .max_capacity = 0,
    };
    if (shift_collection_register(ctx->shift, &conn_info,
                                  &ctx->coll_ids.connections) != shift_ok)
      goto cleanup_pending;
  }

  /* Register on_leave callback for connections cleanup */
  {
    shift_handler_id_t handler_id;
    if (shift_collection_on_leave(ctx->shift, ctx->coll_ids.connections,
                                  connections_on_leave, ctx,
                                  &handler_id) != shift_ok)
      goto cleanup_pending;
  }

  /* Register read collections — all carry
   * {fd, read_buf, io_result, conn_entity, user_conn_entity} */
  shift_component_id_t read_comps[] = {
      ctx->comp_fd, ctx->comp_ids.read_buf, ctx->comp_ids.io_result,
      ctx->comp_ids.conn_entity, ctx->comp_ids.user_conn_entity};

  {
    shift_collection_info_t read_pending_info = {
        .comp_ids   = read_comps,
        .comp_count = 5,
        .max_capacity = 0,
    };
    if (shift_collection_register(ctx->shift, &read_pending_info,
                                  &ctx->coll_read_pending) != shift_ok)
      goto cleanup_pending;
  }

  {
    shift_collection_info_t read_in_info = {
        .comp_ids   = read_comps,
        .comp_count = 5,
        .max_capacity = 0,
    };
    if (shift_collection_register(ctx->shift, &read_in_info,
                                  &ctx->coll_ids.read_in) != shift_ok)
      goto cleanup_pending;
  }

  /* Register write collections — all carry
   * {fd, write_buf, io_result, conn_entity, user_conn_entity} */
  shift_component_id_t write_comps[] = {
      ctx->comp_fd, ctx->comp_ids.write_buf, ctx->comp_ids.io_result,
      ctx->comp_ids.conn_entity, ctx->comp_ids.user_conn_entity};

  {
    shift_collection_info_t write_in_info = {
        .comp_ids   = write_comps,
        .comp_count = 5,
        .max_capacity = 0,
    };
    if (shift_collection_register(ctx->shift, &write_in_info,
                                  &ctx->coll_ids.write_in) != shift_ok)
      goto cleanup_pending;
  }

  {
    shift_collection_info_t write_pending_info = {
        .comp_ids   = write_comps,
        .comp_count = 5,
        .max_capacity = 0,
    };
    if (shift_collection_register(ctx->shift, &write_pending_info,
                                  &ctx->coll_write_pending) != shift_ok)
      goto cleanup_pending;
  }

  {
    shift_collection_info_t write_retry_info = {
        .comp_ids   = write_comps,
        .comp_count = 5,
        .max_capacity = 0,
    };
    if (shift_collection_register(ctx->shift, &write_retry_info,
                                  &ctx->coll_write_retry) != shift_ok)
      goto cleanup_pending;
  }

  /* Initialise io_uring */
  if (cfg->ring_params) {
    if (io_uring_queue_init_params(cfg->ring_entries, &ctx->ring,
                                   cfg->ring_params) < 0) {
      err = sio_error_io;
      goto cleanup_pending;
    }
  } else {
    if (io_uring_queue_init(cfg->ring_entries, &ctx->ring, 0) < 0) {
      err = sio_error_io;
      goto cleanup_pending;
    }
  }
  ctx->ring_initialized = true;

  /* Register a fixed-file pool of max_connections slots (all -1 / empty). */
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

  /* Expose context to component destructors via user_data */
  shift_component_set_user_data(ctx->shift, ctx->comp_ids.read_buf, ctx);
  shift_component_set_user_data(ctx->shift, ctx->comp_fd, ctx);

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

static sio_result_t sio_arm_recv(sio_context_t *ctx, int slot,
                                 shift_entity_t entity) {
  if (slot < 0 || (uint32_t)slot >= ctx->max_connections)
    return sio_error_invalid;

  struct io_uring_sqe *sqe = io_uring_get_sqe(&ctx->ring);
  if (!sqe)
    return sio_error_no_sqe;

  io_uring_prep_recv(sqe, slot, NULL, 0, 0);
  sqe->flags |= IOSQE_FIXED_FILE | IOSQE_BUFFER_SELECT;
  sqe->buf_group = SIO_BUF_GROUP_ID;
  io_uring_sqe_set_data64(sqe, sio_encode_ud(entity));
  return sio_ok;
}

/* --------------------------------------------------------------------------
 * sio_drain_read_in (static)
 * Returns each buffer to the io_uring ring, moves the read-cycle entity back
 * to read_pending, and re-arms recv.  Zeroing the read_buf component before
 * the move prevents the read_buf destructor from double-returning the buffer
 * if the entity is later destroyed while in read_pending.
 * -------------------------------------------------------------------------- */

static void sio_drain_read_in(sio_context_t *ctx) {
  shift_entity_t    *entities = NULL;
  sio_read_buf_t    *rbufs    = NULL;
  sio_conn_entity_t *conns    = NULL;
  sio_fd_t          *fds      = NULL;
  size_t             count    = 0;

  shift_collection_get_entities(ctx->shift, ctx->coll_ids.read_in, &entities,
                                &count);
  shift_collection_get_component_array(ctx->shift, ctx->coll_ids.read_in,
                                       ctx->comp_ids.read_buf,
                                       (void **)&rbufs, NULL);
  shift_collection_get_component_array(ctx->shift, ctx->coll_ids.read_in,
                                       ctx->comp_ids.conn_entity,
                                       (void **)&conns, NULL);
  /* fd is zero-initialized after returning from user's read_results (which
   * lacks fd).  We also need to write the restored value back so read_pending
   * has a valid fd for the next CQE cycle. */
  shift_collection_get_component_array(ctx->shift, ctx->coll_ids.read_in,
                                       ctx->comp_fd, (void **)&fds, NULL);

  int armed = 0;
  for (size_t i = 0; i < count; i++) {
    /* Connection destroyed while this read was in the user's hands —
     * destroy the read entity (read_buf_destructor returns the buffer). */
    if (shift_entity_is_stale(ctx->shift, conns[i].entity)) {
      shift_entity_destroy_one(ctx->shift, entities[i]);
      continue;
    }

    /* Restore fd from the connections entity */
    sio_fd_t *conn_fd = NULL;
    shift_entity_get_component(ctx->shift, conns[i].entity,
                               ctx->comp_fd, (void **)&conn_fd);
    int slot = conn_fd->fd;
    fds[i].fd = slot;

    uint16_t buf_id = rbufs[i].buf_id;
    void    *data   = (char *)ctx->buf_base + (size_t)buf_id * ctx->buf_size;

    rbufs[i].data   = NULL;
    rbufs[i].len    = 0;
    rbufs[i].buf_id = 0;

    io_uring_buf_ring_add(ctx->buf_ring, data, ctx->buf_size, buf_id,
                          io_uring_buf_ring_mask(ctx->buf_count), armed);
    sio_arm_recv(ctx, slot, entities[i]);
    shift_entity_move_one(ctx->shift, entities[i], ctx->coll_read_pending);
    armed++;
  }
  if (armed > 0)
    io_uring_buf_ring_advance(ctx->buf_ring, armed);
}

/* --------------------------------------------------------------------------
 * sio_arm_sends (static)
 * Arms send SQEs for all write entities in coll_id.  Returns true if every
 * entity was armed, false if the SQ ring was exhausted mid-collection.
 * -------------------------------------------------------------------------- */

static bool sio_arm_sends(sio_context_t *ctx, shift_collection_id_t coll_id) {
  shift_entity_t    *ents  = NULL;
  sio_conn_entity_t *conns = NULL;
  sio_write_buf_t   *wbufs = NULL;
  sio_fd_t          *fds   = NULL;
  size_t             count = 0;

  shift_collection_get_entities(ctx->shift, coll_id, &ents, &count);
  shift_collection_get_component_array(ctx->shift, coll_id,
                                       ctx->comp_ids.conn_entity,
                                       (void **)&conns, NULL);
  shift_collection_get_component_array(ctx->shift, coll_id,
                                       ctx->comp_ids.write_buf,
                                       (void **)&wbufs, NULL);
  shift_collection_get_component_array(ctx->shift, coll_id,
                                       ctx->comp_fd, (void **)&fds, NULL);

  for (size_t i = 0; i < count; i++) {
    /* Connection destroyed while this write was queued —
     * discard the write entity. */
    if (shift_entity_is_stale(ctx->shift, conns[i].entity)) {
      shift_entity_destroy_one(ctx->shift, ents[i]);
      continue;
    }

    /* Look up fd from the connections entity */
    sio_fd_t *conn_fd = NULL;
    shift_entity_get_component(ctx->shift, conns[i].entity,
                               ctx->comp_fd, (void **)&conn_fd);
    int slot = conn_fd->fd;
    fds[i].fd = slot; /* restore for write_pending */

    struct io_uring_sqe *sqe = io_uring_get_sqe(&ctx->ring);
    if (!sqe)
      return false;
    io_uring_prep_send(sqe, slot,
                       (const char *)wbufs[i].data + wbufs[i].offset,
                       wbufs[i].len - wbufs[i].offset, MSG_NOSIGNAL);
    sqe->flags |= IOSQE_FIXED_FILE;
    io_uring_sqe_set_data64(sqe, sio_encode_ud(ents[i]));
    shift_entity_move_one(ctx->shift, ents[i], ctx->coll_write_pending);
  }
  return true;
}

/* --------------------------------------------------------------------------
 * sio_release_slot (static)
 * -------------------------------------------------------------------------- */

static void sio_release_slot(sio_context_t *ctx, int slot) {
  if (slot >= 0 && (uint32_t)slot < ctx->max_connections)
    ctx->pending_releases[ctx->pending_release_count++] = (uint32_t)slot;
}

/* --------------------------------------------------------------------------
 * sio_flush_releases (static)
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
  setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));

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
 *
 * New connection flow:
 * 1. Create entity in internal connections collection
 * 2. 2-phase create entity in user's connection_results (with conn_entity set)
 * 3. Create read-cycle entity in read_pending
 * 4. Link them together and arm recv
 * -------------------------------------------------------------------------- */

static sio_result_t sio_handle_accept_cqe(sio_context_t       *ctx,
                                          struct io_uring_cqe *cqe) {
  uint32_t     flags        = cqe->flags;
  int          new_slot     = cqe->res;
  sio_result_t rearm_result = sio_ok;

  if (new_slot < 0) {
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

  if (!(flags & IORING_CQE_F_MORE))
    rearm_result = sio_arm_accept(ctx);

  /* Step 1: Create entity in internal connections collection */
  shift_entity_t conn_entity;
  if (shift_entity_create_one_immediate(ctx->shift, ctx->coll_ids.connections,
                                        &conn_entity) != shift_ok) {
    sio_release_slot(ctx, new_slot);
    return rearm_result;
  }

  /* Set fd on connections entity */
  sio_fd_t *conn_fd = NULL;
  shift_entity_get_component(ctx->shift, conn_entity, ctx->comp_fd,
                             (void **)&conn_fd);
  conn_fd->fd = new_slot;

  /* Set TCP_NODELAY on the accepted socket to disable Nagle's algorithm.
   * Uses io_uring cmd_sock since new_slot is a direct descriptor. */
  {
    static int one = 1;
    struct io_uring_sqe *nodelay_sqe = io_uring_get_sqe(&ctx->ring);
    if (nodelay_sqe) {
      io_uring_prep_cmd_sock(nodelay_sqe, SOCKET_URING_OP_SETSOCKOPT,
                             new_slot, IPPROTO_TCP, TCP_NODELAY,
                             &one, sizeof(one));
      nodelay_sqe->flags |= IOSQE_FIXED_FILE;
      io_uring_sqe_set_data64(nodelay_sqe, SIO_UD_INTERNAL);
    }
  }

  /* Step 2: 2-phase create entity in user's connection_results.
   * Created after the connections entity so we can set conn_entity
   * between begin and end. */
  shift_entity_t user_conn;
  if (shift_entity_create_one_begin(ctx->shift, ctx->coll_connection_results,
                                    &user_conn) != shift_ok) {
    sio_release_slot(ctx, new_slot);
    shift_entity_destroy_one(ctx->shift, conn_entity);
    return rearm_result;
  }

  /* Set conn_entity on user connection entity — link to internal connection */
  sio_conn_entity_t *user_ce = NULL;
  shift_entity_get_component(ctx->shift, user_conn, ctx->comp_ids.conn_entity,
                             (void **)&user_ce);
  user_ce->entity = conn_entity;

  /* Finish 2-phase create — entity is now visible to user */
  if (shift_entity_create_one_end(ctx->shift, user_conn) != shift_ok) {
    sio_release_slot(ctx, new_slot);
    shift_entity_destroy_one(ctx->shift, conn_entity);
    return rearm_result;
  }

  /* Store user connection entity handle on connections entity */
  sio_user_conn_entity_t *uce = NULL;
  shift_entity_get_component(ctx->shift, conn_entity,
                             ctx->comp_ids.user_conn_entity, (void **)&uce);
  uce->entity = user_conn;

  /* Step 3: Create read-cycle entity in read_pending */
  shift_entity_t read_entity;
  if (shift_entity_create_one_immediate(ctx->shift, ctx->coll_read_pending,
                                        &read_entity) != shift_ok) {
    sio_release_slot(ctx, new_slot);
    shift_entity_destroy_one(ctx->shift, conn_entity);
    shift_entity_destroy_one(ctx->shift, user_conn);
    return rearm_result;
  }

  /* Set fd on read-cycle entity */
  sio_fd_t *read_fd = NULL;
  shift_entity_get_component(ctx->shift, read_entity, ctx->comp_fd,
                             (void **)&read_fd);
  read_fd->fd = new_slot;

  /* Set conn_entity on read-cycle entity (link back to connections) */
  sio_conn_entity_t *ce = NULL;
  shift_entity_get_component(ctx->shift, read_entity,
                             ctx->comp_ids.conn_entity, (void **)&ce);
  ce->entity = conn_entity;

  /* Set user_conn_entity on read-cycle entity */
  sio_user_conn_entity_t *read_uce = NULL;
  shift_entity_get_component(ctx->shift, read_entity,
                             ctx->comp_ids.user_conn_entity,
                             (void **)&read_uce);
  read_uce->entity = user_conn;

  /* Store read-cycle entity handle on connections entity for on_leave cleanup */
  sio_read_cycle_entity_t *rce = NULL;
  shift_entity_get_component(ctx->shift, conn_entity,
                             ctx->comp_read_cycle_entity, (void **)&rce);
  rce->entity = read_entity;

  /* Arm recv */
  sio_arm_recv(ctx, new_slot, read_entity);
  return rearm_result;
}

/* --------------------------------------------------------------------------
 * sio_handle_recv_cqe (static)
 * -------------------------------------------------------------------------- */

static void sio_handle_recv_cqe(sio_context_t *ctx, struct io_uring_cqe *cqe) {
  shift_entity_t entity = sio_ud_entity(io_uring_cqe_get_data64(cqe));

  if (shift_entity_is_stale(ctx->shift, entity))
    return;

  int res = cqe->res;

  if (res > 0) {
    if (!(cqe->flags & IORING_CQE_F_BUFFER)) {
      shift_entity_destroy_one(ctx->shift, entity);
      return;
    }

    uint16_t buf_id = (uint16_t)(cqe->flags >> IORING_CQE_BUFFER_SHIFT);
    void    *data   = (char *)ctx->buf_base + (size_t)buf_id * ctx->buf_size;

    sio_read_buf_t *rb = NULL;
    if (shift_entity_get_component(ctx->shift, entity, ctx->comp_ids.read_buf,
                                   (void **)&rb) != shift_ok) {
      io_uring_buf_ring_add(ctx->buf_ring, data, ctx->buf_size, buf_id,
                            io_uring_buf_ring_mask(ctx->buf_count), 0);
      io_uring_buf_ring_advance(ctx->buf_ring, 1);
      shift_entity_destroy_one(ctx->shift, entity);
      return;
    }
    rb->data   = data;
    rb->len    = (uint32_t)res;
    rb->buf_id = buf_id;

    /* Move read-cycle entity to user's read_results */
    shift_entity_move_one(ctx->shift, entity, ctx->coll_read_results);

  } else if (res == 0 || (res != -ENOBUFS && res != -EINTR && res != -EAGAIN)) {
    /* EOF or non-transient error — surface to user via read_results */
    sio_io_result_t *ir = NULL;
    shift_entity_get_component(ctx->shift, entity, ctx->comp_ids.io_result,
                               (void **)&ir);
    if (ir) {
      ir->error = res;
      shift_entity_move_one(ctx->shift, entity, ctx->coll_read_results);
    } else {
      shift_entity_destroy_one(ctx->shift, entity);
    }
  }
  /* -ENOBUFS/-EINTR/-EAGAIN: transient; entity stays in read_pending. */
}

/* --------------------------------------------------------------------------
 * sio_handle_send_cqe (static)
 * -------------------------------------------------------------------------- */

static void sio_handle_send_cqe(sio_context_t *ctx, struct io_uring_cqe *cqe) {
  shift_entity_t entity = sio_ud_entity(io_uring_cqe_get_data64(cqe));

  if (shift_entity_is_stale(ctx->shift, entity))
    return;

  sio_io_result_t *ir = NULL;
  shift_entity_get_component(ctx->shift, entity, ctx->comp_ids.io_result,
                             (void **)&ir);

  if (cqe->res < 0) {
    if (ir)
      ir->error = cqe->res;
    shift_entity_move_one(ctx->shift, entity, ctx->coll_write_results);
    return;
  }

  sio_write_buf_t *wb = NULL;
  if (shift_entity_get_component(ctx->shift, entity, ctx->comp_ids.write_buf,
                                 (void **)&wb) != shift_ok) {
    if (ir)
      ir->error = -EIO;
    shift_entity_move_one(ctx->shift, entity, ctx->coll_write_results);
    return;
  }

  wb->offset += (uint32_t)cqe->res;
  if (wb->offset < wb->len) {
    shift_entity_move_one(ctx->shift, entity, ctx->coll_write_retry);
    return;
  }

  /* Full send complete */
  shift_entity_move_one(ctx->shift, entity, ctx->coll_write_results);
}

/* --------------------------------------------------------------------------
 * sio_submit_and_drain (static)
 * -------------------------------------------------------------------------- */

static sio_result_t sio_submit_and_drain(sio_context_t *ctx,
                                         uint32_t       min_complete) {
  int ret = io_uring_submit_and_wait(&ctx->ring, (unsigned)min_complete);
  if (ret < 0 && ret != -EINTR)
    return sio_error_io;

  struct io_uring_cqe *cqe;
  unsigned             head;
  uint32_t             count  = 0;
  sio_result_t         result = sio_ok;

  io_uring_for_each_cqe(&ctx->ring, head, cqe) {
    uint64_t ud = io_uring_cqe_get_data64(cqe);
    if (sio_ud_is_internal(ud)) {
      /* fire-and-forget (e.g. TCP_NODELAY setsockopt) — ignore */
    } else if (sio_ud_is_accept(ud)) {
      if (sio_handle_accept_cqe(ctx, cqe) != sio_ok)
        result = sio_error_io;
    } else {
      shift_entity_t entity = sio_ud_entity(ud);
      shift_collection_id_t col_id;
      shift_entity_get_collection(ctx->shift, entity, &col_id);
      if (!shift_entity_is_stale(ctx->shift, entity) &&
          col_id == ctx->coll_write_pending)
        sio_handle_send_cqe(ctx, cqe);
      else
        sio_handle_recv_cqe(ctx, cqe);
    }
    count++;
  }

  io_uring_cq_advance(&ctx->ring, count);
  return result;
}

/* --------------------------------------------------------------------------
 * sio_poll
 * -------------------------------------------------------------------------- */

sio_result_t sio_poll(sio_context_t *ctx, uint32_t min_complete) {
  if (!ctx)
    return sio_error_null;

  /* Step 1: flush batched slot releases from previous tick. */
  sio_flush_releases(ctx);

  /* Step 2: drain read_in — return buffers to ring, move back to read_pending,
   * and re-arm recv. */
  sio_drain_read_in(ctx);

  /* Step 3: arm sends — drain write_retry first (partial sends from last tick),
   * then write_in (new sends from the app). */
  if (sio_arm_sends(ctx, ctx->coll_write_retry))
    sio_arm_sends(ctx, ctx->coll_ids.write_in);

  /* Step 4: commit all deferred moves so col_id is current before waiting. */
  shift_flush(ctx->shift);

  /* Steps 5-7: submit SQEs, wait for CQEs, dispatch handlers, advance CQ. */
  sio_result_t poll_result = sio_submit_and_drain(ctx, min_complete);

  /* Step 8: commit deferred entity moves queued during CQE processing. */
  shift_flush(ctx->shift);

  return poll_result;
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
