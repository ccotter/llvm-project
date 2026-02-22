// RUN: %clangxx_tsan %s -o %t
// RUN: %env_tsan_opts=atexit_sleep_ms=0:abort_on_error=0:simulate_scheduler=random:simulate_iterations=2 %run %t 2>&1 | FileCheck %s

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

extern "C" int __tsan_simulate(void (*callback)(void *arg), void *arg);

// TODO - update the test to bail when the pthread timed APIs are called, not rw mutexes

pthread_rwlock_t rwlock;

void *thread_func(void *arg) {
  // This should trigger the unsupported interceptor error
  pthread_rwlock_rdlock(&rwlock);
  pthread_rwlock_unlock(&rwlock);
  return nullptr;
}

void test_callback(void *arg) {
  pthread_rwlock_init(&rwlock, nullptr);

  pthread_t t;
  pthread_create(&t, nullptr, thread_func, nullptr);
  pthread_join(t, nullptr);

  pthread_rwlock_destroy(&rwlock);
}

int main() {
  int result = __tsan_simulate(test_callback, nullptr);

  printf("__tsan_simulate returned: %d\n", result);

  // Should return 2 (unsupported interceptor error)
  if (result == 2) {
    printf(
        "Test PASSED: simulation correctly detected unsupported interceptor\n");
  } else {
    printf("Test FAILED: expected return value 2, got %d\n", result);
    return 1;
  }

  return 0;
}

// CHECK: ThreadSanitizer: simulation error - unsupported interceptor called: pthread_rwlock_rdlock
// CHECK: Simulation does not support this synchronization primitive
// CHECK: ThreadSanitizer: simulation aborted after 1 iterations
// CHECK: __tsan_simulate returned: 2
// CHECK: Test PASSED: simulation correctly detected unsupported interceptor
