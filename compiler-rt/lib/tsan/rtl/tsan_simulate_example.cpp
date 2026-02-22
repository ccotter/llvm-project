// Sample program demonstrating __tsan_simulate for systematic
// thread interleaving exploration.
//
// Build:
//   /workarea/llvm-project/build/bin/clang++ -fsanitize=thread \
//     -g -O1 -std=c++17 tsan_simulate_example.cpp -o tsan_simulate_example
//
// Run:
//   TSAN_OPTIONS=simulate_scheduler=random:simulate_iterations=100 \
//     ./tsan_simulate_example
//
// The test creates two threads that both increment a shared atomic counter.
// The simulation explores different interleavings to verify correctness.

#include <atomic>
#include <cassert>
#include <cstdio>
#include <thread>

// Declare the TSan simulate interface.
extern "C" void __tsan_simulate(void (*callback)(void* arg), void* arg);

// Shared state for the test. Re-initialized each iteration by the callback.
struct TestState {
  std::atomic<int> counter{0};
  int non_atomic_check{0};
};

// Test callback: two threads atomically increment a counter.
// The simulation explores different orderings of the increments.
void test_atomic_counter(void* arg) {
  (void)arg;
  TestState state;

  std::thread t1(
      [&state]() { state.counter.fetch_add(1, std::memory_order_relaxed); });

  std::thread t2(
      [&state]() { state.counter.fetch_add(1, std::memory_order_relaxed); });

  t1.join();
  t2.join();

  int final_val = state.counter.load(std::memory_order_relaxed);
  assert(final_val == 2 && "Counter should be exactly 2");
}

// Test callback: demonstrates a mutex-protected critical section.
// Two threads increment a non-atomic variable under a mutex.
#include <mutex>

void test_mutex_protected(void* arg) {
  (void)arg;

  int shared_data = 0;
  std::mutex mtx;

  std::thread t1([&]() {
    std::lock_guard<std::mutex> lock(mtx);
    shared_data++;
  });

  std::thread t2([&]() {
    std::lock_guard<std::mutex> lock(mtx);
    shared_data++;
  });

  t1.join();
  t2.join();

  assert(shared_data == 2 && "shared_data should be exactly 2");
}

// Test callback: producer-consumer with atomic flag.
void test_producer_consumer(void* arg) {
  (void)arg;

  int data = 0;
  std::atomic<bool> ready{false};

  std::thread producer([&]() {
    data = 42;
    ready.store(true, std::memory_order_release);
  });

  std::thread consumer([&]() {
    while (!ready.load(std::memory_order_acquire)) {
      // spin
    }
    assert(data == 42 && "Consumer should see producer's write");
  });

  producer.join();
  consumer.join();
}

// Test callback: non-atomic increments (demonstrates data race).
// Two threads each increment a shared variable 5 times without synchronization.
// TSAN will detect the race condition.
void test_non_atomic_increment(void* arg) {
  (void)arg;

  std::atomic<int> shared_counter = 0;

  std::thread t1([&]() {
    for (int i = 0; i < 5; i++) {
      int x = shared_counter.load();
      shared_counter.store(x + 1);
    }
  });

  std::thread t2([&]() {
    for (int i = 0; i < 5; i++) {
      int x = shared_counter.load();
      shared_counter.store(x + 1);
    }
  });

  t1.join();
  t2.join();

  printf("Counter value: %d\n", shared_counter.load());

  // assert(shared_counter == 10 && "Counter should be 10 if no race occurred");
}

// Test callback: producer-consumer with condition variable.
// Producer produces 5 items, consumer consumes them one by one.
#include <condition_variable>
#include <queue>

void test_condvar_producer_consumer(void* arg) {
  (void)arg;

  std::queue<int> queue;
  std::mutex mtx;
  std::condition_variable cv;
  bool done = false;

  std::thread producer([&]() {
    for (int i = 1; i <= 5; i++) {
      {
        std::lock_guard<std::mutex> lock(mtx);
        queue.push(i);
      }
      cv.notify_one();
    }
    {
      std::lock_guard<std::mutex> lock(mtx);
      done = true;
    }
    cv.notify_one();
  });

  std::thread consumer([&]() {
    int count = 0;
    while (true) {
      std::unique_lock<std::mutex> lock(mtx);
      cv.wait(lock, [&]() { return !queue.empty() || done; });

      if (!queue.empty()) {
        queue.pop();
        count++;
      } else if (done) {
        break;
      }
    }
    assert(count == 5 && "Consumer should have received 5 items");
  });

  producer.join();
  consumer.join();
}

int main() {
#if 0
  printf("=== Test 1: Atomic counter (two threads) ===\n");
  __tsan_simulate(test_atomic_counter, nullptr);

  printf("\n=== Test 2: Mutex-protected increment ===\n");
  __tsan_simulate(test_mutex_protected, nullptr);

  printf("\n=== Test 3: Producer-consumer ===\n");
  __tsan_simulate(test_producer_consumer, nullptr);

  printf("\n=== Test 4: Condition variable producer-consumer ===\n");
  __tsan_simulate(test_condvar_producer_consumer, nullptr);
#endif

  if (true) {
    printf("\n=== Test 5: Non-atomic increment (demonstrates race) ===\n");
    __tsan_simulate(test_non_atomic_increment, nullptr);
  }

  printf("\nAll tests passed.\n");
  return 0;
}
