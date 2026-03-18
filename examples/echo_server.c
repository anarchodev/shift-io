#include <shift_io.h>
#include <shift.h>

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PORT      7777
#define BACKLOG   128
#define MAX_FDS   4096
#define BUF_COUNT 64
#define BUF_SIZE  4096

static volatile bool g_running = true;

/* Per-fd echo buffers: freed on next echo for same fd, freed all on shutdown */
static char *g_echo_buf[MAX_FDS];

static void handle_signal(int sig) {
  (void)sig;
  g_running = false;
}

int main(void) {
  signal(SIGINT, handle_signal);
  signal(SIGTERM, handle_signal);

  shift_t       *sh     = NULL;
  shift_config_t sh_cfg = {
      .max_entities            = MAX_FDS,
      .max_components          = 8,
      .max_collections         = 8,
      .deferred_queue_capacity = MAX_FDS * 2,
      .allocator               = {NULL, NULL, NULL, NULL},
  };
  if (shift_context_create(&sh_cfg, &sh) != shift_ok) {
    fprintf(stderr, "shift_context_create failed\n");
    return 1;
  }

  sio_context_t *ctx     = NULL;
  sio_config_t   sio_cfg = {
        .shift        = sh,
        .buf_count    = BUF_COUNT,
        .buf_size     = BUF_SIZE,
        .max_fds      = MAX_FDS,
        .ring_entries = 256,
  };
  if (sio_context_create(&sio_cfg, &ctx) != sio_ok) {
    fprintf(stderr, "sio_context_create failed\n");
    shift_context_destroy(sh);
    return 1;
  }

  if (sio_listen(ctx, PORT, BACKLOG) != sio_ok) {
    fprintf(stderr, "sio_listen failed on port %d\n", PORT);
    sio_context_destroy(ctx);
    shift_context_destroy(sh);
    return 1;
  }

  printf("Echo server listening on port %d\n", PORT);

  const sio_component_ids_t  *comp_ids = sio_get_component_ids(ctx);
  const sio_collection_ids_t *coll_ids = sio_get_collection_ids(ctx);

  while (g_running) {
    if (sio_poll(ctx, 1) != sio_ok)
      break;

    shift_flush(sh);

    shift_entity_t *entities = NULL;
    sio_fd_t       *fds      = NULL;
    sio_read_buf_t *rbufs    = NULL;
    size_t          count    = 0;

    shift_collection_get_entities(sh, coll_ids->reading, &entities, &count);
    shift_collection_get_component_array(sh, coll_ids->reading, comp_ids->fd,
                                         (void **)&fds, NULL);
    shift_collection_get_component_array(
        sh, coll_ids->reading, comp_ids->read_buf, (void **)&rbufs, NULL);

    for (size_t i = 0; i < count; i++) {
      int      fd  = fds[i].fd;
      uint32_t len = rbufs[i].len;

      if (fd < 0 || (uint32_t)fd >= MAX_FDS || len == 0)
        continue;

      free(g_echo_buf[fd]);
      g_echo_buf[fd] = malloc(len);
      if (!g_echo_buf[fd]) {
        fprintf(stderr, "malloc failed for fd %d\n", fd);
        continue;
      }
      memcpy(g_echo_buf[fd], rbufs[i].data, len);

      sio_write(ctx, entities[i], g_echo_buf[fd], len);
    }

    shift_flush(sh);
  }

  printf("\nShutting down.\n");

  for (int i = 0; i < MAX_FDS; i++) {
    free(g_echo_buf[i]);
    g_echo_buf[i] = NULL;
  }

  sio_context_destroy(ctx);
  shift_context_destroy(sh);
  return 0;
}
