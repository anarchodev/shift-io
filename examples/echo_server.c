#define _GNU_SOURCE

#include <shift_io.h>
#include <shift.h>
#include <liburing.h>

#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PORT            7777
#define BACKLOG         4096
#define MAX_CONNECTIONS 16384
#define BUF_COUNT       16384
#define BUF_SIZE        4096

static volatile bool g_running = true;

static void handle_signal(int sig) {
  (void)sig;
  g_running = false;
}

typedef struct {
  int worker_id;
  int worker_core;
  int sq_core;
} worker_config_t;

static void pin_to_core(int core) {
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  CPU_SET(core, &cpuset);
  pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
}

static void *worker_fn(void *arg) {
  worker_config_t *cfg = arg;

  pin_to_core(cfg->worker_core);
  printf("Worker %d: pinned to core %d, io_uring SQPOLL on core %d\n",
         cfg->worker_id, cfg->worker_core, cfg->sq_core);

  /* ---- Create per-worker shift context ---- */
  shift_t       *sh     = NULL;
  shift_config_t sh_cfg = {
      .max_entities            = MAX_CONNECTIONS * 6,
      .max_components          = 16,
      .max_collections         = 24,
      .deferred_queue_capacity = MAX_CONNECTIONS * 6,
      .allocator               = {NULL, NULL, NULL, NULL},
  };
  if (shift_context_create(&sh_cfg, &sh) != shift_ok) {
    fprintf(stderr, "Worker %d: shift_context_create failed\n", cfg->worker_id);
    return NULL;
  }

  /* Phase 1: register sio components */
  sio_component_ids_t comp_ids;
  if (sio_register_components(sh, &comp_ids) != sio_ok) {
    fprintf(stderr, "Worker %d: sio_register_components failed\n",
            cfg->worker_id);
    shift_context_destroy(sh);
    return NULL;
  }

  /* Phase 2: create user-owned result collections */
  shift_collection_id_t connection_results_coll;
  {
    shift_component_id_t    comps[] = {comp_ids.conn_entity};
    shift_collection_info_t info    = {.comp_ids = comps, .comp_count = 1};
    shift_collection_register(sh, &info, &connection_results_coll);
  }

  shift_collection_id_t read_results_coll;
  {
    shift_component_id_t comps[] = {comp_ids.read_buf, comp_ids.io_result,
                                    comp_ids.conn_entity,
                                    comp_ids.user_conn_entity};
    shift_collection_info_t info = {.comp_ids = comps, .comp_count = 4};
    shift_collection_register(sh, &info, &read_results_coll);
  }

  shift_collection_id_t write_results_coll;
  {
    shift_component_id_t comps[] = {comp_ids.write_buf, comp_ids.io_result,
                                    comp_ids.conn_entity,
                                    comp_ids.user_conn_entity};
    shift_collection_info_t info = {.comp_ids = comps, .comp_count = 4};
    shift_collection_register(sh, &info, &write_results_coll);
  }

  /* Phase 3: create sio context with SQPOLL pinned to adjacent core */
  struct io_uring_params ring_params = {
      .flags        = IORING_SETUP_SQPOLL | IORING_SETUP_SQ_AFF,
      .sq_thread_cpu = (uint32_t)cfg->sq_core,
      .sq_thread_idle = 1000, /* ms before kernel parks the SQ thread */
  };

  sio_context_t *ctx     = NULL;
  sio_config_t   sio_cfg = {
      .shift              = sh,
      .comp_ids           = comp_ids,
      .buf_count          = BUF_COUNT,
      .buf_size           = BUF_SIZE,
      .max_connections    = MAX_CONNECTIONS,
      .ring_entries       = 4096,
      .connection_results = connection_results_coll,
      .read_results       = read_results_coll,
      .write_results      = write_results_coll,
      .auto_destroy_user_entity = false,
      .ring_params        = &ring_params,
  };
  if (sio_context_create(&sio_cfg, &ctx) != sio_ok) {
    fprintf(stderr, "Worker %d: sio_context_create failed\n", cfg->worker_id);
    shift_context_destroy(sh);
    return NULL;
  }

  const sio_collection_ids_t *coll_ids = sio_get_collection_ids(ctx);

  if (sio_listen(ctx, PORT, BACKLOG) != sio_ok) {
    fprintf(stderr, "Worker %d: sio_listen failed on port %d\n",
            cfg->worker_id, PORT);
    sio_context_destroy(ctx);
    shift_context_destroy(sh);
    return NULL;
  }

  /* ---- Event loop ---- */
  while (g_running) {
    sio_result_t poll_result = sio_poll(ctx, 1);
    if (poll_result != sio_ok) {
      fprintf(stderr, "Worker %d: sio_poll failed (%d)\n", cfg->worker_id,
              poll_result);
      break;
    }

    /* Process read_results: echo back or handle close */
    {
      shift_entity_t         *ro_entities = NULL;
      sio_read_buf_t         *ro_rbufs    = NULL;
      sio_io_result_t        *ro_results  = NULL;
      sio_conn_entity_t      *ro_conns    = NULL;
      sio_user_conn_entity_t *ro_uconns   = NULL;
      size_t                  ro_count    = 0;

      shift_collection_get_entities(sh, read_results_coll, &ro_entities,
                                    &ro_count);
      shift_collection_get_component_array(sh, read_results_coll,
                                           comp_ids.read_buf,
                                           (void **)&ro_rbufs, NULL);
      shift_collection_get_component_array(sh, read_results_coll,
                                           comp_ids.io_result,
                                           (void **)&ro_results, NULL);
      shift_collection_get_component_array(sh, read_results_coll,
                                           comp_ids.conn_entity,
                                           (void **)&ro_conns, NULL);
      shift_collection_get_component_array(sh, read_results_coll,
                                           comp_ids.user_conn_entity,
                                           (void **)&ro_uconns, NULL);

      for (size_t i = 0; i < ro_count; i++) {
        if (ro_results[i].error != 0 || ro_rbufs[i].len == 0) {
          shift_entity_destroy_one(sh, ro_entities[i]);
          if (!shift_entity_is_stale(sh, ro_conns[i].entity))
            shift_entity_destroy_one(sh, ro_conns[i].entity);
          if (!shift_entity_is_stale(sh, ro_uconns[i].entity))
            shift_entity_destroy_one(sh, ro_uconns[i].entity);
          continue;
        }

        shift_entity_t wi_entity;
        if (shift_entity_create_one_begin(sh, coll_ids->write_in,
                                          &wi_entity) != shift_ok) {
          shift_entity_move_one(sh, ro_entities[i], coll_ids->read_in);
          continue;
        }

        sio_conn_entity_t *wi_ce = NULL;
        shift_entity_get_component(sh, wi_entity, comp_ids.conn_entity,
                                   (void **)&wi_ce);
        wi_ce->entity = ro_conns[i].entity;

        sio_user_conn_entity_t *wi_uce = NULL;
        shift_entity_get_component(sh, wi_entity, comp_ids.user_conn_entity,
                                   (void **)&wi_uce);
        wi_uce->entity = ro_uconns[i].entity;

        sio_write_buf_t *wi_wb = NULL;
        shift_entity_get_component(sh, wi_entity, comp_ids.write_buf,
                                   (void **)&wi_wb);
        uint32_t len  = ro_rbufs[i].len;
        void    *copy = malloc(len);
        if (copy) {
          memcpy(copy, ro_rbufs[i].data, len);
          wi_wb->data = copy;
          wi_wb->len  = len;
        }

        shift_entity_create_one_end(sh, wi_entity);
        shift_entity_move_one(sh, ro_entities[i], coll_ids->read_in);
      }
    }

    /* Process write_results: free buffers, destroy entities */
    {
      shift_entity_t  *wo_entities = NULL;
      sio_write_buf_t *wo_wbufs    = NULL;
      size_t           wo_count    = 0;

      shift_collection_get_entities(sh, write_results_coll, &wo_entities,
                                    &wo_count);
      shift_collection_get_component_array(sh, write_results_coll,
                                           comp_ids.write_buf,
                                           (void **)&wo_wbufs, NULL);

      for (size_t i = 0; i < wo_count; i++) {
        free((void *)wo_wbufs[i].data);
        shift_entity_destroy_one(sh, wo_entities[i]);
      }
    }

    shift_flush(sh);
  }

  sio_context_destroy(ctx);
  shift_context_destroy(sh);
  return NULL;
}

/*
 * Multithreaded echo server — share-nothing workers.
 *
 * Spawns N worker threads (N = num_cores / 2, or argv[1]).
 * Each worker is pinned to an even core (0, 2, 4, …) and its io_uring
 * SQPOLL thread is pinned to the adjacent odd core (1, 3, 5, …) so
 * they share L1/L2 cache.  Each worker has its own shift + sio context
 * and listens on the same port via SO_REUSEPORT.
 */
int main(int argc, char **argv) {
  signal(SIGINT, handle_signal);
  signal(SIGTERM, handle_signal);

  long ncpus = sysconf(_SC_NPROCESSORS_ONLN);
  int  nworkers = (int)(ncpus / 2);
  if (argc > 1)
    nworkers = atoi(argv[1]);
  if (nworkers < 1)
    nworkers = 1;
  if (nworkers > (int)(ncpus / 2) && ncpus >= 2)
    nworkers = (int)(ncpus / 2);

  printf("Echo server: %d workers on %ld cores, port %d\n", nworkers, ncpus,
         PORT);

  worker_config_t *configs = calloc((size_t)nworkers, sizeof(worker_config_t));
  pthread_t       *threads = calloc((size_t)nworkers, sizeof(pthread_t));

  for (int i = 0; i < nworkers; i++) {
    configs[i].worker_id   = i;
    configs[i].worker_core = i * 2;
    configs[i].sq_core     = i * 2 + 1;
    pthread_create(&threads[i], NULL, worker_fn, &configs[i]);
  }

  for (int i = 0; i < nworkers; i++)
    pthread_join(threads[i], NULL);

  printf("\nShutting down.\n");

  free(threads);
  free(configs);
  return 0;
}
