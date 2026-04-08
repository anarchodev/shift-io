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
 * ACCEPT:      SIO_UD_ACCEPT   sentinel (UINT64_MAX — no entity)
 * INTERNAL:    SIO_UD_INTERNAL sentinel (UINT64_MAX-1 — fire-and-forget)
 * RECV/SEND:   generation in bits[63:32] | index in bits[31:0]
 *
 * CQE dispatch uses the entity's collection membership (write_pending vs
 * read_pending) rather than a discriminator bit. CQE handlers use
 * _immediate moves so col_id is current for subsequent CQEs in the
 * same batch.
 * -------------------------------------------------------------------------- */

#define SIO_UD_ACCEPT   UINT64_MAX
#define SIO_UD_INTERNAL (UINT64_MAX - 1)

static inline uint64_t sio_encode_ud(shift_entity_t e) {
  return (uint64_t)e.generation << 32 | e.index;
}

static inline bool sio_ud_is_accept(uint64_t ud)   { return ud == SIO_UD_ACCEPT; }
static inline bool sio_ud_is_internal(uint64_t ud) { return ud == SIO_UD_INTERNAL; }

static inline shift_entity_t sio_ud_entity(uint64_t ud) {
  return (shift_entity_t){
      .index      = (uint32_t)ud,
      .generation = (uint32_t)(ud >> 32),
  };
}

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
  sio_collection_ids_t      coll_ids;  /* read_in, write_in */
  /* User-provided collections */
  shift_collection_id_t     coll_connections;
  shift_collection_id_t     coll_read_results;
  shift_collection_id_t     coll_write_results;
  /* Internal collections */
  shift_collection_id_t     coll_read_pending; /* recv armed, waiting for data */
  shift_collection_id_t     coll_write_pending;/* send SQE submitted, waiting for CQE */
  shift_collection_id_t     coll_write_retry;  /* partial send, retried next poll */
  /* Batched fixed-file slot releases — flushed at the top of sio_poll */
  uint32_t                 *pending_releases;      /* slot indices, size max_connections */
  uint32_t                  pending_release_count;
  int                      *release_minus_one;     /* all-(-1) scratch, size max_connections */
  /* Outbound connection support */
  bool                      has_connect;
  shift_collection_id_t     coll_connect_errors;
  shift_collection_id_t     coll_connect_socket_pending;
  shift_collection_id_t     coll_connect_pending;
};
