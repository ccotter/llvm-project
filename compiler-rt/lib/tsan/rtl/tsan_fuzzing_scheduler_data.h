//===-- tsan_fuzzing_scheduler_data.h ---------------------------*- C++ -*-===//
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

#ifndef TSAN_FUZZING_SCHEDULER_DATA_H
#define TSAN_FUZZING_SCHEDULER_DATA_H

#include "sanitizer_common/sanitizer_internal_defs.h"

namespace __tsan {

// The runtime defines cur_thread() to retrieve TLS thread state, and it
// takes care of platform specific implementation details. Rather than the
// IFuzzingScheduler derived types reinventing the wheel, we define all possible
// TLS data in this type, which will be available in cur_thread().
struct FuzzingSchedulerTlsData {
  // For the adaptive scheduler
  // Sliding window delay tracking: 2 buckets of 30 seconds each
  u64 delay_buckets_ns_[2];  // [0] = older 30s, [1] = newer 30s
  u64 bucket_start_ns_;      // When current bucket (index 1) started
  unsigned int tls_random_seed_;
  bool tls_initialized_;
};

}  // namespace __tsan

#endif  // TSAN_FUZZING_SCHEDULER_DATA_H
