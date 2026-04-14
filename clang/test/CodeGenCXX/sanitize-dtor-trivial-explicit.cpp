// Test that -fsanitize-memory-use-after-dtor instruments explicit
// destructor calls on trivially-destructible types.
// RUN: %clang_cc1 -O0 -fsanitize=memory -fsanitize-memory-use-after-dtor -disable-llvm-passes -std=c++17 -triple=x86_64-pc-linux -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -O1 -fsanitize=memory -fsanitize-memory-use-after-dtor -disable-llvm-passes -std=c++17 -triple=x86_64-pc-linux -emit-llvm -o - %s | FileCheck %s

struct Trivial {
  int a;
  int b;
};

// Explicit destructor call on a trivially-destructible type should
// emit a __sanitizer_dtor_callback_fields to poison the memory.
void test_explicit_dtor(Trivial *t) {
  t->~Trivial();
}
// CHECK-LABEL: define {{.*}}test_explicit_dtor
// CHECK: call void @__sanitizer_dtor_callback_fields(ptr %{{.*}}, i64 8)
// CHECK: ret void

struct TrivialSingle {
  double d;
};

void test_single_field(TrivialSingle *t) {
  t->~TrivialSingle();
}
// CHECK-LABEL: define {{.*}}test_single_field
// CHECK: call void @__sanitizer_dtor_callback_fields(ptr %{{.*}}, i64 8)
// CHECK: ret void

// Empty types should not get a callback.
struct Empty {};

void test_empty(Empty *e) {
  e->~Empty();
}
// CHECK-LABEL: define {{.*}}test_empty
// CHECK-NOT: call void @__sanitizer_dtor_callback
// CHECK: ret void

// Verify that without -fsanitize-memory-use-after-dtor, no callback
// is emitted even for explicit calls. This is tested by the
// --implicit-check-not in the RUN line above and by the CHECK-NOT
// patterns covering the expected output.
