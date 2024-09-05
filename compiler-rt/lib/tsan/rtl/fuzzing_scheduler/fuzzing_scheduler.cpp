#include "fuzzing_scheduler.h"
#include <tsan_rtl.h>
#include <sanitizer_common/sanitizer_allocator_internal.h>
#include <sanitizer_common/sanitizer_placement_new.h>
#include <interception/interception.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <syscall.h>

#include "sanitizer_common/sanitizer_errno_codes.h"

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
  extern int (*real_usleep)(long);
}

namespace __tsan {

namespace {

  constexpr size_t MAX_THREADS = 65536;

  struct LockGuard {
    pthread_mutex_t* Mtx;

    LockGuard(LockGuard&&);
    LockGuard& operator=(LockGuard&&);

    LockGuard(pthread_mutex_t* Mtx) : Mtx(Mtx) {
      REAL(pthread_mutex_lock)(Mtx);
    }
    ~LockGuard() {
      REAL(pthread_mutex_unlock)(Mtx);
    }
  };

  struct UnlockGuard {
    pthread_mutex_t* Mtx;

    UnlockGuard(UnlockGuard&&);
    UnlockGuard& operator=(UnlockGuard&&);

    UnlockGuard(pthread_mutex_t* Mtx) : Mtx(Mtx) {
      REAL(pthread_mutex_unlock)(Mtx);
    }
    ~UnlockGuard() {
      REAL(pthread_mutex_lock)(Mtx);
    }
  };

  constexpr u64 NO_TID = (u64)-1;

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
      fprintf(stderr, "Failed with res %d\n", res); \
    } \
  } while (0)

//#define PRINT_DEBUG
#ifdef PRINT_DEBUG
#define DEBUG(x) x
#else
#define DEBUG(x)
#endif

struct NullFuzzingScheduler : IFuzzingScheduler {
  void SynchronizationPoint() override {
  }
  void SetBlocking(bool IsBlocking) override {}
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

struct DelayFuzzingScheduler : NullFuzzingScheduler {
  void SynchronizationPoint() override {
    static int delay_microseconds = getenv("TSAN_SCHEDULE_DELAY") ? strtol(getenv("TSAN_SCHEDULE_DELAY"), nullptr, 10) : 1000;
    REAL(usleep)(rand() % delay_microseconds);
  }
};

static void DEADLOCK(const char* msg)
{
  fprintf(stderr, "DEADLOCK %s\n", msg);
  abort();
}

namespace impl {

static pthread_mutex_t BIGLOCK;
static pthread_cond_t BIGCV;
static int x = [] {
  CHECK_RC(REAL(pthread_mutex_init)(&BIGLOCK, nullptr));
  CHECK_RC(REAL(pthread_cond_init)(&BIGCV, nullptr));
  return 0;
}();

#define Assert0(c) Assert(c, "")
#define Assert(c, fmt) \
  do { \
    bool e = (c); \
    if (!e) { \
      Report("Assertion '" #c "' Failed: " fmt); \
      while(1); \
      Die(); \
    } \
  } while(0)
#define AssertN(c, fmt, ...) \
  do { \
    bool e = (c); \
    if (!e) { \
      Report("Assertion '" #c "' Failed: " fmt, __VA_ARGS__); \
      Die(); \
    } \
  } while(0)

// dumb_map has map-like interfaces, but it's implemented as if it
// were a hashmap with a hash function equal to a constant function.
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
    Assert(false, "No more space in dumb_map");
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

struct ThreadContext {
  ThreadState state = ThreadState::UNKNOWN;
  void* thread_handle = nullptr; // pointer to pthread_t object

  // 'exit_count' tracks each spawned thread's lifetime. 'exit_count'
  // starts out as 2 upon thread creation. It is decremented once
  // when the user supplied callback completes. It is also decremented
  // in the pthread_join, or pthread_detect (calling both is undefined
  // behavior per the pthread spec). Whenever exit_count hits zero, the
  // corresponding ThreadContext object can be recycled to a newly
  // spawned thread.
  int exit_count = 2;
  u64 start_time = 0;
  u64 real_tid = 0;

  bool is_free() const { return exit_count == 0; }
};

// ASSUME: BIGLOCK held while accessing ThreadContexts
struct ThreadContexts {
  ThreadContext Contexts[MAX_THREADS] = {};
  int blocked_calls = 0;

  void SetState(u64 tid, ThreadState NewState) {
    Contexts[tid].state = NewState;
  }
  ThreadState GetCurrentState() {
    return Contexts[s_tid].state;
  }

  void SetCurrentState(ThreadState NewState) {
    Contexts[s_tid].state = NewState;
  }

  u64 GetNextTid() {
    u64 ready_tids[100] = {};
    int c = 0;

    for (u64 i = 1; i <= s_max_tid; i++) {
      ThreadState state = Contexts[i].state;
      if (state != ThreadState::BLOCKED && state != ThreadState::UNKNOWN) {
        ready_tids[c++] = i;
      }
    }

    if (c == 0)
      if (blocked_calls == 0)
        DEADLOCK("OOPS: no ready threads");
      else
        return NO_TID;

    auto choice = ready_tids[rand() % c];
#ifdef PRINT_DEBUG
    DEBUG(fprintf(stderr, "[%ld] GetNextTid there were %d chose %ld [ ", s_tid, c, choice));
    for (int i = 0; i != c; ++i) {
      DEBUG(fprintf(stderr, "%ld ", ready_tids[i]));
    }
    DEBUG(fprintf(stderr, "]\n"));
#endif
    return choice;
  }

  void WakeOne() {
    u64 next_tid = GetNextTid();
    if (next_tid == NO_TID) {
      return;
    }

    DEBUG(fprintf(stderr, "[%ld] WakeOne waking %ld (whose state is %d)\n", s_tid, next_tid, Contexts[next_tid].state));
    Contexts[next_tid].state = ThreadState::RUNNING;
    Contexts[next_tid].start_time = NanoTime();
    DEBUG(fprintf(stderr, "[%ld] WakeOne done storing %ld (whose state is %d)\n", s_tid, next_tid, Contexts[next_tid].state));
  }
  void UnblockOne(ThreadState NewState) {
    auto tid = s_tid;
    Contexts[tid].state = NewState;

    WakeOne();
  }
};

struct monostate{};

struct waitset {
  dumb_map<u64, monostate, 100> waiters;

  waitset() = default;
  waitset(const waitset&) = delete;
  waitset& operator=(const waitset&) = delete;
  waitset(waitset&&) = default;
  waitset& operator=(waitset&&) = default;

  void wait(ThreadContexts& Contexts) {
    Assert0(!waiters.count(s_tid));
    waiters.insert(s_tid, {});

    while (waiters.count(s_tid)) {
      ThreadState OldState = Contexts.GetCurrentState();
      Contexts.UnblockOne(ThreadState::BLOCKED);

      CHECK_RC(REAL(pthread_cond_wait)(&BIGCV, &BIGLOCK));
      DEBUG(fprintf(stderr, "[%ld] Waitset::wait waking up with current state=%d new state=%d\n", s_tid, Contexts.Contexts[s_tid].state, OldState));

      Contexts.SetCurrentState(OldState);
    }
  }

  void notify_one(ThreadContexts& Contexts) {
    // Lock held by the caller

    auto itr = waiters.begin();
    if (itr != waiters.end()) {
      waiters.erase(itr);
      // TODO - should this be the old_state from the wait() call above?
      DEBUG(fprintf(stderr, "[%ld] Waitset::notify_one setting state of %ld to WAIT\n", s_tid, itr->key));
      Contexts.SetState(itr->key, ThreadState::WAIT);

      CHECK_RC(REAL(pthread_cond_broadcast)(&BIGCV));
    }
  }

  void notify_all(ThreadContexts& Contexts) {
    if (waiters.begin() == waiters.end()) return;

    while (waiters.begin() != waiters.end()) {
      auto itr = waiters.begin();
      if (itr != waiters.end()) {
        waiters.erase(itr);
        Contexts.SetState(itr->key, ThreadState::WAIT);
      }
    }
    CHECK_RC(REAL(pthread_cond_broadcast)(&BIGCV));
  }
};

struct mutex {
  size_t owner = FREE;
  waitset ws;
  bool is_recursive = true;
  int recurse_count = 0;

  mutex(bool is_recursive = false) : is_recursive(is_recursive) {
  }

  void lock(ThreadContexts& Contexts) {
    // TODO: assert mutex locked
    {
      UnlockGuard unlockGuard(&BIGLOCK);
      GetFuzzingScheduler().SynchronizationPoint();
    }

    if (owner != s_tid) {
      while (owner != FREE) {
        ws.wait(Contexts);
      }
    }

    if (owner == FREE && recurse_count != 0) {
      DEADLOCK("recurse_count not 0 in lock");
    }

#if 0
    // Always support recursive mutex behavior, and let TSAN itself
    // detect locking a locked mutex that is not configured as recursive.

    if (owner != FREE && !is_recursive) {
      Printf("FATAL: ThreadSanitizer detected attempt to lock non-recursive mutex that was already locked");
      Die();
    }
#endif

    owner = s_tid;
    ++recurse_count;
  }

  int try_lock() {
    // TODO: assert mutex locked
    {
      UnlockGuard unlockGuard(&BIGLOCK);
      GetFuzzingScheduler().SynchronizationPoint();
    }

    if (owner == s_tid) {
      if (is_recursive) {
        ++recurse_count;
        return 0;
      } else {
        return errno_EBUSY;
      }
    }

    if (owner != FREE) {
      return errno_EBUSY;
    }

    if (owner == FREE && recurse_count != 0) {
      DEADLOCK("recurse_count not 0 in try_lock");
    }

    owner = s_tid;
    ++recurse_count;
    return 0;
  }

  void unlock(ThreadContexts& Contexts) {
    // TODO: assert mutex locked
    {
      UnlockGuard unlockGuard(&BIGLOCK);
      GetFuzzingScheduler().SynchronizationPoint();
    }

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
    ws.notify_one(Contexts);
  }

  static constexpr size_t FREE = (size_t)-1;
};

struct condition_variable {
  waitset ws;

  void wait(mutex* m, ThreadContexts& Contexts) {
    // ASSERT: mutex locked

    m->unlock(Contexts);
    ws.wait(Contexts);
    m->lock(Contexts);
  }

  void notify_one(ThreadContexts& Contexts) {
    // TODO: assert mutex locked
    {
      UnlockGuard unlockGuard(&BIGLOCK);
      GetFuzzingScheduler().SynchronizationPoint();
    }

    ws.notify_one(Contexts);
  }

  void notify_all(ThreadContexts& Contexts) {
    // TODO: assert mutex locked
    {
      UnlockGuard unlockGuard(&BIGLOCK);
      GetFuzzingScheduler().SynchronizationPoint();
    }

    ws.notify_all(Contexts);
  }

  static constexpr size_t FREE = (size_t)-1;
};

} // namespace impl

struct RandomFuzzingScheduler : IFuzzingScheduler {
  RandomFuzzingScheduler() {
    unsigned int seed;
    if (getenv("TSAN_RAND_SEED")) {
      seed = strtol(getenv("TSAN_RAND_SEED"), nullptr, 10);
    } else {
      seed = NanoTime();
    }
    Printf("INFO! ThreadSanitizer initialized RandomFuzzingScheduler with seed=%u\n", seed);
    s_max_tid = 1;
    s_tid = 1;
    srand(seed);
    pthread_t t;
    REAL(pthread_create)(&t, NULL, reinterpret_cast<void*(*)(void*)>(&RandomFuzzingScheduler::WatchDog), this);
    Contexts.Contexts[1].state = ThreadState::RUNNING;
    Contexts.Contexts[1].exit_count = 2;
    //REAL(pthread_detach)(&t);
  }


private:

  impl::ThreadContexts Contexts;
  impl::waitset ws[MAX_THREADS];
  impl::dumb_map<void*, impl::mutex, 1000> Mutexes;
  impl::dumb_map<void*, impl::condition_variable, 1000> CVs;

  // ASSUME: BIGLOCK held
  u64 AllocateTid() {
    for (int i = 1; i <= s_max_tid; ++i) {
      if (Contexts.Contexts[i].exit_count == 0) {
        return i;
      }
    }
    u64 new_tid = ++s_max_tid;
    if (new_tid >= MAX_THREADS) {
      Printf("FATAL: ThreadSanitizer The maximum number of threads created during the program should not exceed %lu", MAX_THREADS - 1);
      Die();
    }
    return new_tid;
  }

  void WakeOneIfNeeded() {
    for (u64 i = 1; i <= s_max_tid; i++) {
      ThreadState state = Contexts.Contexts[i].state;
      if (state == ThreadState::RUNNING) {
        return;
      }
    }

    u64 next_tid = Contexts.GetNextTid();
    if (next_tid == NO_TID) {
      return;
    }
    DEBUG(fprintf(stderr, "[%ld] WakeOneIfNeeded waking %ld\n", s_tid, next_tid));
    Contexts.Contexts[next_tid].state = ThreadState::RUNNING;
    Contexts.Contexts[next_tid].start_time = NanoTime();
  }

  // No lock held upon entry
  void SetBlocking(bool IsBlocking) override {
    LockGuard lg(&impl::BIGLOCK);

    DEBUG(fprintf(stderr, "[%ld] SetBlocking IsBlocking=%d current %d\n", s_tid, IsBlocking, Contexts.Contexts[s_tid].state));
    if (IsBlocking) {
      if (Contexts.Contexts[s_tid].state != ThreadState::RUNNING && Contexts.Contexts[s_tid].state != ThreadState::OUT_TIME) {
        DEADLOCK("Unexpected state for SetBlocking(true)");
      }
      Contexts.Contexts[s_tid].state = ThreadState::BLOCKED;
      ++Contexts.blocked_calls;
      WakeOneIfNeeded();
    } else {
      --Contexts.blocked_calls;
      Contexts.Contexts[s_tid].state = ThreadState::RUNNING;
    }
  }

  // No lock held upon call
  void SynchronizationPoint() override {
    {
      LockGuard lg(&impl::BIGLOCK);
      auto OldState = Contexts.Contexts[s_tid].state;
      if (OldState == ThreadState::RUNNING || OldState == ThreadState::OUT_TIME) {
        Contexts.UnblockOne(ThreadState::WAIT);
      }
    }

    while (Contexts.Contexts[s_tid].state == ThreadState::WAIT) {
      internal_sched_yield();
    }
  }

  void EnsureMutex(void* Mtx) {
    if (!Mutexes.count(Mtx)) {
      Mutexes.insert(Mtx, {});
    }
  }
  void EnsureCV(void* CV) {
    if (!CVs.count(CV)) {
      CVs.insert(CV, {});
    }
  }

  int SynchronizationPoint_MutexLock(void* Mtx) override {
    LockGuard lg(&impl::BIGLOCK);
    EnsureMutex(Mtx);

    auto& m = Mutexes.find(Mtx)->value;
    m.lock(Contexts);
    return 0;
  }
  int SynchronizationPoint_MutexTryLock(void* Mtx) override {
    LockGuard lg(&impl::BIGLOCK);
    EnsureMutex(Mtx);

    auto& m = Mutexes.find(Mtx)->value;
    return m.try_lock();
  }
  int SynchronizationPoint_MutexUnlock(void* Mtx) override {
    LockGuard lg(&impl::BIGLOCK);
    Assert0(Mutexes.count(Mtx));

    auto& m = Mutexes.find(Mtx)->value;
    m.unlock(Contexts);
    return 0;
  }
  int SynchronizationPoint_CondWait(void* CV, void *Mtx) override {
    // No sync event here.
    LockGuard lg(&impl::BIGLOCK);
    EnsureCV(CV);
    Assert0(Mutexes.count(Mtx));

    auto& M = Mutexes.find(Mtx)->value;
    auto& C = CVs.find(CV)->value;
    C.wait(&M, Contexts);
    return 0;
  }
  int SynchronizationPoint_CondNotifyOne(void* CV) override {
    LockGuard lg(&impl::BIGLOCK);
    EnsureCV(CV);

    auto& C = CVs.find(CV)->value;
    C.notify_one(Contexts);
    return 0;
  }
  int SynchronizationPoint_CondNotifyAll(void* CV) override {
    LockGuard lg(&impl::BIGLOCK);
    EnsureCV(CV);

    auto& C = CVs.find(CV)->value;
    C.notify_all(Contexts);
    return 0;
  }
  void SynchronizationPoint_MutexInit(void* m, bool recursive) override {
    LockGuard lg(&impl::BIGLOCK);

    // TODO: detect collisions
    auto itr = Mutexes.find(m);
    if (itr != Mutexes.end()) {
      Mutexes.erase(itr);
    }
    Mutexes.insert(m, impl::mutex{recursive});
  }
  void SynchronizationPoint_CondInit(void* c) override {
    LockGuard lg(&impl::BIGLOCK);

    // TODO: detect collisions
    auto itr = CVs.find(c);
    if (itr != CVs.end()) {
      CVs.erase(itr);
    }
    CVs.insert(c, {});
  }

  void DecrementExitCount(u64 tid) {
    DEBUG(fprintf(stderr, "[%ld] DecrementExitCount on %ld exit_count=%d\n", s_tid, tid, Contexts.Contexts[tid].exit_count));
    int NewCount = --Contexts.Contexts[tid].exit_count;
    if (NewCount < 0) {
      Printf("FATAL: ThreadSanitizer exit_count < 0 for tid %llu\n", tid);
      Die();
    } else if (NewCount == 0) {
      Contexts.Contexts[tid].real_tid = 0;
    }
  }

  // ASSUME: BIGLOCK held
  u64 FindTidFor(void* th) {
    for (u64 i = 1; i <= s_max_tid; i++) {
      // TODO: clear out old thread_handles
      if (Contexts.Contexts[i].thread_handle == th && Contexts.Contexts[i].exit_count != 0) {
        return i;
      }
    }
    return (u64)-1;
  }

  int SynchronizationPoint_JoinThread(void* th, void** ret) override {
    if (!th) {
      DEADLOCK("OOPS: th null");
    }

    {
      LockGuard lg(&impl::BIGLOCK);

      u64 join_tid = FindTidFor(th);
      if (join_tid != (u64)-1) {
        // join_tid will be -1 for any internal TSAN runtime threads that weren't
        // started by the app. TODO: We should handle those as well, so we can
        // have a proper assertion that join_tid is never -1.

        DEBUG(fprintf(stderr, "[%ld] JoinThread will enter loop waiting on %ld, state=%d exited=%d\n", s_tid, join_tid, Contexts.Contexts[join_tid].state, Contexts.Contexts[join_tid].exit_count));
        DecrementExitCount(join_tid);
        while (!Contexts.Contexts[join_tid].is_free()) {
          DEBUG(fprintf(stderr, "[%ld] JoinThread waiting on %ld\n", s_tid, join_tid));
          ws[join_tid].wait(Contexts);
        }
        DEBUG(fprintf(stderr, "[%ld] JoinThread finished waiting on %ld\n", s_tid, join_tid));
      } else {
        DEBUG(fprintf(stderr, "[%ld] JoinThread joining a thread not managed by us\n", s_tid));
      }

      // To wake up any WAIT-ing threads.
      auto state = Contexts.Contexts[s_tid].state;
      DEBUG(fprintf(stderr, "[%ld] JoinThread will do sync with state %d\n", s_tid, state));

    }

    SynchronizationPoint();

    return REAL(pthread_join)(th, ret);
  }

  int SynchronizationPoint_DetachThread(void* th) override {
    {
      LockGuard lg(&impl::BIGLOCK);
      u64 detach_tid = FindTidFor(th);
      DEBUG(fprintf(stderr, "[%ld] DetachThread on %ld\n", s_tid, detach_tid, Contexts.Contexts[detach_tid].exit_count));
      DecrementExitCount(detach_tid);
    }

    return REAL(pthread_detach)(th);
  }

  // InitThread is called before the background has actually started
  void SynchronizationPoint_InitThread(void* th) override {
    if (!th) {
      DEADLOCK("OOPS: th null");
    }

    LockGuard lg(&impl::BIGLOCK);
    auto tid = AllocateTid();
    Contexts.Contexts[tid].thread_handle = th;
    Contexts.Contexts[tid].exit_count = 2;
    Contexts.Contexts[tid].real_tid = syscall(SYS_gettid);
    DEBUG(fprintf(stderr, "[%ld] InitThread new_tid %ld to running\n", s_tid, tid));
    Contexts.Contexts[tid].state = ThreadState::RUNNING; // TODO - is this right?
  }

  // Called by newly spawned thread, before the callback has started and before the
  // spawning thread has returned from pthread_create.
  void SynchronizationPoint_CreateThread(void* th) override {
    if (!th) {
      DEADLOCK("OOPS: th null");
    }

    LockGuard lg(&impl::BIGLOCK);

    // Assign s_tid from the allocated tid init InitThread.
    s_tid = -1;
    for (int i = 1; i <= s_max_tid; ++i) {
      if (Contexts.Contexts[i].thread_handle == th && !Contexts.Contexts[i].is_free()) {
        s_tid = i;
        break;
      }
    }
    if (s_tid == -1) {
      Printf("FATAL! ThreadSanitizer could not find allocated TID in InitThread\n");
      Die();
    }
  }
  // Called by thread that is about to exit
  void SynchronizationPoint_ExitThread() override {
    LockGuard lg(&impl::BIGLOCK);

    auto tid = s_tid;
    DEBUG(fprintf(stderr, "[%ld] ExitThread state=%d exit_count=%d\n", tid, Contexts.Contexts[tid].state, Contexts.Contexts[tid].exit_count));
    Contexts.Contexts[tid].state = ThreadState::UNKNOWN;
    ws[tid].notify_one(Contexts);
    DEBUG(fprintf(stderr, "[%ld] ExitThread notified state=%d\n", tid, Contexts.Contexts[tid].state));
    DecrementExitCount(tid);

    // To wake up any WAIT-ing threads.
    WakeOneIfNeeded();
  }

  void* WatchDog() {
    while (true) {
      usleep(20 * 1000);
      internal_sched_yield();

      LockGuard lg(&impl::BIGLOCK);
      for (u64 i = 1; i <= s_max_tid; i++) {
        if (Contexts.Contexts[i].state == ThreadState::RUNNING && Contexts.Contexts[i].start_time + 20 * 1000ULL <= NanoTime()) {
          DEBUG(fprintf(stderr, "[-1] WatchDog timing out %ld\n", i));
          Contexts.Contexts[i].state = ThreadState::OUT_TIME;
        }
      }
      bool exists_running = false;
      for (u64 i = 1; i <= s_max_tid; i++) {
        if (Contexts.Contexts[i].state == ThreadState::RUNNING) {
          exists_running = true;
        }
      }
      if (!exists_running) {
        Contexts.WakeOne();
      }
    }
    return nullptr;
  }

};

IFuzzingScheduler& FuzzingSchedulerDispatcher() {
  if (!internal_strcmp(flags()->fuzzing_scheduler, "")) {
    auto* scheduler = static_cast<NullFuzzingScheduler *>(InternalCalloc(1, sizeof(NullFuzzingScheduler)));
    new (scheduler) NullFuzzingScheduler;
    return *scheduler;
  } else if (!internal_strcmp(flags()->fuzzing_scheduler, "random")) {
    auto* scheduler = static_cast<RandomFuzzingScheduler *>(InternalCalloc(1, sizeof(RandomFuzzingScheduler)));
    new (scheduler) RandomFuzzingScheduler;
    Printf("WARNING! ThreadSanitizer launched under the management of a random fuzzing scheduler new\n");
    return *scheduler;
  } else if (!internal_strcmp(flags()->fuzzing_scheduler, "delay")) {
    auto* scheduler = static_cast<DelayFuzzingScheduler *>(InternalCalloc(1, sizeof(DelayFuzzingScheduler)));
    new (scheduler) DelayFuzzingScheduler;
    Printf("WARNING! ThreadSanitizer launched under the management of a random delay fuzzing scheduler new\n");
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
