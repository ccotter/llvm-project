// RUN: %clangxx_tsan -O1 %s -o %t
// RUN: %env_tsan_opts=atexit_sleep_ms=0:abort_on_error=0:simulate_scheduler=random:simulate_iterations=10 %run %t 2>&1 | FileCheck %s
//
// Test threads creating other threads (nested thread creation).
// Verifies that thread tracking handles hierarchical thread creation.

#include <pthread.h>
#include <stdio.h>

extern "C" int __tsan_simulate(void (*callback)(void*), void* arg);

pthread_mutex_t mutex;
int counter = 0;

void* level3_func(void* arg) {
  pthread_mutex_lock(&mutex);
  counter++;
  pthread_mutex_unlock(&mutex);
  return nullptr;
}

void* level2_func(void* arg) {
  pthread_mutex_lock(&mutex);
  counter++;
  pthread_mutex_unlock(&mutex);

  // Level 2 creates Level 3
  pthread_t t;
  pthread_create(&t, nullptr, level3_func, nullptr);
  pthread_join(t, nullptr);

  return nullptr;
}

void* level1_func(void* arg) {
  pthread_mutex_lock(&mutex);
  counter++;
  pthread_mutex_unlock(&mutex);

  // Level 1 creates Level 2
  pthread_t t;
  pthread_create(&t, nullptr, level2_func, nullptr);
  pthread_join(t, nullptr);

  return nullptr;
}

void test_callback(void* arg) {
  counter = 0;
  pthread_mutex_init(&mutex, nullptr);

  // Main creates Level 1
  pthread_t t;
  pthread_create(&t, nullptr, level1_func, nullptr);
  pthread_join(t, nullptr);

  pthread_mutex_destroy(&mutex);

  // Should have 3 threads total: level1 + level2 + level3
  if (counter != 3) {
    fprintf(stderr, "ERROR: Expected counter=3, got %d\n", counter);
  } else {
    fprintf(stderr, "Nested threads verified: %d\n", counter);
  }
}

int main() {
  int result = __tsan_simulate(test_callback, nullptr);

  if (result == 0) {
    fprintf(stderr, "Test PASSED: nested thread creation works\n");
    return 0;
  } else {
    fprintf(stderr, "Test FAILED: unexpected return value %d\n", result);
    return 1;
  }
}

// CHECK: ThreadSanitizer: simulation starting
// CHECK: Nested threads verified: 3
// CHECK: ThreadSanitizer: simulation finished
// CHECK: Test PASSED: nested thread creation works
