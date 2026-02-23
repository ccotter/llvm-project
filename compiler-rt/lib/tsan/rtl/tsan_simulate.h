//===-- tsan_simulate.h -----------------------------------------*- C++ -*-===//
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
// When simulation is active, exactly one application thread runs at a time.
// Other threads are parked on internal condition variables. At each sync point
// (pthread_* calls, atomic operations), the running thread may yield to another
// thread chosen by the scheduler.
//===----------------------------------------------------------------------===//

#ifndef TSAN_SIMULATE_H
#define TSAN_SIMULATE_H

#include "sanitizer_common/sanitizer_internal_defs.h"

namespace __tsan {

// Run the simulation: invoke `callback(arg)` for `simulate_iterations`
// iterations, exploring thread interleavings using the configured scheduler.
// Returns 0 on success, -1 on error.
//
// Errors include
//  - Pre-existing threads when simulation was started
//  - Unsupported interceptor
//  - Max simulation depth hit
//  - Race detected
//  - Deadlock detected (all simulated threads were blocked)
//    Deadlock results in program termination via Die()
//
// If an unsupported interceptor is invoked, the simlulation enters undefined
// behavior from the ThreadSanitizer simulation perspective. The interceptor
// may lead to the simulation being unable to advance (deadlocked), or the
// simulation may eventually be able to return out from SimulateRun.
int SimulateRun(void (*callback)(void*), void* arg);

extern bool sim_active;

// Returns true if simulation mode is active for the current process.
ALWAYS_INLINE bool SimulateIsActive() { return sim_active; }

// Impl functions are called only when sim_active is true.
void SimulateScheduleImpl();
void SimulateReportUnsupportedImpl(const char* func_name);
void SimulateReportRaceImpl();
void SimulateThreadRegisterImpl(uptr thread_handle);
void SimulateThreadWaitScheduledImpl();
void SimulateThreadFinishImpl();
void SimulateThreadBlockImpl();
void SimulateJoinBlockImpl(uptr thread_handle);
void SimulateThreadUnblockImpl();

// Called at each scheduling point (atomic op, mutex lock/unlock, thread
// create/join, condvar signal/wait, etc.). If simulation is active, this may
// context-switch to another runnable thread.
ALWAYS_INLINE void SimulateSchedule() {
  if (!SimulateIsActive())
    return;
  SimulateScheduleImpl();
}

ALWAYS_INLINE void SimulateReportUnsupported(const char* func_name) {
  if (!SimulateIsActive())
    return;
  SimulateReportUnsupportedImpl(func_name);
}

ALWAYS_INLINE void SimulateReportRace() {
  if (!SimulateIsActive())
    return;
  SimulateReportRaceImpl();
}

// Called by a new thread to register with the scheduler (non-blocking).
// Must be called before signaling the parent thread to ensure deterministic
// thread registration order. The thread_handle is the pthread_t for this
// thread.
ALWAYS_INLINE void SimulateThreadRegister(uptr thread_handle) {
  if (!SimulateIsActive())
    return;
  SimulateThreadRegisterImpl(thread_handle);
}

// Called by a new thread after signaling the parent. Blocks until the
// scheduler selects this thread to run.
ALWAYS_INLINE void SimulateThreadWaitScheduled() {
  if (!SimulateIsActive())
    return;
  SimulateThreadWaitScheduledImpl();
}

// Called when an application thread finishes. Removes the thread from the
// set of runnable threads and wakes the scheduler.
ALWAYS_INLINE void SimulateThreadFinish() {
  if (!SimulateIsActive())
    return;
  SimulateThreadFinishImpl();
}

// Called when a thread is about to block (e.g. mutex lock, condvar wait).
// Marks the thread as blocked so the scheduler won't pick it.
ALWAYS_INLINE void SimulateThreadBlock() {
  if (!SimulateIsActive())
    return;
  SimulateThreadBlockImpl();
}

// Called when a thread is about to block on pthread_join.
// Records the target pthread_t so the thread can be made runnable
// when the target finishes.
ALWAYS_INLINE void SimulateJoinBlock(uptr thread_handle) {
  if (!SimulateIsActive())
    return;
  SimulateJoinBlockImpl(thread_handle);
}

// Called when a thread is unblocked (e.g. mutex acquired, condvar signaled,
// joined thread finished). Marks the thread as runnable again.
ALWAYS_INLINE void SimulateThreadUnblock() {
  if (!SimulateIsActive())
    return;
  SimulateThreadUnblockImpl();
}

// Implementation functions for mutex and condvar.
void SimulateMutexBlockImpl(uptr mutex_addr);
void SimulateMutexUnblockImpl(uptr mutex_addr);
void SimulateCondWaitImpl(uptr cond_addr, uptr mutex_addr);
void SimulateCondSignalImpl(uptr cond_addr);
void SimulateCondBroadcastImpl(uptr cond_addr);

// Called when pthread_mutex_lock cannot acquire the mutex. Adds the thread to
// the mutex's waitset and parks it until pthread_mutex_unlock wakes it.
ALWAYS_INLINE void SimulateMutexBlock(uptr mutex_addr) {
  if (!SimulateIsActive())
    return;
  SimulateMutexBlockImpl(mutex_addr);
}

// Called when pthread_mutex_unlock releases a mutex. Wakes one thread from
// the mutex's waitset (if any).
ALWAYS_INLINE void SimulateMutexUnblock(uptr mutex_addr) {
  if (!SimulateIsActive())
    return;
  SimulateMutexUnblockImpl(mutex_addr);
}

// Called when pthread_cond_wait blocks on a condition variable. Adds the thread
// to the condvar's waitset and parks it. Must be called only when
// SimulateIsActive().
ALWAYS_INLINE void SimulateCondWait(uptr cond_addr, uptr mutex_addr) {
  if (!SimulateIsActive())
    return;
  SimulateCondWaitImpl(cond_addr, mutex_addr);
}

// Called when pthread_cond_signal wakes one thread from a condition variable.
ALWAYS_INLINE void SimulateCondSignal(uptr cond_addr) {
  if (!SimulateIsActive())
    return;
  SimulateCondSignalImpl(cond_addr);
}

// Called when pthread_cond_broadcast wakes all threads from a condition
// variable.
ALWAYS_INLINE void SimulateCondBroadcast(uptr cond_addr) {
  if (!SimulateIsActive())
    return;
  SimulateCondBroadcastImpl(cond_addr);
}

// Implementation functions for annotations.
void SimulateAnnotateWaitImpl(uptr addr);
void SimulateAnnotateWakeOneImpl(uptr addr);
void SimulateAnnotateWakeAllImpl(uptr addr);

// Called to mark a thread as waiting on a custom address (e.g., futex word).
// Adds the thread to the address's waitset and parks it.
ALWAYS_INLINE void SimulateAnnotateWait(uptr addr) {
  if (!SimulateIsActive())
    return;
  SimulateAnnotateWaitImpl(addr);
}

// Called to wake one thread waiting on a custom address (e.g., futex_wake(1)).
ALWAYS_INLINE void SimulateAnnotateWakeOne(uptr addr) {
  if (!SimulateIsActive())
    return;
  SimulateAnnotateWakeOneImpl(addr);
}

// Called to wake all threads waiting on a custom address (e.g.,
// futex_wake_all()).
ALWAYS_INLINE void SimulateAnnotateWakeAll(uptr addr) {
  if (!SimulateIsActive())
    return;
  SimulateAnnotateWakeAllImpl(addr);
}

}  // namespace __tsan

#endif  // TSAN_SIMULATE_H
