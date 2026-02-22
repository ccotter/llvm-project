// RUN: %clangxx_tsan %s -o %t
// RUN: %env_tsan_opts=atexit_sleep_ms=0:abort_on_error=0:simulate_scheduler=random:simulate_iterations=2 %run %t 2>&1 | FileCheck %s

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

extern "C" int __tsan_simulate(void (*callback)(void *arg), void *arg);

pthread_spinlock_t spinlock;

void *thread_func(void *arg) {
  pthread_spin_lock(&spinlock);
  pthread_spin_unlock(&spinlock);
  return nullptr;
}

void test_callback(void *arg) {
  pthread_spin_init(&spinlock, PTHREAD_PROCESS_PRIVATE);

  pthread_t t;
  pthread_create(&t, nullptr, thread_func, nullptr);
  pthread_join(t, nullptr);

  pthread_spin_destroy(&spinlock);
}

int main() {
  int result = __tsan_simulate(test_callback, nullptr);

  printf("__tsan_simulate returned: %d\n", result);

  // Should return 2 (unsupported interceptor error)
  if (result == 2) {
    printf("Test PASSED: simulation correctly detected unsupported spinlock\n");
  } else {
    printf("Test FAILED: expected return value 2, got %d\n", result);
    return 1;
  }

  return 0;
}

// CHECK: ThreadSanitizer: simulation error - unsupported interceptor called: pthread_spin_lock
// CHECK: Simulation does not support this synchronization primitive
// CHECK: ThreadSanitizer: unsupported interceptor at iteration 0
// CHECK: __tsan_simulate returned: 2
// CHECK: Test PASSED: simulation correctly detected unsupported spinlock
