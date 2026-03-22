#include <shift_io.h>
#include <shift.h>

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PORT            7777
#define BACKLOG         128
#define MAX_CONNECTIONS 4096
#define BUF_COUNT       64
#define BUF_SIZE        4096

static volatile bool g_running = true;

static void handle_signal(int sig) {
  (void)sig;
  g_running = false;
}

/*
 * Echo server — demonstrates the user-provided collection model.
 *
 * Setup:
 *   1. sio_register_components() — get component IDs
 *   2. Create user-owned result collections with those IDs
 *   3. sio_context_create() — pass component IDs + collection IDs
 *
 * Connection lifecycle:
 *   accept → connection_results (user sees new connection)
 *          → connections (user destroys to close)
 *          → read_pending (internal, recv armed)
 *
 * Read cycle:
 *   read_pending → read_results (user sees data/EOF/error)
 *               → read_in (user done) → read_pending → …
 *
 * Write cycle:
 *   echo_pending (app staging) → write_in → write_pending
 *                              → write_results (user sees completion)
 *
 * Close:
 *   User destroys entity from connections collection.
 */

int main(void) {
  signal(SIGINT, handle_signal);
  signal(SIGTERM, handle_signal);

  shift_t       *sh     = NULL;
  shift_config_t sh_cfg = {
      .max_entities            = MAX_CONNECTIONS * 4,
      .max_components          = 16,
      .max_collections         = 24,
      .deferred_queue_capacity = MAX_CONNECTIONS * 4,
      .allocator               = {NULL, NULL, NULL, NULL},
  };
  if (shift_context_create(&sh_cfg, &sh) != shift_ok) {
    fprintf(stderr, "shift_context_create failed\n");
    return 1;
  }

  /* Phase 1: register sio components */
  sio_component_ids_t comp_ids;
  if (sio_register_components(sh, &comp_ids) != sio_ok) {
    fprintf(stderr, "sio_register_components failed\n");
    shift_context_destroy(sh);
    return 1;
  }

  /* Phase 2: create user-owned result collections using sio component IDs */
  shift_collection_id_t connection_results_coll;
  {
    shift_collection_info_t info = {.comp_ids = NULL, .comp_count = 0};
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

  /* Phase 3: create sio context with pre-registered components + user collections */
  sio_context_t *ctx     = NULL;
  sio_config_t   sio_cfg = {
      .shift              = sh,
      .comp_ids           = comp_ids,
      .buf_count          = BUF_COUNT,
      .buf_size           = BUF_SIZE,
      .max_connections    = MAX_CONNECTIONS,
      .ring_entries       = 256,
      .connection_results = connection_results_coll,
      .read_results       = read_results_coll,
      .write_results      = write_results_coll,
      .auto_destroy_user_entity = false,
  };
  if (sio_context_create(&sio_cfg, &ctx) != sio_ok) {
    fprintf(stderr, "sio_context_create failed\n");
    shift_context_destroy(sh);
    return 1;
  }

  const sio_collection_ids_t *coll_ids = sio_get_collection_ids(ctx);

  /* Register echo_pending staging collection with write-side components */
  SHIFT_COLLECTION(sh, echo_pending_coll, comp_ids.write_buf,
                   comp_ids.io_result, comp_ids.conn_entity,
                   comp_ids.user_conn_entity);

  if (sio_listen(ctx, PORT, BACKLOG) != sio_ok) {
    fprintf(stderr, "sio_listen failed on port %d\n", PORT);
    sio_context_destroy(ctx);
    shift_context_destroy(sh);
    return 1;
  }

  printf("Echo server listening on port %d\n", PORT);

  bool poll_error = false;
  while (g_running) {
    sio_result_t poll_result = sio_poll(ctx, 1);
    if (poll_result != sio_ok) {
      fprintf(stderr, "sio_poll failed (%d), aborting\n", poll_result);
      poll_error = true;
      break;
    }

    /* ------------------------------------------------------------------ */
    /* Process read_results: create echo write jobs or handle close.       */
    /* ------------------------------------------------------------------ */
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
        /* Connection closed or error */
        if (ro_results[i].error != 0 || ro_rbufs[i].len == 0) {
          shift_entity_destroy_one(sh, ro_entities[i]);
          /* Destroy connections entity → on_leave releases slot */
          if (!shift_entity_is_stale(sh, ro_conns[i].entity))
            shift_entity_destroy_one(sh, ro_conns[i].entity);
          /* Destroy user connection entity */
          if (!shift_entity_is_stale(sh, ro_uconns[i].entity))
            shift_entity_destroy_one(sh, ro_uconns[i].entity);
          continue;
        }

        /* Create echo write job in staging collection */
        shift_entity_t ep_entity;
        if (shift_entity_create_one_immediate(sh, echo_pending_coll,
                                              &ep_entity) != shift_ok) {
          shift_entity_move_one(sh, ro_entities[i], coll_ids->read_in);
          continue;
        }

        /* Copy connection handles for correlation */
        sio_conn_entity_t *ep_ce = NULL;
        shift_entity_get_component(sh, ep_entity, comp_ids.conn_entity,
                                   (void **)&ep_ce);
        ep_ce->entity = ro_conns[i].entity;

        sio_user_conn_entity_t *ep_uce = NULL;
        shift_entity_get_component(sh, ep_entity, comp_ids.user_conn_entity,
                                   (void **)&ep_uce);
        ep_uce->entity = ro_uconns[i].entity;

        /* Malloc and copy received data */
        sio_write_buf_t *ep_wb = NULL;
        shift_entity_get_component(sh, ep_entity, comp_ids.write_buf,
                                   (void **)&ep_wb);
        uint32_t len  = ro_rbufs[i].len;
        void    *copy = malloc(len);
        if (copy) {
          memcpy(copy, ro_rbufs[i].data, len);
          ep_wb->data = copy;
          ep_wb->len  = len;
        }

        /* Return read-cycle entity for re-arming */
        shift_entity_move_one(sh, ro_entities[i], coll_ids->read_in);
      }
    }

    /* ------------------------------------------------------------------ */
    /* Process echo_pending: move ready writes into write_in.              */
    /* ------------------------------------------------------------------ */
    {
      shift_entity_t  *ep_entities = NULL;
      sio_write_buf_t *ep_wbufs    = NULL;
      size_t           ep_count    = 0;

      shift_collection_get_entities(sh, echo_pending_coll, &ep_entities,
                                    &ep_count);
      shift_collection_get_component_array(sh, echo_pending_coll,
                                           comp_ids.write_buf,
                                           (void **)&ep_wbufs, NULL);

      for (size_t i = 0; i < ep_count; i++) {
        if (ep_wbufs[i].data)
          shift_entity_move_one(sh, ep_entities[i], coll_ids->write_in);
        else
          shift_entity_destroy_one(sh, ep_entities[i]);
      }
    }

    /* ------------------------------------------------------------------ */
    /* Process write_results: free buffers, destroy entities.              */
    /* ------------------------------------------------------------------ */
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

  printf("\nShutting down.\n");

  sio_context_destroy(ctx);
  shift_context_destroy(sh);
  return poll_error ? 1 : 0;
}
