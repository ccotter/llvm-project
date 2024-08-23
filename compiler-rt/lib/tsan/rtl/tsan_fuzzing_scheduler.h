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

struct IFuzzingScheduler {
  virtual void Init() = 0;

  virtual void MutexCvOp() = 0;
  virtual void AtomicOpFence(int mo) = 0;
  virtual void AtomicOpAddr(__sanitizer::uptr addr, int mo) = 0;

  virtual int DetachThread(void* th) = 0;
  virtual void AfterThreadCreation() = 0;
  virtual void BeforeChildThreadRuns() = 0;
  virtual void JoinOp() = 0;

 protected:
  IFuzzingScheduler() = default;

  // Derived types of IFuzzingScheduler are only constructed on the stack.
  // No code ever deletes a base pointer, so a non-virtual destructor is OK.
  // There is a separate clang warning, -Wdelete-non-abstract-non-virtual-dtor,
  // that catches deleting pointers of types with virtual methods but a
  // non-virtual destructor.
  //
  // The destructor cannot be virtual, otherwise it would emit references to
  // operator delete, which the TSAN runtime cannot depend on in some
  // environments.
#ifdef __clang__
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Wnon-virtual-dtor"
#endif
  ~IFuzzingScheduler() = default;
#ifdef __clang__
#  pragma clang diagnostic pop
#endif
};

IFuzzingScheduler& GetFuzzingScheduler();

extern bool is_fuzz_scheduler_enabled;

ALWAYS_INLINE bool IsFuzzSchedulerEnabled() {
  return is_fuzz_scheduler_enabled;
}

}  // namespace __tsan

#endif
