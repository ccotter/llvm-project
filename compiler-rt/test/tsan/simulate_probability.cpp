// RUN: %clangxx_tsan -O1 %s -o %t
// RUN: %env_tsan_opts=atexit_sleep_ms=0:abort_on_error=0:simulate_scheduler=random:simulate_iterations=10:simulate_probability=0.5 %run %t 2>&1 | FileCheck %s --check-prefix=CHECK-PROB50
// RUN: %env_tsan_opts=atexit_sleep_ms=0:abort_on_error=0:simulate_scheduler=random:simulate_iterations=10:simulate_probability=1.0 %run %t 2>&1 | FileCheck %s --check-prefix=CHECK-PROB100
//
// Test that simulate_probability parameter is accepted and affects scheduling.
// Lower probability should result in fewer context switches (less exploration).
// Note: This is a basic functional test that the parameter works; statistical
// validation of randomness would require more sophisticated analysis.

#include <pthread.h>
#include <stdio.h>

extern "C" int __tsan_simulate(void (*callback)(void *), void *arg);

pthread_mutex_t mutex;
int counter = 0;

void *thread_func(void *arg) {
  for (int i = 0; i < 10; i++) {
    pthread_mutex_lock(&mutex);
    counter++;
    pthread_mutex_unlock(&mutex);
  }
  return nullptr;
}

void test_callback(void *arg) {
  counter = 0;
  pthread_mutex_init(&mutex, nullptr);

  pthread_t t1, t2;
  pthread_create(&t1, nullptr, thread_func, nullptr);
  pthread_create(&t2, nullptr, thread_func, nullptr);

  pthread_join(t1, nullptr);
  pthread_join(t2, nullptr);

  pthread_mutex_destroy(&mutex);

  if (counter == 20) {
    fprintf(stderr, "Counter verified: %d\n", counter);
  } else {
    fprintf(stderr, "ERROR: Expected counter=20, got %d\n", counter);
  }
}

int main() {
  int result = __tsan_simulate(test_callback, nullptr);

  if (result == 0) {
    fprintf(stderr, "Test PASSED: probability parameter accepted\n");
    return 0;
  } else {
    fprintf(stderr, "Test FAILED: unexpected return value %d\n", result);
    return 1;
  }
}

// CHECK-PROB50: ThreadSanitizer: simulation starting
// CHECK-PROB50: Counter verified: 20
// CHECK-PROB50: ThreadSanitizer: simulation finished
// CHECK-PROB50: Test PASSED: probability parameter accepted

// CHECK-PROB100: ThreadSanitizer: simulation starting
// CHECK-PROB100: Counter verified: 20
// CHECK-PROB100: ThreadSanitizer: simulation finished
// CHECK-PROB100: Test PASSED: probability parameter accepted
