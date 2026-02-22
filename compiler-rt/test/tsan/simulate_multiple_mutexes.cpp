// RUN: %clangxx_tsan -O1 %s -o %t
// RUN: %env_tsan_opts=atexit_sleep_ms=0:abort_on_error=0:simulate_scheduler=random:simulate_iterations=10 %run %t 2>&1 | FileCheck %s
//
// Test that simulation handles many different mutexes used by different thread pairs.
// Verifies that waitset allocation and tracking works with multiple mutexes.

#include <pthread.h>
#include <stdio.h>

extern "C" int __tsan_simulate(void (*callback)(void*), void* arg);

const int num_mutexes = 10;
pthread_mutex_t mutexes[num_mutexes];
int counters[num_mutexes];

void* thread_func(void* arg) {
  int mutex_id = (long)arg;

  pthread_mutex_lock(&mutexes[mutex_id]);
  counters[mutex_id]++;
  pthread_mutex_unlock(&mutexes[mutex_id]);

  return nullptr;
}

void test_callback(void* arg) {
  // Initialize mutexes and counters
  for (int i = 0; i < num_mutexes; i++) {
    pthread_mutex_init(&mutexes[i], nullptr);
    counters[i] = 0;
  }

  // Create thread pairs for each mutex
  const int threads_per_mutex = 2;
  pthread_t threads[num_mutexes * threads_per_mutex];

  for (int i = 0; i < num_mutexes; i++) {
    for (int j = 0; j < threads_per_mutex; j++) {
      pthread_create(&threads[i * threads_per_mutex + j], nullptr,
                     thread_func, (void*)(long)i);
    }
  }

  // Join all threads
  for (int i = 0; i < num_mutexes * threads_per_mutex; i++) {
    pthread_join(threads[i], nullptr);
  }

  // Verify all counters
  int errors = 0;
  for (int i = 0; i < num_mutexes; i++) {
    if (counters[i] != threads_per_mutex) {
      fprintf(stderr, "ERROR: mutex %d counter=%d, expected %d\n",
              i, counters[i], threads_per_mutex);
      errors++;
    }
    pthread_mutex_destroy(&mutexes[i]);
  }

  if (errors == 0) {
    fprintf(stderr, "All %d mutexes verified successfully\n", num_mutexes);
  }
}

int main() {
  int result = __tsan_simulate(test_callback, nullptr);

  if (result == 0) {
    fprintf(stderr, "Test PASSED: multiple mutexes handled correctly\n");
    return 0;
  } else {
    fprintf(stderr, "Test FAILED: unexpected return value %d\n", result);
    return 1;
  }
}

// CHECK: ThreadSanitizer: simulation starting
// CHECK: All 10 mutexes verified successfully
// CHECK: ThreadSanitizer: simulation finished
// CHECK: Test PASSED: multiple mutexes handled correctly
