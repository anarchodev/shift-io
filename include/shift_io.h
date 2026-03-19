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
  int error; /* 0 = success; negative = errno-style error code */
} sio_io_result_t;

typedef struct {
  const void *data;    /* original malloc'd pointer; app always frees this */
  uint32_t    len;     /* total bytes to send */
  uint32_t    offset;  /* bytes already sent; initialise to 0 */
} sio_write_buf_t;

/* --------------------------------------------------------------------------
 * Registered IDs
 * -------------------------------------------------------------------------- */

typedef struct {
  shift_component_id_t fd;
  shift_component_id_t read_buf;
  shift_component_id_t write_buf;
  shift_component_id_t io_result;
} sio_component_ids_t;

typedef struct {
  /* read_result_out: data arrived (error==0, len>0) or connection closed
   *   (error==0 len==0 for EOF; error<0 for reset/error). App destroys entity
   *   on close; moves to read_in after consuming data to re-arm recv. */
  shift_collection_id_t read_result_out;
  shift_collection_id_t read_in;          /* app done; next poll returns buf       */
  shift_collection_id_t close_in;         /* app requests close; next poll closes  */
  shift_collection_id_t write_in;         /* app deposits write jobs here          */
  /* write_result_out: send done (error==0) or failed (error<0).
   *   App must free write_buf.data and destroy entity in both cases. */
  shift_collection_id_t write_result_out;
} sio_collection_ids_t;

/* --------------------------------------------------------------------------
 * Configuration
 * -------------------------------------------------------------------------- */

typedef struct {
  shift_t *shift;
  uint32_t
      buf_count; /* number of pinned receive buffers (must be power of two) */
  uint32_t buf_size;        /* size of each pinned receive buffer in bytes */
  uint32_t max_connections; /* maximum number of concurrent connections */
  uint32_t ring_entries;    /* io_uring queue depth */
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
sio_result_t sio_disconnect(sio_context_t *ctx, shift_entity_t entity);

const sio_component_ids_t  *sio_get_component_ids(const sio_context_t *ctx);
const sio_collection_ids_t *sio_get_collection_ids(const sio_context_t *ctx);
