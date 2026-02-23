// RUN: %clangxx_tsan -O1 %s -o %t
// RUN: %env_tsan_opts=atexit_sleep_ms=0:abort_on_error=0:simulate_scheduler=random:simulate_iterations=10 %run %t 2>&1 | FileCheck %s
//
// Test that threads that exit immediately (do zero work) are handled properly.
// Verifies cleanup and tracking of short-lived threads.

#include <pthread.h>
#include <stdio.h>

extern "C" int __tsan_simulate(void (*callback)(void *), void *arg);

void *thread_func(void *arg) {
  // Thread does nothing and exits immediately
  return nullptr;
}

void test_callback(void *arg) {
  pthread_t threads[5];

  // Create threads that do nothing
  for (int i = 0; i < 5; i++) {
    pthread_create(&threads[i], nullptr, thread_func, nullptr);
  }

  // Join all threads
  for (int i = 0; i < 5; i++) {
    pthread_join(threads[i], nullptr);
  }

  fprintf(stderr, "All immediate-exit threads joined successfully\n");
}

int main() {
  int result = __tsan_simulate(test_callback, nullptr);

  if (result == 0) {
    fprintf(stderr, "Test PASSED: immediate-exit threads handled\n");
    return 0;
  } else {
    fprintf(stderr, "Test FAILED: unexpected return value %d\n", result);
    return 1;
  }
}

// CHECK: ThreadSanitizer: simulation starting
// CHECK: All immediate-exit threads joined successfully
// CHECK: ThreadSanitizer: simulation finished
// CHECK: Test PASSED: immediate-exit threads handled
