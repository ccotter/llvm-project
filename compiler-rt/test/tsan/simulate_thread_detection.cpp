// RUN: %clangxx_tsan -O1 %s -o %t
// RUN: %env_tsan_opts=simulate_scheduler=random:simulate_iterations=2 %run %t 2>&1 | FileCheck %s

#include "test.h"
#include <atomic>

// TODO - why doesn't 'sanitizer/tsan_interface.h' see the decl?
#ifdef __cplusplus
extern "C" {
#endif
void __tsan_simulate(void (*callback)(void *arg), void *arg);
#ifdef __cplusplus
}
#endif

std::atomic<bool> keep_running(true);

void *background_thread(void *arg) {
  while (keep_running.load(std::memory_order_relaxed)) {
    usleep(10000); // 10ms
  }
  return nullptr;
}

void test_callback(void *arg) { fprintf(stderr, "test_callback executed\n"); }

int main() {
  // Test 1: Simulate with pre-existing thread (should fail)
  fprintf(stderr, "=== Test with pre-existing thread ===\n");
  pthread_t bg;
  pthread_create(&bg, nullptr, background_thread, nullptr);

  __tsan_simulate(test_callback, nullptr);

  // Clean up background thread
  keep_running.store(false, std::memory_order_relaxed);
  pthread_join(bg, nullptr);

  // Test 2: Simulate after thread joined (should succeed)
  fprintf(stderr, "\n=== Test after thread joined ===\n");
  __tsan_simulate(test_callback, nullptr);

  fprintf(stderr, "DONE\n");
  return 0;
}

// CHECK: === Test with pre-existing thread ===
// CHECK: ThreadSanitizer: simulation cannot start - other threads are running
// CHECK: Simulation requires that only the calling thread exists
// CHECK-NOT: test_callback executed
// CHECK: === Test after thread joined ===
// CHECK: ThreadSanitizer: simulation starting (iterations 0..1
// CHECK: test_callback executed
// CHECK: ThreadSanitizer: simulation exiting - no threads were spawned
// CHECK: DONE
