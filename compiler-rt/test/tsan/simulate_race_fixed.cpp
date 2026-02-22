// RUN: %clangxx_tsan -O1 %s -o %t
// RUN: %env_tsan_opts=atexit_sleep_ms=0:abort_on_error=0:simulate_scheduler=random:simulate_iterations=50 %run %t 2>&1 | FileCheck %s
//
// Test that proper synchronization prevents race detection.
// Same as simulate_race_basic.cpp but with mutex protection.

#include <pthread.h>
#include <stdio.h>

extern "C" int __tsan_simulate(void (*callback)(void*), void* arg);

int shared_var = 0;
pthread_mutex_t mutex;

void* thread_func(void* arg) {
  for (int i = 0; i < 10; i++) {
    pthread_mutex_lock(&mutex);
    shared_var++;  // NO RACE: protected by mutex
    pthread_mutex_unlock(&mutex);
  }
  return nullptr;
}

void test_callback(void* arg) {
  shared_var = 0;
  pthread_mutex_init(&mutex, nullptr);

  pthread_t t1, t2;
  pthread_create(&t1, nullptr, thread_func, nullptr);
  pthread_create(&t2, nullptr, thread_func, nullptr);

  pthread_join(t1, nullptr);
  pthread_join(t2, nullptr);

  pthread_mutex_destroy(&mutex);

  // With proper synchronization, value should be correct
  if (shared_var != 20) {
    fprintf(stderr, "ERROR: Expected shared_var=20, got %d\n", shared_var);
  }
}

int main() {
  fprintf(stderr, "Starting synchronized test...\n");
  int result = __tsan_simulate(test_callback, nullptr);

  fprintf(stderr, "Simulation returned: %d\n", result);

  if (result == 0) {
    fprintf(stderr, "Test PASSED: no race with proper synchronization\n");
    return 0;
  } else {
    fprintf(stderr, "Test FAILED: expected return value 0, got %d\n", result);
    return 1;
  }
}

// CHECK: Starting synchronized test
// CHECK: ThreadSanitizer: simulation starting
// CHECK-NOT: WARNING: ThreadSanitizer: data race
// CHECK: ThreadSanitizer: simulation finished
// CHECK: Simulation returned: 0
// CHECK: Test PASSED: no race with proper synchronization
