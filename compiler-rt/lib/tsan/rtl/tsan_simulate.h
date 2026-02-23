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

ALWAYS_INLINE bool SimulateIsActive() { return sim_active; }

void SimulateScheduleImpl();
void SimulateReportUnsupportedImpl(const char* func_name);
void SimulateReportRaceImpl();
void SimulateThreadRegisterImpl(uptr thread_handle);
void SimulateThreadWaitScheduledImpl();
void SimulateThreadFinishImpl();
void SimulateThreadBlockImpl();
void SimulateJoinBlockImpl(uptr thread_handle);
void SimulateThreadUnblockImpl();

// SimulateSchedule is the key hook for simulation. It's called at each
// scheduling point (atomic op, mutex/cv op, thread create/join). When
// simulation is active, SimulateSchedule will check if another thread should
// run, and if so, context switch to that thread.
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

ALWAYS_INLINE void SimulateThreadRegister(uptr thread_handle) {
  if (!SimulateIsActive())
    return;
  SimulateThreadRegisterImpl(thread_handle);
}

ALWAYS_INLINE void SimulateThreadWaitScheduled() {
  if (!SimulateIsActive())
    return;
  SimulateThreadWaitScheduledImpl();
}

ALWAYS_INLINE void SimulateThreadFinish() {
  if (!SimulateIsActive())
    return;
  SimulateThreadFinishImpl();
}

ALWAYS_INLINE void SimulateThreadBlock() {
  if (!SimulateIsActive())
    return;
  SimulateThreadBlockImpl();
}

ALWAYS_INLINE void SimulateJoinBlock(uptr thread_handle) {
  if (!SimulateIsActive())
    return;
  SimulateJoinBlockImpl(thread_handle);
}

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

ALWAYS_INLINE void SimulateMutexBlock(uptr mutex_addr) {
  if (!SimulateIsActive())
    return;
  SimulateMutexBlockImpl(mutex_addr);
}

ALWAYS_INLINE void SimulateMutexUnblock(uptr mutex_addr) {
  if (!SimulateIsActive())
    return;
  SimulateMutexUnblockImpl(mutex_addr);
}

ALWAYS_INLINE void SimulateCondWait(uptr cond_addr, uptr mutex_addr) {
  if (!SimulateIsActive())
    return;
  SimulateCondWaitImpl(cond_addr, mutex_addr);
}

ALWAYS_INLINE void SimulateCondSignal(uptr cond_addr) {
  if (!SimulateIsActive())
    return;
  SimulateCondSignalImpl(cond_addr);
}

ALWAYS_INLINE void SimulateCondBroadcast(uptr cond_addr) {
  if (!SimulateIsActive())
    return;
  SimulateCondBroadcastImpl(cond_addr);
}

// TODO - remove below for now.

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
