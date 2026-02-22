// RUN: %clangxx_tsan -O1 %s -o %t
// RUN: %env_tsan_opts=atexit_sleep_ms=0:abort_on_error=0:simulate_scheduler=random:simulate_iterations=5 %run %t 2>&1 | FileCheck %s
//
// Test heavy condition variable signaling.
// Multiple threads wait on condvar, producer signals them.

#include <pthread.h>
#include <stdio.h>

extern "C" int __tsan_simulate(void (*callback)(void*), void* arg);

pthread_mutex_t mutex;
pthread_cond_t condvar;
int ready = 0;
int workers_done = 0;

void* worker_thread(void* arg) {
  pthread_mutex_lock(&mutex);

  // Wait for signal
  while (!ready) {
    pthread_cond_wait(&condvar, &mutex);
  }

  workers_done++;
  pthread_mutex_unlock(&mutex);

  return nullptr;
}

void test_callback(void* arg) {
  ready = 0;
  workers_done = 0;
  pthread_mutex_init(&mutex, nullptr);
  pthread_cond_init(&condvar, nullptr);

  const int num_workers = 3;
  pthread_t threads[num_workers];

  // Create workers that will wait
  for (int i = 0; i < num_workers; i++) {
    pthread_create(&threads[i], nullptr, worker_thread, nullptr);
  }

  // Give threads time to start waiting
  for (volatile int i = 0; i < 10; i++) {}

  // Signal all workers
  pthread_mutex_lock(&mutex);
  ready = 1;
  pthread_mutex_unlock(&mutex);
  pthread_cond_broadcast(&condvar);

  // Join all workers
  for (int i = 0; i < num_workers; i++) {
    pthread_join(threads[i], nullptr);
  }

  pthread_cond_destroy(&condvar);
  pthread_mutex_destroy(&mutex);

  if (workers_done == num_workers) {
    fprintf(stderr, "All workers completed: %d\n", workers_done);
  } else {
    fprintf(stderr, "ERROR: Expected %d workers, got %d\n", num_workers, workers_done);
  }
}

int main() {
  int result = __tsan_simulate(test_callback, nullptr);

  if (result == 0) {
    fprintf(stderr, "Test PASSED: condvar signaling works\n");
    return 0;
  } else {
    fprintf(stderr, "Test FAILED: unexpected return value %d\n", result);
    return 1;
  }
}

// CHECK: ThreadSanitizer: simulation starting
// CHECK: All workers completed: 3
// CHECK: ThreadSanitizer: simulation finished
// CHECK: Test PASSED: condvar signaling works
