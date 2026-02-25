// RUN: %clang_msan -O0 %s -o %t && not %run %t 2>&1 | FileCheck %s

#include <pthread.h>

int main() {
  pthread_mutex_t mutex;
  pthread_mutex_lock(&mutex);
  pthread_mutex_unlock(&mutex);
  return 0;
}

// CHECK: Uninitialized bytes in pthread_mutex_lock
// CHECK: WARNING: MemorySanitizer: use-of-uninitialized-value
// CHECK: {{#0 .* in main }}
