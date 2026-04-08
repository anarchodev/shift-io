#pragma once

#include <shift.h>
struct io_uring_params;

#include <netinet/in.h>
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
  shift_entity_t entity; /* handle to connections entity */
} sio_conn_entity_t;

typedef struct {
  int fd; /* fixed-file slot index */
} sio_fd_t;

/* Stored on connection entities to track the associated read-cycle entity
 * for cleanup when the connection is destroyed. */
typedef struct {
  shift_entity_t entity;
} sio_read_cycle_entity_t;

typedef struct {
  struct sockaddr_in addr; /* target address for outbound connection */
} sio_connect_addr_t;

/* --------------------------------------------------------------------------
 * Registered IDs
 * -------------------------------------------------------------------------- */

typedef struct {
  shift_component_id_t read_buf;
  shift_component_id_t write_buf;
  shift_component_id_t io_result;
  shift_component_id_t conn_entity;      /* sio_conn_entity_t */
  shift_component_id_t fd;               /* sio_fd_t */
  shift_component_id_t read_cycle_entity;/* sio_read_cycle_entity_t */
  shift_component_id_t connect_addr;     /* sio_connect_addr_t */
} sio_component_ids_t;

typedef struct {
  shift_collection_id_t read_in;     /* user moves consumed reads here         */
  shift_collection_id_t write_in;    /* user creates write entities here       */
  shift_collection_id_t connect_in;  /* user creates connect entities here     */
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
  /* User-provided collections.  Each must carry at least the required
   * sio components (validated at context creation via introspection).
   *   connections:   >= {fd, read_cycle_entity}
   *   read_results:  >= {read_buf, io_result, conn_entity}
   *   write_results: >= {write_buf, io_result, conn_entity}             */
  shift_collection_id_t connections;
  shift_collection_id_t read_results;
  shift_collection_id_t write_results;
  /* Optional io_uring params.  When non-NULL the library uses
   * io_uring_queue_init_params() instead of io_uring_queue_init(),
   * allowing the caller to set flags like IORING_SETUP_SQPOLL and
   * sq_thread_cpu.  The struct is read/written (kernel fills output
   * fields).  NULL = default (flags 0). */
  struct io_uring_params *ring_params;
  /* Outbound connection support (optional).
   * Set enable_connect = true and provide connect_errors to use the
   * connect_in collection.  Successful connects move to connections.
   * Failed connects move to connect_errors.
   *   connect_errors: >= {io_result, connect_addr}                      */
  bool                  enable_connect;
  shift_collection_id_t connect_errors;
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
