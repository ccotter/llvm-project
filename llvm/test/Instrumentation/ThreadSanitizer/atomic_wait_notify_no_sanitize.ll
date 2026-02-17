; RUN: opt < %s -passes=tsan -S | FileCheck %s

target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v64:64:64-v128:128:128-a0:0:64-s0:64:64-f80:128:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

; When a function does NOT have the sanitize_thread attribute,
; wait/notify calls should NOT be instrumented

; Test without sanitize_thread attribute - wait should NOT be instrumented
define void @atomic_int_wait_no_sanitize(ptr %a) {
entry:
  call void @_ZN9std6atomicIiE4waitEii(ptr %a, i32 42, i32 2)
  ret void
}

declare void @_ZN9std6atomicIiE4waitEii(ptr, i32, i32)

; CHECK-LABEL: atomic_int_wait_no_sanitize
; CHECK-NOT: __tsan_atomic_wait_for
; CHECK: _ZN9std6atomicIiE4waitEii

; Test without sanitize_thread attribute - notify_one should NOT be instrumented
define void @atomic_int_notify_one_no_sanitize(ptr %a) {
entry:
  call void @_ZN9std6atomicIiE11notify_oneEv(ptr %a)
  ret void
}

declare void @_ZN9std6atomicIiE11notify_oneEv(ptr)

; CHECK-LABEL: atomic_int_notify_one_no_sanitize
; CHECK-NOT: __tsan_atomic_notify_one
; CHECK: _ZN9std6atomicIiE11notify_oneEv

; Test without sanitize_thread attribute - notify_all should NOT be instrumented
define void @atomic_int_notify_all_no_sanitize(ptr %a) {
entry:
  call void @_ZN9std6atomicIiE10notify_allEv(ptr %a)
  ret void
}

declare void @_ZN9std6atomicIiE10notify_allEv(ptr)

; CHECK-LABEL: atomic_int_notify_all_no_sanitize
; CHECK-NOT: __tsan_atomic_notify_all
; CHECK: _ZN9std6atomicIiE10notify_allEv

; But WITH sanitize_thread, they SHOULD be instrumented
define void @atomic_int_wait_with_sanitize(ptr %a) sanitize_thread {
entry:
  call void @_ZN9std6atomicIiE4waitEii(ptr %a, i32 42, i32 2)
  ret void
}

; CHECK-LABEL: atomic_int_wait_with_sanitize
; CHECK: __tsan_atomic_wait_for
