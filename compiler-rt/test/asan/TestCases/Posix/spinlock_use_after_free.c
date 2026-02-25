// RUN: %clang_asan -O2 %s -o %t && not %run %t 2>&1 | FileCheck %s

#include <pthread.h>
#include <stdlib.h>

int main() {
  pthread_spinlock_t *spinlock =
      (pthread_spinlock_t *)malloc(sizeof(pthread_spinlock_t));
  pthread_spin_init(spinlock, 0);

  free(spinlock);

  pthread_spin_lock(spinlock);

  return 0;
}

// CHECK: heap-use-after-free
// CHECK: #0 {{.*}} in main {{.*}}spinlock_use_after_free.c
