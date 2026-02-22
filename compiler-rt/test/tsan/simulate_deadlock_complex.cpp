// RUN: %clangxx_tsan -O1 %s -o %t
// RUN: %env_tsan_opts=atexit_sleep_ms=0:abort_on_error=0:simulate_scheduler=random:simulate_iterations=100 not %run %t 2>&1 | FileCheck %s
//
// Test complex lock-order-inversion detection: 4 threads, 4 mutexes, circular chain.
// Thread i locks mutex[i] then mutex[(i+1) % 4]
// This creates a circular dependency that TSAN should detect.

#include <pthread.h>
#include <stdio.h>

extern "C" int __tsan_simulate(void (*callback)(void*), void* arg);

const int kThreads = 4;
pthread_mutex_t mutexes[kThreads];

struct thread_arg {
  int index;
};

void* thread_func(void* arg) {
  int idx = ((thread_arg*)arg)->index;
  int next_idx = (idx + 1) % kThreads;

  pthread_mutex_lock(&mutexes[idx]);
  // Busy-wait to increase interleaving
  for (volatile int i = 0; i < 100; i++) {}
  pthread_mutex_lock(&mutexes[next_idx]);

  // Critical section
  pthread_mutex_unlock(&mutexes[next_idx]);
  pthread_mutex_unlock(&mutexes[idx]);
  return nullptr;
}

void test_callback(void* arg) {
  // Initialize mutexes
  for (int i = 0; i < kThreads; i++) {
    pthread_mutex_init(&mutexes[i], nullptr);
  }

  pthread_t threads[kThreads];
  thread_arg args[kThreads];

  // Create threads that will form a circular dependency
  for (int i = 0; i < kThreads; i++) {
    args[i].index = i;
    pthread_create(&threads[i], nullptr, thread_func, &args[i]);
  }

  // Join all threads
  for (int i = 0; i < kThreads; i++) {
    pthread_join(threads[i], nullptr);
  }

  // Cleanup
  for (int i = 0; i < kThreads; i++) {
    pthread_mutex_destroy(&mutexes[i]);
  }
}

int main() {
  fprintf(stderr, "Starting complex lock-order-inversion test (4 threads, 4 mutexes)...\n");
  int result = __tsan_simulate(test_callback, nullptr);

  fprintf(stderr, "Simulation returned: %d\n", result);
  fprintf(stderr, "Test PASSED: lock-order-inversion detected\n");
  return 0;
}

// CHECK: Starting complex lock-order-inversion test
// CHECK: WARNING: ThreadSanitizer: lock-order-inversion (potential deadlock)
// CHECK: Cycle in lock order graph
// CHECK: Test PASSED
