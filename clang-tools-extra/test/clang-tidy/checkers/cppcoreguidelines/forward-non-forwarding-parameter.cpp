// RUN: %check_clang_tidy %s cppcoreguidelines-forward-non-forwarding-parameter %t

// NOLINTBEGIN
namespace std {

template <typename T> struct remove_reference      { using type = T; };
template <typename T> struct remove_reference<T&>  { using type = T; };
template <typename T> struct remove_reference<T&&> { using type = T; };

template <typename T> using remove_reference_t = typename remove_reference<T>::type;

template <typename T> constexpr T &&forward(remove_reference_t<T> &t) noexcept;
template <typename T> constexpr T &&forward(remove_reference_t<T> &&t) noexcept;

} // namespace std
// NOLINTEND

struct Obj {
  Obj();
  Obj(const Obj&);
  Obj(Obj&&) noexcept;
  Obj& operator=(const Obj&);
  Obj& operator=(Obj&&) noexcept;
};

template <class... Ts>
void consumes_all(Ts&&...);

namespace positive_cases {

void forward_local_object() {
  Obj obj;
  Obj& obj_ref = obj;

  Obj obj2 = std::forward<Obj>(obj);
  // CHECK-MESSAGES: :[[@LINE-1]]:32: warning: calling std::forward on non-forwarding reference 'obj' [cppcoreguidelines-forward-non-forwarding-parameter]

  Obj obj3 = std::forward<Obj>(obj_ref);
  // CHECK-MESSAGES: :[[@LINE-1]]:32: warning: calling std::forward on non-forwarding reference 'obj_ref' [cppcoreguidelines-forward-non-forwarding-parameter]
}

void forward_value_param(Obj obj) {
  Obj obj2 = std::forward<Obj>(obj);
  // CHECK-MESSAGES: :[[@LINE-1]]:32: warning: calling std::forward on non-forwarding reference 'obj' [cppcoreguidelines-forward-non-forwarding-parameter]
}

void forward_lvalue_ref(Obj& obj) {
  Obj obj2 = std::forward<Obj>(obj);
  // CHECK-MESSAGES: :[[@LINE-1]]:32: warning: calling std::forward on non-forwarding reference 'obj' [cppcoreguidelines-forward-non-forwarding-parameter]
}

void forward_const_lvalue_ref(const Obj& obj) {
  Obj obj2 = std::forward<const Obj>(obj);
  // CHECK-MESSAGES: :[[@LINE-1]]:38: warning: calling std::forward on non-forwarding reference 'obj' [cppcoreguidelines-forward-non-forwarding-parameter]
}

void forward_rvalue_ref(Obj&& obj) {
  Obj obj2 = std::forward<Obj>(obj);
  // CHECK-MESSAGES: :[[@LINE-1]]:32: warning: calling std::forward on non-forwarding reference 'obj' [cppcoreguidelines-forward-non-forwarding-parameter]
}

void forward_const_rvalue_ref(const Obj&& obj) {
  Obj obj2 = std::forward<const Obj>(obj);
  // CHECK-MESSAGES: :[[@LINE-1]]:38: warning: calling std::forward on non-forwarding reference 'obj' [cppcoreguidelines-forward-non-forwarding-parameter]
}

template <class T>
void forward_value_t(T t) {
  T other = std::forward<T>(t);
  // CHECK-MESSAGES: :[[@LINE-1]]:29: warning: calling std::forward on non-forwarding reference 't' [cppcoreguidelines-forward-non-forwarding-parameter]
}

template <class T>
void forward_lvalue_ref_t(T& t) {
  T other = std::forward<T>(t);
  // CHECK-MESSAGES: :[[@LINE-1]]:29: warning: calling std::forward on non-forwarding reference 't' [cppcoreguidelines-forward-non-forwarding-parameter]
}

template <class T>
void forward_const_rvalue_ref_t(const T&& t) {
  T other = std::forward<const T>(t);
  // CHECK-MESSAGES: :[[@LINE-1]]:35: warning: calling std::forward on non-forwarding reference 't' [cppcoreguidelines-forward-non-forwarding-parameter]
}

template <class T>
struct SomeClass
{
  void forwards_lvalue_ref(T& t) {
    T other = std::forward<T>(t);
    // CHECK-MESSAGES: :[[@LINE-1]]:31: warning: calling std::forward on non-forwarding reference 't' [cppcoreguidelines-forward-non-forwarding-parameter]
  }

  void forwards_rvalue_ref(T&& t) {
    T other = std::forward<T>(t);
    // CHECK-MESSAGES: :[[@LINE-1]]:31: warning: calling std::forward on non-forwarding reference 't' [cppcoreguidelines-forward-non-forwarding-parameter]
  }
};

} // namespace positive_cases

namespace negative_cases {

template <class T>
void forwards_param(T&& t) {
  T other = std::forward<T>(t);
}

template <class... Ts>
void forwards_param_pack(Ts&&... ts) {
  consumes_all(std::forward<Ts>(ts)...);
}

template <class A>
struct SomeClass {
  template <class T>
  void forwards_param(T&& t) {
    T other = std::forward<T>(t);
  }

  template <class T, class U>
  void forwards_param(T&& t) {
    T other = std::forward<T>(t);
  }

  template <class... Ts>
  void forwards_param_pack(Ts&&... ts) {
    consumes_all(1, std::forward<Ts>(ts)...);
  }
};

void instantiates_calls() {
  Obj o;
  forwards_param(0);
  forwards_param(Obj{});
  forwards_param(o);

  forwards_param_pack(0);
  forwards_param_pack(Obj{});
  forwards_param_pack(o);
  forwards_param_pack(0, Obj{}, o);

  SomeClass<Obj> someObj;
  someObj.forwards_param(0);
  someObj.forwards_param(Obj{});
  someObj.forwards_param(o);
  someObj.forwards_param_pack(0, Obj{}, o);
}

} // namespace negative_cases
