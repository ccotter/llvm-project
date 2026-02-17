# Waitset Implementation for TSAN Simulation Scheduler

## Overview

Implemented Relacy-style per-mutex waitsets to improve the efficiency of mutex blocking in the TSAN simulation scheduler.

## Previous Approach (Inefficient)

```cpp
while (true) {
  res = REAL(pthread_mutex_trylock)(m);
  if (res != errno_EBUSY)
    break;
  SimulateSchedule();  // Yield and try again
}
```

**Problem**: Wasted scheduler iterations spinning on trylock even when the mutex holder was blocked or running. Every failed trylock consumed one depth unit from the max_depth budget.

## New Approach (Waitset-Based)

### Data Structures

1. **Per-Mutex Waitset** (`MutexWaitset`):
   - Tracks which threads are blocked waiting for a specific mutex
   - Fixed-size array with FIFO removal (fairness)
   - Methods: `AddWaiter(thread_idx)`, `RemoveOne()`

2. **SimScheduler Additions**:
   - Hash map from mutex address → waitset (linear search, max 256 mutexes)
   - Mutex blocking/unblocking methods

### Key Operations

**pthread_mutex_lock**:
```cpp
while (true) {
  res = REAL(pthread_mutex_trylock)(m);
  if (res != errno_EBUSY)
    break;
  SimulateMutexBlock((uptr)m);  // Park until woken by unlock
}
```

**SimulateMutexBlock**:
1. Add current thread to mutex's waitset
2. Mark thread as `Blocked`
3. Pick another `Running` thread and wake it
4. Park on semaphore until woken

**pthread_mutex_unlock**:
```cpp
REAL(pthread_mutex_unlock)(m);
SimulateMutexUnblock((uptr)m);  // Wake one waiter
SimulateSchedule();
```

**SimulateMutexUnblock**:
1. Find waitset for mutex
2. Remove one thread from waitset
3. Mark thread as `Running` (eligible for scheduling)
4. If no thread is current, wake it immediately

### Benefits

1. **Efficiency**: Blocked threads don't consume scheduling iterations
   - Only `Running` threads are picked by the scheduler
   - Blocked threads stay parked until explicitly woken

2. **Correctness**: Matches Relacy's proven design pattern
   - Per-resource waitsets (not per-thread blocked-on tracking)
   - Clean separation of resource management

3. **Scalability**: Extends naturally to other synchronization primitives
   - Same pattern can be used for rwlocks, barriers, etc.
   - Each resource maintains its own list of waiters

4. **Fairness**: FIFO removal from waitsets
   - First blocked thread is first woken
   - Prevents starvation

### Edge Cases Handled

1. **Multiple waiters**: If N threads wait on a mutex, each unlock wakes exactly one
2. **Spurious wakeups**: Thread might be woken but find mutex taken by another thread
   - Handled by retry loop in pthread_mutex_lock
   - Thread re-adds itself to waitset and parks again
3. **No current thread**: If unlock happens when no thread is current (e.g., all blocked),
   woken thread is made current immediately

## Testing

Created 5 tests:
- `simulate_thread_detection.cpp`: Pre-existing thread detection
- `simulate_unsupported_interceptor.cpp`: Unsupported function detection
- `simulate_spinlock.cpp`: Spinlock detection
- `simulate_sleep.cpp`: Sleep function detection
- `simulate_mutex_contention.cpp`: **New** - 4 threads contending for shared mutex

All tests pass with 1000 iterations each.

## Code Changes

- **tsan_simulate.h**: Added `SimulateMutexBlock/Unblock` API
- **tsan_simulate.cpp**:
  - Added `MutexWaitset` struct
  - Added waitset tracking to `SimScheduler`
  - Implemented `MutexBlock/Unblock` methods
- **tsan_interceptors_posix.cpp**:
  - Modified `pthread_mutex_lock` to use waitsets
  - Modified `pthread_mutex_unlock` to wake waiters

## Performance Impact

- **Reduced depth consumption**: Blocked threads don't spin on trylock
- **More interleavings explored**: Saved depth allows deeper exploration
- **No overhead**: Same number of real scheduler operations, just better targeted
