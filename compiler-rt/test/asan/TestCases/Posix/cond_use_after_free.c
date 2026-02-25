// RUN: %clang_asan -O2 %s -o %t && not %run %t 2>&1 | FileCheck %s

#include <pthread.h>
#include <stdlib.h>

int main() {
  pthread_cond_t *cond = (pthread_cond_t *)malloc(sizeof(pthread_cond_t));
  pthread_cond_init(cond, NULL);

  free(cond);

  pthread_cond_signal(cond);

  return 0;
}

// CHECK: heap-use-after-free
// CHECK: #0 {{.*}} in main {{.*}}cond_use_after_free.c
