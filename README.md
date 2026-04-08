# shift-io

A C23 TCP networking library built on [shift](https://github.com/anarchodev/shift) and io_uring.

## Overview

`shift-io` maps TCP connections to shift entities and drives them through collections as I/O events arrive. It supports both inbound (accept) and outbound (connect) connections. Application code never touches sockets or io_uring directly — it iterates SoA arrays from shift collections and moves entities between collections to drive the I/O lifecycle.

The user provides their own collections for connections, reads, writes, and connect errors. These collections must carry at least the required sio components but may include additional application-specific components. This lets the application attach custom state directly to I/O entities without external lookup tables.

## Setup

Construction is two-phase so that component IDs are available before collections are created:

```c
/* Phase 1: register sio component types */
sio_component_ids_t comp_ids;
sio_register_components(sh, &comp_ids);

/* Phase 2: create user-owned collections using sio component IDs.
 * Extra components can be added for application state. */
shift_collection_id_t my_connections;
{
  shift_component_id_t    comps[] = {comp_ids.fd, comp_ids.read_cycle_entity};
  shift_collection_info_t info    = {.name = "connections", .comp_ids = comps, .comp_count = 2};
  shift_collection_register(sh, &info, &my_connections);
}

shift_collection_id_t my_read_results;
{
  shift_component_id_t    comps[] = {comp_ids.read_buf, comp_ids.io_result,
                                     comp_ids.conn_entity};
  shift_collection_info_t info    = {.name = "read_results", .comp_ids = comps, .comp_count = 3};
  shift_collection_register(sh, &info, &my_read_results);
}

shift_collection_id_t my_write_results;
{
  shift_component_id_t    comps[] = {comp_ids.write_buf, comp_ids.io_result,
                                     comp_ids.conn_entity};
  shift_collection_info_t info    = {.name = "write_results", .comp_ids = comps, .comp_count = 3};
  shift_collection_register(sh, &info, &my_write_results);
}

/* Phase 3: create sio context */
sio_config_t cfg = {
    .shift              = sh,
    .comp_ids           = comp_ids,
    .buf_count          = 64,       /* must be power of two */
    .buf_size           = 4096,
    .max_connections    = 4096,
    .ring_entries       = 256,
    .connections         = my_connections,
    .read_results        = my_read_results,
    .write_results       = my_write_results,
};
sio_context_create(&cfg, &ctx);
```

To enable outbound connections, add a `connect_errors` collection and set `enable_connect`:

```c
shift_collection_id_t my_connect_errors;
{
  shift_component_id_t    comps[] = {comp_ids.io_result, comp_ids.connect_addr};
  shift_collection_info_t info    = {.name = "connect_errors", .comp_ids = comps, .comp_count = 2};
  shift_collection_register(sh, &info, &my_connect_errors);
}

sio_config_t cfg = {
    /* ... base config as above ... */
    .enable_connect  = true,
    .connect_errors  = my_connect_errors,
};
```

Successful outbound connects appear in the `connections` collection (same as accepted connections). Failed connects appear in `connect_errors`.

This two-phase pattern generalizes to any library built on shift: register components first, let the user compose collections, then create the library context.

### Component superset

User-provided collections may include extra application-specific components beyond the required sio components. The library's internal collections (`read_in`, `write_in`, `read_pending`, `write_pending`, etc.) are automatically registered with the same archetype as the corresponding user collection (or as supersets when internal components are needed). This means any custom components on the user's collections propagate to the internal collections, so entities can move freely through the I/O pipeline without losing application state.

For example, if the user adds a `custom_tag` component to their `connections` collection, and an entity is accepted into `connections`, it will carry `custom_tag` with its constructor fired as expected.

## Collections

### User-provided collections

The user creates these and passes them in `sio_config_t`. Each must contain at least the listed components (extra components are allowed and preserved across entity moves).

| Collection | Required components | Purpose |
|---|---|---|
| `connections` | `fd`, `read_cycle_entity` | Live connections. New connections appear here on accept or successful outbound connect. User destroys entities here to close connections. |
| `read_results` | `read_buf`, `io_result`, `conn_entity` | Read completions, EOF, and errors arrive here |
| `write_results` | `write_buf`, `io_result`, `conn_entity` | Write completions and errors arrive here |
| `connect_errors` | `io_result`, `connect_addr` | Failed outbound connect attempts. Only required when `enable_connect` is true. |

### Library-created collections (exposed)

Returned via `sio_get_collection_ids()`.

| Collection | Archetype | Purpose |
|---|---|---|
| `read_in` | same as `read_results` | User moves consumed read entities here to re-arm recv |
| `write_in` | same as `write_results` | User creates write entities here to queue sends |
| `connect_in` | superset of `connections` + `{connect_addr, io_result}` | User creates connect entities here to initiate outbound connections. Only available when `enable_connect` is true. |

### Internal collections (not exposed)

| Collection | Archetype | Purpose |
|---|---|---|
| `read_pending` | same as `read_results` | Recv SQE armed, waiting for CQE |
| `write_pending` | same as `write_results` | Send SQE submitted, waiting for CQE |
| `write_retry` | same as `write_results` | Partial send, retried on next poll tick |
| `connect_socket_pending` | same as `connect_in` | Socket creation SQE submitted, waiting for fixed-file slot allocation |
| `connect_pending` | same as `connect_in` | Connect SQE submitted, waiting for CQE |

## Components

Registered by `sio_register_components()` and returned as `sio_component_ids_t`.

| Component | Type | Description |
|---|---|---|
| `read_buf` | `sio_read_buf_t` | Pointer, length, and buffer ID for received data |
| `write_buf` | `sio_write_buf_t` | Pointer, total length, and bytes-sent offset for outgoing data |
| `io_result` | `sio_io_result_t` | Error code: 0 = success, negative = errno-style error |
| `conn_entity` | `sio_conn_entity_t` | Handle to the connection entity. Carried by read/write entities for correlation. |
| `fd` | `sio_fd_t` | Fixed-file slot index. Carried by connection entities. Has a destructor that releases the slot. |
| `read_cycle_entity` | `sio_read_cycle_entity_t` | Handle to the read-cycle entity. Carried by connection entities. Has a destructor that cleans up the read entity. |
| `connect_addr` | `sio_connect_addr_t` | Target `struct sockaddr_in` for outbound connections |

Users identify connections through entity handles (`conn_entity` on read/write entities). The `fd` and `read_cycle_entity` components are required on connection collections but are managed by the library — users include them in the archetype but do not set them directly.

## Entity lifetimes

### Connection entity

```
accept or successful outbound connect
  └--> entity created in connections
      |
      +-- lives until disconnect or user-initiated close
      |
      └--> destroyed by the user
             read_cycle_entity destructor fires: destroys the read-cycle entity
             fd destructor fires: releases fixed-file slot
```

On accept, the library creates the entity directly in the user's `connections` collection. On successful outbound connect, the connect entity moves to `connections` — it becomes the connection (same entity, same identity). The `fd` and `read_cycle_entity` components are set by the library. The user can attach application state via custom components on the collection.

### Read-cycle entity

```
accept or connect success
  └--> entity created in read_pending
      |
      +-- recv CQE arrives (data):
      |     move to read_results --> user consumes data
      |                               move to read_in --> library moves to read_pending
      |                                                    re-arms recv --> ...
      |
      +-- recv CQE arrives (EOF / error):
      |     move to read_results --> user sees error, destroys entity
      |
      └-- connection closed (connection entity destroyed):
            read_cycle_entity destructor destroys this entity if still alive
```

One read-cycle entity exists per connection. It cycles between `read_pending` (internal), the user's `read_results`, and `read_in` (library-created). It carries `conn_entity` for correlation back to the connection.

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

On EOF or error, destroy the read-cycle entity and the connection entity (via `conn_entity`).

### Write entity

```
user creates entity in write_in
  |   (set write_buf.data, write_buf.len, conn_entity)
  |
  +-- library arms send, moves to write_pending
  |
  +-- send CQE (partial): move to write_retry, retry next tick
  |
  +-- send CQE (complete): move to write_results
  |     user frees write_buf.data, destroys entity
  |
  └-- send CQE (error): move to write_results with io_result.error set
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
ce->entity = conn;      /* from a read result's conn_entity, or the connection entity directly */
```

**Important**: `write_buf.data` must point to memory that remains valid until the entity appears in `write_results`. The user must always free `write_buf.data` and destroy the entity after it arrives in `write_results`, regardless of success or error.

### Connect entity

```
user creates entity in connect_in
  |   (set connect_addr.addr)
  |
  +-- library submits socket creation, moves to connect_socket_pending
  |
  +-- socket CQE: stores fd, submits connect + TCP_NODELAY
  |     moves to connect_pending
  |
  +-- connect CQE (success):
  |     creates read-cycle entity, arms recv
  |     moves entity to connections — it IS the connection now
  |
  └-- connect CQE (failure):
        sets io_result.error = negative errno
        moves to connect_errors
```

Connect entities are created by the user, one per outbound connection attempt. On success, the entity moves directly to `connections` — no separate connection entity is created. The same entity that started in `connect_in` becomes the live connection.

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

- Successful connects appear in `connections`. The entity that was in `connect_in` is now the connection entity. Custom components set before the connect are preserved.
- Failed connects appear in `connect_errors` with `io_result.error < 0` (negative errno, e.g. `-ECONNREFUSED`). The `connect_addr` component is preserved for error reporting or retry.

Destroy failed connect entities from `connect_errors` after processing.

## Connection close

There are two paths:

**Remote disconnect** (EOF or error on recv):
1. Read-cycle entity moves to `read_results` with EOF (`io_result.error == 0`, `read_buf.len == 0`) or error (`io_result.error < 0`)
2. User destroys the read-cycle entity
3. User destroys the connection entity to release the fixed-file slot

**User-initiated close**:
1. User destroys the connection entity
2. `read_cycle_entity` destructor destroys the read-cycle entity (returns any held buffer)
3. `fd` destructor releases the fixed-file slot
4. Any in-flight recv/send CQEs are filtered by staleness checks

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
| `connections` | User collection for live connections (required: `fd`, `read_cycle_entity`) |
| `read_results` | User collection for read completions (required: `read_buf`, `io_result`, `conn_entity`) |
| `write_results` | User collection for write completions (required: `write_buf`, `io_result`, `conn_entity`) |
| `ring_params` | Optional `struct io_uring_params *`. When non-NULL, the library uses `io_uring_queue_init_params()` instead of `io_uring_queue_init()`, allowing flags like `IORING_SETUP_SQPOLL` and `sq_thread_cpu`. NULL = default (flags 0). |
| `enable_connect` | Enable outbound connection support (`connect_in` collection) |
| `connect_errors` | User collection for failed outbound connects (required: `io_result`, `connect_addr`). Only required when `enable_connect` is true. Successful connects appear in `connections`. |

### `sio_context_destroy`

```c
void sio_context_destroy(sio_context_t *ctx);
```

Tears down io_uring, frees buffers, closes the listen socket.

### `sio_listen`

```c
sio_result_t sio_listen(sio_context_t *ctx, uint16_t port, int backlog);
```

Opens a non-blocking TCP socket, binds to `INADDR_ANY:port`, and arms a multishot accept. Each accepted connection creates an entity in `connections` and a read-cycle entity in `read_pending`. TCP_NODELAY is set on every accepted socket via an io_uring setsockopt SQE to disable Nagle's algorithm.

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

Return pointers to the registered IDs. Component IDs are the same as those from `sio_register_components`. Collection IDs give access to `read_in`, `write_in`, and `connect_in` (when `enable_connect` is true).

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

See `examples/echo_server.c` for the complete application loop demonstrating setup, read processing, write staging, and cleanup (including `IORING_SETUP_SQPOLL` via `ring_params`).

`examples/smoke_test.c` verifies that custom user components propagate through internal collections.

## Known limitations

- No TLS support.
- IPv4 only.

## License

AGPL-3.0-or-later. See [LICENSE](LICENSE).
