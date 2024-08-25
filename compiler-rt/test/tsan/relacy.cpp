// RUN: %clang_tsan -O1 %s -o %t && TSAN_OPTIONS=fuzzing_scheduler=random %run %t 2>&1 | FileCheck %s
// RUN: %clang_tsan -O1 %s -o %t && %run %t 2>&1 | FileCheck %s

#if 1
#include "test.h"
#include <assert.h>
#include <errno.h>
#else
#include <pthread.h>
#include <stdio.h>
#endif

namespace ping_pong {

int var;
pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t cv = PTHREAD_COND_INITIALIZER;

void *Thread1(void*) {
  for (int i = 0; i != 1000; ++i) {
    pthread_mutex_lock(&mtx);
    var = 1;
    pthread_cond_signal(&cv);
    pthread_mutex_unlock(&mtx);
  }
  return NULL;
}
void *Thread2(void*) {
  for (int i = 0; i != 1000; ++i) {
    pthread_mutex_lock(&mtx);
    while (!var) {
      pthread_cond_wait(&cv, &mtx);
    }
    pthread_mutex_unlock(&mtx);
  }
  return NULL;
}

void run() {
  pthread_t t1 ,t2;
  pthread_create(&t1, 0, Thread1, 0);
  pthread_create(&t2, 0, Thread2, 0);
  pthread_join(t1, NULL);
  pthread_join(t2, NULL);
}

} // namespace ping_pong

namespace recursive_mutex {

void run() {
  pthread_mutex_t mtx;
  pthread_mutexattr_t attr;
  pthread_mutexattr_init(&attr);
  pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
  pthread_mutex_init(&mtx, &attr);

  assert(pthread_mutex_lock(&mtx) == 0);
  assert(pthread_mutex_lock(&mtx) == 0);
  assert(pthread_mutex_unlock(&mtx) == 0);
  assert(pthread_mutex_unlock(&mtx) == 0);
}

} // namespace recursive_mutex

namespace create_1000_therads {

void *Thread(void*) {
  return NULL;
}

void run() {
  for (int i = 0; i != 1000; ++i) {
    pthread_t t;
    pthread_create(&t, 0, Thread, 0);
    pthread_join(t, NULL);
  }
}

} // namespace create_1000_therads

namespace try_lock {

pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;

void *Thread(void*) {
  assert(EBUSY == pthread_mutex_trylock(&mtx));
  return NULL;
}

void run() {
  assert(0 == pthread_mutex_trylock(&mtx));
  pthread_t t;
  pthread_create(&t, 0, Thread, 0);
  pthread_join(t, NULL);
  assert(0 == pthread_mutex_unlock(&mtx));
}

} // namespace try_lock

int main() {
  ping_pong::run();
  recursive_mutex::run();
  create_1000_therads::run();
  try_lock::run();
  fprintf(stderr, "PASS\n");
  return 0;
}

// CHECK-NOT: WARNING: ThreadSanitizer:
// CHECK: PASS
