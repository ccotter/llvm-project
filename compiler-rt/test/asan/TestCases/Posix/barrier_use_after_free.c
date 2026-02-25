// RUN: %clang_asan -O2 %s -o %t && not %run %t 2>&1 | FileCheck %s

#include <pthread.h>
#include <stdlib.h>

int main() {
  pthread_barrier_t *barrier =
      (pthread_barrier_t *)malloc(sizeof(pthread_barrier_t));
  pthread_barrier_init(barrier, NULL, 2);

  free(barrier);

  pthread_barrier_wait(barrier);

  return 0;
}

// CHECK: heap-use-after-free
// CHECK: #0 {{.*}} in main {{.*}}barrier_use_after_free.c
