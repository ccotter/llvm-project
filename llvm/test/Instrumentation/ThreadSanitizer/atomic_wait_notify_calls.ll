; RUN: opt < %s -passes=tsan -S | FileCheck %s

target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v64:64:64-v128:128:128-a0:0:64-s0:64:64-f80:128:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

; Test that std::atomic<int>::wait() is instrumented
define void @atomic_int_wait(ptr %a) sanitize_thread {
entry:
  ; Simulating std::atomic<int>::wait(int, memory_order)
  ; This would come from mangled C++ code like _ZN9std6atomicIiE4waitEii
  call void @_ZN9std6atomicIiE4waitEii(ptr %a, i32 42, i32 2)
  ret void
}

declare void @_ZN9std6atomicIiE4waitEii(ptr, i32, i32)

; CHECK-LABEL: atomic_int_wait
; CHECK: call void @__tsan_atomic_wait_for(ptr %a)

; Test that std::atomic<int>::notify_one() is instrumented
define void @atomic_int_notify_one(ptr %a) sanitize_thread {
entry:
  call void @_ZN9std6atomicIiE11notify_oneEv(ptr %a)
  ret void
}

declare void @_ZN9std6atomicIiE11notify_oneEv(ptr)

; CHECK-LABEL: atomic_int_notify_one
; CHECK: call void @__tsan_atomic_notify_one(ptr %a)

; Test that std::atomic<int>::notify_all() is instrumented
define void @atomic_int_notify_all(ptr %a) sanitize_thread {
entry:
  call void @_ZN9std6atomicIiE10notify_allEv(ptr %a)
  ret void
}

declare void @_ZN9std6atomicIiE10notify_allEv(ptr)

; CHECK-LABEL: atomic_int_notify_all
; CHECK: call void @__tsan_atomic_notify_all(ptr %a)

; Test with long long
define void @atomic_long_wait(ptr %a) sanitize_thread {
entry:
  call void @_ZN9std6atomicIxE4waitExii(ptr %a, i64 100, i32 2)
  ret void
}

declare void @_ZN9std6atomicIxE4waitExii(ptr, i64, i32)

; CHECK-LABEL: atomic_long_wait
; CHECK: call void @__tsan_atomic_wait_for(ptr %a)

; Test with pointer type
define void @atomic_ptr_notify_one(ptr %a) sanitize_thread {
entry:
  call void @_ZN9std6atomicIPvE11notify_oneEv(ptr %a)
  ret void
}

declare void @_ZN9std6atomicIPvE11notify_oneEv(ptr)

; CHECK-LABEL: atomic_ptr_notify_one
; CHECK: call void @__tsan_atomic_notify_one(ptr %a)
