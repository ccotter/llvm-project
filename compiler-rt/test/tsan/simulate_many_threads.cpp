// RUN: %clangxx_tsan -O1 %s -o %t
// RUN: %env_tsan_opts=atexit_sleep_ms=0:abort_on_error=0:simulate_scheduler=random:simulate_iterations=10 %run %t 2>&1 | FileCheck %s
//
// Test that simulation handles many threads being created and destroyed.
// Verifies thread tracking and cleanup with high thread counts.

#include <pthread.h>
#include <stdio.h>

extern "C" int __tsan_simulate(void (*callback)(void *), void *arg);

pthread_mutex_t mutex;
int counter = 0;

void *thread_func(void *arg) {
  pthread_mutex_lock(&mutex);
  counter++;
  pthread_mutex_unlock(&mutex);
  return nullptr;
}

void test_callback(void *arg) {
  counter = 0;
  pthread_mutex_init(&mutex, nullptr);

  const int num_threads = 12;
  pthread_t threads[num_threads];

  // Create and join many threads
  for (int i = 0; i < num_threads; i++) {
    pthread_create(&threads[i], nullptr, thread_func, nullptr);
  }

  for (int i = 0; i < num_threads; i++) {
    pthread_join(threads[i], nullptr);
  }

  pthread_mutex_destroy(&mutex);

  // Verify all threads executed
  if (counter != num_threads) {
    fprintf(stderr, "ERROR: Expected counter=%d, got %d\n", num_threads,
            counter);
  }
}

int main() {
  int result = __tsan_simulate(test_callback, nullptr);

  if (result == 0) {
    fprintf(stderr, "Test PASSED: handled %d threads successfully\n", 12);
    return 0;
  } else {
    fprintf(stderr, "Test FAILED: unexpected return value %d\n", result);
    return 1;
  }
}

// CHECK: ThreadSanitizer: simulation starting
// CHECK: ThreadSanitizer: simulation finished
// CHECK: Test PASSED: handled 12 threads successfully
