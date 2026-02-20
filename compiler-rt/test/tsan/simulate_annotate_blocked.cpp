// RUN: %clangxx_tsan -O1 %s -o %t && env TSAN_OPTIONS="simulate_scheduler=random" %run %t 2>&1 | FileCheck %s
//
// Test the __tsan_simulate_annotate_wait/wake_one/wake_all() APIs for
// manually annotating thread blocking on addresses during simulation.
// This is useful for futex-like primitives or custom synchronization
// not known to TSAN's interceptors.

#include <pthread.h>
#include <stdio.h>
#include <atomic>

extern "C" {
int __tsan_simulate(void (*callback)(void *), void *arg);
void __tsan_simulate_annotate_wait(void *addr);
void __tsan_simulate_annotate_wake_one(void *addr);
void __tsan_simulate_annotate_wake_all(void *addr);
}

pthread_mutex_t mtx;
std::atomic<int> futex_word{0};
std::atomic<int> done{0};

void *thread_func(void *arg) {
  // Simulate blocking on a custom synchronization primitive (futex-like).
  // The thread blocks on the address of futex_word.

  // Manually mark this thread as blocked on this address.
  __tsan_simulate_annotate_wait(&futex_word);

  // When woken, futex_word would be non-zero.
  // (In real futex, you'd check the value, but for testing we just proceed.)

  // Now do normal work with instrumented mutex.
  pthread_mutex_lock(&mtx);
  done = 1;
  pthread_mutex_unlock(&mtx);

  return nullptr;
}

void test_callback(void *) {
  pthread_mutex_init(&mtx, nullptr);

  pthread_t thread;
  pthread_create(&thread, nullptr, thread_func, nullptr);

  // Give worker thread time to call annotate_wait.
  for (volatile int i = 0; i < 100; i++) {}

  // Wake the worker thread (simulating a futex_wake).
  futex_word = 1;
  __tsan_simulate_annotate_wake_one(&futex_word);

  // Wait for worker to finish.
  pthread_join(thread, nullptr);

  if (done == 1) {
    fprintf(stderr, "OK: thread completed\n");
  } else {
    fprintf(stderr, "ERROR: thread did not complete\n");
  }

  pthread_mutex_destroy(&mtx);
}

int main() {
  int ret = __tsan_simulate(test_callback, nullptr);
  if (ret != 0) {
    fprintf(stderr, "Simulation failed with error code: %d\n", ret);
    return 1;
  }
  return 0;
}

// CHECK: OK: thread completed
// CHECK-NOT: ERROR
// CHECK-NOT: WARNING: ThreadSanitizer: data race
