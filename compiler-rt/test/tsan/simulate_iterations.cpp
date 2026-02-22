// RUN: %clangxx_tsan -O1 %s -o %t
// RUN: %env_tsan_opts=atexit_sleep_ms=0:abort_on_error=0:simulate_scheduler=random:simulate_iterations=1 %run %t 2>&1 | FileCheck %s --check-prefix=CHECK-1
// RUN: %env_tsan_opts=atexit_sleep_ms=0:abort_on_error=0:simulate_scheduler=random:simulate_iterations=10 %run %t 2>&1 | FileCheck %s --check-prefix=CHECK-10
// RUN: %env_tsan_opts=atexit_sleep_ms=0:abort_on_error=0:simulate_scheduler=random:simulate_iterations=100 %run %t 2>&1 | FileCheck %s --check-prefix=CHECK-100
//
// Test that the simulate_iterations parameter is respected.
// Verifies that the simulation runs the correct number of iterations.

#include <pthread.h>
#include <stdio.h>

extern "C" int __tsan_simulate(void (*callback)(void*), void* arg);

pthread_mutex_t mutex;
int counter = 0;

void* thread_func(void* arg) {
  pthread_mutex_lock(&mutex);
  counter++;
  pthread_mutex_unlock(&mutex);
  return nullptr;
}

void test_callback(void* arg) {
  counter = 0;
  pthread_mutex_init(&mutex, nullptr);

  pthread_t t;
  pthread_create(&t, nullptr, thread_func, nullptr);
  pthread_join(t, nullptr);

  pthread_mutex_destroy(&mutex);
}

int main() {
  int result = __tsan_simulate(test_callback, nullptr);

  if (result == 0) {
    fprintf(stderr, "Test PASSED\n");
    return 0;
  } else {
    fprintf(stderr, "Test FAILED: unexpected return value %d\n", result);
    return 1;
  }
}

// CHECK-1: ThreadSanitizer: simulation starting (iterations 0..0
// CHECK-1: Test PASSED

// CHECK-10: ThreadSanitizer: simulation starting (iterations 0..9
// CHECK-10: Test PASSED

// CHECK-100: ThreadSanitizer: simulation starting (iterations 0..99
// CHECK-100: Test PASSED
