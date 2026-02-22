// RUN: %clangxx_tsan -O1 %s -o %t
// RUN: %env_tsan_opts=atexit_sleep_ms=0:abort_on_error=0:simulate_scheduler=random:simulate_iterations=10:simulate_max_depth=1000 %run %t 2>&1 | FileCheck %s
//
// Test that simulation completes successfully when within depth limit.
// Opposite of simulate_max_depth_hit.cpp - verifies no false positives.

#include <pthread.h>
#include <stdio.h>
#include <atomic>

extern "C" int __tsan_simulate(void (*callback)(void*), void* arg);

std::atomic<int> counter(0);

void* thread_func(void* arg) {
  // Do moderate number of atomic operations (well under depth limit)
  for (int i = 0; i < 50; i++) {
    counter.fetch_add(1, std::memory_order_relaxed);
  }
  return nullptr;
}

void test_callback(void* arg) {
  counter.store(0, std::memory_order_relaxed);

  pthread_t t1, t2;
  pthread_create(&t1, nullptr, thread_func, nullptr);
  pthread_create(&t2, nullptr, thread_func, nullptr);

  pthread_join(t1, nullptr);
  pthread_join(t2, nullptr);

  int final_count = counter.load(std::memory_order_relaxed);
  fprintf(stderr, "Counter: %d\n", final_count);
}

int main() {
  int result = __tsan_simulate(test_callback, nullptr);

  fprintf(stderr, "__tsan_simulate returned: %d\n", result);

  // Should return 0 (success - within depth limit)
  if (result == 0) {
    fprintf(stderr, "Test PASSED: completed within depth limit\n");
    return 0;
  } else {
    fprintf(stderr, "Test FAILED: expected return value 0, got %d\n", result);
    return 1;
  }
}

// CHECK: ThreadSanitizer: simulation starting (iterations 0..9, max_depth=1000
// CHECK: Counter: 100
// CHECK: ThreadSanitizer: simulation finished
// CHECK: __tsan_simulate returned: 0
// CHECK: Test PASSED: completed within depth limit
