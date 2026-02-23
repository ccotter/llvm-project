//===-- tsan_simulate.cpp -------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file is a part of ThreadSanitizer (TSan), a race detector.
//
// Simulation scheduler for systematic thread interleaving exploration.
// Inspired by Relacy Race Detector's random scheduler.
//
// Design:
//   - Exactly one thread runs user code at a time.
//   - Other threads park on per-thread Semaphores.
//   - At scheduling points (atomic ops, pthread_* calls), the running thread
//     may yield to another thread chosen by the random scheduler.
//   - For blocking OS calls (pthread_join, pthread_mutex_lock when contended),
//     the calling thread marks itself as Blocked before the OS call and
//     re-registers as Running after the OS call returns.
//   - A trylock-loop is used for pthread_mutex_lock to avoid deadlocking
//     when the mutex holder is parked by the scheduler.
//
//===----------------------------------------------------------------------===//

#include "tsan_simulate.h"

#include "sanitizer_common/sanitizer_atomic.h"
#include "sanitizer_common/sanitizer_mutex.h"
#include "sanitizer_common/sanitizer_placement_new.h"
#include "tsan_flags.h"
#include "tsan_rtl.h"

extern "C" void* pthread_self();

namespace __tsan {

struct SimThread {
  enum State : u32 {
    Unused = 0,
    Running,  // Runnable — may be selected by the scheduler.
    Blocked,  // Blocked on mutex/condvar — scheduler must not pick this thread.
    Finished,  // Thread has exited the simulation.
  };

  Semaphore sem;
  State state;
  uptr thread_handle;  // This thread's pthread_t (from pthread_self())
  uptr joining_on;     // pthread_t this thread is joining on (0 if not joining)
};

static constexpr int kMaxSimThreads = 64;

static int sim_current_iteration = 0;

static atomic_uint32_t sim_max_depth_hit;
static atomic_uint32_t sim_race_detected;
static atomic_uint32_t sim_unsupported_interceptor_called;

void SimulateReportUnsupportedImpl(const char* func_name) {
  atomic_store_relaxed(&sim_unsupported_interceptor_called, 1);
  Printf(
      "ThreadSanitizer: simulation error - unsupported interceptor called: "
      "%s\n"
      "Simulation does not support this synchronization primitive.\n",
      func_name);
}

void SimulateReportRaceImpl() {
  atomic_store_relaxed(&sim_race_detected, 1);
  Printf("ThreadSanitizer: data race detected at iteration %d\n",
         sim_current_iteration);
}

void SimulateReportDeadlock() {
  Printf(
      "ThreadSanitizer: deadlock detected at iteration %d - all threads are "
      "blocked\n",
      sim_current_iteration);
  Printf(
      "ThreadSanitizer: to reproduce, set "
      "TSAN_OPTIONS=simulate_start_iteration=%d\n",
      sim_current_iteration);
  Die();
}

// Waitset: tracks threads blocked waiting for a resource (mutex or condvar).
struct Waitset {
  static constexpr int kMaxWaiters = kMaxSimThreads;
  int waiters[kMaxWaiters];
  int count;

  Waitset() : count(0) { internal_memset(waiters, 0, sizeof(waiters)); }

  void AddWaiter(int thread_idx) {
    CHECK_LT(count, kMaxWaiters);
    waiters[count++] = thread_idx;
  }

  // Randomly select and remove one thread from the waitset.
  // Matches Relacy's approach to maximize interleaving exploration.
  int RemoveOne(u32* rng_state) {
    CHECK_GT(count, 0);
    // Pick a random thread from the waitset.
    int idx = RandN(rng_state, count);
    int thread_idx = waiters[idx];
    // Remove it by shifting remaining threads.
    for (int i = idx + 1; i < count; i++) waiters[i - 1] = waiters[i];
    count--;
    return thread_idx;
  }

  // Remove all threads and return count.
  int RemoveAll(int* out_threads) {
    int n = count;
    for (int i = 0; i < count; i++) out_threads[i] = waiters[i];
    count = 0;
    return n;
  }
};

// ---------------------------------------------------------------------------
// Simulation scheduler
// ---------------------------------------------------------------------------

// Controls which thread runs at each scheduling point. Exactly one thread is
// designated as "current" and executes user code. Other runnable threads
// park on their per-thread semaphore until the scheduler selects them.
class SimScheduler {
 public:
  SimScheduler() : current_(-1), thread_count_(0), depth_(0) {
    internal_memset(threads_, 0, sizeof(threads_));
    // Cache and validate schedule_probability once at initialization
    int prob = flags()->simulate_schedule_probability;
    if (prob < 0)
      prob = 0;
    else if (prob > 100)
      prob = 100;
    schedule_probability_ = prob;
  }

  // Register a new thread. Returns its scheduler index.
  int AddThread() {
    SpinMutexLock lock(&mtx_);
    CHECK_LT(thread_count_, kMaxSimThreads);
    int idx = thread_count_++;
    threads_[idx].state = SimThread::Running;
    threads_[idx].thread_handle = 0;
    threads_[idx].joining_on = 0;
    return idx;
  }

  // Store this thread's pthread_t handle.
  void SetThreadHandle(int idx, uptr handle) {
    SpinMutexLock lock(&mtx_);
    threads_[idx].thread_handle = handle;
  }

  // Reset scheduler state for a new iteration.
  void ResetForIteration() {
    SpinMutexLock lock(&mtx_);
    current_ = -1;
    thread_count_ = 0;
    depth_ = 0;
    internal_memset(threads_, 0, sizeof(threads_));
    mutex_waitset_count_ = 0;
    cond_waitset_count_ = 0;
    annotate_waitset_count_ = 0;
  }

  // Seed the RNG and post the first runnable thread's semaphore.
  void StartIteration(u32 seed) {
    SpinMutexLock lock(&mtx_);
    rng_state_ = seed;
    depth_ = 0;
    // Pick the first Running thread (should be the main thread at idx 0).
    for (int i = 0; i < thread_count_; i++) {
      if (threads_[i].state == SimThread::Running) {
        current_ = i;
        threads_[i].sem.Post();
        return;
      }
    }
    current_ = -1;
  }

  void DumpStates(int chosen = -1, int current = -1) {
    // Debug: print thread states and scheduling decision.
    if (common_flags()->verbosity >= 2) {
      if (chosen >= 0) {
        Printf("Chose tid %d to run", chosen);
        if (current >= 0)
          Printf(" (current %d)", current);
        Printf(" - ");
      }
      Printf("Thread states: ");
      for (int i = 0; i < thread_count_; i++) {
        const char* state_str = "?";
        switch (threads_[i].state) {
          case SimThread::Unused:
            state_str = "Unused";
            break;
          case SimThread::Running:
            state_str = "Running";
            break;
          case SimThread::Blocked:
            state_str = "Blocked";
            break;
          case SimThread::Finished:
            state_str = "Finished";
            break;
        }
        Printf("[%d:%s] ", i, state_str);
      }
      Printf("\n");
    }
  }

  // ------- Scheduling point (non-blocking) -------
  //
  // Called by the currently running thread. May randomly switch to another
  // runnable thread. If the caller is NOT the current thread (e.g. during a
  // brief window around a blocking call), this is a no-op.
  void Schedule(int caller_idx) {
    // If max depth already hit, stop all scheduling.
    if (atomic_load_relaxed(&sim_max_depth_hit)) {
      return;
    }

    mtx_.Lock();

    if (caller_idx != current_) {
      // Not the current thread — no-op. This can happen briefly during
      // blocking-call transitions.
      // TODO - is this tue ^^ ??
      mtx_.Unlock();
      return;
    }

    int max_depth = flags()->simulate_max_depth;
    if (++depth_ > max_depth) {
      atomic_store_relaxed(&sim_max_depth_hit, 1);
      Printf("ThreadSanitizer: simulation hit max depth %d at iteration %d\n",
             max_depth, sim_current_iteration);
      mtx_.Unlock();
      return;
    }

    // Count runnable threads.
    int runnable = CountRunnable();
    if (runnable <= 1) {
      mtx_.Unlock();
      return;
    }

    // Random scheduling: pick a random runnable thread.
    int chosen = PickRandomRunnable(runnable);

    DumpStates(chosen, caller_idx);

    if (chosen == caller_idx) {
      // Random picked us — keep running.
      mtx_.Unlock();
      return;
    }

    // Context switch: wake the chosen thread, park ourselves.
    current_ = chosen;
    threads_[chosen].sem.Post();
    mtx_.Unlock();
    threads_[caller_idx].sem.Wait();
  }

  // ------- New thread lifecycle -------

  // Called by a newly created thread after AddThread(). If no thread is
  // currently running (current_ == -1), the new thread becomes current and
  // returns immediately. Otherwise it parks until the scheduler selects it.
  void ThreadStart(int idx) {
    mtx_.Lock();
    if (current_ == -1) {
      current_ = idx;
      mtx_.Unlock();
      return;
    }
    mtx_.Unlock();
    threads_[idx].sem.Wait();
  }

  // Called when a thread finishes its user callback. Removes the thread from
  // the runnable set and wakes the next runnable thread (if any).
  // Also wakes any thread that was blocked joining on this thread.
  void ThreadFinish(int idx) {
    mtx_.Lock();
    threads_[idx].state = SimThread::Finished;
    uptr my_handle = threads_[idx].thread_handle;

    // Find any thread that was joining on this thread and make it runnable.
    if (my_handle != 0) {
      for (int i = 0; i < thread_count_; i++) {
        if (threads_[i].joining_on == my_handle) {
          threads_[i].state = SimThread::Running;
          threads_[i].joining_on = 0;
        }
      }
    }

    if (idx != current_) {
      mtx_.Unlock();
      return;
    }

    // We were current. Pick next runnable thread.
    PickNextAndWake();
    mtx_.Unlock();
  }

  // ------- Blocking-call support -------

  // Called BEFORE a blocking OS call (pthread_join, pthread_cond_wait, etc.).
  // Marks this thread as Blocked so the scheduler won't pick it, and wakes
  // another runnable thread. The calling thread does NOT park — it proceeds
  // to the blocking OS call.
  void BeforeBlockingCall(int idx) {
    mtx_.Lock();
    threads_[idx].state = SimThread::Blocked;

    if (idx == current_) {
      PickNextAndWake();
    }
    mtx_.Unlock();
  }

  // Called BEFORE a pthread_join call. Records the target pthread_t handle.
  void BeforeJoinCall(int idx, uptr target_handle) {
    mtx_.Lock();
    threads_[idx].state = SimThread::Blocked;
    threads_[idx].joining_on = target_handle;

    if (idx == current_) {
      PickNextAndWake();
    }
    mtx_.Unlock();
  }

  // Check if a thread with the given pthread_t handle is still active
  // (i.e., not Finished). Returns false if thread not found or already
  // finished.
  bool IsThreadActive(uptr thread_handle) {
    SpinMutexLock lock(&mtx_);
    for (int i = 0; i < thread_count_; i++) {
      if (threads_[i].thread_handle == thread_handle) {
        return threads_[i].state != SimThread::Finished;
      }
    }
    return false;  // Thread not found
  }

  // Called AFTER a blocking OS call returns. Marks this thread as Running
  // (runnable) again. If no thread is currently running, this thread becomes
  // current and returns immediately. Otherwise it parks until selected.
  void AfterBlockingCall(int idx) {
    mtx_.Lock();
    threads_[idx].state = SimThread::Running;

    if (current_ == -1) {
      current_ = idx;
      mtx_.Unlock();
      return;
    }

    // Another thread is running. Park until selected.
    mtx_.Unlock();
    threads_[idx].sem.Wait();
  }

  Semaphore* GetSemaphore(int idx) { return &threads_[idx].sem; }

  int GetThreadCount() const { return thread_count_; }

  void MutexBlock(int caller_idx, uptr mutex_addr) {
    mtx_.Lock();

    if (caller_idx != current_) {
      // Not the current thread — shouldn't happen.
      mtx_.Unlock();
      return;
    }

    // Add this thread to the mutex's waitset.
    Waitset* ws = GetOrCreateMutexWaitset(mutex_addr);
    ws->AddWaiter(caller_idx);

    // Mark thread as blocked.
    threads_[caller_idx].state = SimThread::Blocked;

    // Pick next runnable thread and wake it.
    PickNextAndWake();

    mtx_.Unlock();

    // Park this thread until woken by unlock.
    threads_[caller_idx].sem.Wait();
  }

  void MutexUnblock(uptr mutex_addr) {
    mtx_.Lock();

    // Find the waitset for this mutex.
    Waitset* ws = nullptr;
    for (int i = 0; i < mutex_waitset_count_; i++) {
      if (mutex_waitset_addrs_[i] == mutex_addr) {
        ws = &mutex_waitsets_[i];
        break;
      }
    }

    if (!ws || ws->count == 0) {
      mtx_.Unlock();
      return;
    }

    // Remove one waiter randomly and mark it as runnable.
    int thread_idx = ws->RemoveOne(&rng_state_);
    threads_[thread_idx].state = SimThread::Running;

    // If no thread is current, make the unblocked thread current and wake it.
    if (current_ == -1) {
      current_ = thread_idx;
      threads_[thread_idx].sem.Post();
    }
    // Otherwise it will be picked up by next Schedule() or when current
    // finishes.

    mtx_.Unlock();
  }

  void CondWait(int caller_idx, uptr cond_addr, uptr mutex_addr) {
    mtx_.Lock();

    if (caller_idx != current_) {
      mtx_.Unlock();
      return;
    }

    // Add this thread to the condvar's waitset.
    Waitset* ws = GetOrCreateCondWaitset(cond_addr);
    ws->AddWaiter(caller_idx);

    // Mark thread as blocked.
    threads_[caller_idx].state = SimThread::Blocked;

    // Pick next runnable thread and wake it.
    PickNextAndWake();

    mtx_.Unlock();

    // Park this thread until woken by signal/broadcast.
    threads_[caller_idx].sem.Wait();
  }

  void CondSignal(uptr cond_addr) {
    mtx_.Lock();

    // Find the waitset for this condvar.
    Waitset* ws = nullptr;
    for (int i = 0; i < cond_waitset_count_; i++) {
      if (cond_waitset_addrs_[i] == cond_addr) {
        ws = &cond_waitsets_[i];
        break;
      }
    }

    if (!ws || ws->count == 0) {
      mtx_.Unlock();
      return;
    }

    // Remove one waiter randomly and mark it as runnable.
    int thread_idx = ws->RemoveOne(&rng_state_);
    threads_[thread_idx].state = SimThread::Running;

    // If no thread is current, make the unblocked thread current and wake it.
    if (current_ == -1) {
      current_ = thread_idx;
      threads_[thread_idx].sem.Post();
    }

    mtx_.Unlock();
  }

  void CondBroadcast(uptr cond_addr) {
    mtx_.Lock();

    // Find the waitset for this condvar.
    Waitset* ws = nullptr;
    for (int i = 0; i < cond_waitset_count_; i++) {
      if (cond_waitset_addrs_[i] == cond_addr) {
        ws = &cond_waitsets_[i];
        break;
      }
    }

    if (!ws || ws->count == 0) {
      mtx_.Unlock();
      return;
    }

    // Wake all waiting threads.
    int woken[kMaxSimThreads];
    int n = ws->RemoveAll(woken);
    for (int i = 0; i < n; i++) {
      threads_[woken[i]].state = SimThread::Running;
    }

    // If no thread is current, pick one of the woken threads.
    if (current_ == -1 && n > 0) {
      int idx = RandN(&rng_state_, n);
      current_ = woken[idx];
      threads_[woken[idx]].sem.Post();
      // Wake the rest later when scheduled.
      for (int i = 0; i < n; i++) {
        if (i != idx && threads_[woken[i]].state == SimThread::Running) {
          // They'll be picked up by scheduler.
        }
      }
    }

    mtx_.Unlock();
  }

  void AnnotateWait(int caller_idx, uptr addr) {
    mtx_.Lock();

    if (caller_idx != current_) {
      mtx_.Unlock();
      return;
    }

    // Add this thread to the annotated address's waitset.
    Waitset* ws = GetOrCreateAnnotateWaitset(addr);
    ws->AddWaiter(caller_idx);

    // Mark thread as blocked.
    threads_[caller_idx].state = SimThread::Blocked;

    // Pick next runnable thread and wake it.
    PickNextAndWake();

    mtx_.Unlock();

    // Park this thread until woken by wake_one/wake_all.
    threads_[caller_idx].sem.Wait();
  }

  void AnnotateWakeOne(uptr addr) {
    mtx_.Lock();

    // Find the waitset for this address.
    Waitset* ws = nullptr;
    for (int i = 0; i < annotate_waitset_count_; i++) {
      if (annotate_waitset_addrs_[i] == addr) {
        ws = &annotate_waitsets_[i];
        break;
      }
    }

    if (!ws || ws->count == 0) {
      mtx_.Unlock();
      return;
    }

    // Remove one waiter randomly and mark it as runnable.
    int thread_idx = ws->RemoveOne(&rng_state_);
    threads_[thread_idx].state = SimThread::Running;

    // If no thread is current, make the unblocked thread current and wake it.
    if (current_ == -1) {
      current_ = thread_idx;
      threads_[thread_idx].sem.Post();
    }

    mtx_.Unlock();
  }

  void AnnotateWakeAll(uptr addr) {
    mtx_.Lock();

    // Find the waitset for this address.
    Waitset* ws = nullptr;
    for (int i = 0; i < annotate_waitset_count_; i++) {
      if (annotate_waitset_addrs_[i] == addr) {
        ws = &annotate_waitsets_[i];
        break;
      }
    }

    if (!ws || ws->count == 0) {
      mtx_.Unlock();
      return;
    }

    // Wake all waiting threads.
    int woken[kMaxSimThreads];
    int n = ws->RemoveAll(woken);
    for (int i = 0; i < n; i++) {
      threads_[woken[i]].state = SimThread::Running;
    }

    // If no thread is current, pick one of the woken threads.
    if (current_ == -1 && n > 0) {
      int idx = RandN(&rng_state_, n);
      current_ = woken[idx];
      threads_[woken[idx]].sem.Post();
      // Wake the rest later when scheduled.
      for (int i = 0; i < n; i++) {
        if (i != idx && threads_[woken[i]].state == SimThread::Running) {
          // They'll be picked up by scheduler.
        }
      }
    }

    mtx_.Unlock();
  }

  // Check if we should perform scheduling at this point based on probability.
  // Always returns true if probability >= 100, otherwise uses RNG.
  bool ShouldSchedule() {
    if (schedule_probability_ >= 100)
      return true;
    if (schedule_probability_ <= 0)
      return false;
    // Generate random value [0, 99] and compare to probability percentage
    u32 rand_val = RandN(&rng_state_, 100);
    return rand_val < static_cast<u32>(schedule_probability_);
  }

 private:
  int CountRunnable() const {
    int n = 0;
    for (int i = 0; i < thread_count_; i++) {
      if (threads_[i].state == SimThread::Running)
        n++;
    }
    return n;
  }

  int PickRandomRunnable(int runnable) {
    int target = RandN(&rng_state_, runnable);
    for (int i = 0; i < thread_count_; i++) {
      if (threads_[i].state == SimThread::Running) {
        if (target == 0)
          return i;
        target--;
      }
    }
    return -1;  // unreachable
  }

  // Must be called with mtx_ held. Picks the next runnable thread and posts
  // its semaphore, or sets current_ = -1 if none are runnable.
  void PickNextAndWake() {
    int runnable = CountRunnable();
    if (runnable == 0) {
      DumpStates();
      current_ = -1;
      // Check if this is a deadlock (blocked threads exist but no runnable
      // ones)
      int blocked = 0;
      for (int i = 0; i < thread_count_; i++) {
        if (threads_[i].state == SimThread::Blocked)
          blocked++;
      }
      if (blocked > 0) {
        // Deadlock: all remaining threads are blocked
        SimulateReportDeadlock();
      }
      return;
    }
    int chosen = PickRandomRunnable(runnable);
    DumpStates(chosen);
    current_ = chosen;
    threads_[chosen].sem.Post();
  }

  // Get or create waitset for a mutex. Uses simple linear search since
  // we don't expect many mutexes per iteration.
  Waitset* GetOrCreateMutexWaitset(uptr mutex_addr) {
    for (int i = 0; i < mutex_waitset_count_; i++) {
      if (mutex_waitset_addrs_[i] == mutex_addr)
        return &mutex_waitsets_[i];
    }
    CHECK_LT(mutex_waitset_count_, kMaxWaitsets);
    int idx = mutex_waitset_count_++;
    mutex_waitset_addrs_[idx] = mutex_addr;
    new (&mutex_waitsets_[idx]) Waitset();
    return &mutex_waitsets_[idx];
  }

  // Get or create waitset for a condition variable.
  Waitset* GetOrCreateCondWaitset(uptr cond_addr) {
    for (int i = 0; i < cond_waitset_count_; i++) {
      if (cond_waitset_addrs_[i] == cond_addr)
        return &cond_waitsets_[i];
    }
    CHECK_LT(cond_waitset_count_, kMaxWaitsets);
    int idx = cond_waitset_count_++;
    cond_waitset_addrs_[idx] = cond_addr;
    new (&cond_waitsets_[idx]) Waitset();
    return &cond_waitsets_[idx];
  }

  // Get or create waitset for an annotated address (e.g., futex).
  Waitset* GetOrCreateAnnotateWaitset(uptr addr) {
    for (int i = 0; i < annotate_waitset_count_; i++) {
      if (annotate_waitset_addrs_[i] == addr)
        return &annotate_waitsets_[i];
    }
    CHECK_LT(annotate_waitset_count_, kMaxWaitsets);
    int idx = annotate_waitset_count_++;
    annotate_waitset_addrs_[idx] = addr;
    new (&annotate_waitsets_[idx]) Waitset();
    return &annotate_waitsets_[idx];
  }

  SpinMutex mtx_;
  u32 rng_state_ = 1;  // Random number generator state
  SimThread threads_[kMaxSimThreads];
  int current_;
  int thread_count_;
  int depth_;
  int schedule_probability_;  // Cached and validated at construction

  // Resource waitsets: map from resource address to waitset.
  static constexpr int kMaxWaitsets = 256;
  uptr mutex_waitset_addrs_[kMaxWaitsets];
  Waitset mutex_waitsets_[kMaxWaitsets];
  int mutex_waitset_count_ = 0;
  uptr cond_waitset_addrs_[kMaxWaitsets];
  Waitset cond_waitsets_[kMaxWaitsets];
  int cond_waitset_count_ = 0;
  uptr annotate_waitset_addrs_[kMaxWaitsets];
  Waitset annotate_waitsets_[kMaxWaitsets];
  int annotate_waitset_count_ = 0;
};

// ---------------------------------------------------------------------------
// Global state
// ---------------------------------------------------------------------------

bool sim_active;

// Pointer to the current scheduler instance (valid while sim_active == true).
static SimScheduler* sim_sched;

// Per-thread scheduler index. -1 when not participating in simulation.
static THREADLOCAL int sim_thread_idx = -1;

class SimStateGuard {
  SimScheduler* sched_;

 public:
  SimStateGuard(SimScheduler* sched) : sched_(sched) { sim_active = true; }
  ~SimStateGuard() {
    sim_active = false;
    sim_thread_idx = -1;
    sim_sched = nullptr;
    if (sched_) {
      sched_->~SimScheduler();
      InternalFree(sched_);
    }
  }
  SimStateGuard(const SimStateGuard&) = delete;
  SimStateGuard& operator=(const SimStateGuard&) = delete;
};

void SimulateScheduleImpl() {
  int idx = sim_thread_idx;
  if (idx < 0)
    return;
  if (!sim_sched->ShouldSchedule())
    return;

  if (flags()->simulate_print_schedule_stacks) {
    ThreadState* thr = cur_thread();
    Printf("=========== Schedule point (thread %d) ===========\n", idx);
    PrintCurrentStack(thr, StackTrace::GetCurrentPc());
    Printf("==================================================\n");
  }

  sim_sched->Schedule(idx);
}

void SimulateThreadRegisterImpl(uptr thread_handle) {
  int idx = sim_sched->AddThread();
  sim_thread_idx = idx;
  sim_sched->SetThreadHandle(idx, thread_handle);
}

void SimulateThreadWaitScheduledImpl() {
  int idx = sim_thread_idx;
  if (idx < 0)
    return;
  // Wait until scheduler picks us (blocking).
  sim_sched->ThreadStart(idx);
}

void SimulateThreadFinishImpl() {
  int idx = sim_thread_idx;
  sim_thread_idx = -1;
  if (idx < 0)
    return;
  sim_sched->ThreadFinish(idx);
}

void SimulateThreadBlockImpl() {
  int idx = sim_thread_idx;
  if (idx < 0)
    return;
  sim_sched->BeforeBlockingCall(idx);
}

void SimulateJoinBlockImpl(uptr thread_handle) {
  int idx = sim_thread_idx;
  if (idx < 0)
    return;
  // Only mark ourselves as blocked if the target thread is still active.
  // If it's already finished, pthread_join will return immediately.
  if (sim_sched->IsThreadActive(thread_handle)) {
    sim_sched->BeforeJoinCall(idx, thread_handle);
  }
}

void SimulateThreadUnblockImpl() {
  int idx = sim_thread_idx;
  if (idx < 0)
    return;
  sim_sched->AfterBlockingCall(idx);
}

void SimulateMutexBlockImpl(uptr mutex_addr) {
  int idx = sim_thread_idx;
  if (idx < 0)
    return;
  sim_sched->MutexBlock(idx, mutex_addr);
}

void SimulateMutexUnblockImpl(uptr mutex_addr) {
  sim_sched->MutexUnblock(mutex_addr);
}

void SimulateCondWaitImpl(uptr cond_addr, uptr mutex_addr) {
  int idx = sim_thread_idx;
  if (idx < 0)
    return;
  sim_sched->CondWait(idx, cond_addr, mutex_addr);
}

void SimulateCondSignalImpl(uptr cond_addr) {
  sim_sched->CondSignal(cond_addr);
}

void SimulateCondBroadcastImpl(uptr cond_addr) {
  sim_sched->CondBroadcast(cond_addr);
}

void SimulateAnnotateWaitImpl(uptr addr) {
  int idx = sim_thread_idx;
  if (idx < 0)
    return;
  sim_sched->AnnotateWait(idx, addr);
}

void SimulateAnnotateWakeOneImpl(uptr addr) {
  sim_sched->AnnotateWakeOne(addr);
}

void SimulateAnnotateWakeAllImpl(uptr addr) {
  sim_sched->AnnotateWakeAll(addr);
}

int SimulateRun(void (*callback)(void*), void* arg) {
  const char* sched = flags()->simulate_scheduler;
  if (!sched || !sched[0] || internal_strcmp(sched, "random") != 0) {
    // No scheduler configured or not "random". Run the callback once without
    // simulation so that __tsan_simulate still works as a simple wrapper.
    callback(arg);
    return 0;
  }

  // Check if there are other threads running. Simulation requires that only
  // the calling thread exists before starting.
  uptr running_threads = 0;
  ctx->thread_registry.GetNumberOfThreads(nullptr, &running_threads, nullptr);
  if (running_threads > 1) {
    Printf(
        "ThreadSanitizer: simulation cannot start - other threads are "
        "running (%zu threads detected).\n"
        "Simulation requires that only the calling thread exists. "
        "Running callback once without simulation.\n",
        running_threads);
    callback(arg);
    return -1;  // Error: pre-existing threads
  }

  // Reset error flags before starting simulation.
  atomic_store_relaxed(&sim_unsupported_interceptor_called, 0);
  atomic_store_relaxed(&sim_max_depth_hit, 0);
  atomic_store_relaxed(&sim_race_detected, 0);

  int iterations = flags()->simulate_iterations;
  if (iterations <= 0)
    iterations = 1000;

  int start_iter = flags()->simulate_start_iteration;
  if (start_iter < 0)
    start_iter = 0;

  int max_depth = flags()->simulate_max_depth;
  Printf(
      "ThreadSanitizer: simulation starting (iterations %d..%d, max_depth=%d, "
      "scheduler=%s)\n",
      start_iter, start_iter + iterations - 1, max_depth, sched);

  // Allocate scheduler once on the heap for all iterations.
  void* sched_mem = InternalAlloc(sizeof(SimScheduler));
  SimScheduler* sched_ptr = new (sched_mem) SimScheduler();
  sim_sched = sched_ptr;

  // Activate simulation
  SimStateGuard guard(sched_ptr);

  for (int iter = start_iter; iter < start_iter + iterations; iter++) {
    // Track current iteration for error reporting.
    sim_current_iteration = iter;

    // Reset scheduler state for this iteration.
    sched_ptr->ResetForIteration();

    // Register the calling (main) thread as thread 0.
    int main_idx = sched_ptr->AddThread();
    sim_thread_idx = main_idx;
    // Set the main thread's pthread_t handle for join tracking.
    sched_ptr->SetThreadHandle(main_idx, (uptr)pthread_self());

    sched_ptr->StartIteration(iter);

    // Wait for our turn (StartIteration posted our semaphore).
    sched_ptr->GetSemaphore(main_idx)->Wait();

    // Run the test callback for this iteration.
    DPrintf(1, "Start callback... iter=%d\n", iter);
    callback(arg);
    DPrintf(1, "End callback...\n");

    // Check if no threads were spawned (only main thread exists).
    // If so, there's no parallelism to explore, so exit successfully.
    if (iter == start_iter && sched_ptr->GetThreadCount() == 1) {
      Printf("ThreadSanitizer: simulation exiting - no threads were spawned\n");
      return 0;  // Success: no parallelism to test
    }

    // Check if an error occurred during this iteration.
    if (atomic_load_relaxed(&sim_unsupported_interceptor_called)) {
      Printf("ThreadSanitizer: unsupported interceptor at iteration %d\n",
             iter);
      Printf(
          "ThreadSanitizer: to reproduce, set "
          "TSAN_OPTIONS=simulate_start_iteration=%d\n",
          iter);
      Printf("ThreadSanitizer: simulation aborted after %d iterations\n",
             iter - start_iter + 1);
      return -1;  // Error: unsupported interceptor
    }

    // Check if max depth was hit during this iteration.
    if (atomic_load_relaxed(&sim_max_depth_hit)) {
      Printf(
          "ThreadSanitizer: to reproduce, set "
          "TSAN_OPTIONS=simulate_start_iteration=%d\n",
          iter);
      Printf(
          "ThreadSanitizer: simulation stopped due to max depth after %d "
          "iterations\n",
          iter - start_iter + 1);
      return -1;  // Error: max depth hit
    }

    // Check if a race was detected during this iteration.
    if (atomic_load_relaxed(&sim_race_detected)) {
      Printf(
          "ThreadSanitizer: to reproduce, set "
          "TSAN_OPTIONS=simulate_start_iteration=%d\n",
          iter);
      Printf(
          "ThreadSanitizer: simulation stopped due to race detection after %d "
          "iterations\n",
          iter - start_iter + 1);
      return -1;  // Error: race detected
    }

    // Main thread finished; unregister from the scheduler.
    sched_ptr->ThreadFinish(main_idx);
  }

  Printf("ThreadSanitizer: simulation finished (%d iterations)\n", iterations);
  return 0;  // Success
}

}  // namespace __tsan
