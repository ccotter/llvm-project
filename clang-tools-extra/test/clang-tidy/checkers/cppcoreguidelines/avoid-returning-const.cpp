// RUN: %check_clang_tidy %s cppcoreguidelines-avoid-returning-const %t

struct Obj {};

const int return_const_int(){ return {}; }
// CHECK-MESSAGES: :[[@LINE-1]]:1: warning: avoid returning const values [cppcoreguidelines-avoid-returning-const]
// CHECK-FIXES: int return_const_int(){ return {}; }

const Obj return_const_obj(){ return {}; }
// CHECK-MESSAGES: :[[@LINE-1]]:1: warning: avoid returning const values [cppcoreguidelines-avoid-returning-const]
// CHECK-FIXES: Obj return_const_obj(){ return {}; }

Obj const return_obj_const(){ return {}; }
// CHECK-MESSAGES: :[[@LINE-1]]:1: warning: avoid returning const values [cppcoreguidelines-avoid-returning-const]
// CHECK-FIXES: Obj const return_obj_const(){ return {}; }


namespace negative_tests {

Obj returns_obj(){ return {}; }
volatile Obj returns_volatile_obj(){ return {}; }
const Obj* returns_const_obj_pointer(){ return {}; }
const Obj& returns_const_obj_reference(){ return {}; }
constexpr Obj returns_constexpr_obj(){ return {}; }

}


namespace qualifiers_combinations {

const static Obj returns_const1(){ return {}; }
// CHECK-MESSAGES: :[[@LINE-1]]:1: warning: avoid returning const values [cppcoreguidelines-avoid-returning-const]
// CHECK-FIXES: static Obj returns_const1(){ return {}; }

volatile const static Obj returns_const2(){ return {}; }
// CHECK-MESSAGES: :[[@LINE-1]]:10: warning: avoid returning const values [cppcoreguidelines-avoid-returning-const]
// CHECK-FIXES: volatile static Obj returns_const2(){ return {}; }

const long static int volatile constexpr unsigned inline long returns_const3(){ return {}; }
// CHECK-MESSAGES: :[[@LINE-1]]:1: warning: avoid returning const values [cppcoreguidelines-avoid-returning-const]
// CHECK-FIXES: long static int volatile constexpr unsigned inline long returns_const3(){ return {}; }

constexpr volatile const inline static Obj returns_const4(){ return {}; }
// CHECK-MESSAGES: :[[@LINE-1]]:20: warning: avoid returning const values [cppcoreguidelines-avoid-returning-const]
// CHECK-FIXES: constexpr volatile inline static Obj returns_const4(){ return {}; }

// Whe only support looking for 'const' behind where FunctionDecl says the TypeLoc starts.

long const static int volatile constexpr unsigned inline long returns_const5(){ return {}; }
// CHECK-MESSAGES: :[[@LINE-1]]:1: warning: avoid returning const values [cppcoreguidelines-avoid-returning-const]
// CHECK-FIXES: long const static int volatile constexpr unsigned inline long returns_const5(){ return {}; }

long static int volatile constexpr unsigned inline long const returns_const6(){ return {}; }
// CHECK-MESSAGES: :[[@LINE-1]]:1: warning: avoid returning const values [cppcoreguidelines-avoid-returning-const]
// CHECK-FIXES: long static int volatile constexpr unsigned inline long const returns_const6(){ return {}; }

} // namespace qualifiers_combinations
