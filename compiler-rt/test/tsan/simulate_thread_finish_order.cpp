// RUN: %clangxx_tsan -O1 %s -o %t
// RUN: %env_tsan_opts=atexit_sleep_ms=0:abort_on_error=0:simulate_scheduler=random:simulate_iterations=10 %run %t 2>&1 | FileCheck %s
//
// Test that pthread_join properly blocks and wakes up when thread finishes.
// Verifies join tracking and wake-up mechanism.

#include <pthread.h>
#include <stdio.h>

extern "C" int __tsan_simulate(void (*callback)(void *), void *arg);

pthread_mutex_t mutex;
int thread_started = 0;
int thread_finished = 0;

void *worker_thread(void *arg) {
  pthread_mutex_lock(&mutex);
  thread_started = 1;
  pthread_mutex_unlock(&mutex);

  // Do some work
  for (volatile int i = 0; i < 100; i++) {
  }

  pthread_mutex_lock(&mutex);
  thread_finished = 1;
  pthread_mutex_unlock(&mutex);

  return nullptr;
}

void test_callback(void *arg) {
  thread_started = 0;
  thread_finished = 0;
  pthread_mutex_init(&mutex, nullptr);

  pthread_t t;
  pthread_create(&t, nullptr, worker_thread, nullptr);

  // Join will block until worker finishes
  pthread_join(t, nullptr);

  pthread_mutex_destroy(&mutex);

  // After join, both flags should be set
  if (thread_started && thread_finished) {
    fprintf(stderr, "Thread lifecycle verified\n");
  } else {
    fprintf(stderr, "ERROR: thread_started=%d, thread_finished=%d\n",
            thread_started, thread_finished);
  }
}

int main() {
  int result = __tsan_simulate(test_callback, nullptr);

  if (result == 0) {
    fprintf(stderr, "Test PASSED: join wake-up works correctly\n");
    return 0;
  } else {
    fprintf(stderr, "Test FAILED: unexpected return value %d\n", result);
    return 1;
  }
}

// CHECK: ThreadSanitizer: simulation starting
// CHECK: Thread lifecycle verified
// CHECK: ThreadSanitizer: simulation finished
// CHECK: Test PASSED: join wake-up works correctly
