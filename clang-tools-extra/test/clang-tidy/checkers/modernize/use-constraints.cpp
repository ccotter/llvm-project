// RUN: %check_clang_tidy -std=c++20 %s modernize-use-constraints %t

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


////////////////////////////////
// General tests
////////////////////////////////

template <typename T>
typename std::enable_if<T::some_value, Obj>::type basic() {
  return Obj{};
}
// CHECK-MESSAGES: :[[@LINE-3]]:1: warning: use C++20 requires constraints instead of enable_if [modernize-use-constraints]
// CHECK-FIXES: {{^}}Obj basic() requires (T::some_value) {{{$}}

template <typename T>
auto basic_trailing() -> typename std::enable_if<T::some_value, Obj>::type {
  return Obj{};
}
// CHECK-MESSAGES: :[[@LINE-3]]:26: warning: use C++20 requires constraints instead of enable_if [modernize-use-constraints]
// CHECK-FIXES: {{^}}auto basic_trailing() -> Obj requires (T::some_value) {{{$}}

template <typename T, typename U>
typename std::enable_if<T::some_value && U::another_value, Obj>::type conjunction() {
  return Obj{};
}
// CHECK-MESSAGES: :[[@LINE-3]]:1: warning: use C++20 requires constraints instead of enable_if [modernize-use-constraints]
// CHECK-FIXES: {{^}}Obj conjunction() requires (T::some_value && U::another_value) {{{$}}

template <typename T, typename U>
typename std::enable_if<T::some_value && !U::another_value, Obj>::type conjunction_negate() {
  return Obj{};
}
// CHECK-MESSAGES: :[[@LINE-3]]:1: warning: use C++20 requires constraints instead of enable_if [modernize-use-constraints]
// CHECK-FIXES: {{^}}Obj conjunction_negate() requires (T::some_value && !U::another_value) {{{$}}

template <typename T, typename U>
typename std::enable_if<T::some_value || U::another_value, Obj>::type disjunction() {
  return Obj{};
}
// CHECK-MESSAGES: :[[@LINE-3]]:1: warning: use C++20 requires constraints instead of enable_if [modernize-use-constraints]
// CHECK-FIXES: {{^}}Obj disjunction() requires (T::some_value || U::another_value) {{{$}}

template <typename T>
typename std::enable_if<T::some_value, Obj>::type existing_constraint() requires (T::another_value) {
  return Obj{};
}
// CHECK-MESSAGES: :[[@LINE-3]]:1: warning: use C++20 requires constraints instead of enable_if [modernize-use-constraints]
// CHECK-FIXES: {{^}}typename std::enable_if<T::some_value, Obj>::type existing_constraint() requires (T::another_value) {{{$}}

template <typename T>
typename std::enable_if<T::some_value, Obj>::type decl_without_def();

template <typename T>
typename std::enable_if<T::some_value, Obj>::type decl_with_separate_def();

template <typename T>
typename std::enable_if<T::some_value, Obj>::type decl_with_separate_def() {
  return Obj{};
}
// FIXME - Support definitions with separate decls

template <typename T>
std::enable_if<T::some_value, Obj> not_enable_if() {
  return {};
}

template <typename T>
typename std::enable_if<T::some_value, Obj> also_not_enable_if() {
  return {};
}


////////////////////////////////
// Functions with specifier
////////////////////////////////

template <typename T>
constexpr typename std::enable_if<T::some_value, int>::type constexpr_decl() {
  return 10;
}
// CHECK-MESSAGES: :[[@LINE-3]]:11: warning: use C++20 requires constraints instead of enable_if [modernize-use-constraints]
// CHECK-FIXES: {{^}}constexpr int constexpr_decl() requires (T::some_value) {{{$}}

template <typename T>
static inline constexpr typename std::enable_if<T::some_value, int>::type static_inline_constexpr_decl() {
  return 10;
}
// CHECK-MESSAGES: :[[@LINE-3]]:25: warning: use C++20 requires constraints instead of enable_if [modernize-use-constraints]
// CHECK-FIXES: {{^}}static inline constexpr int static_inline_constexpr_decl() requires (T::some_value) {{{$}}

template <typename T>
static
typename std::enable_if<T::some_value, int>::type
static_decl() {
  return 10;
}
// CHECK-MESSAGES: :[[@LINE-4]]:1: warning: use C++20 requires constraints instead of enable_if [modernize-use-constraints]
// CHECK-FIXES: {{^}}static{{$}}
// CHECK-FIXES-NEXT: {{^}}int{{$}}
// CHECK-FIXES-NEXT: {{^}}static_decl() requires (T::some_value) {{{$}}

template <typename T>
constexpr /* comment */ typename std::enable_if<T::some_value, int>::type constexpr_comment_decl() {
  return 10;
}
// CHECK-MESSAGES: :[[@LINE-3]]:25: warning: use C++20 requires constraints instead of enable_if [modernize-use-constraints]
// CHECK-FIXES: {{^}}constexpr /* comment */ int constexpr_comment_decl() requires (T::some_value) {{{$}}


////////////////////////////////
// Class definition tests
////////////////////////////////

struct AClass {
  template <typename T>
  static typename std::enable_if<T::some_value, Obj>::type static_method() {
    return Obj{};
  }
  // CHECK-MESSAGES: :[[@LINE-3]]:10: warning: use C++20 requires constraints instead of enable_if [modernize-use-constraints]
  // CHECK-FIXES: {{^}}  static Obj static_method() requires (T::some_value) {{{$}}

  template <typename T>
  typename std::enable_if<T::some_value, Obj>::type member() {
    return Obj{};
  }
  // CHECK-MESSAGES: :[[@LINE-3]]:3: warning: use C++20 requires constraints instead of enable_if [modernize-use-constraints]
  // CHECK-FIXES: {{^}}  Obj member() requires (T::some_value) {{{$}}

  template <typename T>
  typename std::enable_if<T::some_value, Obj>::type const_qualifier() const {
    return Obj{};
  }
  // CHECK-MESSAGES: :[[@LINE-3]]:3: warning: use C++20 requires constraints instead of enable_if [modernize-use-constraints]
  // CHECK-FIXES: {{^}}  Obj const_qualifier() const requires (T::some_value) {{{$}}

  template <typename T>
  typename std::enable_if<T::some_value, Obj>::type rvalue_ref_qualifier() && {
    return Obj{};
  }
  // CHECK-MESSAGES: :[[@LINE-3]]:3: warning: use C++20 requires constraints instead of enable_if [modernize-use-constraints]
  // CHECK-FIXES: {{^}}  Obj rvalue_ref_qualifier() && requires (T::some_value) {{{$}}

  template <typename T>
  typename std::enable_if<T::some_value, Obj>::type rvalue_ref_qualifier_comment() /* c1 */ && /* c2 */ {
    return Obj{};
  }
  // CHECK-MESSAGES: :[[@LINE-3]]:3: warning: use C++20 requires constraints instead of enable_if [modernize-use-constraints]
  // CHECK-FIXES: {{^}}  Obj rvalue_ref_qualifier_comment() /* c1 */ && /* c2 */ requires (T::some_value) {{{$}}
};


////////////////////////////////
// Comments and whitespace tests
////////////////////////////////

template <typename T>
typename std::enable_if</* check1 */ T::some_value, Obj>::type leading_comment() {
  return Obj{};
}
// CHECK-MESSAGES: :[[@LINE-3]]:1: warning: use C++20 requires constraints instead of enable_if [modernize-use-constraints]
// CHECK-FIXES: {{^}}Obj leading_comment() requires (/* check1 */ T::some_value) {{{$}}

template <typename T>
typename std::enable_if<T::some_value, Obj>::type body_on_next_line()
{
  return Obj{};
}
// CHECK-MESSAGES: :[[@LINE-4]]:1: warning: use C++20 requires constraints instead of enable_if [modernize-use-constraints]
// CHECK-FIXES: {{^}}Obj body_on_next_line(){{$}}
// CHECK-FIXES-NEXT: {{^}}requires (T::some_value) {{{$}}

template <typename T>
typename std::enable_if<  /* check1 */ T::some_value, Obj>::type leading_comment_whitespace() {
  return Obj{};
}
// CHECK-MESSAGES: :[[@LINE-3]]:1: warning: use C++20 requires constraints instead of enable_if [modernize-use-constraints]
// CHECK-FIXES: {{^}}Obj leading_comment_whitespace() requires (/* check1 */ T::some_value) {{{$}}

template <typename T>
typename std::enable_if</* check1 */ T::some_value /* check2 */, Obj>::type leading_and_trailing_comment() {
  return Obj{};
}
// CHECK-MESSAGES: :[[@LINE-3]]:1: warning: use C++20 requires constraints instead of enable_if [modernize-use-constraints]
// CHECK-FIXES: {{^}}Obj leading_and_trailing_comment() requires (/* check1 */ T::some_value /* check2 */) {{{$}}

template <typename T, typename U>
typename std::enable_if<T::some_value &&
                        U::another_value, Obj>::type condition_on_two_lines() {
  return Obj{};
}
// CHECK-MESSAGES: :[[@LINE-4]]:1: warning: use C++20 requires constraints instead of enable_if [modernize-use-constraints]
// CHECK-FIXES: {{^}}Obj condition_on_two_lines() requires (T::some_value &&{{$}}
// CHECK-FIXES-NEXT: U::another_value) {{{$}}
