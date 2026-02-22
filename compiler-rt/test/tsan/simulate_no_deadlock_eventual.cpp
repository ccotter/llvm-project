// RUN: %clangxx_tsan -O1 %s -o %t
// RUN: %env_tsan_opts=atexit_sleep_ms=0:abort_on_error=0:simulate_scheduler=random:simulate_iterations=10 %run %t 2>&1 | FileCheck %s

// Test that simulation doesn't falsely detect deadlock when threads eventually make progress.
// Scenario: Threads temporarily block on mutex but eventually acquire and complete

#include <pthread.h>
#include <stdio.h>
#include <assert.h>

extern "C" int __tsan_simulate(void (*callback)(void*), void* arg);

pthread_mutex_t mutex;
int counter = 0;

void* thread_func(void* arg) {
  // Each thread acquires the mutex, increments counter, and releases
  pthread_mutex_lock(&mutex);
  counter++;
  pthread_mutex_unlock(&mutex);
  return nullptr;
}

void test_callback(void* arg) {
  pthread_mutex_init(&mutex, nullptr);
  
  const int num_threads = 4;
  pthread_t threads[num_threads];
  
  for (int i = 0; i < num_threads; i++) {
    pthread_create(&threads[i], nullptr, thread_func, nullptr);
  }
  
  for (int i = 0; i < num_threads; i++) {
    pthread_join(threads[i], nullptr);
  }
  
  pthread_mutex_destroy(&mutex);
  
  fprintf(stderr, "Counter value: %d (expected %d)\n", counter, num_threads);
  // Note: counter might not reach num_threads in all schedules
  // The important thing is that simulation doesn't hang or detect false deadlock
}

int main() {
  int result = __tsan_simulate(test_callback, nullptr);
  
  fprintf(stderr, "__tsan_simulate returned: %d\n", result);
  
  // Should return 0 (success - no deadlock)
  if (result == 0) {
    fprintf(stderr, "Test PASSED: no false deadlock reported\n");
    return 0;
  } else {
    fprintf(stderr, "Test FAILED: expected return value 0, got %d\n", result);
    return 1;
  }
}

// CHECK: ThreadSanitizer: simulation starting
// CHECK: ThreadSanitizer: simulation finished
// CHECK: __tsan_simulate returned: 0
// CHECK: Test PASSED: no false deadlock reported
