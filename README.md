# shift-io

A C23 TCP networking library built on [shift](https://github.com/anarchodev/shift) and io_uring.

## Overview

`shift-io` maps TCP connections to shift entities and drives them through named collections as I/O events arrive. Application code never touches sockets or io_uring directly — it iterates SoA arrays from shift collections and calls a small set of functions to consume reads and queue writes.

**Collections:**

| Collection | Components | Meaning |
|------------|------------|---------|
| `connected` | `fd` | Connection established, recv armed |
| `reading` | `fd`, `read_buf` | Data has arrived, waiting for the application to consume it |
| `writing` | `fd`, `write_buf` | Application has queued a send, waiting for io_uring to complete it |

**Lifecycle of a connection:**

```
accept → connected → (recv CQE) → reading → sio_write → writing → (send CQE) → connected → …
                                           → sio_read_consume → connected → …
                                           → sio_disconnect → destroyed
```

**Key design points:**

- Receive buffers are io_uring provided buffers — zero-copy from kernel to application.
- `sio_write` returns the receive buffer to the ring immediately, so no buffer is held across a send.
- All entity mutations are deferred through shift — calling `sio_write` or `sio_read_consume` inside an iteration loop is safe; changes take effect on the next `shift_flush`.

## Requirements

- Linux kernel ≥ 6.0
- liburing ≥ 2.4
- CMake ≥ 3.21
- C23-capable compiler (GCC 13+ or Clang 16+)

## Building

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

The `shift` dependency is fetched automatically via CMake FetchContent.

## Example: echo server

```sh
cmake --build build --target echo_server

./build/examples/echo_server          # terminal 1
echo "hello" | nc 127.0.0.1 7777      # terminal 2
```

The echo server (`examples/echo_server.c`) shows the full application loop:

```c
while (running) {
    sio_poll(ctx, 1);   /* block until at least one event */
    shift_flush(sh);    /* commit connected→reading moves */

    /* get parallel arrays — same index = same entity */
    shift_collection_get_entities(sh, coll_ids->reading, &entities, &count);
    shift_collection_get_component_array(sh, coll_ids->reading, comp_ids->fd,       &fds,   NULL);
    shift_collection_get_component_array(sh, coll_ids->reading, comp_ids->read_buf, &rbufs, NULL);

    for (size_t i = 0; i < count; i++) {
        /* copy out of the io_uring buffer, then hand it back + queue send */
        memcpy(reply, rbufs[i].data, rbufs[i].len);
        sio_write(ctx, entities[i], reply, rbufs[i].len);
    }

    shift_flush(sh);    /* commit reading→writing moves; next poll submits sends */
}
```

## API

### Result codes

| Code | Value | Meaning |
|------|-------|---------|
| `sio_ok` | 0 | Success |
| `sio_error_null` | -1 | NULL argument |
| `sio_error_oom` | -2 | Allocation failed |
| `sio_error_invalid` | -3 | Invalid argument |
| `sio_error_io` | -4 | io_uring or socket error |
| `sio_error_stale` | -5 | Entity handle is stale |
| `sio_error_no_sqe` | -6 | io_uring submission queue full |

### Context

```c
sio_result_t sio_context_create(const sio_config_t *cfg, sio_context_t **out);
void         sio_context_destroy(sio_context_t *ctx);
```

`sio_config_t` fields:

| Field | Meaning |
|-------|---------|
| `shift` | Caller-owned shift context |
| `buf_count` | Number of io_uring provided receive buffers (must be a power of two) |
| `buf_size` | Size of each receive buffer in bytes |
| `max_fds` | Maximum file descriptor value + 1 |
| `ring_entries` | io_uring queue depth |

### Listening

```c
sio_result_t sio_listen(sio_context_t *ctx, uint16_t port, int backlog);
```

Opens a `SOCK_STREAM` socket, binds to `INADDR_ANY:port`, and arms a multishot accept. Each accepted connection becomes a new entity in the `connected` collection.

### Polling

```c
sio_result_t sio_poll(sio_context_t *ctx, uint32_t min_complete);
```

Submits pending sends from the `writing` collection, then waits for at least `min_complete` CQEs and drains the completion queue. Entity moves are queued into shift but not committed — call `shift_flush` afterwards.

### Consuming a read

```c
sio_result_t sio_read_consume(sio_context_t *ctx, shift_entity_t entity);
```

Returns the io_uring receive buffer to the ring, moves the entity back to `connected`, and re-arms recv. Use this when the application does not need to send a reply.

### Sending a reply

```c
sio_result_t sio_write(sio_context_t *ctx, shift_entity_t entity,
                        const void *data, uint32_t len);
```

Returns the io_uring receive buffer to the ring, stores `(data, len)` as a pending send, and moves the entity to `writing`. The next `sio_poll` will submit the send SQE. `data` must remain valid until after the send completes (i.e. until the entity returns to `connected`). Recv is not re-armed until the send CQE fires.

### Disconnecting

```c
sio_result_t sio_disconnect(sio_context_t *ctx, shift_entity_t entity);
```

Closes the file descriptor and destroys the entity.

### Accessors

```c
const sio_component_ids_t  *sio_get_component_ids(const sio_context_t *ctx);
const sio_collection_ids_t *sio_get_collection_ids(const sio_context_t *ctx);
```

Return the component and collection IDs registered by shift-io, needed to query shift arrays directly.

## License

AGPL-3.0-or-later. See [LICENSE](LICENSE).

## Known limitations

- Partial sends are not retried. The full send is assumed to complete in a single CQE.
- No TLS support.
- IPv4 only.
