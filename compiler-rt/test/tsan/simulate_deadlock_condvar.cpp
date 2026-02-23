// RUN: %clangxx_tsan -O1 %s -o %t
// RUN: %env_tsan_opts=atexit_sleep_ms=0:abort_on_error=0:simulate_scheduler=random:simulate_iterations=2 not %run %t 2>&1 | FileCheck %s

// Test condition variable deadlock detection.
// Scenario: Two threads both wait on condvar, no one signals - they're deadlocked

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <unistd.h>

extern "C" int __tsan_simulate(void (*callback)(void *), void *arg);

pthread_mutex_t mutex;
pthread_cond_t condvar;

void *thread_func(void *arg) {
  pthread_mutex_lock(&mutex);
  // Wait on condition variable that will never be signaled
  pthread_cond_wait(&condvar, &mutex);
  pthread_mutex_unlock(&mutex);
  return nullptr;
}

void test_callback(void *arg) {
  pthread_mutex_init(&mutex, nullptr);
  pthread_cond_init(&condvar, nullptr);

  pthread_t t1, t2;
  pthread_create(&t1, nullptr, thread_func, nullptr);
  pthread_create(&t2, nullptr, thread_func, nullptr);

  // Both threads will block on pthread_cond_wait(), creating a deadlock
  // since no one will signal the condition variable

  pthread_join(t1, nullptr);
  pthread_join(t2, nullptr);

  pthread_cond_destroy(&condvar);
  pthread_mutex_destroy(&mutex);
}

int main() {
  // Deadlock will cause Die() - this will not return
  alarm(10); // Safety timeout
  __tsan_simulate(test_callback, nullptr);
  fprintf(stderr, "Test FAILED: simulation should have died on deadlock\n");
  return 1;
}

// CHECK: ThreadSanitizer: simulation starting
// CHECK: ThreadSanitizer: deadlock detected at iteration {{[0-9]+}} - all threads are blocked
// CHECK: ThreadSanitizer: to reproduce, set TSAN_OPTIONS=simulate_scheduler=random:simulate_start_iteration={{[0-9]+}}
