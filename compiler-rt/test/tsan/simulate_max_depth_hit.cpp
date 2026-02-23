// RUN: %clangxx_tsan -O1 %s -o %t
// RUN: %env_tsan_opts=atexit_sleep_ms=0:abort_on_error=0:simulate_scheduler=random:simulate_iterations=10:simulate_max_depth=100 %run %t 2>&1 | FileCheck %s
//
// Test that max depth limit is enforced.
// We create a loop with many scheduling points that should exceed the depth limit.

#include <pthread.h>
#include <stdio.h>
#include <atomic>

extern "C" int __tsan_simulate(void (*callback)(void*), void* arg);

std::atomic<int> counter(0);

void* thread_func(void* arg) {
  // Create many scheduling points by doing atomic operations
  // Each atomic operation is a potential schedule point
  for (int i = 0; i < 200; i++) {
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

  fprintf(stderr, "Iteration completed with counter=%d\n", counter.load(std::memory_order_relaxed));
}

int main() {
  fprintf(stderr, "Starting max depth test (limit=100)...\n");
  int result = __tsan_simulate(test_callback, nullptr);

  fprintf(stderr, "Simulation returned: %d\n", result);

  if (result == -1) {
    fprintf(stderr, "Test PASSED: max depth correctly detected (exit code -1)\n");
    return 0;
  } else {
    fprintf(stderr, "Test FAILED: expected exit code -1, got %d\n", result);
    return 1;
  }
}

// CHECK: Starting max depth test
// CHECK: ThreadSanitizer: simulation stopped due to max depth
// CHECK: Simulation returned: -1
// CHECK: Test PASSED: max depth correctly detected (exit code -1)
