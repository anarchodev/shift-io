# shift-io

A C23 TCP networking library built on [shift](https://github.com/anarchodev/shift) and io_uring.

## Overview

`shift-io` maps TCP connections to shift entities and drives them through collections as I/O events arrive. Application code never touches sockets or io_uring directly — it iterates SoA arrays from shift collections and moves entities between collections to drive the I/O lifecycle.

The user provides their own result collections for connections, reads, and writes. These collections must carry at least the required sio components but may include additional application-specific components. This lets the application attach custom state directly to I/O entities without external lookup tables.

## Setup

Construction is two-phase so that component IDs are available before collections are created:

```c
/* Phase 1: register sio component types */
sio_component_ids_t comp_ids;
sio_register_components(sh, &comp_ids);

/* Phase 2: create user-owned result collections using sio component IDs */
SHIFT_COLLECTION(sh, my_read_results,
                 comp_ids.read_buf, comp_ids.io_result,
                 comp_ids.user_data, comp_ids.conn_entity,
                 comp_ids.user_conn_entity);

/* Phase 3: create sio context */
sio_config_t cfg = {
    .shift              = sh,
    .comp_ids           = comp_ids,
    .buf_count          = 64,
    .buf_size           = 4096,
    .max_connections    = 4096,
    .ring_entries       = 256,
    .connection_results = my_connection_results,
    .read_results       = my_read_results,
    .write_results      = my_write_results,
};
sio_context_create(&cfg, &ctx);
```

This two-phase pattern generalizes to any library built on shift: register components first, let the user compose collections, then create the library context.

## Collections

### User-provided result collections

The user creates these and passes them in `sio_config_t`. Each must contain at least the listed components (extra components are allowed and preserved across moves).

| Collection | Required components | Purpose |
|---|---|---|
| `connection_results` | `user_data` | New connections appear here after accept |
| `read_results` | `read_buf`, `io_result`, `user_data`, `conn_entity`, `user_conn_entity` | Read completions, EOF, and errors arrive here |
| `write_results` | `write_buf`, `io_result`, `user_data`, `conn_entity`, `user_conn_entity` | Write completions and errors arrive here |

### Library-owned collections

Returned via `sio_get_collection_ids()`.

| Collection | Purpose |
|---|---|
| `connections` | Internal connection tracking. User destroys entities here to close a connection. |
| `read_in` | User moves consumed read entities here to re-arm recv. |
| `write_in` | User creates write entities here to queue sends. |

### Internal collections (not exposed)

| Collection | Purpose |
|---|---|
| `read_pending` | Recv SQE armed, waiting for CQE |
| `write_pending` | Send SQE submitted, waiting for CQE |
| `write_retry` | Partial send, retried on next poll tick |

## Components

Registered by `sio_register_components()` and returned as `sio_component_ids_t`.

| Component | Type | Description |
|---|---|---|
| `read_buf` | `sio_read_buf_t` | Pointer, length, and buffer ID for received data |
| `write_buf` | `sio_write_buf_t` | Pointer, total length, and bytes-sent offset for outgoing data |
| `io_result` | `sio_io_result_t` | Error code: 0 = success, negative = errno-style error |
| `user_data` | `sio_user_data_t` | Application-owned uint64 (initialized to UINT64_MAX) |
| `conn_entity` | `sio_conn_entity_t` | Handle to the internal `connections` entity for this connection |
| `user_conn_entity` | `sio_user_conn_entity_t` | Handle to the user's `connection_results` entity |

The `fd` component is internal to the library and not exposed. Users identify connections through entity handles (`conn_entity` and `user_conn_entity`), not file descriptors. The last two components provide zero-lookup correlation: any read or write result entity carries handles to both the internal connection and the user's connection entity.

## Entity lifetimes

### Connection entity (user's `connection_results`)

```
accept
  └─► entity created in connection_results (2-phase: begin → end)
      │
      ├─ lives until disconnect or user-initiated close
      │
      └─► destroyed by:
            • the user (after seeing EOF/error in read_results)
            • the library (if auto_destroy_user_entity is true)
```

Created on accept via two-phase creation. The user can read the `user_data` component and attach application state. No fd is exposed — connections are identified by entity handle. This entity is long-lived — it persists for the entire connection lifetime. The user is responsible for destroying it after the connection ends (unless `auto_destroy_user_entity` is set).

### Internal connection entity (`connections`)

```
accept
  └─► entity created in connections (immediate)
      │
      ├─ carries: fd, user_conn_entity, read_cycle_entity (internal)
      │
      └─► destroyed by:
            • the user (to initiate close)
            • on_leave callback fires:
                • destroys the read-cycle entity
                • optionally destroys user's connection entity
            • fd destructor fires: releases fixed-file slot
```

Created on accept alongside the user connection entity. The user destroys this entity to close a connection. The `on_leave` callback handles cascading cleanup.

### Read-cycle entity

```
accept
  └─► entity created in read_pending (immediate)
      │
      ├─► recv CQE arrives (data):
      │     move to read_results ──► user consumes data
      │                               move to read_in ──► library moves to read_pending
      │                                                    re-arms recv ──► …
      │
      ├─► recv CQE arrives (EOF / error):
      │     move to read_results ──► user sees error, destroys entity
      │
      └─► connection closed (connections entity destroyed):
            on_leave destroys this entity if still alive
```

One read-cycle entity exists per connection. It cycles between `read_pending` (internal), the user's `read_results`, and `read_in` (library-owned). It carries `conn_entity` and `user_conn_entity` for correlation.

**Important**: after consuming data from `read_results`, the user must move the entity to `read_in` to re-arm recv. Failing to do so stalls reads on that connection.

### Write entity

```
user creates entity in write_in
  │   (set fd, write_buf.data, write_buf.len, conn_entity, user_conn_entity)
  │
  ├─► library arms send, moves to write_pending
  │
  ├─► send CQE (partial): move to write_retry, retry next tick
  │
  ├─► send CQE (complete): move to write_results
  │     user frees write_buf.data, destroys entity
  │
  └─► send CQE (error): move to write_results with io_result.error set
        user frees write_buf.data, destroys entity
```

Write entities are created by the user, one per send operation. Multiple writes can be in flight for the same connection. The user must always free `write_buf.data` and destroy the entity after it appears in `write_results`, regardless of success or error. Partial sends are retried automatically.

## Connection close

There are two paths:

**Remote disconnect** (EOF or error on recv):
1. Read-cycle entity moves to `read_results` with `io_result.error` set (0 for EOF, negative for error) and `read_buf.len == 0`
2. User sees the error, destroys the read-cycle entity
3. User destroys the `connections` entity to release the slot
4. User destroys the `connection_results` entity to clean up application state

**User-initiated close**:
1. User destroys the `connections` entity
2. `on_leave` callback destroys the read-cycle entity (returns any held buffer)
3. `fd` destructor releases the fixed-file slot
4. Any in-flight recv/send CQEs are filtered by staleness checks

If `auto_destroy_user_entity` is set in the config, the library automatically destroys the user's `connection_results` entity when the `connections` entity is destroyed.

## API reference

### Result codes

| Code | Value | Meaning |
|---|---|---|
| `sio_ok` | 0 | Success |
| `sio_error_null` | -1 | NULL argument |
| `sio_error_oom` | -2 | Allocation failed |
| `sio_error_invalid` | -3 | Invalid argument or collection validation failed |
| `sio_error_io` | -4 | io_uring or socket error |
| `sio_error_no_sqe` | -5 | io_uring submission queue full |

### `sio_register_components`

```c
sio_result_t sio_register_components(shift_t *sh, sio_component_ids_t *out);
```

Registers all sio component types (with constructors and destructors) on the shift context. Must be called before creating user collections. The returned IDs are passed to `sio_context_create` via `sio_config_t::comp_ids`.

### `sio_context_create`

```c
sio_result_t sio_context_create(const sio_config_t *cfg, sio_context_t **out);
```

Creates the sio context. Validates that user-provided collections contain the required components (via shift introspection). Registers internal collections, sets up io_uring, allocates the fixed-file pool and provided buffer ring.

`sio_config_t` fields:

| Field | Description |
|---|---|
| `shift` | Caller-owned shift context |
| `comp_ids` | Component IDs from `sio_register_components` |
| `buf_count` | Number of io_uring provided receive buffers (power of two) |
| `buf_size` | Size of each receive buffer in bytes |
| `max_connections` | Maximum concurrent connections (fixed-file pool size) |
| `ring_entries` | io_uring queue depth |
| `connection_results` | User collection for new connections |
| `read_results` | User collection for read completions |
| `write_results` | User collection for write completions |
| `auto_destroy_user_entity` | Auto-destroy user connection entity on disconnect |

### `sio_context_destroy`

```c
void sio_context_destroy(sio_context_t *ctx);
```

Tears down io_uring, frees buffers, closes the listen socket.

### `sio_listen`

```c
sio_result_t sio_listen(sio_context_t *ctx, uint16_t port, int backlog);
```

Opens a non-blocking TCP socket, binds to `INADDR_ANY:port`, and arms a multishot accept. Each accepted connection creates entities in `connection_results`, `connections`, and `read_pending`.

### `sio_poll`

```c
sio_result_t sio_poll(sio_context_t *ctx, uint32_t min_complete);
```

Drives the I/O loop for one tick:
1. Flush batched fixed-file slot releases
2. Drain `read_in`: return buffers, move to `read_pending`, re-arm recv
3. Arm sends from `write_retry` (partial) then `write_in` (new)
4. `shift_flush` (collection membership current before submit)
5. Submit SQEs, wait for `min_complete` CQEs, dispatch handlers
6. `shift_flush` (application sees consistent view on return)

### `sio_get_component_ids` / `sio_get_collection_ids`

```c
const sio_component_ids_t  *sio_get_component_ids(const sio_context_t *ctx);
const sio_collection_ids_t *sio_get_collection_ids(const sio_context_t *ctx);
```

Return pointers to the registered IDs. Component IDs are the same as those from `sio_register_components`. Collection IDs give access to `connections`, `read_in`, and `write_in`.

## Requirements

- Linux kernel >= 6.0
- liburing >= 2.4
- CMake >= 3.21
- C23-capable compiler (GCC 13+ or Clang 16+)

## Building

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

The `shift` dependency is fetched automatically via CMake FetchContent.

## Example

```sh
cmake --build build --target echo_server
./build/examples/echo_server   # terminal 1
echo "hello" | nc -w 1 localhost 7777   # terminal 2
```

See `examples/echo_server.c` for the complete application loop demonstrating setup, read processing, write staging, and cleanup.

## Known limitations

- No TLS support.
- IPv4 only.
- Single sio context per process (global destructor pointer).

## License

AGPL-3.0-or-later. See [LICENSE](LICENSE).
