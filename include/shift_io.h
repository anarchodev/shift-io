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
constexpr sio_result_t sio_error_no_sqe  = -5;

/* --------------------------------------------------------------------------
 * Component types
 * -------------------------------------------------------------------------- */

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

typedef struct {
  shift_entity_t entity; /* handle to internal connections entity */
} sio_conn_entity_t;

typedef struct {
  shift_entity_t entity; /* handle to user's connection_results entity */
} sio_user_conn_entity_t;

/* --------------------------------------------------------------------------
 * Registered IDs
 * -------------------------------------------------------------------------- */

typedef struct {
  shift_component_id_t read_buf;
  shift_component_id_t write_buf;
  shift_component_id_t io_result;
  shift_component_id_t conn_entity;      /* sio_conn_entity_t */
  shift_component_id_t user_conn_entity; /* sio_user_conn_entity_t */
} sio_component_ids_t;

typedef struct {
  shift_collection_id_t connections; /* user destroys entities here to close   */
  shift_collection_id_t read_in;     /* user moves consumed reads here         */
  shift_collection_id_t write_in;    /* user creates write entities here       */
} sio_collection_ids_t;

/* --------------------------------------------------------------------------
 * Configuration
 * -------------------------------------------------------------------------- */

typedef struct {
  shift_t *shift;
  sio_component_ids_t comp_ids; /* from sio_register_components */
  uint32_t
      buf_count; /* number of pinned receive buffers (must be power of two) */
  uint32_t buf_size;        /* size of each pinned receive buffer in bytes */
  uint32_t max_connections; /* maximum number of concurrent connections */
  uint32_t ring_entries;    /* io_uring queue depth */
  /* User-provided result collections.  Each must carry at least the required
   * sio components (validated at context creation via introspection).
   *   connection_results: >= {conn_entity}
   *   read_results:       >= {read_buf, io_result,
   *                            conn_entity, user_conn_entity}
   *   write_results:      >= {write_buf, io_result,
   *                            conn_entity, user_conn_entity}           */
  shift_collection_id_t connection_results;
  shift_collection_id_t read_results;
  shift_collection_id_t write_results;
  /* When true, the library destroys the user's connection_results entity
   * automatically when the connection disconnects (EOF or error). */
  bool auto_destroy_user_entity;
} sio_config_t;

/* --------------------------------------------------------------------------
 * Opaque context
 * -------------------------------------------------------------------------- */

typedef struct sio_context sio_context_t;

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

/* Register all sio component types on the given shift context.  Call this
 * BEFORE creating user collections so that the returned component IDs can be
 * used in collection registration.  The same IDs must be passed to
 * sio_context_create via sio_config_t::comp_ids. */
sio_result_t sio_register_components(shift_t *sh, sio_component_ids_t *out);

sio_result_t sio_context_create(const sio_config_t *cfg, sio_context_t **out);
void         sio_context_destroy(sio_context_t *ctx);

sio_result_t sio_listen(sio_context_t *ctx, uint16_t port, int backlog);
sio_result_t sio_poll(sio_context_t *ctx, uint32_t min_complete);

const sio_component_ids_t  *sio_get_component_ids(const sio_context_t *ctx);
const sio_collection_ids_t *sio_get_collection_ids(const sio_context_t *ctx);
