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
 * ACCEPT:  SIO_UD_ACCEPT sentinel (UINT64_MAX — no entity)
 * RECV:    bit63=0 | generation[30:0] in bits[62:32] | index in bits[31:0]
 * SEND:    bit63=1 | generation[30:0] in bits[62:32] | index in bits[31:0]
 *
 * Generation is truncated to 31 bits; the top bit is used as the RECV/SEND
 * discriminator. Wraparound requires 2^31 slot reuses between SQE and CQE —
 * negligible in practice.
 * -------------------------------------------------------------------------- */

#define SIO_UD_ACCEPT UINT64_MAX

static inline uint64_t sio_encode_ud_recv(shift_entity_t e) {
  return ((uint64_t)(e.generation & 0x7FFFFFFFu) << 32) | e.index;
}

static inline uint64_t sio_encode_ud_send(shift_entity_t e) {
  return ((uint64_t)1 << 63) |
         ((uint64_t)(e.generation & 0x7FFFFFFFu) << 32) | e.index;
}

static inline bool sio_ud_is_accept(uint64_t ud) { return ud == UINT64_MAX; }
static inline bool sio_ud_is_send(uint64_t ud)   { return (ud >> 63) & 1; }

static inline shift_entity_t sio_ud_entity(uint64_t ud) {
  return (shift_entity_t){
      .index      = (uint32_t)ud,
      .generation = (uint32_t)((ud >> 32) & 0x7FFFFFFFu),
  };
}

/* --------------------------------------------------------------------------
 * Per-fd tracking slot
 * -------------------------------------------------------------------------- */

typedef struct {
  shift_entity_t entity;
  bool           valid;
  bool           recv_armed;
} sio_fd_slot_t;

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
  sio_fd_slot_t            *fd_table; /* indexed by slot value */
  uint32_t                  max_connections;
  int                       listen_fd; /* -1 if not listening */
  sio_component_ids_t       comp_ids;
  sio_collection_ids_t      coll_ids;          /* app-facing; returned by sio_get_collection_ids */
  shift_collection_id_t     coll_read_pending; /* internal: recv armed, waiting for data */
  shift_collection_id_t     coll_write_pending;/* internal: send SQE submitted, waiting for CQE */
  shift_collection_id_t     coll_write_retry;  /* internal: partial send, retried next poll */
  /* Batched fixed-file slot releases — flushed at the top of sio_poll */
  uint32_t                 *pending_releases;      /* slot indices, size max_connections */
  uint32_t                  pending_release_count;
  int                      *release_minus_one;     /* all-(-1) scratch, size max_connections */
};
