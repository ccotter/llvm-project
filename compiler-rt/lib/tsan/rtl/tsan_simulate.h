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

struct ThreadState;

// Returns true if simulation mode is active for the current process.
bool SimulateIsActive();

// Called at each scheduling point (atomic op, mutex lock/unlock, thread
// create/join, condvar signal/wait, etc.). If simulation is active, this may
// context-switch to another runnable thread.
void SimulateSchedule();

// Called when an unsupported interceptor is invoked during simulation.
// Prints an error message and sets the failure flag.
void SimulateReportUnsupported(const char* func_name);

// Called when a data race is detected during simulation.
// Sets the race_detected flag to abort the simulation.
void SimulateReportRace();

// Called when a deadlock is detected during simulation.
// Sets the deadlock_detected flag to abort the simulation.
void SimulateReportDeadlock();

// Called by a new thread to register with the scheduler (non-blocking).
// Must be called before signaling the parent thread to ensure deterministic
// thread registration order. The thread_handle is the pthread_t for this
// thread.
void SimulateThreadRegister(uptr thread_handle);

// Called by a new thread after signaling the parent. Blocks until the
// scheduler selects this thread to run.
void SimulateThreadWaitScheduled();

// Called when an application thread finishes. Removes the thread from the
// set of runnable threads and wakes the scheduler.
void SimulateThreadFinish();

// Called when a thread is about to block (e.g. mutex lock, condvar wait).
// Marks the thread as blocked so the scheduler won't pick it.
void SimulateThreadBlock();

// Called when a thread is about to block on pthread_join.
// Records the target pthread_t so the thread can be made runnable
// when the target finishes.
void SimulateJoinBlock(uptr thread_handle);

// Called when a thread is unblocked (e.g. mutex acquired, condvar signaled,
// joined thread finished). Marks the thread as runnable again.
void SimulateThreadUnblock();

// Called when pthread_mutex_lock cannot acquire the mutex. Adds the thread to
// the mutex's waitset and parks it until pthread_mutex_unlock wakes it.
void SimulateMutexBlock(uptr mutex_addr);

// Called when pthread_mutex_unlock releases a mutex. Wakes one thread from
// the mutex's waitset (if any).
void SimulateMutexUnblock(uptr mutex_addr);

// Called when pthread_cond_wait blocks on a condition variable. Adds the thread
// to the condvar's waitset and parks it. Must be called only when
// SimulateIsActive().
void SimulateCondWait(uptr cond_addr, uptr mutex_addr);

// Called when pthread_cond_signal wakes one thread from a condition variable.
void SimulateCondSignal(uptr cond_addr);

// Called when pthread_cond_broadcast wakes all threads from a condition
// variable.
void SimulateCondBroadcast(uptr cond_addr);

// Called to mark a thread as waiting on a custom address (e.g., futex word).
// Adds the thread to the address's waitset and parks it.
void SimulateAnnotateWait(uptr addr);

// Called to wake one thread waiting on a custom address (e.g., futex_wake(1)).
void SimulateAnnotateWakeOne(uptr addr);

// Called to wake all threads waiting on a custom address (e.g.,
// futex_wake_all()).
void SimulateAnnotateWakeAll(uptr addr);

// Run the simulation: invoke `callback(arg)` for `iterations` iterations,
// exploring thread interleavings using the configured scheduler.
// Returns 0 on success, non-zero on error (1=pre-existing threads,
// 2=unsupported interceptor called).
int SimulateRun(void (*callback)(void*), void* arg);

}  // namespace __tsan

#endif  // TSAN_SIMULATE_H
