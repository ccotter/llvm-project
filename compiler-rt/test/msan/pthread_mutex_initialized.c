// RUN: %clang_msan -O0 %s -o %t && %run %t

#include <pthread.h>

int main() {
  {
    pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&mutex);
    pthread_mutex_unlock(&mutex);
  }
  {
    pthread_mutex_t mutex;
    pthread_mutex_init(&mutex, NULL);
    pthread_mutex_lock(&mutex);
    pthread_mutex_unlock(&mutex);
  }
  return 0;
}
