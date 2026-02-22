// RUN: %clangxx_tsan -O1 %s -o %t
// RUN: %env_tsan_opts=atexit_sleep_ms=0:abort_on_error=0:simulate_scheduler=random:simulate_iterations=10 not %run %t 2>&1 | FileCheck %s
// XFAIL: *
// TODO: Circular pthread_join dependencies cause CHECK failure in sanitizer_thread_registry.cpp:364
// Thread trying to join another thread whose user_id hasn't been registered yet.
// This reveals a limitation/bug in the simulation framework's thread join handling.

// Test join-based deadlock detection.
// Scenario: Thread A waits on B, B waits on C, C waits on A - circular join dependency

#include <pthread.h>
#include <stdio.h>
#include <assert.h>

extern "C" int __tsan_simulate(void (*callback)(void*), void* arg);

pthread_t thread_a, thread_b, thread_c;
int ready_count = 0;

void* thread_a_func(void* arg) {
  __atomic_fetch_add(&ready_count, 1, __ATOMIC_SEQ_CST);
  while (__atomic_load_n(&ready_count, __ATOMIC_SEQ_CST) < 3)
    ; // spin
  pthread_join(thread_b, nullptr);
  return nullptr;
}

void* thread_b_func(void* arg) {
  __atomic_fetch_add(&ready_count, 1, __ATOMIC_SEQ_CST);
  while (__atomic_load_n(&ready_count, __ATOMIC_SEQ_CST) < 3)
    ; // spin
  pthread_join(thread_c, nullptr);
  return nullptr;
}

void* thread_c_func(void* arg) {
  __atomic_fetch_add(&ready_count, 1, __ATOMIC_SEQ_CST);
  while (__atomic_load_n(&ready_count, __ATOMIC_SEQ_CST) < 3)
    ;
  pthread_join(thread_a, nullptr);
  return nullptr;
}

void test_callback(void* arg) {
  // Reset counter
  __atomic_store_n(&ready_count, 0, __ATOMIC_SEQ_CST);

  // Create threads in sequence
  pthread_create(&thread_a, nullptr, thread_a_func, nullptr);
  pthread_create(&thread_b, nullptr, thread_b_func, nullptr);
  pthread_create(&thread_c, nullptr, thread_c_func, nullptr);

  // All three threads will be blocked on join, creating a deadlock
  // A joins B, B joins C, C joins A - impossible to resolve
}

int main() {
  int result = __tsan_simulate(test_callback, nullptr);

  fprintf(stderr, "__tsan_simulate returned: %d\n", result);

  // Should return 5 (deadlock detected)
  if (result == 5) {
    fprintf(stderr, "Test PASSED: join-cycle deadlock correctly detected\n");
    return 0;
  } else {
    fprintf(stderr, "Test FAILED: expected return value 5, got %d\n", result);
    return 1;
  }
}

// CHECK: ThreadSanitizer: simulation starting
// CHECK: WARNING: ThreadSanitizer: lock-order-inversion
// CHECK: __tsan_simulate returned: 5
// CHECK: Test PASSED: join-cycle deadlock correctly detected
