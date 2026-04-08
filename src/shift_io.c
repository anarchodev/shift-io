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
 * Only releases fixed-file slots for connection entities.  Connection
 * collections are identified by having the read_cycle_entity component.
 * Other collections (read/write) carry fd as a copied value but do not
 * own the slot.
 * -------------------------------------------------------------------------- */

static void fd_destructor(shift_t *sh, shift_collection_id_t col_id,
                          const shift_entity_t *entities, void *data,
                          uint32_t offset, uint32_t count,
                          void *user_data) {
  (void)entities;
  sio_context_t *ctx = user_data;
  if (!ctx)
    return;

  /* Check if this is a connection collection (has read_cycle_entity) */
  const shift_component_id_t *coll_comps = NULL;
  uint32_t coll_comp_count = 0;
  if (shift_collection_get_components(sh, col_id, &coll_comps,
                                      &coll_comp_count) != shift_ok)
    return;
  bool is_connection_coll = false;
  for (uint32_t i = 0; i < coll_comp_count; i++) {
    if (coll_comps[i] == ctx->comp_ids.read_cycle_entity) {
      is_connection_coll = true;
      break;
    }
  }
  if (!is_connection_coll)
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
 * read_cycle_entity component destructor
 * When a connection entity is destroyed, this destructor cleans up the
 * associated read-cycle entity.
 * -------------------------------------------------------------------------- */

static void read_cycle_entity_destructor(shift_t *sh,
                                         shift_collection_id_t col_id,
                                         const shift_entity_t *entities,
                                         void *data, uint32_t offset,
                                         uint32_t count, void *user_data) {
  (void)col_id;
  sio_context_t *ctx = user_data;
  if (!ctx)
    return;

  sio_read_cycle_entity_t *rces = (sio_read_cycle_entity_t *)data + offset;
  for (uint32_t i = 0; i < count; i++) {
    if (!shift_entity_is_stale(sh, rces[i].entity))
      shift_entity_destroy_one(sh, rces[i].entity);
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
static void sio_drain_connect_in(sio_context_t *ctx);
static void sio_handle_connect_socket_cqe(sio_context_t       *ctx,
                                          struct io_uring_cqe *cqe);
static void sio_handle_connect_cqe(sio_context_t       *ctx,
                                   struct io_uring_cqe *cqe);

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
 * sio_superset_components (static)
 * Builds a sorted, deduplicated component array that is the union of a
 * user-provided collection's components and additional internal components.
 * Caller must free() the returned array.  Returns NULL on failure.
 * -------------------------------------------------------------------------- */

static shift_component_id_t *
sio_superset_components(shift_t *sh,
                        shift_collection_id_t user_coll,
                        const shift_component_id_t *extra,
                        uint32_t extra_count,
                        uint32_t *out_count) {
  const shift_component_id_t *uc = NULL;
  uint32_t                    un = 0;
  if (shift_collection_get_components(sh, user_coll, &uc, &un) != shift_ok)
    return NULL;

  shift_component_id_t *buf = malloc((un + extra_count) * sizeof(*buf));
  if (!buf)
    return NULL;

  /* Start with the user's sorted component list. */
  memcpy(buf, uc, un * sizeof(*buf));
  uint32_t n = un;

  /* Insert each extra component in sorted order, skipping duplicates. */
  for (uint32_t e = 0; e < extra_count; e++) {
    shift_component_id_t id = extra[e];
    bool dup = false;
    uint32_t pos = n;
    for (uint32_t i = 0; i < n; i++) {
      if (buf[i] == id) { dup = true; break; }
      if (buf[i] > id)  { pos = i;    break; }
    }
    if (dup)
      continue;
    memmove(buf + pos + 1, buf + pos, (n - pos) * sizeof(*buf));
    buf[pos] = id;
    n++;
  }

  *out_count = n;
  return buf;
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

  shift_component_info_t fd_info = {
      .element_size = sizeof(sio_fd_t),
      .destructor   = fd_destructor,
  };
  if (shift_component_register(sh, &fd_info, &out->fd) != shift_ok)
    return sio_error_invalid;

  shift_component_info_t rce_info = {
      .element_size = sizeof(sio_read_cycle_entity_t),
      .destructor   = read_cycle_entity_destructor,
  };
  if (shift_component_register(sh, &rce_info, &out->read_cycle_entity) !=
      shift_ok)
    return sio_error_invalid;

  shift_component_info_t ca_info = {
      .element_size = sizeof(sio_connect_addr_t),
  };
  if (shift_component_register(sh, &ca_info, &out->connect_addr) != shift_ok)
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

  /* Set user_data on components registered in sio_register_components so
   * their destructors can access the sio context. */
  shift_component_set_user_data(ctx->shift, ctx->comp_ids.fd, ctx);
  shift_component_set_user_data(ctx->shift, ctx->comp_ids.read_cycle_entity,
                                ctx);

  /* Validate user-provided collections have required components.
   * Component IDs must be sorted for the two-pointer check. */
  err = sio_error_invalid;

  {
    /* connections must have >= {fd, read_cycle_entity} */
    shift_component_id_t conn_req[2] = {ctx->comp_ids.fd,
                                        ctx->comp_ids.read_cycle_entity};
    if (conn_req[0] > conn_req[1]) {
      shift_component_id_t tmp = conn_req[0];
      conn_req[0] = conn_req[1];
      conn_req[1] = tmp;
    }
    if (!sio_collection_has_components(ctx->shift, cfg->connections,
                                      conn_req, 2))
      goto cleanup_pending;
  }

  {
    /* read_results must have >= {read_buf, io_result, conn_entity} */
    shift_component_id_t read_req[3] = {
        ctx->comp_ids.read_buf, ctx->comp_ids.io_result,
        ctx->comp_ids.conn_entity};
    for (int i = 1; i < 3; i++) {
      shift_component_id_t key = read_req[i];
      int j = i - 1;
      while (j >= 0 && read_req[j] > key) {
        read_req[j + 1] = read_req[j];
        j--;
      }
      read_req[j + 1] = key;
    }
    if (!sio_collection_has_components(ctx->shift, cfg->read_results,
                                      read_req, 3))
      goto cleanup_pending;
  }

  {
    /* write_results must have >= {write_buf, io_result, conn_entity} */
    shift_component_id_t write_req[3] = {
        ctx->comp_ids.write_buf, ctx->comp_ids.io_result,
        ctx->comp_ids.conn_entity};
    for (int i = 1; i < 3; i++) {
      shift_component_id_t key = write_req[i];
      int j = i - 1;
      while (j >= 0 && write_req[j] > key) {
        write_req[j + 1] = write_req[j];
        j--;
      }
      write_req[j + 1] = key;
    }
    if (!sio_collection_has_components(ctx->shift, cfg->write_results,
                                      write_req, 3))
      goto cleanup_pending;
  }

  /* Store user-provided collection IDs */
  ctx->coll_connections   = cfg->connections;
  ctx->coll_read_results  = cfg->read_results;
  ctx->coll_write_results = cfg->write_results;

  err = sio_error_oom;

  /* Register read collections: same archetype as read_results.
   * Both read_pending and read_in share the archetype so entities can
   * move freely through the read pipeline. */
  {
    const shift_component_id_t *comps = NULL;
    uint32_t count = 0;
    if (shift_collection_get_components(ctx->shift, cfg->read_results,
                                        &comps, &count) != shift_ok)
      goto cleanup_pending;

    shift_collection_info_t read_pending_info = {
        .name       = "read_pending",
        .comp_ids   = comps,
        .comp_count = count,
    };
    if (shift_collection_register(ctx->shift, &read_pending_info,
                                  &ctx->coll_read_pending) != shift_ok)
      goto cleanup_pending;

    shift_collection_info_t read_in_info = {
        .name       = "read_in",
        .comp_ids   = comps,
        .comp_count = count,
    };
    if (shift_collection_register(ctx->shift, &read_in_info,
                                  &ctx->coll_ids.read_in) != shift_ok)
      goto cleanup_pending;
  }

  /* Register write collections: same archetype as write_results.
   * write_in, write_pending, and write_retry share the archetype so
   * entities move freely through the write pipeline. */
  {
    const shift_component_id_t *comps = NULL;
    uint32_t count = 0;
    if (shift_collection_get_components(ctx->shift, cfg->write_results,
                                        &comps, &count) != shift_ok)
      goto cleanup_pending;

    shift_collection_info_t write_in_info = {
        .name       = "write_in",
        .comp_ids   = comps,
        .comp_count = count,
    };
    if (shift_collection_register(ctx->shift, &write_in_info,
                                  &ctx->coll_ids.write_in) != shift_ok)
      goto cleanup_pending;

    shift_collection_info_t write_pending_info = {
        .name       = "write_pending",
        .comp_ids   = comps,
        .comp_count = count,
    };
    if (shift_collection_register(ctx->shift, &write_pending_info,
                                  &ctx->coll_write_pending) != shift_ok)
      goto cleanup_pending;

    shift_collection_info_t write_retry_info = {
        .name       = "write_retry",
        .comp_ids   = comps,
        .comp_count = count,
    };
    if (shift_collection_register(ctx->shift, &write_retry_info,
                                  &ctx->coll_write_retry) != shift_ok)
      goto cleanup_pending;
  }

  /* Optional: outbound connection support */
  if (cfg->enable_connect) {
    /* Validate connect_errors has required components:
     *   {io_result, connect_addr} */
    shift_component_id_t ce_req[2] = {ctx->comp_ids.io_result,
                                      ctx->comp_ids.connect_addr};
    if (ce_req[0] > ce_req[1]) {
      shift_component_id_t tmp = ce_req[0];
      ce_req[0] = ce_req[1];
      ce_req[1] = tmp;
    }
    if (!sio_collection_has_components(ctx->shift, cfg->connect_errors,
                                      ce_req, 2))
      goto cleanup_pending;

    ctx->coll_connect_errors = cfg->connect_errors;

    /* connect_in, connect_socket_pending, connect_pending: superset of
     * user's connections plus {connect_addr, io_result}.  The connect
     * entity must be able to move to connections (success) or
     * connect_errors (failure), so the archetype must be a superset of
     * both destinations. */
    {
      shift_component_id_t extra[] = {ctx->comp_ids.connect_addr,
                                      ctx->comp_ids.io_result};
      uint32_t count = 0;
      shift_component_id_t *comps = sio_superset_components(
          ctx->shift, cfg->connections, extra, 2, &count);
      if (!comps)
        goto cleanup_pending;

      shift_collection_info_t ci_info = {
          .name       = "connect_in",
          .comp_ids   = comps,
          .comp_count = count,
      };
      if (shift_collection_register(ctx->shift, &ci_info,
                                    &ctx->coll_ids.connect_in) != shift_ok) {
        free(comps);
        goto cleanup_pending;
      }

      shift_collection_info_t csp_info = {
          .name       = "connect_socket_pending",
          .comp_ids   = comps,
          .comp_count = count,
      };
      if (shift_collection_register(ctx->shift, &csp_info,
                                    &ctx->coll_connect_socket_pending) !=
          shift_ok) {
        free(comps);
        goto cleanup_pending;
      }

      shift_collection_info_t cp_info = {
          .name       = "connect_pending",
          .comp_ids   = comps,
          .comp_count = count,
      };
      bool ok = shift_collection_register(ctx->shift, &cp_info,
                                          &ctx->coll_connect_pending) == shift_ok;
      free(comps);
      if (!ok)
        goto cleanup_pending;
    }

    ctx->has_connect = true;
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

  /* Expose context to read_buf destructor via user_data */
  shift_component_set_user_data(ctx->shift, ctx->comp_ids.read_buf, ctx);

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
  size_t             count    = 0;

  shift_collection_get_entities(ctx->shift, ctx->coll_ids.read_in, &entities,
                                &count);
  shift_collection_get_component_array(ctx->shift, ctx->coll_ids.read_in,
                                       ctx->comp_ids.read_buf,
                                       (void **)&rbufs, NULL);
  shift_collection_get_component_array(ctx->shift, ctx->coll_ids.read_in,
                                       ctx->comp_ids.conn_entity,
                                       (void **)&conns, NULL);

  int armed = 0;
  for (size_t i = 0; i < count; i++) {
    /* Connection destroyed while this read was in the user's hands —
     * destroy the read entity (read_buf_destructor returns the buffer). */
    if (shift_entity_is_stale(ctx->shift, conns[i].entity)) {
      shift_entity_destroy_one(ctx->shift, entities[i]);
      continue;
    }

    /* Look up fd from the connection entity */
    sio_fd_t *conn_fd = NULL;
    shift_entity_get_component(ctx->shift, conns[i].entity,
                               ctx->comp_ids.fd, (void **)&conn_fd);
    int slot = conn_fd->fd;

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
  size_t             count = 0;

  shift_collection_get_entities(ctx->shift, coll_id, &ents, &count);
  shift_collection_get_component_array(ctx->shift, coll_id,
                                       ctx->comp_ids.conn_entity,
                                       (void **)&conns, NULL);
  shift_collection_get_component_array(ctx->shift, coll_id,
                                       ctx->comp_ids.write_buf,
                                       (void **)&wbufs, NULL);

  for (size_t i = 0; i < count; i++) {
    /* Connection destroyed while this write was queued —
     * discard the write entity. */
    if (shift_entity_is_stale(ctx->shift, conns[i].entity)) {
      shift_entity_destroy_one(ctx->shift, ents[i]);
      continue;
    }

    /* Look up fd from the connection entity */
    sio_fd_t *conn_fd = NULL;
    shift_entity_get_component(ctx->shift, conns[i].entity,
                               ctx->comp_ids.fd, (void **)&conn_fd);
    int slot = conn_fd->fd;

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
 * 1. Create connection entity in user's connections collection
 * 2. Create read-cycle entity in read_pending
 * 3. Link them together and arm recv
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

  /* Step 1: Create connection entity in user's connections collection */
  shift_entity_t conn_entity;
  if (shift_entity_create_one_immediate(ctx->shift, ctx->coll_connections,
                                        &conn_entity) != shift_ok) {
    sio_release_slot(ctx, new_slot);
    return rearm_result;
  }

  /* Set fd on connection entity */
  sio_fd_t *conn_fd = NULL;
  shift_entity_get_component(ctx->shift, conn_entity, ctx->comp_ids.fd,
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

  /* Step 2: Create read-cycle entity in read_pending */
  shift_entity_t read_entity;
  if (shift_entity_create_one_immediate(ctx->shift, ctx->coll_read_pending,
                                        &read_entity) != shift_ok) {
    sio_release_slot(ctx, new_slot);
    shift_entity_destroy_one(ctx->shift, conn_entity);
    return rearm_result;
  }

  /* Set conn_entity on read-cycle entity */
  sio_conn_entity_t *ce = NULL;
  shift_entity_get_component(ctx->shift, read_entity,
                             ctx->comp_ids.conn_entity, (void **)&ce);
  ce->entity = conn_entity;

  /* Store read-cycle entity handle on connection entity for destructor cleanup */
  sio_read_cycle_entity_t *rce = NULL;
  shift_entity_get_component(ctx->shift, conn_entity,
                             ctx->comp_ids.read_cycle_entity, (void **)&rce);
  rce->entity = read_entity;

  /* Arm recv */
  sio_arm_recv(ctx, new_slot, read_entity);
  return rearm_result;
}

/* --------------------------------------------------------------------------
 * sio_drain_connect_in (static)
 * For each entity in connect_in, submit a socket_direct_alloc SQE and move
 * the entity to connect_socket_pending.
 * -------------------------------------------------------------------------- */

static void sio_drain_connect_in(sio_context_t *ctx) {
  shift_entity_t *entities = NULL;
  size_t          count    = 0;

  shift_collection_get_entities(ctx->shift, ctx->coll_ids.connect_in,
                                &entities, &count);

  for (size_t i = 0; i < count; i++) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(&ctx->ring);
    if (!sqe)
      break;

    io_uring_prep_socket_direct_alloc(
        sqe, AF_INET, SOCK_STREAM, 0, 0);
    io_uring_sqe_set_data64(sqe, sio_encode_ud(entities[i]));
    shift_entity_move_one(ctx->shift, entities[i],
                          ctx->coll_connect_socket_pending);
  }
}

/* --------------------------------------------------------------------------
 * sio_handle_connect_socket_cqe (static)
 *
 * Socket creation for an outbound connect completed.  On success, store the
 * allocated fixed-file slot, submit the connect SQE + TCP_NODELAY, and move
 * the entity to connect_pending.  On failure, set io_result and move to
 * connect_results.
 * -------------------------------------------------------------------------- */

static void sio_handle_connect_socket_cqe(sio_context_t       *ctx,
                                          struct io_uring_cqe *cqe) {
  shift_entity_t entity = sio_ud_entity(io_uring_cqe_get_data64(cqe));

  if (shift_entity_is_stale(ctx->shift, entity))
    return;

  int slot = cqe->res;
  if (slot < 0) {
    /* Socket creation failed — surface error to user */
    sio_io_result_t *ir = NULL;
    shift_entity_get_component(ctx->shift, entity, ctx->comp_ids.io_result,
                               (void **)&ir);
    ir->error = slot;
    shift_entity_move_one(ctx->shift, entity, ctx->coll_connect_errors);
    return;
  }

  /* Store the allocated slot */
  sio_fd_t *fd = NULL;
  shift_entity_get_component(ctx->shift, entity, ctx->comp_ids.fd,
                             (void **)&fd);
  fd->fd = slot;

  /* Set TCP_NODELAY */
  {
    static int               one         = 1;
    struct io_uring_sqe *nodelay_sqe = io_uring_get_sqe(&ctx->ring);
    if (nodelay_sqe) {
      io_uring_prep_cmd_sock(nodelay_sqe, SOCKET_URING_OP_SETSOCKOPT, slot,
                             IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
      nodelay_sqe->flags |= IOSQE_FIXED_FILE;
      io_uring_sqe_set_data64(nodelay_sqe, SIO_UD_INTERNAL);
    }
  }

  /* Submit connect SQE */
  struct io_uring_sqe *sqe = io_uring_get_sqe(&ctx->ring);
  if (!sqe) {
    sio_release_slot(ctx, slot);
    fd->fd = -1; /* slot released; prevent fd_destructor double-release */
    sio_io_result_t *ir = NULL;
    shift_entity_get_component(ctx->shift, entity, ctx->comp_ids.io_result,
                               (void **)&ir);
    ir->error = -EAGAIN;
    shift_entity_move_one(ctx->shift, entity, ctx->coll_connect_errors);
    return;
  }

  sio_connect_addr_t *ca = NULL;
  shift_entity_get_component(ctx->shift, entity, ctx->comp_ids.connect_addr,
                             (void **)&ca);

  io_uring_prep_connect(sqe, slot, (struct sockaddr *)&ca->addr,
                        sizeof(ca->addr));
  sqe->flags |= IOSQE_FIXED_FILE;
  io_uring_sqe_set_data64(sqe, sio_encode_ud(entity));
  shift_entity_move_one(ctx->shift, entity, ctx->coll_connect_pending);
}

/* --------------------------------------------------------------------------
 * sio_handle_connect_cqe (static)
 *
 * Async connect completed.  On success, the connect entity becomes the
 * connection: create a read-cycle entity, arm recv, and move the entity
 * to connections.  On failure, move to connect_errors.
 * -------------------------------------------------------------------------- */

static void sio_handle_connect_cqe(sio_context_t       *ctx,
                                   struct io_uring_cqe *cqe) {
  shift_entity_t entity = sio_ud_entity(io_uring_cqe_get_data64(cqe));

  if (shift_entity_is_stale(ctx->shift, entity))
    return;

  sio_fd_t *ent_fd = NULL;
  shift_entity_get_component(ctx->shift, entity, ctx->comp_ids.fd,
                             (void **)&ent_fd);
  int slot = ent_fd->fd;

  sio_io_result_t *ir = NULL;
  shift_entity_get_component(ctx->shift, entity, ctx->comp_ids.io_result,
                             (void **)&ir);

  if (cqe->res < 0) {
    /* Connect failed — release slot and move to connect_errors */
    sio_release_slot(ctx, slot);
    ent_fd->fd = -1; /* slot released; prevent fd_destructor double-release */
    ir->error = cqe->res;
    shift_entity_move_one(ctx->shift, entity, ctx->coll_connect_errors);
    return;
  }

  /* --- Success: the connect entity becomes the connection --- */

  /* fd is already set on the entity from socket allocation.
   * Create a read-cycle entity in read_pending. */
  shift_entity_t read_entity;
  if (shift_entity_create_one_immediate(ctx->shift, ctx->coll_read_pending,
                                        &read_entity) != shift_ok) {
    sio_release_slot(ctx, slot);
    ent_fd->fd = -1;
    ir->error = -ENOMEM;
    shift_entity_move_one(ctx->shift, entity, ctx->coll_connect_errors);
    return;
  }

  /* Set conn_entity on read entity → the connect entity (now the connection) */
  sio_conn_entity_t *ce = NULL;
  shift_entity_get_component(ctx->shift, read_entity,
                             ctx->comp_ids.conn_entity, (void **)&ce);
  ce->entity = entity;

  /* Set read_cycle_entity on the connect entity for destructor cleanup */
  sio_read_cycle_entity_t *rce = NULL;
  shift_entity_get_component(ctx->shift, entity,
                             ctx->comp_ids.read_cycle_entity, (void **)&rce);
  rce->entity = read_entity;

  /* Arm recv */
  sio_arm_recv(ctx, slot, read_entity);

  /* Move the entity to connections — it IS the connection now */
  shift_entity_move_one(ctx->shift, entity, ctx->coll_connections);
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
          ctx->has_connect &&
          col_id == ctx->coll_connect_socket_pending)
        sio_handle_connect_socket_cqe(ctx, cqe);
      else if (!shift_entity_is_stale(ctx->shift, entity) &&
               ctx->has_connect &&
               col_id == ctx->coll_connect_pending)
        sio_handle_connect_cqe(ctx, cqe);
      else if (!shift_entity_is_stale(ctx->shift, entity) &&
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

  /* Step 2b: drain connect_in — submit socket creation SQEs, move to
   * connect_socket_pending. */
  if (ctx->has_connect)
    sio_drain_connect_in(ctx);

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
