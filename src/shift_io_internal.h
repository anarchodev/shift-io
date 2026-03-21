#pragma once

#include <shift_io.h>
#include <liburing.h>
#include <stdbool.h>
#include <stdint.h>

/* --------------------------------------------------------------------------
 * Version check — requires liburing >= 2.4
 * -------------------------------------------------------------------------- */

static_assert(IO_URING_VERSION_MAJOR > 2 ||
                  (IO_URING_VERSION_MAJOR == 2 && IO_URING_VERSION_MINOR >= 4),
              "shift-io requires liburing >= 2.4");

/* --------------------------------------------------------------------------
 * Constants
 * -------------------------------------------------------------------------- */

constexpr uint16_t SIO_BUF_GROUP_ID = 0;

/* --------------------------------------------------------------------------
 * SQE user_data encoding
 *
 * ACCEPT:    SIO_UD_ACCEPT sentinel (UINT64_MAX — no entity)
 * RECV/SEND: generation in bits[63:32] | index in bits[31:0]
 *
 * CQE dispatch uses the entity's collection membership (write_pending vs
 * read_pending) rather than a discriminator bit. sio_poll flushes all
 * deferred moves before submit_and_wait so col_id is always current.
 * -------------------------------------------------------------------------- */

#define SIO_UD_ACCEPT UINT64_MAX

static inline uint64_t sio_encode_ud(shift_entity_t e) {
  return (uint64_t)e.generation << 32 | e.index;
}

static inline bool sio_ud_is_accept(uint64_t ud) { return ud == UINT64_MAX; }

static inline shift_entity_t sio_ud_entity(uint64_t ud) {
  return (shift_entity_t){
      .index      = (uint32_t)ud,
      .generation = (uint32_t)(ud >> 32),
  };
}

/* --------------------------------------------------------------------------
 * Internal component types (not exposed to users)
 * -------------------------------------------------------------------------- */

typedef struct {
  int fd; /* fixed-file slot index */
} sio_fd_t;

/* Stored on connections entities to track the associated read-cycle entity
 * for cleanup in the on_leave callback. */
typedef struct {
  shift_entity_t entity;
} sio_read_cycle_entity_t;

/* --------------------------------------------------------------------------
 * Internal context
 * -------------------------------------------------------------------------- */

struct sio_context {
  shift_t                  *shift;
  struct io_uring           ring;
  bool                      ring_initialized;
  struct io_uring_buf_ring *buf_ring;
  void                     *buf_base; /* malloc'd: buf_count * buf_size */
  uint32_t                  buf_count;
  uint32_t                  buf_size;
  uint32_t                  max_connections;
  int                       listen_fd; /* -1 if not listening */
  sio_component_ids_t       comp_ids;
  sio_collection_ids_t      coll_ids;  /* connections, read_in, write_in */
  /* Internal-only component IDs (not exposed via sio_component_ids_t) */
  shift_component_id_t      comp_fd;
  shift_component_id_t      comp_read_cycle_entity;
  /* User-provided result collections */
  shift_collection_id_t     coll_connection_results;
  shift_collection_id_t     coll_read_results;
  shift_collection_id_t     coll_write_results;
  /* Internal collections */
  shift_collection_id_t     coll_read_pending; /* recv armed, waiting for data */
  shift_collection_id_t     coll_write_pending;/* send SQE submitted, waiting for CQE */
  shift_collection_id_t     coll_write_retry;  /* partial send, retried next poll */
  bool                      auto_destroy_user_entity;
  /* Batched fixed-file slot releases — flushed at the top of sio_poll */
  uint32_t                 *pending_releases;      /* slot indices, size max_connections */
  uint32_t                  pending_release_count;
  int                      *release_minus_one;     /* all-(-1) scratch, size max_connections */
};
