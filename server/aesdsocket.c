#define _GNU_SOURCE
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <syslog.h>

#define BUFFER_SIZE 1024

static void safe_fclose(FILE* fp) {
  if (fp == NULL) {
    return;
  }
  if (fclose(fp) != 0) {
    syslog(LOG_ERR, "Fail to close file");
  }
}

int main(int args, char** argv) {
  openlog("aesdsocket", LOG_CONS | LOG_PERROR, LOG_USER);

  struct addrinfo hints;

  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE;

  struct addrinfo *result, *rp;
  const int status = getaddrinfo(NULL, "9000", &hints, &result);
  if (status != 0) {
    syslog(LOG_ERR, "getaddrinfo: %s", gai_strerror(status));
    closelog();
    exit(EXIT_FAILURE);
  }

  int socket_fd = -1;
  for (rp = result; rp != NULL; rp = rp->ai_next) {
    socket_fd =
        socket(rp->ai_family, rp->ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC,
               rp->ai_protocol);
    if (socket_fd == -1) {
      continue;
    }
    if (bind(socket_fd, rp->ai_addr, rp->ai_addrlen) == 0) {
      break;  // Success
    }
    close(socket_fd);
  }
  freeaddrinfo(result);  // No longer needed

  if (rp == NULL) {
    syslog(LOG_ERR, "Could not bind");
    closelog();
    exit(-1);
  }

  if (listen(socket_fd, 5) < 0) {
    syslog(LOG_ERR, "Could not listen");
    closelog();
    exit(EXIT_FAILURE);
  }

  const char* write_path = "/var/tmp/aesdsocketdata";
  FILE* write_file = fopen(write_path, "a");
  if (!write_file) {
    syslog(LOG_ERR, "Cannot open file: %s", write_path);
    close(socket_fd);
    closelog();
    exit(EXIT_FAILURE);
  }

  while (1) {
    struct sockaddr_in client_addr;
    int client_fd =
        accept(socket_fd, (struct sockaddr*)&client_addr, sizeof(client_addr));
    if (client_fd < 0) {
      syslog(LOG_ERR, "Could not accept");
      close(socket_fd);
      closelog();
      exit(-1);
    }
    syslog(LOG_INFO, "Accepted connection from %s",
           inet_ntoa(client_addr.sin_addr));

    char buffer[BUFFER_SIZE];
    size_t buffer_pos = 0;
    while (1) {
      const ssize_t received_size =
          recv(socket_fd, buffer + buffer_pos, BUFFER_SIZE - buffer_pos - 1, 0);
      if (received_size <= 0) break;
      buffer[buffer_pos + received_size] = '\0';

      char* newline = strchr(buffer[buffer_pos], '\n');
      if (newline != NULL) {
        *newline = '\0';
        break;
      }
      buffer_pos += received_size;
      if (buffer_pos >= BUFFER_SIZE - 1) {
        syslog(LOG_WARNING, "Buffer is full");
        break;
      }
    }
    fprintf(write_file, "%s\n", buffer);
    const ssize_t sent_size = send(client_fd, buffer, buffer_pos, 0);
    if (sent_size < 0) {
      syslog(LOG_ERR, "Failed to send data");
      safe_fclose(write_file);
      close(socket_fd);
      closelog();
      exit(EXIT_FAILURE);
    }
    syslog(LOG_INFO, "Closed connection from %s", inet_ntoa(client_addr));
  }
  safe_fclose(write_file);
  close(socket_fd);
  closelog();
  return 0;
}
