// RUN: %clangxx_tsan -O1 %s -o %t
// RUN: %env_tsan_opts=atexit_sleep_ms=0:abort_on_error=0:simulate_scheduler=random:simulate_iterations=10 %run %t 2>&1 | FileCheck %s

#include <pthread.h>
#include <stdio.h>

extern "C" int __tsan_simulate(void (*callback)(void *), void *arg);

pthread_mutex_t mutex;
pthread_cond_t cond;
int ready = 0;
int woken_count = 0;

void *waiter_thread(void *arg) {
  pthread_mutex_lock(&mutex);
  while (ready == 0) {
    pthread_cond_wait(&cond, &mutex);
  }
  woken_count++;
  pthread_mutex_unlock(&mutex);
  return nullptr;
}

void *signaler_thread(void *arg) {
  // Signal twice to wake both waiters
  pthread_mutex_lock(&mutex);
  ready = 1;
  pthread_mutex_unlock(&mutex);

  pthread_cond_signal(&cond);
  pthread_cond_signal(&cond);

  return nullptr;
}

void test_callback(void *arg) {
  ready = 0;
  woken_count = 0;
  pthread_mutex_init(&mutex, nullptr);
  pthread_cond_init(&cond, nullptr);

  pthread_t waiter1, waiter2, signaler;

  // Create two waiters
  pthread_create(&waiter1, nullptr, waiter_thread, nullptr);
  pthread_create(&waiter2, nullptr, waiter_thread, nullptr);

  // Create signaler
  pthread_create(&signaler, nullptr, signaler_thread, nullptr);

  // Join all threads
  pthread_join(signaler, nullptr);
  pthread_join(waiter1, nullptr);
  pthread_join(waiter2, nullptr);

  pthread_cond_destroy(&cond);
  pthread_mutex_destroy(&mutex);

  if (woken_count == 2) {
    fprintf(stderr, "Both threads woken: %d\n", woken_count);
  } else {
    fprintf(stderr, "ERROR: Expected 2 threads woken, got %d\n", woken_count);
  }
}

int main() { return __tsan_simulate(test_callback, nullptr); }

// CHECK: ThreadSanitizer: simulation starting
// CHECK: Both threads woken: 2
// CHECK: ThreadSanitizer: simulation finished
