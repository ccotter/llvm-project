// RUN: %clang_asan -O1 -fsanitize-address-use-after-return=always %s -o %t && not %run %t 2>&1 | FileCheck %s

#include <pthread.h>

pthread_mutex_t *global_mutex;

__attribute__((noinline)) void create_stack_mutex() {
  pthread_mutex_t stack_mutex;
  pthread_mutex_init(&stack_mutex, NULL);
  global_mutex = &stack_mutex;
  // stack_mutex goes out of scope here
}

int main() {
  create_stack_mutex();

  pthread_mutex_lock(global_mutex);

  return 0;
}

// CHECK: stack-use-after-return
// CHECK: #0 {{.*}} in main
