// RUN: %clangxx_tsan %s -o %t
// RUN: %env_tsan_opts=simulate_scheduler=random:simulate_iterations=10:simulate_start_iteration=-1 not %run %t 2>&1 | FileCheck %s --check-prefix=CHECK-NEG1
// RUN: %env_tsan_opts=simulate_scheduler=random:simulate_iterations=10:simulate_start_iteration=-5 not %run %t 2>&1 | FileCheck %s --check-prefix=CHECK-NEG5

// Test that invalid simulate_start_iteration values are properly rejected

#include <pthread.h>
#include <stdio.h>

extern "C" int __tsan_simulate(void (*callback)(void *arg), void *arg);

void test_callback(void *arg) { fprintf(stderr, "Callback should not run\n"); }

int main() {
  int result = __tsan_simulate(test_callback, nullptr);

  fprintf(stderr, "__tsan_simulate returned: %d\n", result);

  if (result == -1) {
    fprintf(stderr, "Test PASSED: invalid start_iteration rejected\n");
    return 1;
  } else {
    fprintf(stderr, "Test FAILED: invalid start_iteration not detected\n");
    return 0;
  }
}

// CHECK-NEG1: ThreadSanitizer: simulate_start_iteration must be >= 0 (got -1)
// CHECK-NEG1: __tsan_simulate returned: -1
// CHECK-NEG1: Test PASSED: invalid start_iteration rejected

// CHECK-NEG5: ThreadSanitizer: simulate_start_iteration must be >= 0 (got -5)
// CHECK-NEG5: __tsan_simulate returned: -1
// CHECK-NEG5: Test PASSED: invalid start_iteration rejected
