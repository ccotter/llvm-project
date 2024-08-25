#include "fuzzing_scheduler.h"
#include <tsan_rtl.h>
#include <sanitizer_common/sanitizer_allocator_internal.h>
#include <tsan_dense_alloc.h>
#include <sanitizer_common/sanitizer_placement_new.h>
#include <interception/interception.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <assert.h>
#include <syscall.h>
#include <errno.h>
#include <signal.h>

__tsan::u64 TID() {
  static thread_local auto tid = syscall(SYS_gettid);
  return tid;
}

thread_local __tsan::u64 s_tid = 0;
__tsan::u64 s_max_tid = 0;

namespace __interception {
  extern int (*real_pthread_create)(void*, void*, void *(*)(void*), void*);
  extern int (*real_pthread_join)(void*, void**);
  extern int (*real_pthread_detach)(void*);
  extern int (*real_pthread_mutex_lock)(void*);
  extern int (*real_pthread_mutex_trylock)(void*);
  extern int (*real_pthread_mutex_unlock)(void*);
  extern int (*real_pthread_cond_wait)(void*, void*);
  extern int (*real_pthread_cond_signal)(void*);
  extern int (*real_pthread_cond_broadcast)(void*);
  extern int (*real_pthread_mutex_init)(void*, void*);
  extern int (*real_pthread_cond_init)(void*, void*);
}

namespace __tsan {

namespace {

  struct LockGuard {
    pthread_mutex_t* mut_;

    LockGuard(LockGuard&&);
    LockGuard& operator=(LockGuard&&);

    LockGuard(pthread_mutex_t* mut) : mut_(mut) {
      REAL(pthread_mutex_lock)(mut_);
    }
    ~LockGuard() {
      REAL(pthread_mutex_unlock)(mut_);
    }
  };

  enum class ThreadState {
    UNKNOWN = 0,
    RUNNING = 1,
    WAIT = 2,
    BLOCKED = 3,
    OUT_TIME =  4
  };

#define CHECK_RC(e) \
  do { \
    int res = e; \
    if (res) { \
      fprintf(stderr, "Failed with res %d errno %d\n", res, errno); \
      perror("oops"); \
      while(true); \
    } \
  } while (0)

#if 1
#define DEBUG(x)
#else
#define DEBUG(x) x
#endif

struct NullFuzzingScheduler : IFuzzingScheduler {
  void SynchronizationPoint() override {
  }
  void UnblockOne(int) override {}
  int GetCurrentState() override { return 0; }
  void SetCurrentState(int new_state) override {}
  void SetState(u64 tid, int new_state) override {}
  int SynchronizationPoint_MutexLock(void* m) override {
    return REAL(pthread_mutex_lock)(m);
  }
  int SynchronizationPoint_MutexTryLock(void* m) override {
    return REAL(pthread_mutex_trylock)(m);
  }
  int SynchronizationPoint_MutexUnlock(void* m) override {
    return REAL(pthread_mutex_unlock)(m);
  }
  int SynchronizationPoint_CondWait(void* c, void* m) override {
    return REAL(pthread_cond_wait)(c, m);
  }
  int SynchronizationPoint_CondNotifyOne(void* c) override {
    return REAL(pthread_cond_signal)(c);
  }
  int SynchronizationPoint_CondNotifyAll(void* c) override {
    return REAL(pthread_cond_broadcast)(c);
  }
  void SynchronizationPoint_MutexInit(void* m, bool recursive) override {}
  void SynchronizationPoint_CondInit(void* c) override {}
  int SynchronizationPoint_JoinThread(void* th, void** ret) override { return REAL(pthread_join)(th, ret); }
  int SynchronizationPoint_DetachThread(void* th) override { return REAL(pthread_detach)(th); }
  void SynchronizationPoint_InitThread(void* th) override {}
  void SynchronizationPoint_CreateThread(void* th) override {}
  void SynchronizationPoint_ExitThread() override { }
};

static void DEADLOCK(const char* msg)
{
  fprintf(stderr, "DEADLOCK %s\n", msg);
}

namespace my {

static pthread_mutex_t SCHED_LOCK;
static pthread_mutex_t BIGLOCK;
static pthread_cond_t CV;
static int x = [] {
  CHECK_RC(REAL(pthread_mutex_init)(&BIGLOCK, nullptr));
  CHECK_RC(REAL(pthread_mutex_init)(&SCHED_LOCK, nullptr));
  CHECK_RC(REAL(pthread_cond_init)(&CV, nullptr));
  return 0;
}();

template <class K, class V, size_t Size>
struct dumb_map {
  struct data_t {
    bool in_use = false;
    K key;
    V value;
  };
  data_t data[Size];

  void insert(K key, V value) {
    for (int i = 0; i != Size; ++i) {
      if (!data[i].in_use) {
        data[i].in_use = true;
        data[i].key = key;
        data[i].value = (V&&)value;
        return;
      }
    }
    assert(false && "No more space");
  }

  data_t* find(K key) {
    for (int i = 0; i != Size; ++i) {
      if (data[i].in_use && data[i].key == key) {
        return &data[i];
      }
    }
    return end();
  }
  size_t count(K key) {
    return find(key) != end() ? 1 : 0;
  }
  void erase(data_t* itr) {
    itr->in_use = false;
  }

  data_t* begin() {
    for (int i = 0; i != Size; ++i) {
      if (data[i].in_use) {
        return &data[i];
      }
    }
    return end();
  }
  data_t* end() {
    return &data[Size];
  }
};

#if 0
 = truetemplate <class T>
struct malloc_allocator {
  using value_type = T;

  T* allocate(size_t n) {
    return (T*)REAL(malloc)(sizeof(T) * n);
  }
  void deallocate(T* ptr, size_t n) {
    return (T*)REAL(free)(ptr);
  }
};
#endif

struct monostate{};

struct waitset {
  dumb_map<u64, monostate, 100> waiters;
  //pthread_cond_t cv;

  waitset() {
    //CHECK_RC(REAL(pthread_cond_init)(&cv, nullptr));
  }
  waitset(const waitset&) = delete;
  waitset& operator=(const waitset&) = delete;
  waitset(waitset&&) = default;
  waitset& operator=(waitset&&) = default;

  void wait() {
    assert(!waiters.count(s_tid));
    waiters.insert(s_tid, {});

    while (waiters.count(s_tid)) {
      REAL(pthread_mutex_lock)(&my::SCHED_LOCK);
      int old_state = GetFuzzingScheduler().GetCurrentState();
      GetFuzzingScheduler().UnblockOne((int)ThreadState::BLOCKED);
      REAL(pthread_mutex_unlock)(&my::SCHED_LOCK);

      CHECK_RC(REAL(pthread_cond_wait)(&CV, &BIGLOCK));

      REAL(pthread_mutex_lock)(&my::SCHED_LOCK);
      GetFuzzingScheduler().SetCurrentState(old_state);
      REAL(pthread_mutex_unlock)(&my::SCHED_LOCK);
    }
  }

  void notify_one_impl() {
    // no lock here

    auto itr = waiters.begin();
    if (itr != waiters.end()) {
      waiters.erase(itr);
      // TODO - should this be the old_state from the wait() call above?
      DEBUG(fprintf(stderr, "[%ld] Waitset::notify_one setting state of %ld to WAIT\n", s_tid, itr->key));
      GetFuzzingScheduler().SetState(itr->key, (int)ThreadState::WAIT);

      CHECK_RC(REAL(pthread_cond_broadcast)(&CV));
    }
  }

  void notify_one() {
    CHECK_RC(REAL(pthread_mutex_lock)(&my::SCHED_LOCK));
    notify_one_impl();
    CHECK_RC(REAL(pthread_mutex_unlock)(&my::SCHED_LOCK));
  }

  void notify_all_impl() {
    if (waiters.begin() == waiters.end()) return;

    while (waiters.begin() != waiters.end()) {
      auto itr = waiters.begin();
      if (itr != waiters.end()) {
        waiters.erase(itr);
        GetFuzzingScheduler().SetState(itr->key, (int)ThreadState::WAIT);
      }
    }
    CHECK_RC(REAL(pthread_cond_broadcast)(&CV));
  }

  void notify_all() {
    CHECK_RC(REAL(pthread_mutex_lock)(&my::SCHED_LOCK));
    notify_all_impl();
    CHECK_RC(REAL(pthread_mutex_unlock)(&my::SCHED_LOCK));
  }

};

struct mutex {
  size_t owner = FREE;
  waitset ws;
  bool is_recursive = true;
  int recurse_count = 0;

  mutex(bool is_recursive = false) : is_recursive(is_recursive) {
  }

  void lock(bool need_lock = true) {
    // XXX ??? confusing mutex logic!
    if (!need_lock) CHECK_RC(REAL(pthread_mutex_unlock)(&BIGLOCK));
    GetFuzzingScheduler().SynchronizationPoint();
    if (!need_lock) CHECK_RC(REAL(pthread_mutex_lock)(&BIGLOCK));

    if (need_lock) CHECK_RC(REAL(pthread_mutex_lock)(&BIGLOCK));

    if (owner != s_tid) {
      while (owner != FREE) {
        ws.wait();
      }
    }

    if (owner == FREE && recurse_count != 0) {
      DEADLOCK("recurse_count not 0 in lock");
    }
    // Always support recursive mutex behavior, and let TSAN itself
    // detect locking a locked mutex that is not configured as recursive.
#if 0
    if (owner != FREE && !is_recursive) {
      Printf("FATAL: ThreadSanitizer detected attempt to lock non-recursive mutex that was already locked");
      Die();
    }
#endif

    owner = s_tid;
    ++recurse_count;

    if (need_lock) CHECK_RC(REAL(pthread_mutex_unlock)(&BIGLOCK));
  }

  int try_lock() {
    // XXX ??? confusing mutex logic!
    GetFuzzingScheduler().SynchronizationPoint();

    LockGuard lg(&my::BIGLOCK);

    if (owner == s_tid) {
      if (is_recursive) {
        ++recurse_count;
        return 0;
      } else {
        return EBUSY;
      }
    }

    if (owner != FREE) {
      return EBUSY;
    }

    if (owner == FREE && recurse_count != 0) {
      DEADLOCK("recurse_count not 0 in try_lock");
    }

    owner = s_tid;
    ++recurse_count;
    return 0;
  }

  void unlock(bool need_lock = true) {
    if (!need_lock) CHECK_RC(REAL(pthread_mutex_unlock)(&BIGLOCK));
    GetFuzzingScheduler().SynchronizationPoint();
    if (!need_lock) CHECK_RC(REAL(pthread_mutex_lock)(&BIGLOCK));

    if (need_lock) CHECK_RC(REAL(pthread_mutex_lock)(&BIGLOCK));
    struct Unlock {
      ~Unlock() {
        if (need_lock) CHECK_RC(REAL(pthread_mutex_unlock)(&BIGLOCK));
      }
      bool need_lock;
    } unlock{need_lock};

    if (recurse_count == 0) {
      DEADLOCK("recurse_count is 0 in unlock");
    }
    if (s_tid != owner) {
      DEADLOCK("tids do not match in unlock");
    }
    --recurse_count;
    if (recurse_count) {
      return;
    }

    owner = FREE;
    ws.notify_one();
  }

  static constexpr size_t FREE = (size_t)-1;
};

struct condition_variable {
  waitset ws;

  void wait(mutex* m) {
    CHECK_RC(REAL(pthread_mutex_lock)(&BIGLOCK));
    m->unlock(false);
    ws.wait();
    m->lock(false);
    CHECK_RC(REAL(pthread_mutex_unlock)(&BIGLOCK));
  }

  void notify_one() {
    GetFuzzingScheduler().SynchronizationPoint();

    CHECK_RC(REAL(pthread_mutex_lock)(&BIGLOCK));
    ws.notify_one();
    CHECK_RC(REAL(pthread_mutex_unlock)(&BIGLOCK));
  }

  void notify_all() {
    GetFuzzingScheduler().SynchronizationPoint();

    CHECK_RC(REAL(pthread_mutex_lock)(&BIGLOCK));
    ws.notify_all();
    CHECK_RC(REAL(pthread_mutex_unlock)(&BIGLOCK));
  }

  static constexpr size_t FREE = (size_t)-1;
};

static dumb_map<void*, mutex, 1000> Mutexes;
static dumb_map<void*, condition_variable, 1000> CVs;

void lock(void* mtx) {
  REAL(pthread_mutex_lock)(&my::BIGLOCK);
  if (!my::Mutexes.count(mtx)) {
    my::Mutexes.insert(mtx, {});
  }
  REAL(pthread_mutex_unlock)(&my::BIGLOCK);

  assert(Mutexes.count((void*)mtx));
  auto& m = Mutexes.find((void*)mtx)->value;
  m.lock();
}
int try_lock(void* mtx) {
  REAL(pthread_mutex_lock)(&my::BIGLOCK);
  if (!my::Mutexes.count(mtx)) {
    my::Mutexes.insert(mtx, {});
  }
  REAL(pthread_mutex_unlock)(&my::BIGLOCK);

  assert(Mutexes.count((void*)mtx));
  auto& m = Mutexes.find((void*)mtx)->value;
  return m.try_lock();
}
void unlock(void* mtx) {
  assert(Mutexes.count((void*)mtx));
  auto& m = Mutexes.find((void*)mtx)->value;
  m.unlock();
}
void wait(void* cv, void* mtx) {
  REAL(pthread_mutex_lock)(&my::BIGLOCK);
  if (!my::CVs.count(cv)) {
    my::CVs.insert(cv, {});
  }
  REAL(pthread_mutex_unlock)(&my::BIGLOCK);

  assert(Mutexes.count((void*)mtx));
  auto& m = Mutexes.find((void*)mtx)->value;
  assert(CVs.count((void*)cv));
  auto& c = CVs.find((void*)cv)->value;
  c.wait(&m);
}
void notify_one(void* cv) {
  REAL(pthread_mutex_lock)(&my::BIGLOCK);
  if (!my::CVs.count(cv)) {
    my::CVs.insert(cv, {});
  }
  REAL(pthread_mutex_unlock)(&my::BIGLOCK);

  assert(CVs.count((void*)cv));
  auto& c = CVs.find((void*)cv)->value;
  c.notify_one();
}

void notify_all(void* cv) {
  REAL(pthread_mutex_lock)(&my::BIGLOCK);
  if (!my::CVs.count(cv)) {
    my::CVs.insert(cv, {});
  }
  REAL(pthread_mutex_unlock)(&my::BIGLOCK);

  assert(CVs.count((void*)cv));
  auto& c = CVs.find((void*)cv)->value;
  c.notify_all();
}

} // namespace my

struct RandomFuzzingScheduler : IFuzzingScheduler {
  RandomFuzzingScheduler() {
    unsigned int seed;
    if (getenv("TSAN_RAND_SEED")) {
      seed = strtol(getenv("TSAN_RAND_SEED"), nullptr, 10);
    } else {
      seed = NanoTime();
    }
    Printf("INFO! ThreadSanitizer initialized RandomFuzzingScheduler with seed=%u\n", seed);
    srand(seed);
    pthread_t t;
    REAL(pthread_create)(&t, NULL, reinterpret_cast<void*(*)(void*)>(&RandomFuzzingScheduler::WatchDog), this);
    contexts[1].state = ThreadState::RUNNING;
    contexts[1].exit_count = 2;
    //REAL(pthread_detach)(&t);
  }


private:

  struct ThreadContext {
    ThreadState state = ThreadState::UNKNOWN;
    void* thread_handle = nullptr; // pointer to pthread_thread_t object
    bool exited = false;
    int exit_count = 2;
    my::waitset ws;
    u64 start_time = 0;
  };

  ThreadContext contexts[65536] = {};

  u64 AllocateTid() {
    for (int i = 1; i <= s_max_tid; ++i) {
      if (contexts[i].exit_count == 0) {
        return i;
      }
    }
    u64 new_tid =__atomic_add_fetch(&s_max_tid, 1, __ATOMIC_RELAXED);
    return new_tid;
  }


  u64 GetTid() {
    if (s_tid == 0) {
      s_tid = __atomic_add_fetch(&s_max_tid, 1, __ATOMIC_RELAXED);
      if (s_tid != 1) {
        DEADLOCK("Unexpectedly allocated new TID on the fly");
        while(1);
      }
    }
    if (s_tid > 65535) {
      Printf("FATAL: ThreadSanitizer The maximum number of threads created during the program should not exceed 65535");
      Die();
    }
    return s_tid;
  }

  void UnblockOne(int new_state) override {
    auto tid = GetTid();
    __atomic_store_n(&contexts[tid].state, new_state, __ATOMIC_SEQ_CST);

    WakeOne(false);
  }
  int GetCurrentState() override { 
    return (int)__atomic_load_n(&contexts[s_tid].state, __ATOMIC_SEQ_CST);
  }

  // ASSUME: SCHED_LOCK held
  void SetCurrentState(int new_state) override {
    __atomic_store_n(&contexts[s_tid].state, new_state, __ATOMIC_SEQ_CST);
  }
  // ASSUME: SCHED_LOCK held
  void SetState(u64 tid, int new_state) override {
    __atomic_store_n(&contexts[tid].state, new_state, __ATOMIC_SEQ_CST);
  }

  void SynchronizationPoint() override {
    auto tid = GetTid();
    auto old_state = __atomic_load_n(&contexts[tid].state, __ATOMIC_SEQ_CST);
    REAL(pthread_mutex_lock)(&my::SCHED_LOCK);
    if (old_state == ThreadState::RUNNING) {
      UnblockOne((int)ThreadState::WAIT);
    }
    REAL(pthread_mutex_unlock)(&my::SCHED_LOCK);

    //PrintStates();
    while (__atomic_load_n(&contexts[tid].state, __ATOMIC_SEQ_CST) == ThreadState::WAIT) {
      internal_sched_yield();
    }
  }

  int SynchronizationPoint_MutexLock(void* m) override {
    my::lock(m);
    return 0;
  }
  int SynchronizationPoint_MutexTryLock(void* m) override {
    return my::try_lock(m);
  }
  int SynchronizationPoint_MutexUnlock(void* m) override {
    my::unlock(m);
    return 0;
  }
  int SynchronizationPoint_CondWait(void* c, void* m) override {
    // No sync event here.
    my::wait(c, m);
    return 0;
  }
  int SynchronizationPoint_CondNotifyOne(void* c) override {
    my::notify_one(c);
    return 0;
  }
  int SynchronizationPoint_CondNotifyAll(void* c) override {
    my::notify_all(c);
    return 0;
  }
  void SynchronizationPoint_MutexInit(void* m, bool recursive) override {
    REAL(pthread_mutex_lock)(&my::BIGLOCK);
    // TODO: detect collisions
    auto itr = my::Mutexes.find(m);
    if (itr != my::Mutexes.end()) {
      my::Mutexes.erase(itr);
    }
    my::Mutexes.insert(m, my::mutex{recursive});
    REAL(pthread_mutex_unlock)(&my::BIGLOCK);
  }
  void SynchronizationPoint_CondInit(void* c) override {
    REAL(pthread_mutex_lock)(&my::BIGLOCK);
    // TODO: detect collisions
    auto itr = my::CVs.find(c);
    if (itr != my::CVs.end()) {
      my::CVs.erase(itr);
    }
    my::CVs.insert(c, {});
    REAL(pthread_mutex_unlock)(&my::BIGLOCK);
  }


  void DecrementExitCount(u64 tid) {
    DEBUG(fprintf(stderr, "[%ld] DecrementExitCount on %ld exit_count=%d\n", s_tid, tid, contexts[tid].exit_count));
    --contexts[tid].exit_count;
    if (contexts[tid].exit_count < 0) {
      Printf("FATAL: ThreadSanitizer exit_count < 0 for tid %d\n", tid);
      Die();
    }
  }

  // ASSUME: BIGLOCK held
  u64 FindTidFor(void* th) {
    u64 local_max_tid = __atomic_load_n(&s_max_tid, __ATOMIC_SEQ_CST);
    for (u64 i = 1; i <= local_max_tid; i++) {
      // TODO: clear out old thread_handles
      if (contexts[i].thread_handle == th && contexts[i].exit_count != 0) {
        return i;
      }
    }
    return (u64)-1;
  }

  int SynchronizationPoint_JoinThread(void* th, void** ret) override {
    if (!th) {
      DEADLOCK("OOPS: th null");
      while(1);
    }

    {
      REAL(pthread_mutex_lock)(&my::BIGLOCK);

      u64 join_tid = FindTidFor(th);
      if (join_tid != (u64)-1) {
        // join_tid will be -1 for any internal TSAN runtime threads that weren't
        // started by the app. TODO: We should handle those as well, so we can
        // have a proper assertion that join_tid is never -1.

        DEBUG(fprintf(stderr, "[%ld] JoinThread will enter loop waiting on %ld, state=%d exited=%d\n", s_tid, join_tid, contexts[join_tid].state, contexts[join_tid].exited));
        while (!contexts[join_tid].exited) {
          DEBUG(fprintf(stderr, "[%ld] JoinThread waiting on %ld\n", s_tid, join_tid));
          contexts[join_tid].ws.wait();
        }
        DEBUG(fprintf(stderr, "[%ld] JoinThread finished waiting on %ld\n", s_tid, join_tid));
        DecrementExitCount(join_tid);
      } else {
        DEBUG(fprintf(stderr, "[%ld] JoinThread NO WAIT\n", s_tid));
      }

      // To wake up any WAIT-ing threads.
      auto state = __atomic_load_n(&contexts[s_tid].state, __ATOMIC_SEQ_CST);
      DEBUG(fprintf(stderr, "[%ld] JoinThread will do sync with state %d\n", s_tid, state));

      REAL(pthread_mutex_unlock)(&my::BIGLOCK);
    }

    SynchronizationPoint();

    return REAL(pthread_join)(th, ret);
  }

  int SynchronizationPoint_DetachThread(void* th) override {
    REAL(pthread_mutex_lock)(&my::BIGLOCK);
    u64 detach_tid = FindTidFor(th);
    DEBUG(fprintf(stderr, "[%ld] DetachThread on %ld\n", s_tid, detach_tid, contexts[detach_tid].exit_count));
    DecrementExitCount(detach_tid);
    REAL(pthread_mutex_unlock)(&my::BIGLOCK);

    return REAL(pthread_detach)(th);
  }

  // InitThread is called before the background has actually started
  void SynchronizationPoint_InitThread(void* th) override {
    if (!th) {
      DEADLOCK("OOPS: th null");
      while(1);
    }
    REAL(pthread_mutex_lock)(&my::BIGLOCK);
    auto tid = AllocateTid();
    contexts[tid].thread_handle = th;
    contexts[tid].exited = false;
    contexts[tid].exit_count = 2;
    DEBUG(fprintf(stderr, "[%ld] InitThread new_tid %ld to running\n", s_tid, tid));
    contexts[tid].state = ThreadState::RUNNING; // TODO - is this right?
    REAL(pthread_mutex_unlock)(&my::BIGLOCK);
  }

  // Called by newly spawned thread, before the callback has started and before the
  // spawning thread has returned from pthread_create.
  void SynchronizationPoint_CreateThread(void* th) override {
    if (!th) {
      DEADLOCK("OOPS: th null");
      while(1);
    }
    REAL(pthread_mutex_lock)(&my::BIGLOCK);
    REAL(pthread_mutex_lock)(&my::SCHED_LOCK);

    // Assign s_tid from the allocated tid init InitThread.
    const u64 local_max_tid = __atomic_load_n(&s_max_tid, __ATOMIC_SEQ_CST);
    s_tid = -1;
    for (int i = 1; i <= local_max_tid; ++i) {
      if (contexts[i].thread_handle == th && !contexts[i].exited) {
        s_tid = i;
        break;
      }
    }
    if (s_tid == -1) {
      Printf("FATAL! ThreadSanitizer could not find allocated TID in InitThread\n");
      Die();
    }

    REAL(pthread_mutex_unlock)(&my::SCHED_LOCK);
    REAL(pthread_mutex_unlock)(&my::BIGLOCK);
  }
  // Called by thread that is about to exit
  void SynchronizationPoint_ExitThread() override {
    CHECK_RC(REAL(pthread_mutex_lock)(&my::BIGLOCK));
    CHECK_RC(REAL(pthread_mutex_lock)(&my::SCHED_LOCK));
    auto tid = GetTid();
    DEBUG(fprintf(stderr, "[%ld] ExitThread state=%d exited=%d\n", tid, contexts[tid].state, contexts[tid].exited));
    __atomic_store_n(&contexts[tid].state, ThreadState::UNKNOWN, __ATOMIC_SEQ_CST);
    contexts[tid].exited = true;
    contexts[tid].ws.notify_one_impl();
    DEBUG(fprintf(stderr, "[%ld] ExitThread notified state=%d\n", tid, __atomic_load_n(&contexts[tid].state, __ATOMIC_SEQ_CST)));
    DecrementExitCount(tid);
    CHECK_RC(REAL(pthread_mutex_unlock)(&my::SCHED_LOCK));
    REAL(pthread_mutex_unlock)(&my::BIGLOCK);

    // To wake up any WAIT-ing threads.
    WakeOneIfNeeded();
  }

  u64 GetNextTid() {

    u64 ready_tids[100] = {};
    int c = 0;

    const u64 local_max_tid = __atomic_load_n(&s_max_tid, __ATOMIC_SEQ_CST);
    for (u64 i = 1; i <= local_max_tid; i++) {
      ThreadState state = __atomic_load_n(&contexts[i].state, __ATOMIC_SEQ_CST);
      if (state != ThreadState::BLOCKED && state != ThreadState::UNKNOWN) {
        ready_tids[c++] = i;
      }
    }

    if (c == 0) {
      DEADLOCK("OOPS: th null");
      while (1);
    }

    auto choice = ready_tids[rand() % c];
#if 1
    DEBUG(fprintf(stderr, "[%ld] GetNextTid there were %d chose %ld [ ", s_tid, c, choice));
    for (int i = 0; i != c; ++i) {
      DEBUG(fprintf(stderr, "%ld ", ready_tids[i]));
    }
    DEBUG(fprintf(stderr, "]\n"));
#endif
    return choice;

#if 0

    const u64 local_max_tid = __atomic_load_n(&s_max_tid, __ATOMIC_SEQ_CST);
    const u64 next_tid = local_max_tid == 0 ? 1 : ((rand() % local_max_tid) + 1);
    for (u64 i = 0; i < local_max_tid; i++) {
      ThreadState state = __atomic_load_n(&contexts[(next_tid + i) % local_max_tid + 1].state, __ATOMIC_SEQ_CST);
      if (state != ThreadState::BLOCKED && state != ThreadState::UNKNOWN) {
        return (next_tid + i) % local_max_tid + 1;
      }
    }

    ThreadState state = __atomic_load_n(&contexts[next_tid].state, __ATOMIC_SEQ_CST);
    if (state == ThreadState::BLOCKED || state == ThreadState::UNKNOWN) {
      DEBUG(fprintf(stderr, "DEADLOCK\n"));
      DEADLOCK();
      if (getenv("KILL_DEADLOCK")) _exit(1);
      //while(1);
    }
    return next_tid;
#endif
  }

  void WakeOne(bool need_lock = true) {
    if (need_lock) CHECK_RC(REAL(pthread_mutex_lock)(&my::SCHED_LOCK));
    u64 next_tid = GetNextTid();
    DEBUG(fprintf(stderr, "[%ld] WakeOne waking %ld (whose state is %d)\n", s_tid, next_tid, __atomic_load_n(&contexts[next_tid].state, __ATOMIC_SEQ_CST)));
    __atomic_store_n(&contexts[next_tid].state, ThreadState::RUNNING, __ATOMIC_SEQ_CST);
    __atomic_store_n(&contexts[next_tid].start_time, NanoTime(), __ATOMIC_SEQ_CST);
    DEBUG(fprintf(stderr, "[%ld] WakeOne done storing %ld (whose state is %d)\n", s_tid, next_tid, __atomic_load_n(&contexts[next_tid].state, __ATOMIC_SEQ_CST)));
    if (need_lock) CHECK_RC(REAL(pthread_mutex_unlock)(&my::SCHED_LOCK));
  }

  void WakeOneIfNeeded() {
    LockGuard lg(&my::SCHED_LOCK);

    const u64 local_max_tid = __atomic_load_n(&s_max_tid, __ATOMIC_SEQ_CST);
    for (u64 i = 1; i <= local_max_tid; i++) {
      ThreadState state = __atomic_load_n(&contexts[i].state, __ATOMIC_SEQ_CST);
      if (state == ThreadState::RUNNING) {
        return;
      }
    }

    u64 next_tid = GetNextTid();
    DEBUG(fprintf(stderr, "[%ld] WakeOneIfNeeded waking %ld\n", s_tid, next_tid));
    __atomic_store_n(&contexts[next_tid].state, ThreadState::RUNNING, __ATOMIC_SEQ_CST);
    __atomic_store_n(&contexts[next_tid].start_time, NanoTime(), __ATOMIC_SEQ_CST);
  }

  void* WatchDog() {
    while (true) {
      usleep(20 * 1000);
      internal_sched_yield();
      CHECK_RC(REAL(pthread_mutex_lock)(&my::SCHED_LOCK));
      u64 local_max_tid = __atomic_load_n(&s_max_tid, __ATOMIC_SEQ_CST);
      for (u64 i = 1; i <= local_max_tid; i++) {
        if (__atomic_load_n(&contexts[i].state, __ATOMIC_SEQ_CST) == ThreadState::RUNNING && __atomic_load_n(&contexts[i].start_time, __ATOMIC_SEQ_CST) + 20 * 1000ULL <= NanoTime()) {
          DEBUG(fprintf(stderr, "[-1] WatchDog timing out %ld\n", i));
          __atomic_store_n(&contexts[i].state, ThreadState::OUT_TIME, __ATOMIC_SEQ_CST);
        }
      }
      bool exists_running = false;
      for (u64 i = 1; i <= local_max_tid; i++) {
        if (__atomic_load_n(&contexts[i].state, __ATOMIC_SEQ_CST) == ThreadState::RUNNING) {
          exists_running = true;
        }
      }
      if (!exists_running) {
        WakeOne(false);
      }
      CHECK_RC(REAL(pthread_mutex_unlock)(&my::SCHED_LOCK));
    }
    return nullptr;
  }

};

IFuzzingScheduler& FuzzingSchedulerDispatcher() {
  if (!strcmp(flags()->fuzzing_scheduler, "")) {
    auto* scheduler = static_cast<NullFuzzingScheduler *>(InternalCalloc(1, sizeof(NullFuzzingScheduler)));
    new (scheduler) NullFuzzingScheduler;
    return *scheduler;
  } else if (!strcmp(flags()->fuzzing_scheduler, "random")) {
    auto* scheduler = static_cast<RandomFuzzingScheduler *>(InternalCalloc(1, sizeof(RandomFuzzingScheduler)));
    new (scheduler) RandomFuzzingScheduler;
    Printf("WARNING! ThreadSanitizer launched under the management of a random fuzzing scheduler new\n");
    return *scheduler;
  } else {
    Printf("FATAL: ThreadSanitizer invalid fuzzing scheduler. Please check TSAN_OPTIONS!\n");
    Die();
  }
}

}

IFuzzingScheduler& GetFuzzingScheduler() {
  static IFuzzingScheduler& scheduler = FuzzingSchedulerDispatcher();
  return scheduler;
}

}  // namespace __tsan
