#include "threading.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

// Optional: use these functions to add debug or error prints to your
// application
#define DEBUG_LOG(msg, ...)
// #define DEBUG_LOG(msg,...) printf("threading: " msg "\n" , ##__VA_ARGS__)
#define ERROR_LOG(msg, ...) printf("threading ERROR: " msg "\n", ##__VA_ARGS__)

static void millisleep(const int ms) {
  struct timespec ts = {.tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L};
  nanosleep(&ts, NULL);
}

void* threadfunc(void* thread_param) {
  // Wait, obtain mutex, wait, release mutex as described by thread_data
  // structure hint: use a cast like the one below to obtain thread arguments
  // from your parameter
  // struct thread_data* thread_func_args = (struct thread_data *) thread_param;
  struct thread_data* thread_func_args = thread_param;

  millisleep(thread_func_args->wait_to_obtain_ms);
  pthread_mutex_lock(thread_func_args->mutex);
  millisleep(thread_func_args->wait_to_release_ms);
  pthread_mutex_unlock(thread_func_args->mutex);

  thread_func_args->thread_complete_success = true;
  return thread_param;
}

bool start_thread_obtaining_mutex(
    pthread_t* thread, pthread_mutex_t* mutex,
    int wait_to_obtain_ms,  // NOLINT(bugprone-easily-swappable-parameters)
    int wait_to_release_ms) {
  /**
   * Allocate memory for thread_data, setup mutex and wait arguments, pass
   * thread_data to created thread using threadfunc() as entry point.
   *
   * return true if successful.
   *
   * See implementation details in threading.h file comment block
   */
  struct thread_data* data = malloc(sizeof(struct thread_data));
  if (!data) {
    perror("malloc");
    return false;
  }
  data->mutex = mutex;
  data->wait_to_obtain_ms = wait_to_obtain_ms;
  data->wait_to_release_ms = wait_to_release_ms;
  int result = pthread_create(thread, NULL, threadfunc, data);
  if (result) {
    free(data);
    return false;
  }
  return true;
}
