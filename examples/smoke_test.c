#include <shift_io.h>
#include <shift.h>

#include <stdio.h>
#include <string.h>

#define MAX_CONNECTIONS 64
#define BUF_COUNT       64
#define BUF_SIZE        4096

/* Custom user component to prove superset propagation. */
typedef struct {
  uint64_t tag;
} custom_tag_t;

/* Check that collection `col` contains component `comp`. */
static bool collection_has(shift_t *sh, shift_collection_id_t col,
                           shift_component_id_t comp) {
  const shift_component_id_t *ids = NULL;
  uint32_t count = 0;
  if (shift_collection_get_components(sh, col, &ids, &count) != shift_ok)
    return false;
  for (uint32_t i = 0; i < count; i++)
    if (ids[i] == comp)
      return true;
  return false;
}

int main(void) {
  int failures = 0;

  shift_t       *sh     = NULL;
  shift_config_t sh_cfg = {
      .max_entities            = MAX_CONNECTIONS * 6,
      .max_components          = 32,
      .max_collections         = 32,
      .deferred_queue_capacity = MAX_CONNECTIONS * 6,
  };
  if (shift_context_create(&sh_cfg, &sh) != shift_ok) {
    fprintf(stderr, "FAIL: shift_context_create\n");
    return 1;
  }

  /* Register sio components */
  sio_component_ids_t comp_ids;
  if (sio_register_components(sh, &comp_ids) != sio_ok) {
    fprintf(stderr, "FAIL: sio_register_components\n");
    return 1;
  }

  /* Register a custom user component */
  shift_component_id_t custom_comp;
  {
    shift_component_info_t ci = {.element_size = sizeof(custom_tag_t)};
    if (shift_component_register(sh, &ci, &custom_comp) != shift_ok) {
      fprintf(stderr, "FAIL: register custom component\n");
      return 1;
    }
  }

  /* Create user collections WITH the custom component */
  shift_collection_id_t connections_coll;
  {
    shift_component_id_t comps[] = {comp_ids.fd, comp_ids.read_cycle_entity,
                                    custom_comp};
    shift_collection_info_t info = {
        .name = "connections", .comp_ids = comps, .comp_count = 3};
    shift_collection_register(sh, &info, &connections_coll);
  }

  shift_collection_id_t read_results_coll;
  {
    shift_component_id_t comps[] = {comp_ids.read_buf, comp_ids.io_result,
                                    comp_ids.conn_entity, custom_comp};
    shift_collection_info_t info = {
        .name = "read_results", .comp_ids = comps, .comp_count = 4};
    shift_collection_register(sh, &info, &read_results_coll);
  }

  shift_collection_id_t write_results_coll;
  {
    shift_component_id_t comps[] = {comp_ids.write_buf, comp_ids.io_result,
                                    comp_ids.conn_entity, custom_comp};
    shift_collection_info_t info = {
        .name = "write_results", .comp_ids = comps, .comp_count = 4};
    shift_collection_register(sh, &info, &write_results_coll);
  }

  /* Create sio context */
  sio_context_t *ctx     = NULL;
  sio_config_t   sio_cfg = {
      .shift              = sh,
      .comp_ids           = comp_ids,
      .buf_count          = BUF_COUNT,
      .buf_size           = BUF_SIZE,
      .max_connections    = MAX_CONNECTIONS,
      .ring_entries       = 64,
      .connections         = connections_coll,
      .read_results        = read_results_coll,
      .write_results       = write_results_coll,
  };
  if (sio_context_create(&sio_cfg, &ctx) != sio_ok) {
    fprintf(stderr, "FAIL: sio_context_create\n");
    return 1;
  }

  printf("sio_context_create: OK\n");

  /* Verify the custom component propagated to internal collections */
  const sio_collection_ids_t *coll_ids = sio_get_collection_ids(ctx);

#define CHECK(name, coll)                                                     \
  do {                                                                        \
    if (collection_has(sh, coll, custom_comp))                                \
      printf("  %-14s has custom_comp: OK\n", name);                          \
    else {                                                                    \
      printf("  %-14s has custom_comp: FAIL\n", name);                        \
      failures++;                                                             \
    }                                                                         \
  } while (0)

  CHECK("connections",  connections_coll);
  CHECK("read_in",      coll_ids->read_in);
  CHECK("write_in",     coll_ids->write_in);

#undef CHECK

  printf("\n%s\n", failures == 0 ? "ALL PASSED" : "SOME TESTS FAILED");

  sio_context_destroy(ctx);
  shift_context_destroy(sh);
  return failures == 0 ? 0 : 1;
}
