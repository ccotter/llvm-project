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

#ifndef TSAN_FUZZING_SCHEDULER_H
#define TSAN_FUZZING_SCHEDULER_H

#include "sanitizer_common/sanitizer_common.h"
#include "sanitizer_common/sanitizer_internal_defs.h"

namespace __tsan {

extern bool is_adaptive_delay_enabled;

// AdaptiveDelay injects delays at synchronization points, atomic operations,
// and thread lifecycle events to increase the likelihood of exposing data
// races. The delay injection is controlled by a time budget to maintain a
// configurable overhead target.
struct AdaptiveDelay {
  ALWAYS_INLINE static void Init() {
    InitImpl();
  }

  ALWAYS_INLINE static void MutexCvOp() {
    if (!is_adaptive_delay_enabled) return;
    MutexCvOpImpl();
  }

  ALWAYS_INLINE static void AtomicOpFence(int mo) {
    if (!is_adaptive_delay_enabled) return;
    AtomicOpFenceImpl(mo);
  }

  ALWAYS_INLINE static void AtomicOpAddr(__sanitizer::uptr addr, int mo) {
    if (!is_adaptive_delay_enabled) return;
    AtomicOpAddrImpl(addr, mo);
  }

  ALWAYS_INLINE static void DetachThread() {
    if (!is_adaptive_delay_enabled) return;
    DetachThreadImpl();
  }

  ALWAYS_INLINE static void AfterThreadCreation() {
    if (!is_adaptive_delay_enabled) return;
    AfterThreadCreationImpl();
  }

  ALWAYS_INLINE static void BeforeChildThreadRuns() {
    if (!is_adaptive_delay_enabled) return;
    BeforeChildThreadRunsImpl();
  }

  ALWAYS_INLINE static void JoinOp() {
    if (!is_adaptive_delay_enabled) return;
    JoinOpImpl();
  }

private:

  static void InitImpl();

  static void MutexCvOpImpl();
  static void AtomicOpFenceImpl(int mo);
  static void AtomicOpAddrImpl(__sanitizer::uptr addr, int mo);
  static void DetachThreadImpl();
  static void AfterThreadCreationImpl();
  static void BeforeChildThreadRunsImpl();
  static void JoinOpImpl();
};

AdaptiveDelay& GetAdaptiveDelay();

ALWAYS_INLINE bool IsAdaptiveDelayEnabled() {
  return is_adaptive_delay_enabled;
}

// Fixed-point arithmetic type that mimics floating point operations
class Percent {
  using u32 = __sanitizer::u32;
  using u64 = __sanitizer::u64;

  u32 bp_{};  // basis points (0-10000 represents 0.0-1.0)
  bool is_valid_{};

  static constexpr u32 kBasisPointsPerUnit = 10000;

  Percent(u32 bp, bool is_valid) : bp_(bp), is_valid_(is_valid) {}

 public:
  Percent() = default;
  Percent(const Percent&) = default;
  Percent& operator=(const Percent&) = default;
  Percent(Percent&&) = default;
  Percent& operator=(Percent&&) = default;

  static Percent FromPct(u32 pct) { return Percent{pct * 100, true}; }
  static Percent FromRatio(u64 numerator, u64 denominator) {
    if (denominator == 0)
      return Percent{0, false};
    // Avoid overflow: scale down if needed
    if (numerator > UINT64_MAX / kBasisPointsPerUnit) {
      return Percent{(u32)((numerator / denominator) * kBasisPointsPerUnit),
                     true};
    }
    return Percent{(u32)((numerator * kBasisPointsPerUnit) / denominator),
                   true};
  }

  bool IsValid() const { return is_valid_; }

  // Returns true with probability equal to the percentage.
  bool RandomCheck(u32* seed) const {
    return (Rand(seed) % kBasisPointsPerUnit) < bp_;
  }

  int GetPct() const { return bp_ / 100; }
  int GetBasisPoints() const { return bp_; }

  bool operator==(const Percent& other) const { return bp_ == other.bp_; }
  bool operator!=(const Percent& other) const { return bp_ != other.bp_; }
  bool operator<(const Percent& other) const { return bp_ < other.bp_; }
  bool operator>(const Percent& other) const { return bp_ > other.bp_; }
  bool operator<=(const Percent& other) const { return bp_ <= other.bp_; }
  bool operator>=(const Percent& other) const { return bp_ >= other.bp_; }

  Percent operator-(const Percent& other) const {
    if (!is_valid_ || !other.is_valid_)
      return Percent{0, false};
    if (bp_ < other.bp_)
      return Percent{0, false};
    return Percent{bp_ - other.bp_, true};
  }

  Percent operator/(const Percent& other) const {
    if (!is_valid_ || !other.is_valid_)
      return Percent{0, false};
    if (other.bp_ == 0)
      return Percent{0, false};
    return Percent{(bp_ * kBasisPointsPerUnit) / other.bp_, true};
  }
};

}  // namespace __tsan

#endif
