// RUN: %clangxx_tsan %s -o %t
// RUN: %env_tsan_opts=simulate_scheduler=random:simulate_iterations=0 not %run %t 2>&1 | FileCheck %s --check-prefix=CHECK-ZERO
// RUN: %env_tsan_opts=simulate_scheduler=random:simulate_iterations=-1 not %run %t 2>&1 | FileCheck %s --check-prefix=CHECK-NEGATIVE

// Test that invalid simulate_iterations values are properly rejected

#include <pthread.h>
#include <stdio.h>

extern "C" int __tsan_simulate(void (*callback)(void *arg), void *arg);

void test_callback(void *arg) { fprintf(stderr, "Callback should not run\n"); }

int main() {
  int result = __tsan_simulate(test_callback, nullptr);

  fprintf(stderr, "__tsan_simulate returned: %d\n", result);

  if (result == -1) {
    fprintf(stderr, "Test PASSED: invalid iterations rejected\n");
    return 1;
  } else {
    fprintf(stderr, "Test FAILED: invalid iterations not detected\n");
    return 0;
  }
}

// CHECK-ZERO: ThreadSanitizer: simulate_iterations must be > 0 (got 0)
// CHECK-ZERO: __tsan_simulate returned: -1
// CHECK-ZERO: Test PASSED: invalid iterations rejected

// CHECK-NEGATIVE: ThreadSanitizer: simulate_iterations must be > 0 (got -1)
// CHECK-NEGATIVE: __tsan_simulate returned: -1
// CHECK-NEGATIVE: Test PASSED: invalid iterations rejected
