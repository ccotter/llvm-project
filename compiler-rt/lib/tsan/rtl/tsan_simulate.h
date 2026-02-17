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
void SimulateReportUnsupported(const char *func_name);

// Called when a new application thread starts. Registers the thread with the
// scheduler and blocks until the scheduler selects it to run.
void SimulateThreadStart();

// Called when an application thread finishes. Removes the thread from the
// set of runnable threads and wakes the scheduler.
void SimulateThreadFinish();

// Called when a thread is about to block (e.g. mutex lock, condvar wait,
// pthread_join). Marks the thread as blocked so the scheduler won't pick it.
void SimulateThreadBlock();

// Called when a thread is unblocked (e.g. mutex acquired, condvar signaled,
// joined thread finished). Marks the thread as runnable again.
void SimulateThreadUnblock();

// Run the simulation: invoke `callback(arg)` for `iterations` iterations,
// exploring thread interleavings using the configured scheduler.
// Returns 0 on success, non-zero on error (1=pre-existing threads,
// 2=unsupported interceptor called).
int SimulateRun(void (*callback)(void *), void *arg);

}  // namespace __tsan

#endif  // TSAN_SIMULATE_H
