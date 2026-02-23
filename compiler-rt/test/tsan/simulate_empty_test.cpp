// RUN: %clangxx_tsan -O1 %s -o %t
// RUN: %env_tsan_opts=atexit_sleep_ms=0:simulate_scheduler=random %run %t 2>&1 | FileCheck %s

#include <pthread.h>
#include <stdio.h>

extern "C" int __tsan_simulate(void (*callback)(void *), void *arg);

void test_callback(void *arg) {
  fprintf(stderr, "Callback executed with no threads\n");
}

int main() {
  int result = __tsan_simulate(test_callback, nullptr);
  if (result == 0) {
    fprintf(stderr, "Test PASSED: empty test handled correctly\n");
    return 0;
  } else {
    fprintf(stderr, "Test FAILED: expected return value 0, got %d\n", result);
    return 1;
  }
}

// CHECK: ThreadSanitizer: simulation starting (iterations 0..1
// CHECK: Callback executed with no threads
// CHECK: ThreadSanitizer: simulation exiting - no threads were spawned
// CHECK: __tsan_simulate returned: 0
// CHECK: Test PASSED: empty test handled correctly
