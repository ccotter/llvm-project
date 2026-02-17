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
//   - Real OS threads, not fibers. Exactly one thread runs user code at a time.
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

namespace __tsan {

// ---------------------------------------------------------------------------
// Random number generator (LCG)
// ---------------------------------------------------------------------------

class RandomGenerator {
 public:
  void Seed(u32 s) { state_ = s ? s : 1; }
  u32 Next() {
    state_ = state_ * 1103515245u + 12345u;
    return (state_ >> 16) & 0x7fff;
  }
  u32 NextRange(u32 n) { return Next() % n; }

 private:
  u32 state_ = 1;
};

// ---------------------------------------------------------------------------
// Per-thread simulation state
// ---------------------------------------------------------------------------

struct SimThread {
  enum State : u32 {
    Unused = 0,
    Running,   // Runnable — may be selected by the scheduler.
    Blocked,   // Blocked in an OS call — scheduler must not pick this thread.
    Finished,  // Thread has exited the simulation.
  };

  Semaphore sem;
  State state;
};

static constexpr int kMaxSimThreads = 64;

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
  }

  // Register a new thread. Returns its scheduler index.
  int AddThread() {
    SpinMutexLock lock(&mtx_);
    CHECK_LT(thread_count_, kMaxSimThreads);
    int idx = thread_count_++;
    threads_[idx].state = SimThread::Running;
    return idx;
  }

  // Seed the RNG and post the first runnable thread's semaphore.
  void StartIteration(u32 seed) {
    SpinMutexLock lock(&mtx_);
    rng_.Seed(seed);
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

  // ------- Scheduling point (non-blocking) -------
  //
  // Called by the currently running thread. May randomly switch to another
  // runnable thread. If the caller is NOT the current thread (e.g. during a
  // brief window around a blocking call), this is a no-op.
  void Schedule(int caller_idx) {
    mtx_.Lock();

    if (caller_idx != current_) {
      // Not the current thread — no-op. This can happen briefly during
      // blocking-call transitions.
      mtx_.Unlock();
      return;
    }

    int max_depth = flags()->simulate_max_depth;
    if (++depth_ > max_depth) {
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

    VPrintf(1, "Chose tid %d to run current %d\n", chosen, caller_idx);

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
  void ThreadFinish(int idx) {
    mtx_.Lock();
    threads_[idx].state = SimThread::Finished;

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

  Semaphore *GetSemaphore(int idx) { return &threads_[idx].sem; }

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
    int target = rng_.NextRange(runnable);
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
      current_ = -1;
      return;
    }
    int chosen = PickRandomRunnable(runnable);
    current_ = chosen;
    threads_[chosen].sem.Post();
  }

  SpinMutex mtx_;
  RandomGenerator rng_;
  SimThread threads_[kMaxSimThreads];
  int current_;
  int thread_count_;
  int depth_;
};

// ---------------------------------------------------------------------------
// Global state
// ---------------------------------------------------------------------------

// 0 = inactive, 1 = active.
static atomic_uint32_t sim_active;

// Pointer to the current scheduler instance (valid while sim_active == 1).
static SimScheduler *sim_sched;

// Per-thread scheduler index. -1 when not participating in simulation.
static THREADLOCAL int sim_thread_idx = -1;

// Set to 1 if an unsupported interceptor is called during simulation.
static atomic_uint32_t sim_error;

// ---------------------------------------------------------------------------
// Public API (called from interceptors and tsan_interface.cpp)
// ---------------------------------------------------------------------------

bool SimulateIsActive() {
  return atomic_load_relaxed(&sim_active) != 0;
}

void SimulateReportUnsupported(const char *func_name) {
  if (!SimulateIsActive())
    return;
  atomic_store_relaxed(&sim_error, 1);
  Printf(
      "ThreadSanitizer: simulation error - unsupported interceptor called: "
      "%s\n"
      "Simulation does not support this synchronization primitive.\n",
      func_name);
}

void SimulateSchedule() {
  if (!SimulateIsActive())
    return;
  int idx = sim_thread_idx;
  if (idx < 0)
    return;
  sim_sched->Schedule(idx);
}

void SimulateThreadStart() {
  if (!SimulateIsActive())
    return;
  int idx = sim_sched->AddThread();
  sim_thread_idx = idx;
  sim_sched->ThreadStart(idx);
}

void SimulateThreadFinish() {
  int idx = sim_thread_idx;
  sim_thread_idx = -1;
  if (!SimulateIsActive() || idx < 0)
    return;
  sim_sched->ThreadFinish(idx);
}

void SimulateThreadBlock() {
  if (!SimulateIsActive())
    return;
  int idx = sim_thread_idx;
  if (idx < 0)
    return;
  sim_sched->BeforeBlockingCall(idx);
}

void SimulateThreadUnblock() {
  if (!SimulateIsActive())
    return;
  int idx = sim_thread_idx;
  if (idx < 0)
    return;
  sim_sched->AfterBlockingCall(idx);
}

int SimulateRun(void (*callback)(void *), void *arg) {
  const char *sched = flags()->simulate_scheduler;
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
    return 1;  // Error: pre-existing threads
  }

  // Reset error flag before starting simulation.
  atomic_store_relaxed(&sim_error, 0);

  int iterations = flags()->simulate_iterations;
  if (iterations <= 0)
    iterations = 1000;

  int max_depth = flags()->simulate_max_depth;
  Printf(
      "ThreadSanitizer: simulation starting (%d iterations, max_depth=%d, "
      "scheduler=%s)\n",
      iterations, max_depth, sched);

  for (int iter = 0; iter < iterations; iter++) {
    // Allocate a fresh scheduler on the stack for each iteration.
    ALIGNED(64) char sched_buf[sizeof(SimScheduler)];
    SimScheduler *sched_ptr = new (sched_buf) SimScheduler();
    sim_sched = sched_ptr;

    // Register the calling (main) thread as thread 0.
    int main_idx = sched_ptr->AddThread();
    sim_thread_idx = main_idx;

    // Activate simulation before starting the iteration.
    atomic_store_relaxed(&sim_active, 1);

    // Seed the RNG and post the first thread's semaphore.
    sched_ptr->StartIteration(iter + 1);

    // Wait for our turn (StartIteration posted our semaphore).
    sched_ptr->GetSemaphore(main_idx)->Wait();

    // Run the test callback for this iteration.
    VPrintf(1, "Start callback...\n");
    callback(arg);
    VPrintf(1, "End callback...\n");

    // Check if an error occurred during this iteration.
    if (atomic_load_relaxed(&sim_error)) {
      // Deactivate simulation and clean up.
      atomic_store_relaxed(&sim_active, 0);
      sim_thread_idx = -1;
      sim_sched = nullptr;
      sched_ptr->~SimScheduler();
      Printf("ThreadSanitizer: simulation aborted after %d iterations\n",
             iter + 1);
      return 2;  // Error: unsupported interceptor called
    }

    // Main thread finished; unregister from the scheduler.
    sched_ptr->ThreadFinish(main_idx);

    // Deactivate simulation and clean up.
    atomic_store_relaxed(&sim_active, 0);
    sim_thread_idx = -1;
    sim_sched = nullptr;

    sched_ptr->~SimScheduler();
  }

  Printf("ThreadSanitizer: simulation finished (%d iterations)\n", iterations);
  return 0;  // Success
}

}  // namespace __tsan
