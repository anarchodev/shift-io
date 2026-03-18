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

constexpr uint8_t SIO_OP_ACCEPT = 0;
constexpr uint8_t SIO_OP_RECV   = 1;
constexpr uint8_t SIO_OP_SEND   = 2;

constexpr uint16_t SIO_BUF_GROUP_ID = 0;

/* --------------------------------------------------------------------------
 * SQE user_data encoding
 * op in bits [39:32], fd in bits [31:0]
 * -------------------------------------------------------------------------- */

static inline uint64_t sio_encode_ud(uint8_t op, int fd) {
  return ((uint64_t)(uint8_t)op << 32) | (uint32_t)fd;
}

static inline uint8_t sio_ud_op(uint64_t ud) {
  return (uint8_t)(ud >> 32);
}

static inline int sio_ud_fd(uint64_t ud) {
  return (int)(uint32_t)ud;
}

/* --------------------------------------------------------------------------
 * Per-fd tracking slot
 * -------------------------------------------------------------------------- */

typedef struct {
  shift_entity_t entity;
  bool           valid;
  bool           recv_armed;
  const void    *pending_write_data; /* set by sio_write, cleared by sio_poll */
  uint32_t       pending_write_len;
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
  sio_fd_slot_t            *fd_table; /* indexed by fd value */
  uint32_t                  max_fds;
  int                       listen_fd; /* -1 if not listening */
  sio_component_ids_t       comp_ids;
  sio_collection_ids_t      coll_ids;
};
