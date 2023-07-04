// RUN: %check_clang_tidy -std=c++20 %s readability-use-named-cast %t -- -- -fno-delayed-template-parsing
// RUN: %check_clang_tidy -check-suffix=,NONDEPENDENT -std=c++20 %s readability-use-named-cast %t -- \
// RUN: -config="{CheckOptions: [{key: readability-use-named-cast.IgnoreDependentTypesForMove, value: false}]}" -- -fno-delayed-template-parsing

// CHECK-FIXES: #include <utility>

struct Something {};

Something& getSomething();

void move_casts() {
  Something s1;
  Something s2 = (Something&&)s1;
  // CHECK-MESSAGES: :[[@LINE-1]]:18: warning: use std::move instead of cast expression [readability-use-named-cast]
  // CHECK-FIXES: Something s2 = std::move(s1);

  Something s3 = static_cast<Something&&>(s1);
  // CHECK-MESSAGES: :[[@LINE-1]]:18: warning: use std::move instead of cast expression [readability-use-named-cast]
  // CHECK-FIXES: Something s3 = std::move(s1);

  Something s4 = (Something&&)getSomething();
  // CHECK-MESSAGES: :[[@LINE-1]]:18: warning: use std::move instead of cast expression [readability-use-named-cast]
  // CHECK-FIXES: Something s4 = std::move(getSomething());

  Something s5 = (Something&&)/*comment*/getSomething();
  // CHECK-MESSAGES: :[[@LINE-1]]:18: warning: use std::move instead of cast expression [readability-use-named-cast]
  // CHECK-FIXES: Something s5 = std::move/*comment*/(getSomething());

  Something s6 = (Something&&)
    s1;
  // CHECK-MESSAGES: :[[@LINE-2]]:18: warning: use std::move instead of cast expression [readability-use-named-cast]
  // CHECK-FIXES: Something s6 = std::move(s1);

  using SomethingAlias = Something;
  Something s7 = (SomethingAlias&&)s1;
  // CHECK-MESSAGES: :[[@LINE-1]]:18: warning: use std::move instead of cast expression [readability-use-named-cast]
  // CHECK-FIXES: Something s7 = std::move(s1);

  SomethingAlias s8;
  Something s9 = (Something&&)s8;
  // CHECK-MESSAGES: :[[@LINE-1]]:18: warning: use std::move instead of cast expression [readability-use-named-cast]
  // CHECK-FIXES: Something s9 = std::move(s8);

  Something s10 = (Something&&) s1;
  // CHECK-MESSAGES: :[[@LINE-1]]:19: warning: use std::move instead of cast expression [readability-use-named-cast]
  // CHECK-FIXES: Something s10 = std::move(s1);

  Something s11 = static_cast<Something&&> (s1);
  // CHECK-MESSAGES: :[[@LINE-1]]:19: warning: use std::move instead of cast expression [readability-use-named-cast]
  // CHECK-FIXES: Something s11 = std::move (s1);

  Something s12 = (Something&&) /**/ s1;
  // CHECK-MESSAGES: :[[@LINE-1]]:19: warning: use std::move instead of cast expression [readability-use-named-cast]
  // CHECK-FIXES: Something s12 = std::move/**/ (s1);
}

template <class T>
void deduced_value(T t) {
  T other = (T&&)t;
  // CHECK-MESSAGES-NONDEPENDENT: :[[@LINE-1]]:13: warning: use std::move instead of cast expression [readability-use-named-cast]
  // CHECK-FIXES-NONDEPENDENT: T other = std::move(t);

  T other2 = (T&&)other;
  // CHECK-MESSAGES-NONDEPENDENT: :[[@LINE-1]]:14: warning: use std::move instead of cast expression [readability-use-named-cast]
  // CHECK-FIXES-NONDEPENDENT: T other2 = std::move(other);
}

template <class T>
void deduced_lvalue_ref(T& t) {
  Something other = (T&&)t;
  Something other2 = (T&&)other;
}

template <class... Ts> void consume(Ts&&...);

template <class T>
void forward_arg_c_cast(T&& t) {
  T other = (T&&)t;
  // CHECK-MESSAGES: :[[@LINE-1]]:13: warning: use std::forward instead of cast expression [readability-use-named-cast]
  // CHECK-FIXES: T other = std::forward<T>(t);
}

template <class T>
void not_a_forward(T&& t) {
  {
    Something local;
    T other = (T&&)local;
  }

  {
    T local;
    T other = (T&&)local;
  }
}

template <class T>
void forward_arg_static_cast(T&& t) {
  T other = static_cast<T&&>(t);
  // CHECK-MESSAGES: :[[@LINE-1]]:13: warning: use std::forward instead of cast expression [readability-use-named-cast]
  // CHECK-FIXES: T other = std::forward<T>(t);
}

template <class... Ts>
void forward_args(Ts&&... ts) {
  consume((Ts&&)ts...);
  // CHECK-MESSAGES: :[[@LINE-1]]:11: warning: use std::forward instead of cast expression [readability-use-named-cast]
  // CHECK-FIXES: consume(std::forward<Ts>(ts)...);
}

template <class T>
struct AClass {
  template <class U>
  void rvalue_ref_and_forwarding_ref(T&& t, U&& u) {
    T t2 = (T&&)t;
    // CHECK-MESSAGES-NONDEPENDENT: :[[@LINE-1]]:12: warning: use std::move instead of cast expression [readability-use-named-cast]
    // CHECK-FIXES-NONDEPENDENT: T t2 = std::move(t);
    U u2 = (U&&)u;
    // CHECK-MESSAGES: :[[@LINE-1]]:12: warning: use std::forward instead of cast expression [readability-use-named-cast]
    // CHECK-FIXES: U u2 = std::forward<U>(u);
  }
  void accepts_t(T t) {
    T t2 = (T&&)t;
    // CHECK-MESSAGES-NONDEPENDENT: :[[@LINE-1]]:12: warning: use std::move instead of cast expression [readability-use-named-cast]
    // CHECK-FIXES-NONDEPENDENT: T t2 = std::move(t);
  }

  void accepts_lvalue_ref(T& t) {
    T t2 = (T&&)t;
  }
};

#if 0
template <class T>
concept CanDo = requires(T&& t) {
  consume((T&&)t);
  // cHECK-MESSAGES: :[[@LINE-1]]:11: warning: use std::move instead of cast expression [readability-use-named-cast]
  // cHECK-FIXES: consume((T&&)t);
};
#endif
