// RUN: %clangxx_tsan -O1 %s -o %t
// RUN: %env_tsan_opts=atexit_sleep_ms=0:abort_on_error=0:simulate_scheduler=random:simulate_iterations=10 %run %t 2>&1 | FileCheck %s

#include <assert.h>
#include <pthread.h>
#include <stdio.h>

extern "C" int __tsan_simulate(void (*callback)(void *), void *arg);

const int num_mutexes = 10;
pthread_mutex_t mutexes[num_mutexes];
int counters[num_mutexes];

void *thread_func(void *arg) {
  long mutex_id = (long)arg;

  pthread_mutex_lock(&mutexes[mutex_id]);
  counters[mutex_id]++;
  pthread_mutex_unlock(&mutexes[mutex_id]);

  return nullptr;
}

void test_callback(void *arg) {
  for (int i = 0; i < num_mutexes; i++) {
    pthread_mutex_init(&mutexes[i], nullptr);
    counters[i] = 0;
  }

  // Create thread pairs for each mutex
  const int threads_per_mutex = 2;
  pthread_t threads[num_mutexes * threads_per_mutex];

  for (int i = 0; i < num_mutexes; i++) {
    for (int j = 0; j < threads_per_mutex; j++) {
      pthread_create(&threads[i * threads_per_mutex + j], nullptr, thread_func,
                     (void *)(long)i);
    }
  }

  // Join all threads
  for (int i = 0; i < num_mutexes * threads_per_mutex; i++) {
    pthread_join(threads[i], nullptr);
  }

  for (int i = 0; i < num_mutexes; i++) {
    assert(counters[i] == threads_per_mutex);
    pthread_mutex_destroy(&mutexes[i]);
  }
}

int main() { return __tsan_simulate(test_callback, nullptr); }

// CHECK: ThreadSanitizer: simulation starting
