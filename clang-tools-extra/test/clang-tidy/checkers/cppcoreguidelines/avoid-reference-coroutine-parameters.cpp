// RUN: %check_clang_tidy -check-suffix=DEFAULT -std=c++20 %s cppcoreguidelines-avoid-reference-coroutine-parameters %t
// RUN: %check_clang_tidy -check-suffix=COAWAIT -std=c++20 %s cppcoreguidelines-avoid-reference-coroutine-parameters %t -- \
// RUN: -config="{CheckOptions: [{key: cppcoreguidelines-avoid-reference-coroutine-parameters.IgnoreCoawaitExprs, value: true}]}"

// NOLINTBEGIN
namespace std {
  template <typename T, typename... Args>
  struct coroutine_traits {
    using promise_type = typename T::promise_type;
  };
  template <typename T = void>
  struct coroutine_handle;
  template <>
  struct coroutine_handle<void> {
    coroutine_handle() noexcept;
    coroutine_handle(decltype(nullptr)) noexcept;
    static constexpr coroutine_handle from_address(void*);
  };
  template <typename T>
  struct coroutine_handle {
    coroutine_handle() noexcept;
    coroutine_handle(decltype(nullptr)) noexcept;
    static constexpr coroutine_handle from_address(void*);
    operator coroutine_handle<>() const noexcept;
  };
} // namespace std

template <class T>
struct Awaiter {
  bool await_ready() noexcept;
  void await_suspend(std::coroutine_handle<>) noexcept;
  T await_resume() noexcept;
};

struct Coro {
  struct promise_type {
    Awaiter<void> initial_suspend();
    Awaiter<void> final_suspend() noexcept;
    void return_void();
    Coro get_return_object();
    void unhandled_exception();
  };
  Awaiter<int> operator co_await();
};
// NOLINTEND

struct Obj {};

Coro no_args() {
  co_return;
}

Coro accepts_int(int x) {
  co_return;
}

template <class... Ts>
Coro accepts_all_values(Ts... ts) {
  co_return;
}

Coro no_references(int x, int* y, Obj z, const Obj w) {
  co_return;
}

Coro accepts_references(int& x, const int &y) {
  // CHECK-MESSAGES-DEFAULT: :[[@LINE-1]]:25: warning: coroutine parameters should not be references [cppcoreguidelines-avoid-reference-coroutine-parameters]
  // CHECK-MESSAGES-DEFAULT: :[[@LINE-2]]:33: warning: coroutine parameters should not be references [cppcoreguidelines-avoid-reference-coroutine-parameters]
  co_return;
}

Coro accepts_references_and_non_references(int& x, int y) {
  // CHECK-MESSAGES-DEFAULT: :[[@LINE-1]]:44: warning: coroutine parameters should not be references [cppcoreguidelines-avoid-reference-coroutine-parameters]
  co_return;
}

Coro accepts_references_to_objects(const Obj& x) {
  // CHECK-MESSAGES-DEFAULT: :[[@LINE-1]]:36: warning: coroutine parameters should not be references [cppcoreguidelines-avoid-reference-coroutine-parameters]
  co_return;
}

template <class T>
Coro accepts_template_type_ref(const T& x) {
  // CHECK-MESSAGES-DEFAULT: :[[@LINE-1]]:32: warning: coroutine parameters should not be references [cppcoreguidelines-avoid-reference-coroutine-parameters]
  co_return;
}

template <class T>
Coro accepts_template_type_forwarding_ref(T&& x) {
  // CHECK-MESSAGES-DEFAULT: :[[@LINE-1]]:43: warning: coroutine parameters should not be references [cppcoreguidelines-avoid-reference-coroutine-parameters]
  co_return;
}

Coro non_coro_accepts_references(int& x) {
  if (x);
  return Coro{};
}

void defines_a_lambda() {
  auto NoArgs = [](int x) -> Coro { co_return; };

  auto NoReferences = [](int x) -> Coro { co_return; };

  auto WithReferences = [](int& x) -> Coro { co_return; };
  // CHECK-MESSAGES-DEFAULT: :[[@LINE-1]]:28: warning: coroutine parameters should not be references [cppcoreguidelines-avoid-reference-coroutine-parameters]

  auto WithReferences2 = [](int&) -> Coro { co_return; };
  // CHECK-MESSAGES-DEFAULT: :[[@LINE-1]]:29: warning: coroutine parameters should not be references [cppcoreguidelines-avoid-reference-coroutine-parameters]
}

template <class... Ts> Coro accepts_all_refs(Ts&&...) { co_return; }
// CHECK-MESSAGES-DEFAULT: :[[@LINE-1]]:46: warning: coroutine parameters should not be references [cppcoreguidelines-avoid-reference-coroutine-parameters]

Coro immediately_awaits_coroutines() {
  Obj o;
  co_await accepts_all_refs(o);
  co_await accepts_all_refs(Obj{});
  co_await accepts_all_refs(
    co_await accepts_all_refs(co_await accepts_all_refs(), Obj{}),
    Obj{},
    10);
}

Coro calls_coroutine_without_immediate_await() {
  Obj o;

  Coro c;

  c = accepts_int(10);
  c = no_args();

  accepts_all_refs(o);
  // CHECK-MESSAGES-COAWAIT: :[[@LINE-1]]:3: warning: call to coroutine that accepts reference parameters not immediately co_await-ed [cppcoreguidelines-avoid-reference-coroutine-parameters]
  accepts_all_refs(co_await accepts_all_refs(10));
  // CHECK-MESSAGES-COAWAIT: :[[@LINE-1]]:3: warning: call to coroutine that accepts reference parameters not immediately co_await-ed [cppcoreguidelines-avoid-reference-coroutine-parameters]

  c = accepts_references_to_objects(o);
  // CHECK-MESSAGES-COAWAIT: :[[@LINE-1]]:12: warning: call to coroutine that accepts reference parameters not immediately co_await-ed [cppcoreguidelines-avoid-reference-coroutine-parameters]
  co_await c;
}

Coro non_coro_calls_coroutines() {
  Coro c = accepts_references_to_objects(Obj{});
  // CHECK-MESSAGES-COAWAIT: :[[@LINE-1]]:12: warning: call to coroutine that accepts reference parameters not immediately co_await-ed [cppcoreguidelines-avoid-reference-coroutine-parameters]
  return c;
}
