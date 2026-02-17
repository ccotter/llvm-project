; RUN: opt < %s -passes=tsan -S | FileCheck %s

target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v64:64:64-v128:128:128-a0:0:64-s0:64:64-f80:128:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

; Test wait with i8 / unsigned char
define void @atomic_uchar_wait(ptr %a) sanitize_thread {
entry:
  call void @_ZN9std6atomicIhE4waitEhii(ptr %a, i8 1, i32 2)
  ret void
}

declare void @_ZN9std6atomicIhE4waitEhii(ptr, i8, i32)

; CHECK-LABEL: atomic_uchar_wait
; CHECK: call void @__tsan_atomic_wait_for(ptr %a)

; Test wait with i16 / short
define void @atomic_short_wait(ptr %a) sanitize_thread {
entry:
  call void @_ZN9std6atomicIsE4waitEsii(ptr %a, i16 42, i32 2)
  ret void
}

declare void @_ZN9std6atomicIsE4waitEsii(ptr, i16, i32)

; CHECK-LABEL: atomic_short_wait
; CHECK: call void @__tsan_atomic_wait_for(ptr %a)

; Test wait with i32 / int
define void @atomic_int_wait(ptr %a) sanitize_thread {
entry:
  call void @_ZN9std6atomicIiE4waitEiii(ptr %a, i32 100, i32 2)
  ret void
}

declare void @_ZN9std6atomicIiE4waitEiii(ptr, i32, i32)

; CHECK-LABEL: atomic_int_wait
; CHECK: call void @__tsan_atomic_wait_for(ptr %a)

; Test wait with i64 / long long
define void @atomic_longlong_wait(ptr %a) sanitize_thread {
entry:
  call void @_ZN9std6atomicIxE4waitExii(ptr %a, i64 999, i32 2)
  ret void
}

declare void @_ZN9std6atomicIxE4waitExii(ptr, i64, i32)

; CHECK-LABEL: atomic_longlong_wait
; CHECK: call void @__tsan_atomic_wait_for(ptr %a)

; Test notify_one with various types
define void @atomic_uchar_notify_one(ptr %a) sanitize_thread {
entry:
  call void @_ZN9std6atomicIhE11notify_oneEv(ptr %a)
  ret void
}

declare void @_ZN9std6atomicIhE11notify_oneEv(ptr)

; CHECK-LABEL: atomic_uchar_notify_one
; CHECK: call void @__tsan_atomic_notify_one(ptr %a)

define void @atomic_short_notify_all(ptr %a) sanitize_thread {
entry:
  call void @_ZN9std6atomicIsE10notify_allEv(ptr %a)
  ret void
}

declare void @_ZN9std6atomicIsE10notify_allEv(ptr)

; CHECK-LABEL: atomic_short_notify_all
; CHECK: call void @__tsan_atomic_notify_all(ptr %a)

; Test with unsigned long
define void @atomic_ulong_wait(ptr %a) sanitize_thread {
entry:
  call void @_ZN9std6atomicImE4waitEmii(ptr %a, i64 5555, i32 2)
  ret void
}

declare void @_ZN9std6atomicImE4waitEmii(ptr, i64, i32)

; CHECK-LABEL: atomic_ulong_wait
; CHECK: call void @__tsan_atomic_wait_for(ptr %a)

; Test with pointer type
define void @atomic_ptr_wait(ptr %a) sanitize_thread {
entry:
  call void @_ZN9std6atomicIPvE4waitEPvii(ptr %a, ptr null, i32 2)
  ret void
}

declare void @_ZN9std6atomicIPvE4waitEPvii(ptr, ptr, i32)

; CHECK-LABEL: atomic_ptr_wait
; CHECK: call void @__tsan_atomic_wait_for(ptr %a)
