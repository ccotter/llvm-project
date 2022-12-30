// RUN: %check_clang_tidy -std=c++20-or-later %s cppcoreguidelines-avoid-lambda-capturing-coroutines %t

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

struct Awaiter {
  bool await_ready() noexcept;
  void await_suspend(std::coroutine_handle<>) noexcept;
  void await_resume() noexcept;
};

struct Coro {
  struct promise_type {
    Awaiter initial_suspend();
    Awaiter final_suspend() noexcept;
    void return_void();
    Coro get_return_object();
    void unhandled_exception();
  };
};
// NOLINTEND

void lambda_functions() {
  static int Static;

  int X{}, Y{};

  [X]() {};
  []() -> Coro { co_return; };

  auto LambdaStoredToVariable = [X]() -> Coro { co_return; };
  // CHECK-NOTES: [[@LINE-1]]:33: warning: Coroutine lambdas should not capture [cppcoreguidelines-avoid-lambda-capturing-coroutines]

  [X]() -> Coro { int Z = X + 1; co_return; };
  // CHECK-NOTES: [[@LINE-1]]:3: warning: Coroutine lambdas should not capture [cppcoreguidelines-avoid-lambda-capturing-coroutines]

  [&X]() -> Coro { co_return; };
  // CHECK-NOTES: [[@LINE-1]]:3: warning: Coroutine lambdas should not capture [cppcoreguidelines-avoid-lambda-capturing-coroutines]

  [&X, Y]() -> Coro { co_return; };
  // CHECK-NOTES: [[@LINE-1]]:3: warning: Coroutine lambdas should not capture [cppcoreguidelines-avoid-lambda-capturing-coroutines]

  // Using a capture default without referencing any variables does not
  // constitute a lambda capture.
  [=]() -> Coro { co_return; };
  [&]() -> Coro { co_return; };

  [=]() -> Coro { int Z = X + 1; co_return; };
  // CHECK-NOTES: [[@LINE-1]]:3: warning: Coroutine lambdas should not capture [cppcoreguidelines-avoid-lambda-capturing-coroutines]

  [&]() -> Coro { int Z = X + 1; co_return; };
  // CHECK-NOTES: [[@LINE-1]]:3: warning: Coroutine lambdas should not capture [cppcoreguidelines-avoid-lambda-capturing-coroutines]

  []() -> Coro { int Z = Static; co_return; };
  [=]() -> Coro { int Z = Static; co_return; };
  [&]() -> Coro { int Z = Static; co_return; };
}

struct Object {
  void capture_this() {
    [this]() { return X; };

    [this]() -> Coro { int Z = X + 1; co_return; };
    // CHECK-NOTES: [[@LINE-1]]:5: warning: Coroutine lambdas should not capture [cppcoreguidelines-avoid-lambda-capturing-coroutines]
  }
  int X;
};
