// RUN: %clang_asan -O2 %s -o %t && not %run %t 2>&1 | FileCheck %s

#include <pthread.h>
#include <stdlib.h>

int main() {
  pthread_mutex_t *mutex = (pthread_mutex_t *)malloc(sizeof(pthread_mutex_t));
  pthread_mutex_init(mutex, NULL);

  free(mutex);

  pthread_mutex_lock(mutex);

  return 0;
}

// CHECK: heap-use-after-free
// CHECK: #0 {{.*}} in main {{.*}}mutex_heap_use_after_free.c
