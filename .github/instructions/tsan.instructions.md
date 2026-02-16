# ThreadSanitizer (TSan) Runtime Library Instructions

## Overview
ThreadSanitizer (TSan) is a data race detector for C/C++ and Go programs. This document provides guidance for AI agents working with the TSan runtime library located in `compiler-rt/lib/tsan/rtl/`.

## Code Style & Conventions

### Naming Conventions

#### Files
- **Core runtime**: `tsan_rtl*.{h,cpp}` - Main runtime implementation
- **Interfaces**: `tsan_interface*.{h,cpp}` - External APIs (C, Java, annotations)
- **Interceptors**: `tsan_interceptors*.cpp` - Function interception for POSIX, macOS, libdispatch
- **Platform-specific**: `tsan_platform_{linux,mac,posix,windows}.cpp`
- **Assembly**: `tsan_rtl_{amd64,aarch64,mips64,ppc64,s390x,loongarch64,riscv64}.S`

#### Namespace & Types
```cpp
namespace __tsan {
  // All TSan code resides in __tsan namespace

  // Strong typedefs for type safety
  enum class Sid : u8 {};      // Thread slot ID
  enum class Epoch : u16 {};   // Vector clock element
  enum class RawShadow : u32 {}; // Shadow memory value

  // Tid is a typedef for thread ID (see ThreadContext)
  typedef u32 Tid;
  constexpr Tid kInvalidTid = ~0U;
}
```

#### Classes & Structs
- **PascalCase**: `ThreadState`, `ThreadContext`, `SyncVar`, `VectorClock`
- **Prefixes**: No Hungarian notation, use descriptive names
- **Alignment**: Use `alignas(SANITIZER_CACHE_LINE_SIZE)` for performance-critical structures

#### Functions
- **CamelCase**: Public/exported functions: `MutexCreate()`, `MemoryAccess()`
- **lowercase_snake**: Internal helpers are rare; prefer CamelCase
- **Macros**: `UPPERCASE_SNAKE_CASE` (e.g., `SCOPED_INTERCEPTOR_RAW`)

#### Variables
- **lowercase with underscores**: `shadow_stack_pos`, `trace_prev_pc`
- **Trailing underscore for private members**: `part_.sid_`, `rep_`
- **Constants**: `kConstantName` (e.g., `kShadowMultiplier`, `kThreadSlotCount`)

### Comment Style

#### File Headers
```cpp
//===-- tsan_file.h ---------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file is a part of ThreadSanitizer (TSan), a race detector.
//
// [Brief description of file purpose]
//===----------------------------------------------------------------------===//
```

#### Documentation
- Use `//` for single-line comments
- Place comments above the code they describe
- Document WHY, not WHAT (code should be self-explanatory)
- Mark important invariants with `DCHECK` and comments

### File Organization Pattern
```cpp
#ifndef TSAN_FILE_H
#define TSAN_FILE_H

// System/sanitizer headers
#include "sanitizer_common/sanitizer_*.h"

// TSan headers (alphabetically)
#include "tsan_defs.h"
#include "tsan_platform.h"
// ...

namespace __tsan {

// Forward declarations
struct ThreadState;

// Constants
const uptr kConstant = 42;

// Type definitions
struct MyStruct { ... };

// Function declarations
void MyFunction(...);

}  // namespace __tsan

#endif  // TSAN_FILE_H
```

### Ground Rules (from tsan_rtl.h)
1. **No C++ runtime**: No static constructors, RTTI, exceptions, or function-scope static locals
2. **Namespace**: All code in `__tsan` namespace except `tsan_interface.h` declarations
3. **Platform abstraction**: Use platform-specific files instead of `#ifdef` where possible
4. **Header inclusion**: No system headers in header files (except when inlining is critical)
5. **64-bit only**: `SANITIZER_WORDSIZE` must be 64

## Architecture

### Core Components

#### 1. **tsan_rtl.{h,cpp}** - Main Runtime
- **Purpose**: Entry points and core runtime logic
- **Key structures**:
  - `Context`: Global runtime context (singleton `ctx`)
  - `ThreadState`: Per-thread state (TLS storage)
  - `Processor`: Physical thread resource (allocator cache)
  - `TidSlot`: Slot in the slot-based threading model

**ThreadState** is the heart of per-thread tracking:
```cpp
struct ThreadState {
  FastState fast_state;              // Current SID + epoch + ignore bit
  VectorClock clock;                 // Thread's vector clock
  atomic_uintptr_t trace_pos;        // Current position in trace
  uptr trace_prev_pc;                // For PC delta compression
  uptr *shadow_stack_pos;            // Shadow call stack
  MutexSet mset;                     // Currently held mutexes
  Processor *proc1;                  // Wired processor
  TidSlot *slot;                     // Current slot
  ThreadContext *tctx;               // Thread context
  // ... many more fields
};
```

#### 2. **tsan_shadow.h** - Shadow Memory System
- **Shadow cells**: Each 8 bytes of application memory maps to 4 shadow values (32 bytes)
- **Shadow encoding**: Stores SID, epoch, access type, address within 8-byte cell
- **Key classes**:
  - `FastState`: Compact thread state (SID + epoch)
  - `Shadow`: Single shadow value with access info
  - `RawShadow`: Type-safe u32 wrapper

**Shadow memory mapping** (see tsan_platform.h):
```
User memory:  [Lo, Mid, Hi regions]
                    ↓
Shadow:       [×8 larger, stores RawShadow values]
Meta shadow:  [Sync objects and memory blocks]
```

#### 3. **tsan_sync.{h,cpp}** - Synchronization Objects
- **SyncVar**: Descriptor for mutexes and atomics
  - Contains vector clocks (`clock`, `read_clock` for rwlocks)
  - Tracks ownership, recursion, creation stack
  - Managed by `MetaMap` using dense allocation
- **MetaMap**: Maps addresses to `SyncVar` and memory block metadata

#### 4. **tsan_vector_clock.{h,cpp}** - Happens-Before Tracking
- **Fixed-size array**: 256 slots (one per `Sid`)
- **Operations**:
  - `Acquire(src)`: Merge src clock into this
  - `Release(dstp)`: Copy this clock to dst
  - `ReleaseAcquire`, `ReleaseStore`, etc.
- **Purpose**: Implements Lamport logical clocks for race detection

#### 5. **tsan_trace.h** - Execution History
- **TracePart**: Circular buffer of events
- **Event types** (tagged union):
  - `EventAccess`: Compressed memory access (15-bit PC delta)
  - `EventAccessExt`: Full PC memory access
  - `EventAccessRange`: Range access (memcpy, etc.)
  - `EventFunc`: Function entry/exit
  - `EventLock/RLock/Unlock`: Mutex operations
  - `EventTime`: Vector clock checkpoint
- **Trace switching**: When part is full, allocate new part (see `TracePartAlloc`)

#### 6. **tsan_rtl_access.cpp** - Memory Access Handling
- **Fast path**: Inline race check in `MemoryAccess` / `MemoryAccessRange`
- **Race detection**:
  1. Check shadow memory for conflicting access
  2. Compare SID/epoch using vector clocks
  3. If race detected, call `DoReportRace`
- **Trace recording**: Compress accesses into trace events

#### 7. **tsan_rtl_mutex.cpp** - Mutex Operations
- **API**: `MutexCreate`, `MutexDestroy`, `MutexLock`, `MutexUnlock`, etc.
- **Deadlock detection**: Integration with `DDMutex` (deadlock detector)
- **Happens-before**: Acquire/release semantics via vector clocks
- **MutexSet**: Track currently held locks per thread

#### 8. **tsan_rtl_thread.cpp** - Thread Lifecycle
- **ThreadCreate**: Allocate slot, initialize ThreadState
- **ThreadFinish**: Release resources, join bookkeeping
- **Slot system**: Reuse thread slots (256 max concurrent)

#### 9. **tsan_rtl_report.cpp** - Race Reporting
- **ReportDesc**: Describes a race (stacks, memory locations, threads involved)
- **Symbolization**: Convert PCs to source locations
- **Suppressions**: Filter reports based on suppressions file
- **Deduplication**: Track reported races by hash

### Interceptors

#### Purpose
Wrap library functions to:
1. Track synchronization (pthread_mutex_lock, etc.)
2. Track memory allocation (malloc, free)
3. Ensure happens-before on I/O operations
4. Handle special cases (setjmp/longjmp)

#### Key Files
- **tsan_interceptors_posix.cpp**: ~3300 lines, POSIX interceptors
- **tsan_interceptors_mac.cpp**: macOS-specific (dispatch, etc.)
- **tsan_interceptors_libdispatch.cpp**: Grand Central Dispatch
- **tsan_interceptors_memintrinsics.cpp**: memcpy, memset, memmove

#### Pattern
```cpp
TSAN_INTERCEPTOR(int, pthread_mutex_lock, void *m) {
  // SCOPED_TSAN_INTERCEPTOR sets up thread state
  SCOPED_TSAN_INTERCEPTOR(pthread_mutex_lock, m);

  // Pre-lock actions (deadlock detection, etc.)
  MutexPreLock(thr, pc, (uptr)m);

  // Call real function
  int res = REAL(pthread_mutex_lock)(m);

  // Post-lock actions (update vector clock, record in trace)
  if (res == 0 || res == EDEADLK)
    MutexPostLock(thr, pc, (uptr)m);

  return res;
}
```

#### ScopedInterceptor
Manages thread state entry/exit:
- Initializes `ThreadState` if needed
- Handles `ignore_interceptors` flag
- Manages `in_ignored_lib` tracking

### Platform-Specific Code

#### Memory Layout (tsan_platform.h)
Different address space mappings for:
- **Linux/FreeBSD x86_64**: 48-bit address space
- **macOS ARM64**: Custom mapping
- **Linux MIPS64**: 40-bit VMA
- **Many more**: aarch64, loongarch64, ppc64, s390x, riscv64

Each defines:
- `kLoAppMemBeg/End`, `kMidAppMemBeg/End`, `kHiAppMemBeg/End`
- `kShadowBeg/End`, `kMetaShadowBeg/End`
- `kHeapMemBeg/End`

#### Assembly Files
Low-level routines for:
- **setjmp/longjmp interception**: Save/restore trace state
- **Function entry/exit**: Fast path for `-fsanitize=thread` instrumentation
- Platform calling conventions

### Memory Management

#### User Allocations (tsan_mman.{h,cpp})
- **Custom allocator**: `SizeClassAllocator32/64`
- **Hooks**: `user_alloc`, `user_free`, `user_realloc`
- **Tracking**: Allocate sync metadata, record freed regions
- **Hook invocation**: `invoke_malloc_hook`, `invoke_free_hook`

#### Internal Allocations
- `Alloc(size)`, `Free(ptr)`: Raw internal allocation
- `New<T>(...)`, `DestroyAndFree<T>(p)`: C++ object helpers
- **Processor cache**: `alloc_cache`, `internal_alloc_cache`

#### Dense Allocation (tsan_dense_alloc.h)
- **Purpose**: Allocate sync objects and trace parts densely
- **2-level array**: IndexT instead of pointers (space efficient)
- **Cache per processor**: Reduce contention
- **Used for**: `SyncVar`, `TracePart`

## Build System

### CMakeLists.txt Structure
```cmake
# compiler-rt/lib/tsan/rtl/CMakeLists.txt
set(TSAN_SOURCES
  tsan_debugging.cpp
  tsan_external.cpp
  tsan_rtl.cpp
  tsan_rtl_access.cpp
  tsan_rtl_mutex.cpp
  tsan_rtl_thread.cpp
  tsan_rtl_report.cpp
  # ... ~45 source files
)

# Platform-specific sources added conditionally
if(APPLE)
  list(APPEND TSAN_SOURCES
    tsan_interceptors_mac.cpp
    tsan_platform_mac.cpp
  )
elseif(UNIX)
  list(APPEND TSAN_SOURCES
    tsan_platform_linux.cpp
  )
endif()

# Assembly files for each architecture
# tsan_rtl_amd64.S, tsan_rtl_aarch64.S, etc.
```

### Compiler Flags
- **-fPIE**: Performance-critical for TSan (reduce register spills)
- **-msse4.2**: SIMD for vector clock operations
- **-Wframe-larger-than=530**: Limit stack frame size
- **-DTSAN_DEBUG_OUTPUT=2**: Optional debug builds

### External Dependencies
- **sanitizer_common**: Shared infrastructure (allocators, symbolization, platform utils)
- **interception**: Function interception infrastructure
- **ubsan** (optional): Undefined behavior sanitizer integration

## Testing

### Test Structure
- **tests/rtl/**: Unit tests using Google Test
  - `tsan_test.cpp`: Basic functionality
  - `tsan_mop.cpp`: Memory access tests
  - `tsan_mutex.cpp`: Synchronization tests
  - `tsan_posix.cpp`: POSIX API tests
  - `tsan_thread.cpp`: Thread lifecycle tests

### Test Utilities
- **tsan_test_util.h**: Helper classes
  - `ScopedThread`: RAII thread management
  - `MemLoc`: Memory location wrapper
- **Pattern**:
```cpp
TEST_F(ThreadSanitizer, BasicRace) {
  ScopedThread t1, t2;
  MemLoc l;
  t1.Write1(l);           // Write in thread 1
  t2.Write1(l, true);     // Write in thread 2 (expect race)
}
```

### Integration Tests
- **tests/**: Lit-based tests (not in rtl/)
- Compile test programs with `-fsanitize=thread`
- Check for expected race reports

## External Interfaces

### 1. Compiler Instrumentation Interface (tsan_interface.h)
```cpp
// Called by compiler instrumentation
void __tsan_init();
void __tsan_read1/2/4/8/16(void *addr);
void __tsan_write1/2/4/8/16(void *addr);
void __tsan_func_entry(void *pc);
void __tsan_func_exit();
void __tsan_vptr_update(void **vptr_p, void *new_val);
```

### 2. Annotation Interface (tsan_interface_ann.h)
```cpp
// User annotations for custom synchronization
void __tsan_acquire(void *addr);
void __tsan_release(void *addr);
```

### 3. External Tag Interface (tsan_interface.h, tsan_external.cpp)
```cpp
// For Swift, Objective-C, etc.
void *__tsan_external_register_tag(const char *object_type);
void __tsan_external_assign_tag(void *addr, void *tag);
void __tsan_external_read(void *addr, void *caller_pc, void *tag);
```

### 4. Java Interface (tsan_interface_java.h)
```cpp
// For JVM integration
void __tsan_java_init(jptr heap_begin, jptr heap_size);
void __tsan_java_alloc(jptr ptr, jptr size);
void __tsan_java_mutex_lock(jptr addr);
void __tsan_java_acquire(jptr addr);
// ... etc.
```

### 5. Atomic Operations (tsan_interface_atomic.cpp)
```cpp
// C11/C++11 atomic operations
#define ATOMIC_RMW(op, size, mo) \
  __tsan_atomic##size##_##op(a##size *a, a##size v, morder mo)
// e.g., __tsan_atomic64_fetch_add(...)
```

## Race Detection Algorithm

### Shadow State
For each 8-byte aligned memory cell, store 4 shadow values:
- **Per shadow**: SID (thread), Epoch (timestamp), AccessType, offset within cell
- **Race check**: If access to same location has different SID and not ordered by vector clocks → race

### Happens-Before
1. **Release**: `lock.unlock()` copies thread clock to lock's clock
2. **Acquire**: `lock.lock()` merges lock's clock into thread clock
3. **Detection**: If access A's epoch > thread's clock[A.sid] → not ordered → race

### Fast Path Optimization
```cpp
// Check shadow memory inline
Shadow cur(thr->fast_state, addr, size, typ);
RawShadow *shadow_mem = MemToShadow(addr);
Shadow old(*shadow_mem);

// Fast check: same thread, or both reads, or both atomic
if (old.sid() == cur.sid() || old.IsBothReadsOrAtomic(typ)) {
  *shadow_mem = cur.raw();  // Update shadow
  return;  // No race
}

// Slow path: check vector clocks
if (thr->clock.Get(old.sid()) < old.epoch()) {
  ReportRace(...);  // Race detected!
}
```

## Configuration & Flags

### Runtime Flags (tsan_flags.inc)
```
TSAN_FLAG(bool, report_bugs, true, "Enable race reporting")
TSAN_FLAG(bool, report_thread_leaks, true, "Report thread leaks at exit")
TSAN_FLAG(bool, report_destroy_locked, true, "Report mutex destroyed while locked")
TSAN_FLAG(bool, report_signal_unsafe, true, "Report signal-unsafe calls")
TSAN_FLAG(uptr, history_size, 0, "Per-thread history size")
TSAN_FLAG(int, io_sync, 1, "I/O synchronization level 0-2")
TSAN_FLAG(const char*, suppressions, "", "Suppressions file")
```

### Environment Variables
- `TSAN_OPTIONS="flag=value:flag2=value2"`
- `__tsan_default_options()`: Weak symbol for embedding options

## Security Considerations

### Signal Safety
- **atomic_uintptr_t in_signal_handler**: Track signal handler depth
- **nomalloc flag**: Prevent malloc in signal handlers
- **signum field**: Track which signal triggered

### Fork Safety
- `die_after_fork=true`: Detect unsafe post-fork threading
- `after_multithreaded_fork`: Track state
- Memory must be reset or marked safe

### Interceptor Safety
- Always check `REAL(func)` is non-null
- Use `ScopedGlobalProcessor` for late interceptors
- Handle `in_symbolizer` to avoid recursion

### Suppression Loading
- External suppressions file prevents DoS via reports
- Stack hashing avoids duplicate reports

## Common Patterns & Idioms

### 1. Slot Locking
```cpp
SlotLocker locker(thr);  // RAII lock current thread's slot
// Now safe to access sync objects
```

### 2. Fast State
```cpp
FastState fs;
fs.SetSid(thr->slot->sid);
fs.SetEpoch(thr->slot->epoch());
```

### 3. Stack Trace Capture
```cpp
VarSizeStackTrace stack;
ObtainCurrentStack(thr, pc, &stack);
StackID id = CurrentStackId(thr, pc);
```

### 4. Scope-based Reporting
```cpp
ScopedReport rep(ReportTypeRace);
rep.AddMemoryAccess(addr, ...);
rep.AddStack(stack);
rep.AddThread(thr->tid);
// ~ScopedReport() prints and potentially exits
```

### 5. Dense Allocation
```cpp
DenseSlabAllocCache cache;
ctx->sync_alloc.InitCache(&cache);
IndexT idx = ctx->sync_alloc.Alloc(&cache);
SyncVar *s = ctx->sync_alloc.Map(idx);
```

## Key Constants

```cpp
const uptr kThreadSlotCount = 256;      // Max concurrent threads
const uptr kShadowStackSize = 64 * 1024; // Shadow call stack
const uptr kShadowCnt = 4;               // Shadow values per cell
const uptr kShadowCell = 8;              // Bytes per shadow cell
const uptr kShadowMultiplier = 32;       // Shadow expansion factor
const uptr kCompressedAddrBits = 44;     // Compressed address size
const uptr kEpochBits = 14;              // Bits for epoch counter
```

## Debugging & Development

### Debug Output
```cpp
DPrintf("#%d: FunctionName %zx\n", thr->tid, addr);
```
Enable with `-DTSAN_DEBUG_OUTPUT=2` build flag.

### DCHECK Assertions
```cpp
DCHECK_EQ(value, expected);
DCHECK_NE(ptr, nullptr);
DCHECK_LT(a, b);
```
Active only in debug builds.

### Trace Inspection
- Set `history_size` flag to retain more trace
- Use `__tsan_print_report()` in debugger
- Symbolize with `addr2line` or `llvm-symbolizer`

## Anti-Patterns to Avoid

❌ **Don't use C++ standard library** in runtime code (no `std::vector`, `std::string`)
✅ **Use** `sanitizer_common/sanitizer_vector.h`, manual string handling

❌ **Don't use malloc/new directly** in hot paths
✅ **Use** `Processor` cache allocators, placement new

❌ **Don't add virtual functions** (no RTTI)
✅ **Use** tagged unions, explicit type fields

❌ **Don't include system headers** in `.h` files
✅ **Include** only in `.cpp`, use forward declarations

❌ **Don't use thread-local storage** (except for `ThreadState`)
✅ **Store** in `ThreadState` or `Processor`

## References

### Key Papers & Documentation
- **FastTrack algorithm**: PLDI 2009 (basis for TSan race detection)
- **Vector clocks**: Lamport timestamps for partial ordering
- **C++11 memory model**: Foundation for atomic operations

### Related Code
- **sanitizer_common/**: Shared sanitizer infrastructure
- **llvm/lib/Transforms/Instrumentation/ThreadSanitizer.cpp**: Compiler pass
- **clang/lib/CodeGen/**: Code generation for instrumentation calls

### External Resources
- https://github.com/google/sanitizers (upstream project)
- https://clang.llvm.org/docs/ThreadSanitizer.html (user documentation)

---

## Quick Reference for Common Tasks

### Adding a New Interceptor
1. Declare in `tsan_interceptors_posix.cpp` (or platform-specific file)
2. Use `TSAN_INTERCEPTOR(ret, func, ...)` macro
3. Add `SCOPED_TSAN_INTERCEPTOR(func, ...)` in body
4. Call `REAL(func)` for original behavior
5. Add synchronization tracking (e.g., `MutexLock`, `Acquire`)

### Adding a New Flag
1. Add to `tsan_flags.inc`: `TSAN_FLAG(type, name, default, "description")`
2. Access via `flags()->name`
3. Document user-facing flags in docs

### Adding a Platform
1. Define memory layout in `tsan_platform.h`
2. Create `tsan_platform_<os>.cpp` with platform-specific helpers
3. Add assembly file `tsan_rtl_<arch>.S` if needed
4. Update `CMakeLists.txt` with conditionals

### Modifying Race Detection
- **Fast path**: `tsan_rtl_access.cpp::MemoryAccess()`
- **Shadow encoding**: `tsan_shadow.h::Shadow` class
- **Vector clock operations**: `tsan_vector_clock.{h,cpp}`
- **Report generation**: `tsan_rtl_report.cpp::DoReportRace()`

### Performance Tuning
- Inline hot paths with `ALWAYS_INLINE`
- Use `LIKELY`/`UNLIKELY` macros for branch hints
- Minimize trace events (compress, batch)
- Tune `history_size` flag for memory/precision tradeoff
- Use `VECTOR_ALIGNED` for vector clock arrays

---

**Version**: Based on analysis of LLVM project as of February 2026
**Applies to**: `compiler-rt/lib/tsan/rtl/**/*`
