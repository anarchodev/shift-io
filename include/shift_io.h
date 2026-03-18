#pragma once

#include <shift.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* --------------------------------------------------------------------------
 * Result codes
 * -------------------------------------------------------------------------- */

typedef int sio_result_t;

constexpr sio_result_t sio_ok            = 0;
constexpr sio_result_t sio_error_null    = -1;
constexpr sio_result_t sio_error_oom     = -2;
constexpr sio_result_t sio_error_invalid = -3;
constexpr sio_result_t sio_error_io      = -4;
constexpr sio_result_t sio_error_stale   = -5;
constexpr sio_result_t sio_error_no_sqe  = -6;

/* --------------------------------------------------------------------------
 * Component types
 * -------------------------------------------------------------------------- */

typedef struct {
  int fd;
} sio_fd_t;

typedef struct {
  void    *data;
  uint32_t len;
  uint16_t buf_id;
} sio_read_buf_t;

typedef struct {
  const void *data; /* NULL means already submitted */
  uint32_t    len;
} sio_write_buf_t;

/* --------------------------------------------------------------------------
 * Registered IDs
 * -------------------------------------------------------------------------- */

typedef struct {
  shift_component_id_t fd;
  shift_component_id_t read_buf;
  shift_component_id_t write_buf;
} sio_component_ids_t;

typedef struct {
  shift_collection_id_t connected;
  shift_collection_id_t reading;
  shift_collection_id_t writing;
} sio_collection_ids_t;

/* --------------------------------------------------------------------------
 * Configuration
 * -------------------------------------------------------------------------- */

typedef struct {
  shift_t *shift;
  uint32_t
      buf_count; /* number of pinned receive buffers (must be power of two) */
  uint32_t buf_size;     /* size of each pinned receive buffer in bytes */
  uint32_t max_fds;      /* maximum file descriptor value + 1 */
  uint32_t ring_entries; /* io_uring queue depth */
} sio_config_t;

/* --------------------------------------------------------------------------
 * Opaque context
 * -------------------------------------------------------------------------- */

typedef struct sio_context sio_context_t;

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

sio_result_t sio_context_create(const sio_config_t *cfg, sio_context_t **out);
void         sio_context_destroy(sio_context_t *ctx);

sio_result_t sio_listen(sio_context_t *ctx, uint16_t port, int backlog);
sio_result_t sio_poll(sio_context_t *ctx, uint32_t min_complete);
sio_result_t sio_read_consume(sio_context_t *ctx, shift_entity_t entity);
sio_result_t sio_write(sio_context_t *ctx, shift_entity_t entity,
                       const void *data, uint32_t len);
sio_result_t sio_disconnect(sio_context_t *ctx, shift_entity_t entity);

const sio_component_ids_t  *sio_get_component_ids(const sio_context_t *ctx);
const sio_collection_ids_t *sio_get_collection_ids(const sio_context_t *ctx);
