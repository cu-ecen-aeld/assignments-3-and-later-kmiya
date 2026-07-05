// NOLINTNEXTLINE(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp)
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <syslog.h>
#include <time.h>

#include "queue.h"

enum { BUFFER_SIZE = 1024 };

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
_Atomic bool caught_signal = false;

static void signal_handler(const int signal_number) {
  if ((signal_number == SIGINT) || (signal_number == SIGTERM)) {
    caught_signal = true;
  }
}

void register_signal_handler() {
  struct sigaction handler;
  memset(&handler, 0, sizeof(struct sigaction));
  handler.sa_handler = signal_handler;
  if (sigaction(SIGTERM, &handler, NULL) != 0) {
    syslog(LOG_ERR, "Error %d (%s) registering for SIGTERM", errno,
           strerror(errno));  // NOLINT(concurrency-mt-unsafe)
    exit(EXIT_FAILURE);       // NOLINT(concurrency-mt-unsafe)
  }
  if (sigaction(SIGINT, &handler, NULL)) {
    syslog(LOG_ERR, "Error %d (%s) registering for SIGINT", errno,
           strerror(errno));  // NOLINT(concurrency-mt-unsafe)
    exit(EXIT_FAILURE);       // NOLINT(concurrency-mt-unsafe)
  }
}

int create_socket_and_bind() {
  struct addrinfo hints;

  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE;

  struct addrinfo* result = NULL;
  struct addrinfo* rp = NULL;
  const int status = getaddrinfo(NULL, "9000", &hints, &result);
  if (status != 0) {
    syslog(LOG_ERR, "getaddrinfo: %s", gai_strerror(status));
    closelog();
    exit(EXIT_FAILURE);  // NOLINT(concurrency-mt-unsafe)
  }

  int socket_fd = -1;
  for (rp = result; rp != NULL; rp = rp->ai_next) {
    socket_fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
    if (socket_fd == -1) {
      continue;
    }

    // Allow reusing the local address
    int opt = 1;
    setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    if (bind(socket_fd, rp->ai_addr, rp->ai_addrlen) == 0) {
      break;  // Success
    }
    close(socket_fd);
  }
  freeaddrinfo(result);  // No longer needed

  if (rp == NULL) {
    syslog(LOG_ERR, "Could not bind");
    // Assignment specification: Return -1 when failure
    return -1;
  }
  return socket_fd;
}

int write_log_file(char* buffer, size_t size, FILE* write_file) {
  const size_t written = fwrite(buffer, 1, size, write_file);
  if (written != size) {
    syslog(LOG_ERR, "Could not write file");
    return -1;
  }
  return 0;
}

int send_file(const int client_fd, FILE* write_file) {
  char* line = NULL;
  size_t len = 0;
  while (getline(&line, &len, write_file) != -1) {
    size_t length = strlen(line);
    const ssize_t sent_size = send(client_fd, line, length, 0);
    if (sent_size < 0) {
      syslog(LOG_ERR, "Failed to send data");
      return -1;
    }
  }
  free(line);
  return 0;
}

static void safe_fclose(FILE* fp) {
  if (fp == NULL) {
    return;
  }
  if (fclose(fp) != 0) {
    syslog(LOG_ERR, "Failed to close file");
  }
}

bool parse_daemon_arg(const int argc, char** argv) {
  if (argc > 2) {
    syslog(LOG_ERR, "Too many arguments");
    exit(EXIT_FAILURE);  // NOLINT(concurrency-mt-unsafe)
  }
  if (argc == 2) {
    if (strcmp(argv[1], "-d") == 0) {
      syslog(LOG_INFO, "Run as a daemon");
      return true;
    }
  }
  return false;
}

typedef struct {
  FILE* write_file;
  pthread_mutex_t* mutex;
  int client_fd;
  struct sockaddr_in client_addr;
  bool completed;
} thread_args;

void* timer_thread_func(void* timer_thread_args) {
  thread_args* args = timer_thread_args;
  struct tm tm_buf;

  int elapsed_ms = 0;
  const int check_interval_ms = 100;        // Check signal every 100ms
  const int timestamp_interval_ms = 10000;  // Write timestamp every 10s

  while (!caught_signal) {
    const struct timespec interval = {.tv_sec = 0,
                                      .tv_nsec = check_interval_ms * 1000000L};
    nanosleep(&interval, NULL);
    elapsed_ms += check_interval_ms;

    if (elapsed_ms < timestamp_interval_ms) {
      continue;
    }
    elapsed_ms = 0;

    const time_t now = time(NULL);
    if (localtime_r(&now, &tm_buf) != NULL) {
      char buf[128];
      strcpy(buf, "timestamp:");
      strftime(buf + strlen(buf), sizeof(buf) - strlen(buf),
               "%a, %d %b %Y %T %z\n", &tm_buf);
      pthread_mutex_lock(args->mutex);
      if (write_log_file(buf, (ssize_t)strlen(buf), args->write_file) == -1) {
        syslog(LOG_ERR, "Failed to write timestamp");
      }
      pthread_mutex_unlock(args->mutex);
    }
  }
  return NULL;
}

void* connection_thread_func(void* con_thread_args) {
  thread_args* args = con_thread_args;

  char buffer[BUFFER_SIZE];
  ssize_t received_size = 0;
  pthread_mutex_lock(args->mutex);
  while ((received_size = recv(args->client_fd, buffer, BUFFER_SIZE - 1, 0)) >
         0) {
    buffer[received_size] = '\0';

    if (write_log_file(buffer, received_size, args->write_file) == -1) {
      syslog(LOG_ERR, "Failed to write received data to file");
      pthread_mutex_unlock(args->mutex);
      args->completed = true;
      return NULL;
    }

    if (memchr(buffer, '\n', received_size) != NULL) {
      break;
    }
  }

  if (fseek(args->write_file, 0, SEEK_SET) != 0) {
    syslog(LOG_ERR, "fseek failed");
  }

  // send -----------------------------------------------
  if (send_file(args->client_fd, args->write_file) == -1) {
    syslog(LOG_ERR, "Failed to send back received data");
    pthread_mutex_unlock(args->mutex);
    args->completed = true;
    return NULL;
  }
  pthread_mutex_unlock(args->mutex);
  args->completed = true;
  return NULL;
}

typedef struct th_node_s th_node_t;
struct th_node_s {
  thread_args* data;
  pthread_t thread_id;
  SLIST_ENTRY(th_node_s) next;
};

int main(int argc, char** argv) {
  openlog("aesdsocket", LOG_CONS | LOG_PERROR, LOG_USER);

  const bool daemon_mode = parse_daemon_arg(argc, argv);

  // Register signal handler ------------------------------
  register_signal_handler();

  // Create socket and bind -------------------------------
  const int socket_fd = create_socket_and_bind();
  if (socket_fd == -1) {
    closelog();
    // Assignment specification: Return -1 when failure
    return -1;
  }

  // Listen -----------------------------------------------
  if (listen(socket_fd, 5) < 0) {
    syslog(LOG_ERR, "Could not listen");
    closelog();
    exit(EXIT_FAILURE);  // NOLINT(concurrency-mt-unsafe)
  }

  if (daemon_mode) {
    if (daemon(0, 0) == -1) {
      syslog(LOG_ERR, "Failed to create a daemon");
      closelog();
      exit(EXIT_FAILURE);  // NOLINT(concurrency-mt-unsafe)
    }
  }

  // Open file --------------------------------------------
  const char* write_path = "/var/tmp/aesdsocketdata";
  FILE* write_file = fopen(write_path, "a+");
  if (!write_file) {
    syslog(LOG_ERR, "Cannot open file: %s", write_path);
    close(socket_fd);
    closelog();
    exit(EXIT_FAILURE);  // NOLINT(concurrency-mt-unsafe)
  }

  // mutex for write file
  static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

  // Start timer thread -----------------------------------
  pthread_t timer_thread = 0;
  thread_args* timer_args = malloc(sizeof(thread_args));
  timer_args->write_file = write_file;
  timer_args->mutex = &mutex;
  int timer_result =
      pthread_create(&timer_thread, NULL, timer_thread_func, (void*)timer_args);
  if (timer_result) {
    syslog(LOG_ERR, "Cannot create timer thread");
    free(timer_args);
    close(socket_fd);
    closelog();
    exit(EXIT_FAILURE);  // NOLINT(concurrency-mt-unsafe)
  }

  SLIST_HEAD(thread_list, th_node_s) head;
  SLIST_INIT(&head);

  // Main loop --------------------------------------------
  while (!caught_signal) {
    // Accept ---------------------------------------------
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);
    int client_fd =
        accept(socket_fd, (struct sockaddr*)&client_addr, &addr_len);
    if (client_fd < 0) {
      // Ignore cancelling by signal
      if (errno == EINTR) {
        continue;
      }
      syslog(LOG_ERR, "Could not accept");
      continue;
    }
    syslog(LOG_INFO, "Accepted connection from %s",
           inet_ntoa(client_addr.sin_addr));  // NOLINT(concurrency-mt-unsafe)

    // Create a thread for each connection -----------------
    pthread_t con_thread = 0;
    thread_args* con_args = malloc(sizeof(thread_args));
    con_args->write_file = write_file;
    con_args->mutex = &mutex;
    con_args->client_fd = client_fd;
    con_args->client_addr = client_addr;
    con_args->completed = false;
    int con_result = pthread_create(&con_thread, NULL, connection_thread_func,
                                    (void*)con_args);
    if (con_result) {
      syslog(LOG_ERR, "Cannot create timer thread");
      close(client_fd);
      free(con_args);
      continue;
    }

    th_node_t* node = malloc(sizeof(th_node_t));
    node->data = con_args;
    node->thread_id = con_thread;
    SLIST_INSERT_HEAD(&head, node, next);

    // Remove completed connection from thread list---------
    th_node_t* curr = NULL;
    th_node_t* tmp = NULL;
    SLIST_FOREACH_SAFE(curr, &head, next, tmp) {
      if (curr->data->completed) {
        syslog(LOG_INFO, "Terminate thread %ld", curr->thread_id);
        pthread_join(curr->thread_id, NULL);
        close(curr->data->client_fd);
        syslog(LOG_INFO, "Closed connection from %s",
               inet_ntoa(curr->data->client_addr
                             .sin_addr));  // NOLINT(concurrency-mt-unsafe)
        SLIST_REMOVE(&head, curr, th_node_s, next);
        free(curr->data);
        free(curr);
      }
    }
  }
  syslog(LOG_INFO, "Caught signal, exiting");

  // Close socket as soon as possible
  close(socket_fd);

  while (!SLIST_EMPTY(&head)) {
    th_node_t* node = SLIST_FIRST(&head);
    pthread_join(node->thread_id, NULL);
    close(node->data->client_fd);
    SLIST_REMOVE_HEAD(&head, next);
    free(node->data);
    free(node);
  }

  // Wait for timer thread to finish
  pthread_join(timer_thread, NULL);
  free(timer_args);

  safe_fclose(write_file);
  if (remove(write_path) == -1) {
    syslog(LOG_ERR, "Failed to remove %s", write_path);
  }
  closelog();
  return 0;
}
