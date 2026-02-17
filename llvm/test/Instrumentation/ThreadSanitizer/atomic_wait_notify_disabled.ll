; RUN: opt < %s -passes=tsan -tsan-instrument-atomic-wait=false -S | FileCheck %s

target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v64:64:64-v128:128:128-a0:0:64-s0:64:64-f80:128:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

; When -tsan-instrument-atomic-wait=false, wait/notify calls should NOT be instrumented
; Note: Functions with sanitize_thread still get func entry/exit instrumentation,
; but the atomic wait/notify calls themselves should NOT get __tsan_* hooks

; Test that wait is NOT instrumented when flag is disabled
define void @atomic_int_wait_disabled(ptr %a) sanitize_thread {
entry:
  call void @_ZN9std6atomicIiE4waitEii(ptr %a, i32 42, i32 2)
  ret void
}

declare void @_ZN9std6atomicIiE4waitEii(ptr, i32, i32)

; CHECK-LABEL: atomic_int_wait_disabled
; CHECK-NOT: __tsan_atomic_wait_for
; CHECK: _ZN9std6atomicIiE4waitEii

; Test that notify_one is NOT instrumented when flag is disabled
define void @atomic_int_notify_one_disabled(ptr %a) sanitize_thread {
entry:
  call void @_ZN9std6atomicIiE11notify_oneEv(ptr %a)
  ret void
}

declare void @_ZN9std6atomicIiE11notify_oneEv(ptr)

; CHECK-LABEL: atomic_int_notify_one_disabled
; CHECK-NOT: __tsan_atomic_notify_one
; CHECK: _ZN9std6atomicIiE11notify_oneEv

; Test that notify_all is NOT instrumented when flag is disabled
define void @atomic_int_notify_all_disabled(ptr %a) sanitize_thread {
entry:
  call void @_ZN9std6atomicIiE10notify_allEv(ptr %a)
  ret void
}

declare void @_ZN9std6atomicIiE10notify_allEv(ptr)

; CHECK-LABEL: atomic_int_notify_all_disabled
; CHECK-NOT: __tsan_atomic_notify_all
; CHECK: _ZN9std6atomicIiE10notify_allEv
