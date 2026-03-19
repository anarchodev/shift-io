#include <shift_io.h>
#include <shift.h>

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PORT         7777
#define BACKLOG      128
#define MAX_CONNECTIONS      4096
#define BUF_COUNT    64
#define BUF_SIZE     4096

static volatile bool g_running = true;

static void handle_signal(int sig) {
  (void)sig;
  g_running = false;
}

/*
 * Echo server — demonstrates the decoupled read/write entity pattern.
 *
 * Connection entity lifecycle (managed by shift-io):
 *   read_pending → read_out → read_in → read_pending → …
 *
 * Write entity lifecycle:
 *   echo_pending (app) → write_in → write_pending → write_out → destroyed
 *
 * echo_pending is an application-owned staging collection.  It exists to
 * represent the "work item" for an echo job: the data has been copied out of
 * the io_uring buffer, the connection entity has been returned to read_in, and
 * now we need to send the copy back.  In a real application this staging
 * collection might hold a job that waits on a database query or other async
 * work before the write is ready.
 *
 * Components carried by write entities: {sio_fd_t, sio_write_buf_t}.
 * echo_pending is registered with the same components as write_in so that
 * entities can be moved between them with a plain shift_entity_move_one call.
 */

int main(void) {
  signal(SIGINT, handle_signal);
  signal(SIGTERM, handle_signal);

  shift_t       *sh     = NULL;
  shift_config_t sh_cfg = {
      .max_entities            = MAX_CONNECTIONS * 2,   /* connection + write entities */
      .max_components          = 8,
      .max_collections         = 16,
      .deferred_queue_capacity = MAX_CONNECTIONS * 4,
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
        .max_connections      = MAX_CONNECTIONS,
        .ring_entries = 256,
  };
  if (sio_context_create(&sio_cfg, &ctx) != sio_ok) {
    fprintf(stderr, "sio_context_create failed\n");
    shift_context_destroy(sh);
    return 1;
  }

  const sio_component_ids_t  *comp_ids = sio_get_component_ids(ctx);
  const sio_collection_ids_t *coll_ids = sio_get_collection_ids(ctx);

  /*
   * Register the echo_pending collection.
   * It uses the same {fd, write_buf} components as write_in so that entities
   * can be moved from echo_pending directly into write_in.
   */
  shift_collection_id_t echo_pending_coll;
  {
    shift_component_id_t    ep_comps[] = {comp_ids->fd, comp_ids->write_buf,
                                          comp_ids->io_result};
    shift_collection_info_t ep_info    = {
           .comp_ids     = ep_comps,
           .comp_count   = 3,
           .max_capacity = 0,
           .on_enter     = NULL,
           .on_leave     = NULL,
    };
    if (shift_collection_register(sh, &ep_info, &echo_pending_coll) != shift_ok) {
      fprintf(stderr, "failed to register echo_pending collection\n");
      sio_context_destroy(ctx);
      shift_context_destroy(sh);
      return 1;
    }
  }

  if (sio_listen(ctx, PORT, BACKLOG) != sio_ok) {
    fprintf(stderr, "sio_listen failed on port %d\n", PORT);
    sio_context_destroy(ctx);
    shift_context_destroy(sh);
    return 1;
  }

  printf("Echo server listening on port %d\n", PORT);

  bool poll_error = false;
  while (g_running) {
    /* ------------------------------------------------------------------ */
    /* Poll: arm recvs, drain read_in, drain write_in, wait for CQEs.     */
    /* sio_poll calls shift_flush internally before returning.            */
    /* ------------------------------------------------------------------ */
    sio_result_t poll_result = sio_poll(ctx, 1);
    if (poll_result != sio_ok) {
      fprintf(stderr, "sio_poll failed (%d), aborting\n", poll_result);
      poll_error = true;
      break;
    }

    /* ------------------------------------------------------------------ */
    /* Process read_result_out: create echo_pending write jobs.           */
    /*                                                                    */
    /* Entities arrive here for two reasons:                              */
    /*   - Data received (io_result.error == 0, read_buf.len > 0)        */
    /*   - Connection closed/error (error < 0, or error == 0 + len == 0) */
    /*                                                                    */
    /* On close/error: destroy the entity (slot already released).       */
    /* On data: create echo job, move connection entity to read_in.       */
    /* ------------------------------------------------------------------ */
    {
      shift_entity_t  *ro_entities = NULL;
      sio_fd_t        *ro_fds      = NULL;
      sio_read_buf_t  *ro_rbufs    = NULL;
      sio_io_result_t *ro_results  = NULL;
      size_t           ro_count    = 0;

      shift_collection_get_entities(sh, coll_ids->read_result_out, &ro_entities,
                                    &ro_count);
      shift_collection_get_component_array(sh, coll_ids->read_result_out,
                                           comp_ids->fd,
                                           (void **)&ro_fds, NULL);
      shift_collection_get_component_array(sh, coll_ids->read_result_out,
                                           comp_ids->read_buf,
                                           (void **)&ro_rbufs, NULL);
      shift_collection_get_component_array(sh, coll_ids->read_result_out,
                                           comp_ids->io_result,
                                           (void **)&ro_results, NULL);

      for (size_t i = 0; i < ro_count; i++) {
        /* Connection closed or error — entity already disconnected. */
        if (ro_results[i].error != 0 || ro_rbufs[i].len == 0) {
          shift_entity_destroy_one(sh, ro_entities[i]);
          continue;
        }

        /* Create a write entity in echo_pending (eager placement). */
        shift_entity_t ep_entity;
        if (shift_entity_create_one(sh, echo_pending_coll, &ep_entity) !=
            shift_ok) {
          /* OOM — drop this connection's reply; move to read_in to re-arm. */
          shift_entity_move_one(sh, ro_entities[i], coll_ids->read_in);
          continue;
        }

        /* Copy the fd. */
        sio_fd_t *ep_fd = NULL;
        shift_entity_get_component(sh, ep_entity, comp_ids->fd,
                                   (void **)&ep_fd);
        ep_fd->fd = ro_fds[i].fd;

        /* Malloc and copy the received data. */
        sio_write_buf_t *ep_wb = NULL;
        shift_entity_get_component(sh, ep_entity, comp_ids->write_buf,
                                   (void **)&ep_wb);

        uint32_t len  = ro_rbufs[i].len;
        void    *copy = malloc(len);
        if (copy) {
          memcpy(copy, ro_rbufs[i].data, len);
          ep_wb->data = copy;
          ep_wb->len  = len;
        }
        /* If malloc failed, ep_wb->data remains NULL; the echo_pending
         * processing step below will destroy the write entity rather than
         * submitting a zero-length send. */

        /* Return the connection entity to the read cycle. */
        shift_entity_move_one(sh, ro_entities[i], coll_ids->read_in);
      }
    }

    /* ------------------------------------------------------------------ */
    /* Process echo_pending: move ready write jobs into write_in.         */
    /*                                                                    */
    /* echo_pending entities were created eagerly above and are visible   */
    /* in the collection immediately (before shift_flush).                */
    /* ------------------------------------------------------------------ */
    {
      shift_entity_t  *ep_entities = NULL;
      sio_write_buf_t *ep_wbufs    = NULL;
      size_t           ep_count    = 0;

      shift_collection_get_entities(sh, echo_pending_coll, &ep_entities,
                                    &ep_count);
      shift_collection_get_component_array(sh, echo_pending_coll,
                                           comp_ids->write_buf,
                                           (void **)&ep_wbufs, NULL);

      for (size_t i = 0; i < ep_count; i++) {
        if (ep_wbufs[i].data) {
          /* Hand off to shift-io for sending. */
          shift_entity_move_one(sh, ep_entities[i], coll_ids->write_in);
        } else {
          /* malloc failed earlier — discard this write job. */
          shift_entity_destroy_one(sh, ep_entities[i]);
        }
      }
    }

    /* ------------------------------------------------------------------ */
    /* Process write_result_out: free send buffers, destroy entities.     */
    /* io_result.error may be checked here if send failure needs handling.*/
    /* ------------------------------------------------------------------ */
    {
      shift_entity_t  *wo_entities = NULL;
      sio_write_buf_t *wo_wbufs    = NULL;
      size_t           wo_count    = 0;

      shift_collection_get_entities(sh, coll_ids->write_result_out, &wo_entities,
                                    &wo_count);
      shift_collection_get_component_array(sh, coll_ids->write_result_out,
                                           comp_ids->write_buf,
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
