/*
 * Multithreaded load test for the echo server.
 *
 * Usage: ./load_test [options]
 *   -t <threads>       Number of worker threads (default: 1)
 *   -c <connections>   Number of concurrent connections (default: 100)
 *   -n <requests>      Total requests per connection (default: 1000)
 *   -s <size>          Payload size in bytes (default: 128)
 *   -h <host>          Server host (default: 127.0.0.1)
 *   -p <port>          Server port (default: 7777)
 */

#define _GNU_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <time.h>
#include <unistd.h>

#define MAX_CONNECTIONS  8192
#define MAX_PAYLOAD_SIZE 4096
#define MAX_EVENTS       256

static volatile bool g_running = true;

static void handle_signal(int sig) {
  (void)sig;
  g_running = false;
}

typedef enum {
  CONN_CONNECTING,
  CONN_SENDING,
  CONN_RECEIVING,
  CONN_DONE,
} conn_state_t;

typedef struct {
  int          fd;
  conn_state_t state;
  uint32_t     requests_done;
  uint32_t     send_offset;
  uint32_t     recv_offset;
  uint64_t     rtt_sum_ns;
  uint64_t     req_start_ns;
  uint64_t     min_rtt_ns;
  uint64_t     max_rtt_ns;
} connection_t;

typedef struct {
  const char *host;
  uint16_t    port;
  uint32_t    num_connections;
  uint32_t    requests_per_conn;
  uint32_t    payload_size;
} config_t;

typedef struct {
  int            worker_id;
  const config_t *cfg;
  const uint8_t  *payload;
  uint32_t       num_connections; /* this worker's share */

  /* results */
  uint64_t total_requests_done;
  uint64_t total_bytes_echoed;
  uint64_t total_rtt_ns;
  uint64_t min_rtt_ns;
  uint64_t max_rtt_ns;
  uint32_t errors;
  uint32_t verify_failures;
  uint32_t established;
} worker_ctx_t;

static uint64_t now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static int set_nonblocking(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags == -1) return -1;
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int connect_to_server(const config_t *cfg) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;

  set_nonblocking(fd);

  int yes = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));

  struct sockaddr_in addr = {
      .sin_family = AF_INET,
      .sin_port   = htons(cfg->port),
  };
  inet_pton(AF_INET, cfg->host, &addr.sin_addr);

  int ret = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
  if (ret < 0 && errno != EINPROGRESS) {
    close(fd);
    return -1;
  }
  return fd;
}

static void start_request(connection_t *conn) {
  conn->state       = CONN_SENDING;
  conn->send_offset = 0;
  conn->recv_offset = 0;
  conn->req_start_ns = now_ns();
}

static void *worker_fn(void *arg) {
  worker_ctx_t *wctx = arg;
  const config_t *cfg = wctx->cfg;
  const uint8_t *payload = wctx->payload;
  uint32_t nconns = wctx->num_connections;

  uint8_t recv_buf[MAX_PAYLOAD_SIZE];

  int epfd = epoll_create1(0);
  if (epfd < 0) {
    fprintf(stderr, "Worker %d: epoll_create1 failed\n", wctx->worker_id);
    return NULL;
  }

  connection_t *conns = calloc(nconns, sizeof(connection_t));
  if (!conns) {
    close(epfd);
    return NULL;
  }

  /* Establish connections */
  uint32_t active = 0;
  for (uint32_t i = 0; i < nconns; i++) {
    conns[i].fd = connect_to_server(cfg);
    if (conns[i].fd < 0) continue;
    conns[i].state      = CONN_CONNECTING;
    conns[i].min_rtt_ns = UINT64_MAX;

    struct epoll_event ev = {.events = EPOLLOUT, .data.u32 = i};
    epoll_ctl(epfd, EPOLL_CTL_ADD, conns[i].fd, &ev);
    active++;
  }
  wctx->established = active;
  wctx->min_rtt_ns  = UINT64_MAX;

  struct epoll_event events[MAX_EVENTS];

  while (g_running && active > 0) {
    int n = epoll_wait(epfd, events, MAX_EVENTS, 100);
    if (n < 0) {
      if (errno == EINTR) continue;
      break;
    }

    for (int i = 0; i < n; i++) {
      uint32_t      idx  = events[i].data.u32;
      connection_t *conn = &conns[idx];
      uint32_t      ev   = events[i].events;

      if (ev & (EPOLLERR | EPOLLHUP)) {
        epoll_ctl(epfd, EPOLL_CTL_DEL, conn->fd, NULL);
        close(conn->fd);
        conn->state = CONN_DONE;
        wctx->errors++;
        active--;
        continue;
      }

      switch (conn->state) {
      case CONN_CONNECTING:
        start_request(conn);
        {
          struct epoll_event mod = {.events = EPOLLOUT, .data.u32 = idx};
          epoll_ctl(epfd, EPOLL_CTL_MOD, conn->fd, &mod);
        }
        break;

      case CONN_SENDING: {
        ssize_t sent = write(conn->fd, payload + conn->send_offset,
                             cfg->payload_size - conn->send_offset);
        if (sent <= 0) {
          if (errno == EAGAIN || errno == EWOULDBLOCK) break;
          epoll_ctl(epfd, EPOLL_CTL_DEL, conn->fd, NULL);
          close(conn->fd);
          conn->state = CONN_DONE;
          wctx->errors++;
          active--;
          break;
        }
        conn->send_offset += (uint32_t)sent;
        if (conn->send_offset >= cfg->payload_size) {
          conn->state = CONN_RECEIVING;
          struct epoll_event mod = {.events = EPOLLIN, .data.u32 = idx};
          epoll_ctl(epfd, EPOLL_CTL_MOD, conn->fd, &mod);
        }
        break;
      }

      case CONN_RECEIVING: {
        ssize_t recvd = read(conn->fd, recv_buf + conn->recv_offset,
                             cfg->payload_size - conn->recv_offset);
        if (recvd <= 0) {
          if (recvd == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
            epoll_ctl(epfd, EPOLL_CTL_DEL, conn->fd, NULL);
            close(conn->fd);
            conn->state = CONN_DONE;
            wctx->errors++;
            active--;
          }
          break;
        }
        conn->recv_offset += (uint32_t)recvd;
        if (conn->recv_offset >= cfg->payload_size) {
          if (memcmp(recv_buf, payload, cfg->payload_size) != 0)
            wctx->verify_failures++;

          uint64_t rtt = now_ns() - conn->req_start_ns;
          conn->rtt_sum_ns += rtt;
          if (rtt < conn->min_rtt_ns) conn->min_rtt_ns = rtt;
          if (rtt > conn->max_rtt_ns) conn->max_rtt_ns = rtt;

          conn->requests_done++;
          wctx->total_requests_done++;
          wctx->total_bytes_echoed += cfg->payload_size;

          if (conn->requests_done >= cfg->requests_per_conn) {
            epoll_ctl(epfd, EPOLL_CTL_DEL, conn->fd, NULL);
            close(conn->fd);
            conn->state = CONN_DONE;
            active--;
          } else {
            start_request(conn);
            struct epoll_event mod = {.events = EPOLLOUT, .data.u32 = idx};
            epoll_ctl(epfd, EPOLL_CTL_MOD, conn->fd, &mod);
          }
        }
        break;
      }

      case CONN_DONE:
        break;
      }
    }
  }

  /* Aggregate per-connection RTT stats */
  for (uint32_t i = 0; i < nconns; i++) {
    if (conns[i].requests_done > 0) {
      wctx->total_rtt_ns += conns[i].rtt_sum_ns;
      if (conns[i].min_rtt_ns < wctx->min_rtt_ns)
        wctx->min_rtt_ns = conns[i].min_rtt_ns;
      if (conns[i].max_rtt_ns > wctx->max_rtt_ns)
        wctx->max_rtt_ns = conns[i].max_rtt_ns;
    }
  }

  free(conns);
  close(epfd);
  return NULL;
}

int main(int argc, char *argv[]) {
  signal(SIGINT, handle_signal);
  signal(SIGPIPE, SIG_IGN);

  config_t cfg = {
      .host              = "127.0.0.1",
      .port              = 7777,
      .num_connections   = 100,
      .requests_per_conn = 1000,
      .payload_size      = 128,
  };
  int nthreads = 1;

  int opt;
  while ((opt = getopt(argc, argv, "t:c:n:s:h:p:")) != -1) {
    switch (opt) {
    case 't': nthreads = atoi(optarg); break;
    case 'c': cfg.num_connections = (uint32_t)atoi(optarg); break;
    case 'n': cfg.requests_per_conn = (uint32_t)atoi(optarg); break;
    case 's': cfg.payload_size = (uint32_t)atoi(optarg); break;
    case 'h': cfg.host = optarg; break;
    case 'p': cfg.port = (uint16_t)atoi(optarg); break;
    default:
      fprintf(stderr,
              "Usage: %s [-t threads] [-c conns] [-n reqs] [-s size] "
              "[-h host] [-p port]\n",
              argv[0]);
      return 1;
    }
  }

  if (nthreads < 1) nthreads = 1;
  if (cfg.num_connections > MAX_CONNECTIONS) {
    fprintf(stderr, "Max connections: %d\n", MAX_CONNECTIONS);
    return 1;
  }
  if (cfg.payload_size > MAX_PAYLOAD_SIZE || cfg.payload_size == 0) {
    fprintf(stderr, "Payload size must be 1-%d\n", MAX_PAYLOAD_SIZE);
    return 1;
  }

  uint8_t payload[MAX_PAYLOAD_SIZE];
  for (uint32_t i = 0; i < cfg.payload_size; i++)
    payload[i] = (uint8_t)(i & 0xFF);

  printf("Load test config:\n");
  printf("  Server:       %s:%u\n", cfg.host, cfg.port);
  printf("  Threads:      %d\n", nthreads);
  printf("  Connections:  %u\n", cfg.num_connections);
  printf("  Requests/conn:%u\n", cfg.requests_per_conn);
  printf("  Payload size: %u bytes\n", cfg.payload_size);
  printf("  Total reqs:   %lu\n",
         (unsigned long)cfg.num_connections * cfg.requests_per_conn);
  printf("\n");

  /* Distribute connections across threads */
  worker_ctx_t *workers = calloc((size_t)nthreads, sizeof(worker_ctx_t));
  pthread_t    *threads = calloc((size_t)nthreads, sizeof(pthread_t));

  uint32_t base = cfg.num_connections / (uint32_t)nthreads;
  uint32_t rem  = cfg.num_connections % (uint32_t)nthreads;

  for (int i = 0; i < nthreads; i++) {
    workers[i].worker_id       = i;
    workers[i].cfg             = &cfg;
    workers[i].payload         = payload;
    workers[i].num_connections = base + (uint32_t)(i < (int)rem ? 1 : 0);
  }

  uint64_t start_time = now_ns();

  for (int i = 0; i < nthreads; i++)
    pthread_create(&threads[i], NULL, worker_fn, &workers[i]);

  for (int i = 0; i < nthreads; i++)
    pthread_join(threads[i], NULL);

  uint64_t end_time = now_ns();
  double elapsed_s = (double)(end_time - start_time) / 1e9;

  /* Aggregate results across all workers */
  uint64_t total_requests_done = 0;
  uint64_t total_bytes_echoed  = 0;
  uint64_t total_rtt_ns        = 0;
  uint64_t global_min_rtt_ns   = UINT64_MAX;
  uint64_t global_max_rtt_ns   = 0;
  uint32_t total_established   = 0;
  uint32_t total_errors        = 0;
  uint32_t total_verify        = 0;

  for (int i = 0; i < nthreads; i++) {
    total_requests_done += workers[i].total_requests_done;
    total_bytes_echoed  += workers[i].total_bytes_echoed;
    total_rtt_ns        += workers[i].total_rtt_ns;
    total_established   += workers[i].established;
    total_errors        += workers[i].errors;
    total_verify        += workers[i].verify_failures;
    if (workers[i].min_rtt_ns < global_min_rtt_ns)
      global_min_rtt_ns = workers[i].min_rtt_ns;
    if (workers[i].max_rtt_ns > global_max_rtt_ns)
      global_max_rtt_ns = workers[i].max_rtt_ns;
  }

  uint64_t total_requests =
      (uint64_t)cfg.num_connections * cfg.requests_per_conn;

  printf("Established %u/%u connections\n\n", total_established,
         cfg.num_connections);

  printf("========== Results ==========\n");
  printf("Duration:         %.3f s\n", elapsed_s);
  printf("Requests done:    %lu / %lu\n", (unsigned long)total_requests_done,
         (unsigned long)total_requests);
  printf("Throughput:       %.0f req/s\n",
         (double)total_requests_done / elapsed_s);
  printf("Data throughput:  %.2f MB/s (send+recv)\n",
         (double)total_bytes_echoed * 2.0 / (1024.0 * 1024.0) / elapsed_s);
  printf("Errors:           %u\n", total_errors);
  printf("Verify failures:  %u\n", total_verify);

  if (total_requests_done > 0) {
    double avg_rtt_us =
        (double)total_rtt_ns / (double)total_requests_done / 1e3;
    double min_rtt_us = (double)global_min_rtt_ns / 1e3;
    double max_rtt_us = (double)global_max_rtt_ns / 1e3;
    printf("RTT avg:          %.1f us\n", avg_rtt_us);
    printf("RTT min:          %.1f us\n", min_rtt_us);
    printf("RTT max:          %.1f us\n", max_rtt_us);
  }
  printf("=============================\n");

  free(threads);
  free(workers);
  return (total_errors > 0 || total_verify > 0) ? 1 : 0;
}
