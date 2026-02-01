//===-- tsan_fuzzing_scheduler.h --------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file is a part of ThreadSanitizer (TSan), a race detector.
//
//===----------------------------------------------------------------------===//

#include "tsan_fuzzing_scheduler.h"

#include "interception/interception.h"
#include "sanitizer_common/sanitizer_allocator_internal.h"
#include "sanitizer_common/sanitizer_common.h"
#include "sanitizer_common/sanitizer_errno_codes.h"
#include "tsan_interface.h"
#include "tsan_rtl.h"

extern "C" int pthread_detach(void*);

namespace __interception {
extern int (*real_pthread_detach)(void*);
}  // namespace __interception

namespace __tsan {

namespace {

#ifdef __clang__
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Wnon-virtual-dtor"
#endif
struct NullFuzzingScheduler : IFuzzingScheduler {
#ifdef __clang__
#  pragma clang diagnostic pop
#endif
  void Init() override {}
  void AtomicOpFence(int mo) override {}
  void AtomicOpAddr(uptr addr, int mo) override {}
  void MutexCvOp() override {}
  int DetachThread(void* th) override { return REAL(pthread_detach)(th); }
  void BeforeChildThreadRuns() override {}
  void AfterThreadCreation() override {}
  void JoinOp() override {}
};

static constexpr u64 microseconds_per_second = 1000000ULL;

// =============================================================================
// AdaptiveDelayScheduler: Time-budget aware delay injection for race exposure
// =============================================================================
//
// This scheduler injects delays to expose data races while maintaining a
// configurable overhead target. It uses several strategies:
//
// 1. Time-Budget Controller: Tracks cumulative delays vs wall-clock time
//    and adjusts delay probability to maintain target overhead.
//
// 2. Tiered Delays: Different delay strategies for different op types:
//    - Relaxed atomics: Very rare sampling, tiny spin delays
//    - Sync atomics (acq/rel/seq_cst): Moderate sampling, small usleep
//    - Mutex/CV ops: Higher sampling, larger delays
//    - Thread create/join: Always delay (rare but high value)
//
// 3. Address-based Sampling: Exponential backoff per address to avoid
//    repeatedly delaying hot atomics.
//
// 4. Per-thread Quotas: Each thread has a delay budget per time window.

#ifdef __clang__
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Wnon-virtual-dtor"
#endif
struct AdaptiveDelayScheduler : NullFuzzingScheduler {
#ifdef __clang__
#  pragma clang diagnostic pop
#endif
  struct TimeBudget {
    atomic_uint64_t total_delay_ns_;
    u64 program_start_ns_;
    int target_overhead_pct_;
    Percent target_low_;
    Percent target_high_;

    void Init(int target_pct) {
      atomic_store(&total_delay_ns_, 0, memory_order_relaxed);
      program_start_ns_ = NanoTime();
      target_overhead_pct_ = target_pct;
      target_low_ = Percent::FromPct(
          target_overhead_pct_ >= 5 ? target_overhead_pct_ - 5 : 0);
      target_high_ = Percent::FromPct(target_overhead_pct_ + 5);
    }

    void RecordDelay(u64 delay_ns) {
      atomic_fetch_add(&total_delay_ns_, delay_ns, memory_order_relaxed);
    }

    Percent GetOverheadPercent() {
      u64 elapsed = NanoTime() - program_start_ns_;
      u64 one_millisecond = microseconds_per_second;
      if (elapsed < one_millisecond)
        return Percent::FromPct(0);
      u64 delay = atomic_load(&total_delay_ns_, memory_order_relaxed);
      return Percent::FromRatio(delay, elapsed);
    }

    bool ShouldDelay() {
      Percent ratio = GetOverheadPercent();

      if (ratio < target_low_)
        return true;
      if (ratio > target_high_)
        return false;

      // Linear interpolation: at target_low -> 100%, at target_high -> 0%
      Percent prob = (target_high_ - ratio) / (target_high_ - target_low_);
      return prob.RandomCheck(GetRandomSeed());
    }
  };

  // Address Sampler with Exponential Backoff
  struct AddressSampler {
    static constexpr u64 TABLE_SIZE = 2048;
    struct Entry {
      atomic_uintptr_t addr_;
      atomic_uint32_t count_;
    };
    Entry table_[TABLE_SIZE];
    static constexpr u32 ExponentialBackoffCap = 128;

    void Init() {
      for (u64 i = 0; i < TABLE_SIZE; ++i) {
        atomic_store(&table_[i].addr_, 0, memory_order_relaxed);
        atomic_store(&table_[i].count_, 0, memory_order_relaxed);
      }
    }

    static ALWAYS_INLINE u64 splitmix64(u64 x) {
      x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
      x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
      x = x ^ (x >> 31);
      return x;
    }

    // Uses exponential backoff: delay on 1st, 2nd, 4th, 8th, 16th, ...
    bool ShouldDelayAddr(uptr addr) {
      u64 idx = splitmix64(addr >> 3) & (TABLE_SIZE - 1);
      Entry& e = table_[idx];

      // This function is not thread safe.
      // If two threads access the same hashed entry in parallel,
      // worst case, we may end up returning true too often. This is
      // acceptable...instead of full locking.

      uptr stored_addr = atomic_load(&e.addr_, memory_order_relaxed);
      if (stored_addr != addr) {
        // Hash Collision - reset
        atomic_store(&e.addr_, addr, memory_order_relaxed);
        atomic_store(&e.count_, 1, memory_order_relaxed);
        return true;
      }

      u32 count = atomic_fetch_add(&e.count_, 1, memory_order_relaxed) + 1;

      if ((count & (count - 1)) == 0 && count <= ExponentialBackoffCap)
        return true;
      return false;
    }
  };

  TimeBudget budget_;
  AddressSampler sampler_;

  int relaxed_sample_rate_;
  int sync_atomic_sample_rate_;
  int mutex_sample_rate_;
  int max_atomic_delay_us_;
  int max_sync_delay_us_;
  u64 window_ms_;

  ALWAYS_INLINE static FuzzingSchedulerTlsData* TLS() {
    return &cur_thread()->fuzzingSchedulerTlsData;
  }
  ALWAYS_INLINE static unsigned int* GetRandomSeed() {
    return &cur_thread()->fuzzingSchedulerTlsData.tls_random_seed_;
  }
  ALWAYS_INLINE static void SetRandomSeed(unsigned int seed) {
    cur_thread()->fuzzingSchedulerTlsData.tls_random_seed_ = seed;
  }

  bool CanDelayThread() {
    u64 now = NanoTime();
    bool needs_reset =
        now - TLS()->window_start_ns_ > TLS()->window_duration_ns_;
    if (needs_reset) {
      TLS()->window_start_ns_ = now;
      TLS()->delays_this_window_ = 0;
    }

    if (TLS()->delays_this_window_ >= TLS()->max_delays_per_window_)
      return false;
    return true;
  }

  void RecordOneDelayThisThread() { ++TLS()->delays_this_window_; }

  void Init() override { InitTls(); }

  void InitTls() {
    TLS()->window_start_ns_ = NanoTime();
    TLS()->delays_this_window_ = 0;
    static constexpr int max_delays_per_window_default = 500;
    TLS()->max_delays_per_window_ = max_delays_per_window_default;
    TLS()->window_duration_ns_ = window_ms_ * microseconds_per_second;

    SetRandomSeed(flags()->adaptive_delay_random_seed);
    if (*GetRandomSeed() == 0)
      SetRandomSeed(NanoTime());
    TLS()->tls_initialized_ = true;
  }

  bool IsTlsInitialized() const { return TLS()->tls_initialized_; }

  AdaptiveDelayScheduler() {
    relaxed_sample_rate_ = flags()->adaptive_delay_relaxed_sample_rate;
    sync_atomic_sample_rate_ = flags()->adaptive_delay_sync_atomic_sample_rate;
    mutex_sample_rate_ = flags()->adaptive_delay_mutex_sample_rate;
    max_atomic_delay_us_ = flags()->adaptive_delay_max_atomic_us;
    max_sync_delay_us_ = flags()->adaptive_delay_max_sync_us;
    window_ms_ = flags()->adaptive_delay_window_ms;

    int target_pct = flags()->adaptive_delay_target_overhead_pct;
    if (target_pct < 1)
      target_pct = 1;

    budget_.Init(target_pct);
    sampler_.Init();

    Printf("INFO: ThreadSanitizer AdaptiveDelayScheduler initialized\n");
    Printf("  Target overhead: %d%%\n", target_pct);
    Printf("  Random seed: %u\n", *GetRandomSeed());
    Printf("  Relaxed atomic sample rate: 1/%d\n", relaxed_sample_rate_);
    Printf("  Sync atomic sample rate: 1/%d\n", sync_atomic_sample_rate_);
    Printf("  Mutex sample rate: 1/%d\n", mutex_sample_rate_);
    Printf("  Max atomic delay: %d us\n", max_atomic_delay_us_);
    Printf("  Max sync delay: %d us\n", max_sync_delay_us_);
    Printf("  Delay window: %llu ms\n", window_ms_);
  }

  void DoSpinDelay(int cycles) {
    volatile int v = 0;
    for (int i = 0; i < cycles; ++i) v = i;
    (void)v;
  }

  void DoYieldDelay() { internal_sched_yield(); }

  void UsleepDelay(int max_us) {
    int delay_us = 1 + (Rand(GetRandomSeed()) % max_us);
    internal_usleep(delay_us);
    budget_.RecordDelay(delay_us * 1000ULL);
  }

  void AtomicRelaxedOpDelay() {
    if ((Rand(GetRandomSeed()) % relaxed_sample_rate_) != 0)
      return;
    if (!budget_.ShouldDelay())
      return;
    if (!CanDelayThread())
      return;

    DoSpinDelay(10 + (Rand(GetRandomSeed()) % 10));
    RecordOneDelayThisThread();
    static constexpr int spin_delay_estimate_ns = 50;
    budget_.RecordDelay(spin_delay_estimate_ns);
  }

  void AtomicSyncOpDelay(uptr* addr) {
    if ((Rand(GetRandomSeed()) % sync_atomic_sample_rate_) != 0)
      return;
    if (!budget_.ShouldDelay())
      return;
    if (!CanDelayThread())
      return;

    if (addr && !sampler_.ShouldDelayAddr(*addr))
      return;

    if (max_atomic_delay_us_ <= 1) {
      DoYieldDelay();
      static constexpr int yield_delay_estimate_ns = 100;
      budget_.RecordDelay(yield_delay_estimate_ns);
    } else
      UsleepDelay(max_atomic_delay_us_);
    RecordOneDelayThisThread();
  }

  void AtomicOpFence(int mo) override {
    CHECK(IsTlsInitialized());

    if (mo < mo_acquire)
      AtomicRelaxedOpDelay();
    else
      AtomicSyncOpDelay(nullptr);
  }

  void AtomicOpAddr(uptr addr, int mo) override {
    CHECK(IsTlsInitialized());

    if (mo < mo_acquire)
      AtomicRelaxedOpDelay();
    else
      AtomicSyncOpDelay(&addr);
  }

  void UnsampledDelay() {
    CHECK(IsTlsInitialized());

    if (!budget_.ShouldDelay())
      return;
    if (!CanDelayThread())
      return;

    UsleepDelay(max_sync_delay_us_);
    RecordOneDelayThisThread();
  }

  void MutexCvOp() override {
    CHECK(IsTlsInitialized());

    if ((Rand(GetRandomSeed()) % mutex_sample_rate_) != 0)
      return;
    if (!budget_.ShouldDelay())
      return;
    if (!CanDelayThread())
      return;

    UsleepDelay(max_sync_delay_us_);
    RecordOneDelayThisThread();
  }

  void JoinOp() override { UnsampledDelay(); }

  void BeforeChildThreadRuns() override {
    InitTls();
    UnsampledDelay();
  }

  void AfterThreadCreation() override { UnsampledDelay(); }

  int DetachThread(void* th) override {
    int res = REAL(pthread_detach)(th);
    UnsampledDelay();
    return res;
  }
};

IFuzzingScheduler& FuzzingSchedulerDispatcher() {
  if (!internal_strcmp(flags()->fuzzing_scheduler, "")) {
    is_fuzz_scheduler_enabled = false;
    static NullFuzzingScheduler scheduler;
    return scheduler;
  } else if (!internal_strcmp(flags()->fuzzing_scheduler, "adaptive")) {
    is_fuzz_scheduler_enabled = true;
    static AdaptiveDelayScheduler scheduler;
    return scheduler;
  } else {
    Printf(
        "FATAL: ThreadSanitizer invalid fuzzing scheduler. Please check "
        "TSAN_OPTIONS!\n");
    Die();
  }
}

}  // namespace

bool is_fuzz_scheduler_enabled;

IFuzzingScheduler& GetFuzzingScheduler() {
  static IFuzzingScheduler& scheduler = FuzzingSchedulerDispatcher();
  return scheduler;
}

}  // namespace __tsan
