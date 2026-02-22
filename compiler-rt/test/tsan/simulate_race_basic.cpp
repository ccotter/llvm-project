// RUN: %clangxx_tsan -O1 %s -o %t
// RUN: %env_tsan_opts=atexit_sleep_ms=0:abort_on_error=0:simulate_scheduler=random:simulate_iterations=50 not %run %t 2>&1 | FileCheck %s
//
// Test data race detection during simulation.
// Two threads increment a shared variable without synchronization.

#include <pthread.h>
#include <stdio.h>

extern "C" int __tsan_simulate(void (*callback)(void*), void* arg);

int shared_var = 0;  // Deliberately unprotected to cause race

void* thread_func(void* arg) {
  for (int i = 0; i < 10; i++) {
    shared_var++;  // RACE: no synchronization
  }
  return nullptr;
}

void test_callback(void* arg) {
  shared_var = 0;

  pthread_t t1, t2;
  pthread_create(&t1, nullptr, thread_func, nullptr);
  pthread_create(&t2, nullptr, thread_func, nullptr);

  pthread_join(t1, nullptr);
  pthread_join(t2, nullptr);

  // The value might be wrong due to race, but that's not the point
  // We're testing that TSAN detects the race
}

int main() {
  fprintf(stderr, "Starting race detection test...\n");
  int result = __tsan_simulate(test_callback, nullptr);

  fprintf(stderr, "Simulation returned: %d\n", result);
  fprintf(stderr, "Test PASSED: race was detected by TSAN\n");
  return 0;
}

// CHECK: Starting race detection test
// CHECK: WARNING: ThreadSanitizer: data race
// CHECK: ThreadSanitizer: data race detected at iteration
// CHECK: ThreadSanitizer: simulation stopped due to race detection
// CHECK: Simulation returned: 4
// CHECK: Test PASSED
