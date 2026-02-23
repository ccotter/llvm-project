// RUN: %clangxx_tsan -O1 %s -o %t
// RUN: %env_tsan_opts=atexit_sleep_ms=0:abort_on_error=0:simulate_scheduler=random:simulate_iterations=10 %run %t 2>&1 | FileCheck %s

// Test simple thread join scenario - should work correctly.
// Scenario: Main creates A and B, then A joins B - normal operation, no deadlock

#include <pthread.h>
#include <stdio.h>

extern "C" int __tsan_simulate(void (*callback)(void*), void* arg);

pthread_t thread_a, thread_b;
int ready_count = 0;

void* thread_b_func(void* arg) {
  // Thread B just increments counter and exits
  __atomic_fetch_add(&ready_count, 1, __ATOMIC_SEQ_CST);
  return nullptr;
}

void* thread_a_func(void* arg) {
  // Thread A waits for B to be ready, then joins it
  while (__atomic_load_n(&ready_count, __ATOMIC_SEQ_CST) < 1) {
    // Wait for B to signal ready
  }
  pthread_join(thread_b, nullptr);
  return nullptr;
}

void test_callback(void* arg) {
  // Reset counter
  __atomic_store_n(&ready_count, 0, __ATOMIC_SEQ_CST);

  // Create threads in sequence - B first, then A
  pthread_create(&thread_b, nullptr, thread_b_func, nullptr);
  pthread_create(&thread_a, nullptr, thread_a_func, nullptr);

  // Wait for A to complete (which waits for B)
  pthread_join(thread_a, nullptr);
}

int main() {
  int result = __tsan_simulate(test_callback, nullptr);

  fprintf(stderr, "__tsan_simulate returned: %d\n", result);

  // Should return 0 (success - no deadlock, no race)
  if (result == 0) {
    fprintf(stderr, "Test PASSED: simple join completed successfully\n");
    return 0;
  } else {
    fprintf(stderr, "Test FAILED: expected return value 0, got %d\n", result);
    return 1;
  }
}

// CHECK: ThreadSanitizer: simulation starting
// CHECK: __tsan_simulate returned: 0
// CHECK: Test PASSED: simple join completed successfully
