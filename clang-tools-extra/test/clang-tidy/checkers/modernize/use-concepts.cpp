// RUN: %check_clang_tidy -std=c++20 %s modernize-use-concepts %t

// NOLINTBEGIN
namespace std {
template <bool B, class T = void> struct enable_if { };

template <class T> struct enable_if<true, T> { typedef T type; };

template <bool B, class T = void>
using enable_if_t = typename enable_if<B, T>::type;

} // namespace std
// NOLINTEND

struct Obj {
};

template <typename T>
typename std::enable_if<T::some_value, Obj>::type basic() {
  return Obj{};
}
// CHECK-MESSAGES: :[[@LINE-3]]:1: warning: use C++20 requires constraints instead of enable_if [modernize-use-concepts]
// CHECK-FIXES: {{^}}Obj basic() requires T::some_value {{{$}}

template <typename T>
auto basic_trailing() -> typename std::enable_if<T::some_value, Obj>::type {
  return Obj{};
}
// CHECK-MESSAGES: :[[@LINE-3]]:26: warning: use C++20 requires constraints instead of enable_if [modernize-use-concepts]
// CHECK-FIXES: {{^}}auto basic_trailing() -> Obj requires T::some_value {{{$}}
