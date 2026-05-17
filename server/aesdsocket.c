// NOLINTNEXTLINE(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp)
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <syslog.h>

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

int receive_and_write(const int client_fd, FILE* write_file) {
  char buffer[BUFFER_SIZE];
  ssize_t received_size = 0;
  while ((received_size = recv(client_fd, buffer, BUFFER_SIZE - 1, 0)) > 0) {
    buffer[received_size] = '\0';

    const size_t written = fwrite(buffer, 1, received_size, write_file);
    if (written != (size_t)received_size) {
      syslog(LOG_ERR, "Could not write file");
      return -1;
    }

    if (memchr(buffer, '\n', received_size) != NULL) {
      break;
    }
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
      close(socket_fd);
      closelog();
      exit(EXIT_FAILURE);  // NOLINT(concurrency-mt-unsafe)
    }
    syslog(LOG_INFO, "Accepted connection from %s",
           inet_ntoa(client_addr.sin_addr));  // NOLINT(concurrency-mt-unsafe)

    // recv ---------------------------------------------
    if (receive_and_write(client_fd, write_file) == -1) {
      close(socket_fd);
      closelog();
      return 1;
    }

    if (fseek(write_file, 0, SEEK_SET) != 0) {
      syslog(LOG_ERR, "fseek failed");
    }

    // send -----------------------------------------------
    if (send_file(client_fd, write_file) == -1) {
      safe_fclose(write_file);
      close(socket_fd);
      closelog();
      exit(EXIT_FAILURE);  // NOLINT(concurrency-mt-unsafe)
    }

    close(client_fd);
    syslog(LOG_INFO, "Closed connection from %s",
           inet_ntoa(client_addr.sin_addr));  // NOLINT(concurrency-mt-unsafe)
  }
  syslog(LOG_INFO, "Caught signal, exiting");

  safe_fclose(write_file);
  close(socket_fd);
  if (remove(write_path) == -1) {
    syslog(LOG_ERR, "Failed to remove %s", write_path);
    closelog();
    exit(EXIT_FAILURE);  // NOLINT(concurrency-mt-unsafe)
  }
  closelog();
  return 0;
}
