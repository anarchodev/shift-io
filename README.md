# shift-io

A C23 TCP networking library built on [shift](https://github.com/anarchodev/shift) and io_uring.

## Overview

`shift-io` maps TCP connections to shift entities and drives them through collections as I/O events arrive. It supports both inbound (accept) and outbound (connect) connections. Application code never touches sockets or io_uring directly — it iterates SoA arrays from shift collections and moves entities between collections to drive the I/O lifecycle.

The user provides their own result collections for connections, reads, and writes. These collections must carry at least the required sio components but may include additional application-specific components. This lets the application attach custom state directly to I/O entities without external lookup tables.

## Setup

Construction is two-phase so that component IDs are available before collections are created:

```c
/* Phase 1: register sio component types */
sio_component_ids_t comp_ids;
sio_register_components(sh, &comp_ids);

/* Phase 2: create user-owned result collections using sio component IDs.
 * Extra components can be added for application state. */
SHIFT_COLLECTION(sh, my_connection_results,
                 comp_ids.conn_entity);

SHIFT_COLLECTION(sh, my_read_results,
                 comp_ids.read_buf, comp_ids.io_result,
                 comp_ids.conn_entity, comp_ids.user_conn_entity);

SHIFT_COLLECTION(sh, my_write_results,
                 comp_ids.write_buf, comp_ids.io_result,
                 comp_ids.conn_entity, comp_ids.user_conn_entity);

/* Phase 3: create sio context */
sio_config_t cfg = {
    .shift              = sh,
    .comp_ids           = comp_ids,
    .buf_count          = 64,       /* must be power of two */
    .buf_size           = 4096,
    .max_connections    = 4096,
    .ring_entries       = 256,
    .connection_results = my_connection_results,
    .read_results       = my_read_results,
    .write_results      = my_write_results,
};
sio_context_create(&cfg, &ctx);
```

To enable outbound connections, add a `connect_results` collection and set `enable_connect`:

```c
SHIFT_COLLECTION(sh, my_connect_results,
                 comp_ids.io_result, comp_ids.conn_entity,
                 comp_ids.user_conn_entity);

sio_config_t cfg = {
    /* ... base config as above ... */
    .enable_connect  = true,
    .connect_results = my_connect_results,
};
```

This two-phase pattern generalizes to any library built on shift: register components first, let the user compose collections, then create the library context.

## Collections

### User-provided result collections

The user creates these and passes them in `sio_config_t`. Each must contain at least the listed components (extra components are allowed and preserved across entity moves).

| Collection | Required components | Purpose |
|---|---|---|
| `connection_results` | `conn_entity` | New connections appear here after accept or successful outbound connect. `conn_entity` links to the internal `connections` entity. Add custom components to track per-connection application state — component constructors fire on entity creation. |
| `read_results` | `read_buf`, `io_result`, `conn_entity`, `user_conn_entity` | Read completions, EOF, and errors arrive here |
| `write_results` | `write_buf`, `io_result`, `conn_entity`, `user_conn_entity` | Write completions and errors arrive here |
| `connect_results` | `io_result`, `conn_entity`, `user_conn_entity` | Outbound connect outcomes (success or failure). Only required when `enable_connect` is true. |

### Library-owned collections

Returned via `sio_get_collection_ids()`.

| Collection | Purpose |
|---|---|
| `connections` | Internal connection tracking. **Only destroy entities here** — do not move entities out or modify components. Destroying an entity closes the connection. |
| `read_in` | User moves consumed read entities here to re-arm recv. |
| `write_in` | User creates write entities here to queue sends. |
| `connect_in` | User creates connect entities here to initiate outbound connections. Only available when `enable_connect` is true. |

### Internal collections (not exposed)

| Collection | Purpose |
|---|---|
| `read_pending` | Recv SQE armed, waiting for CQE |
| `write_pending` | Send SQE submitted, waiting for CQE |
| `write_retry` | Partial send, retried on next poll tick |
| `connect_socket_pending` | Socket creation SQE submitted, waiting for fixed-file slot allocation |
| `connect_pending` | Connect SQE submitted, waiting for CQE |

## Components

Registered by `sio_register_components()` and returned as `sio_component_ids_t`.

| Component | Type | Description |
|---|---|---|
| `read_buf` | `sio_read_buf_t` | Pointer, length, and buffer ID for received data |
| `write_buf` | `sio_write_buf_t` | Pointer, total length, and bytes-sent offset for outgoing data |
| `io_result` | `sio_io_result_t` | Error code: 0 = success, negative = errno-style error |
| `conn_entity` | `sio_conn_entity_t` | Handle to the internal `connections` entity for this connection |
| `user_conn_entity` | `sio_user_conn_entity_t` | Handle to the user's `connection_results` entity |
| `connect_addr` | `sio_connect_addr_t` | Target `struct sockaddr_in` for outbound connections |

The `fd` component is internal to the library and not exposed. Users identify connections through entity handles (`conn_entity` and `user_conn_entity`), not file descriptors. These two components provide zero-lookup correlation: any read or write result entity carries handles to both the internal connection and the user's connection entity.

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

Created on accept via two-phase creation. The `conn_entity` component is set by the library, linking this entity to the internal `connections` entity. The user can attach application state via custom components on the collection — component constructors fire during creation, making this the natural place for per-connection initialization. No fd is exposed; connections are identified by entity handle. This entity is long-lived and persists for the entire connection lifetime. The user is responsible for destroying it after the connection ends (unless `auto_destroy_user_entity` is set).

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

**Consuming a read result:**

1. Read `read_buf.data` and `read_buf.len` to access the received data
2. Copy out any data you need — `read_buf.data` points into a kernel provided buffer that is returned when the entity moves
3. Move the entity to `read_in`
4. **Do not** hold a pointer to `read_buf.data` after the move — the buffer is immediately returned to the kernel and may be reused

Failing to move the entity to `read_in` stalls reads on that connection.

**Detecting EOF vs error:**

- `io_result.error == 0` and `read_buf.len > 0`: data received
- `io_result.error == 0` and `read_buf.len == 0`: EOF (remote closed gracefully)
- `io_result.error < 0`: error (negative errno, e.g. `-ECONNRESET`)

On EOF or error, destroy the read-cycle entity and the `connections` entity (via `conn_entity`).

### Write entity

```
user creates entity in write_in
  │   (set write_buf.data, write_buf.len, conn_entity, user_conn_entity)
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

Write entities are created by the user, one per send operation. Multiple writes can be in flight for the same connection. Partial sends are retried automatically.

**Creating a write entity:**

```c
shift_entity_t wr;
shift_entity_create_one_immediate(sh, coll_ids->write_in, &wr);

sio_write_buf_t *wb = NULL;
shift_entity_get_component(sh, wr, comp_ids.write_buf, (void **)&wb);
wb->data   = my_data;   /* must remain valid until write_results */
wb->len    = my_len;
wb->offset = 0;         /* must be initialized to 0 */

sio_conn_entity_t *ce = NULL;
shift_entity_get_component(sh, wr, comp_ids.conn_entity, (void **)&ce);
ce->entity = conn;      /* from a read result's conn_entity */

sio_user_conn_entity_t *uce = NULL;
shift_entity_get_component(sh, wr, comp_ids.user_conn_entity, (void **)&uce);
uce->entity = user_conn; /* from a read result's user_conn_entity */
```

**Important**: `write_buf.data` must point to memory that remains valid until the entity appears in `write_results`. The user must always free `write_buf.data` and destroy the entity after it arrives in `write_results`, regardless of success or error.

### Connect entity

```
user creates entity in connect_in
  │   (set connect_addr.addr)
  │
  ├─► library submits socket creation, moves to connect_socket_pending
  │
  ├─► socket CQE: stores fd, submits connect + TCP_NODELAY
  │     moves to connect_pending
  │
  ├─► connect CQE (success):
  │     creates connection entities (connections, connection_results, read_pending)
  │     sets io_result.error = 0, conn_entity, user_conn_entity
  │     moves to connect_results
  │     connection is now live — reads/writes work as normal
  │
  └─► connect CQE (failure):
        sets io_result.error = negative errno
        moves to connect_results
```

Connect entities are created by the user, one per outbound connection attempt. The library uses `io_uring_prep_socket_direct_alloc` to safely allocate a fixed-file slot (avoiding races with multishot accept's slot allocation).

**Creating a connect entity:**

```c
shift_entity_t ce;
shift_entity_create_one_begin(sh, coll_ids->connect_in, &ce);

sio_connect_addr_t *ca = NULL;
shift_entity_get_component(sh, ce, comp_ids.connect_addr, (void **)&ca);
ca->addr = (struct sockaddr_in){
    .sin_family      = AF_INET,
    .sin_port        = htons(8080),
    .sin_addr.s_addr = inet_addr("192.168.1.1"),
};

shift_entity_create_one_end(sh, ce);
```

**Processing connect results:**

- `io_result.error == 0`: connection established. `conn_entity` and `user_conn_entity` point to the new connection. A `connection_results` entity was also created. Use `conn_entity` and `user_conn_entity` for writes and to correlate with reads.
- `io_result.error < 0`: connect failed (negative errno, e.g. `-ECONNREFUSED`). No connection entities were created.

Destroy the connect result entity after processing.

## Connection close

There are two paths:

**Remote disconnect** (EOF or error on recv):
1. Read-cycle entity moves to `read_results` with EOF (`io_result.error == 0`, `read_buf.len == 0`) or error (`io_result.error < 0`)
2. User destroys the read-cycle entity
3. User destroys the `connections` entity (via `conn_entity`) to release the fixed-file slot
4. User destroys the `connection_results` entity (via `user_conn_entity`) to clean up application state — or omit this step if `auto_destroy_user_entity` is set

**User-initiated close**:
1. User destroys the `connections` entity
2. `on_leave` callback destroys the read-cycle entity (returns any held buffer)
3. `fd` destructor releases the fixed-file slot
4. Any in-flight recv/send CQEs are filtered by staleness checks

If `auto_destroy_user_entity` is set in the config, the library automatically destroys the user's `connection_results` entity when the `connections` entity is destroyed.

## Flush timing

`sio_poll` calls `shift_flush` internally (twice: before submit and after CQE dispatch). However, if the user creates, moves, or destroys entities between `sio_poll` calls (e.g. creating write entities, moving reads to `read_in`, destroying on close), those operations are deferred. The user should call `shift_flush` after their processing loop so that the next `sio_poll` sees a consistent state.

```c
while (running) {
    sio_poll(ctx, 1);
    /* ... process read_results, create writes, handle close ... */
    shift_flush(sh);  /* commit user-side entity operations */
}
```

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
| `buf_count` | Number of io_uring provided receive buffers (must be power of two) |
| `buf_size` | Size of each receive buffer in bytes |
| `max_connections` | Maximum concurrent connections (fixed-file pool size) |
| `ring_entries` | io_uring queue depth |
| `connection_results` | User collection for new connections |
| `read_results` | User collection for read completions |
| `write_results` | User collection for write completions |
| `auto_destroy_user_entity` | Auto-destroy user connection entity on disconnect |
| `enable_connect` | Enable outbound connection support (`connect_in` collection) |
| `connect_results` | User collection for outbound connect outcomes (required when `enable_connect` is true) |

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
3. Drain `connect_in`: submit socket creation SQEs, move to `connect_socket_pending`
4. Arm sends from `write_retry` (partial) then `write_in` (new)
5. `shift_flush` (collection membership current before submit)
6. Submit SQEs, wait for `min_complete` CQEs, dispatch handlers
7. `shift_flush` (application sees consistent view on return)

### `sio_get_component_ids` / `sio_get_collection_ids`

```c
const sio_component_ids_t  *sio_get_component_ids(const sio_context_t *ctx);
const sio_collection_ids_t *sio_get_collection_ids(const sio_context_t *ctx);
```

Return pointers to the registered IDs. Component IDs are the same as those from `sio_register_components`. Collection IDs give access to `connections`, `read_in`, `write_in`, and `connect_in` (when `enable_connect` is true).

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
