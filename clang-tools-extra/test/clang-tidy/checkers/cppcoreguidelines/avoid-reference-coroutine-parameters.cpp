// RUN: %check_clang_tidy -std=c++20 %s cppcoreguidelines-avoid-reference-coroutine-parameters %t

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
struct InitialAwaiter {
  bool await_ready() noexcept;
  void await_suspend(std::coroutine_handle<>) noexcept;
  void await_resume() noexcept;
};
struct FinalAwaiter {
  bool await_ready() noexcept;
  void await_suspend(std::coroutine_handle<>) noexcept;
  void await_resume() noexcept;
};

struct Coro {
  struct promise_type {
    InitialAwaiter initial_suspend();
    FinalAwaiter final_suspend() noexcept;
    void return_void();
    Coro get_return_object();
    void unhandled_exception();
  };
};
// NOLINTEND

Coro no_references(int x, int* y) {
  if (x);
  if (y);

  co_await Awaiter{};

  if (x);
  if (y);
}

Coro accepts_references(int& x, int &y) {
  if (x);
  // CHECK-MESSAGES: :[[@LINE-1]]:7: warning: coroutine reference parameter 'x' accessed after suspend point [cppcoreguidelines-avoid-reference-coroutine-parameters]
  if (y);
  // CHECK-MESSAGES: :[[@LINE-1]]:7: warning: coroutine reference parameter 'y' accessed after suspend point [cppcoreguidelines-avoid-reference-coroutine-parameters]

  co_await Awaiter{};

  if (x);
  // CHECK-MESSAGES: :[[@LINE-1]]:7: warning: coroutine reference parameter 'x' accessed after suspend point [cppcoreguidelines-avoid-reference-coroutine-parameters]

  co_return;
}

Coro not_reference_accessed_after_await(int& x, int y) {
  co_await Awaiter{};

  if (y);

  co_return;
}

Coro non_coro_accepts_references(int& x) {
  if (x);
  return Coro{};
}
