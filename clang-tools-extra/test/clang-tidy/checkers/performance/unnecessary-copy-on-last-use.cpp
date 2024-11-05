// RUN: %check_clang_tidy -std=c++11-or-later %s performance-unnecessary-copy-on-last-use %t
// CHECK-FIXES: #include <utility>

namespace std {

template <typename>
struct remove_reference;

template <typename _Tp>
struct remove_reference {
  typedef _Tp type;
};

template <typename _Tp>
struct remove_reference<_Tp &> {
  typedef _Tp type;
};

template <typename _Tp>
struct remove_reference<_Tp &&> {
  typedef _Tp type;
};

template <typename _Tp>
constexpr typename std::remove_reference<_Tp>::type &&move(_Tp &&__t) noexcept {
  return static_cast<typename remove_reference<_Tp>::type &&>(__t);
}

}

namespace std {

template <typename T> struct vector { // NOLINT
  vector();
  vector(const vector&);
  vector(vector&&);
  vector& operator=(const vector&);
  vector& operator=(vector&&);

  unsigned size() const;
};

} // namespace std

template <class... Ts>
bool value_receiver(Ts... ts);
template <class T>
void constRefReceiver(const T& Mov);

struct HasMove {
  HasMove();
  HasMove(const HasMove&);
  HasMove(HasMove&&);
  HasMove& operator=(const HasMove&);
  HasMove& operator=(HasMove&&);
};

struct NoMove {
  NoMove();
  NoMove(const NoMove&);
  NoMove& operator=(const NoMove&);
};

struct DerivedHasMove : HasMove {
  DerivedHasMove();
  // Move constructor is implicitly defaulted
};

struct DerivedHasNoMove : HasMove {
  DerivedHasNoMove();
  DerivedHasNoMove(const DerivedHasNoMove&);
  DerivedHasNoMove& operator=(const DerivedHasNoMove&);
  // Move constructor is not available
};

void last_use_suggests() {
  {
    HasMove Val;
    HasMove Val2{Val};
    HasMove Val3{Val};
    // CHECK-MESSAGES: :[[@LINE-1]]:18: warning: parameter 'Val' is copied on last use, consider moving it instead [performance-unnecessary-copy-on-last-use]
    // CHECK-FIXES: HasMove Val3{std::move(Val)};
    Val = HasMove{};
    HasMove Val4{Val};
    // CHECK-MESSAGES: :[[@LINE-1]]:18: warning: parameter 'Val' is copied on last use, consider moving it instead [performance-unnecessary-copy-on-last-use]
    // CHECK-FIXES: HasMove Val4{std::move(Val)};
  }

  {
    HasMove Val;
    value_receiver(Val);
    // CHECK-MESSAGES: :[[@LINE-1]]:20: warning: parameter 'Val' is copied on last use, consider moving it instead [performance-unnecessary-copy-on-last-use]
    // CHECK-FIXES: value_receiver(std::move(Val));
  }

  {
    HasMove Val;
    value_receiver(Val, Val);
  }

  {
    HasMove Val;
    HasMove Val2;
    Val2 = Val;
    // CHECK-MESSAGES: :[[@LINE-1]]:12: warning: parameter 'Val' is copied on last use, consider moving it instead [performance-unnecessary-copy-on-last-use]
    // CHECK-FIXES: Val2 = std::move(Val);
  }

  {
    HasMove Val;
    constRefReceiver(Val);
  }

  {
    HasMove Val;
    constRefReceiver(HasMove{Val});
    // CHECK-MESSAGES: :[[@LINE-1]]:30: warning: parameter 'Val' is copied on last use, consider moving it instead [performance-unnecessary-copy-on-last-use]
    // CHECK-FIXES: constRefReceiver(HasMove{std::move(Val)});
  }

  {
    HasMove Val;
    HasMove Val2{Val};
    HasMove Val3{Val};
    // CHECK-MESSAGES: :[[@LINE-1]]:18: warning: parameter 'Val' is copied on last use, consider moving it instead [performance-unnecessary-copy-on-last-use]
    // CHECK-FIXES: HasMove Val2{Val};
    // CHECK-FIXES: HasMove Val3{std::move(Val)};
  }

  {
    DerivedHasMove Val;
    DerivedHasMove Val2{Val};
    // CHECK-MESSAGES: :[[@LINE-1]]:25: warning: parameter 'Val' is copied on last use, consider moving it instead [performance-unnecessary-copy-on-last-use]
    // CHECK-FIXES: DerivedHasMove Val2{std::move(Val)};
  }

  {
    HasMove Val;
    if (value_receiver(Val))
      HasMove Val2{Val};
    // CHECK-MESSAGES: :[[@LINE-1]]:20: warning: parameter 'Val' is copied on last use, consider moving it instead [performance-unnecessary-copy-on-last-use]
    // CHECK-FIXES: HasMove Val2{std::move(Val)};
  }

  {
    HasMove Val;
    for (int i = 0; i != 10; ++i)
      value_receiver(Val);
  }
}

HasMove test_return() {
  HasMove Val;
  return Val; // no warning, copy elision
}

HasMove test_return_ternary(HasMove&& Val, bool F) {
  return F ? Val : HasMove{}; 
  // CHECK-MESSAGES: [[@LINE-1]]:14: warning: parameter 'Val' is copied on last use, consider moving it instead [performance-unnecessary-copy-on-last-use] 
  // CHECK-FIXES: return F ? std::move(Val) : HasMove{};
}

#define FUN(Val) value_receiver((Val))
void macros_warned_bug_not_Fixed() {
  HasMove Val;
  FUN(Val);
  // CHECK-MESSAGES: [[@LINE-1]]:7: warning: parameter 'Val' is copied on last use, consider moving it instead [performance-unnecessary-copy-on-last-use] 
  // CHECK-FIXES: FUN(Val);
}

void rValReference_tester(HasMove&& Val) {
  value_receiver(Val);
  value_receiver(Val);
  // CHECK-MESSAGES: [[@LINE-1]]:18: warning: parameter 'Val' is copied on last use, consider moving it instead [performance-unnecessary-copy-on-last-use]
  // CHECK-FIXES: value_receiver(std::move(Val));
  Val = HasMove{};
  value_receiver(Val);
  // CHECK-MESSAGES: [[@LINE-1]]:18: warning: parameter 'Val' is copied on last use, consider moving it instead [performance-unnecessary-copy-on-last-use]
  // CHECK-FIXES: value_receiver(std::move(Val));
}

void reference_tester(HasMove& Val) {
  value_receiver(Val);
  value_receiver(Val);
  Val = HasMove{};
  value_receiver(Val);
}

void pointer_tester(HasMove* Val) {
  value_receiver(*Val);
  value_receiver(*Val);
  *Val = HasMove{};
  value_receiver(*Val);
}


void const_value_doesnt_suggest() {
  const HasMove Val;
  HasMove Val2{Val};
}

void non_movable_doesnt_suggest() {
  NoMove Val;
  NoMove Val2{Val};

  DerivedHasNoMove Val3;
  DerivedHasNoMove Val4{Val3};
}

struct NoCopyMoveData {
};
struct NoCopyMove {
  void* x;
};
struct DefaultedCopyMove {
  DefaultedCopyMove();
  DefaultedCopyMove(const DefaultedCopyMove&) = default;
  DefaultedCopyMove(DefaultedCopyMove&&) = default;
  void* x;
};
struct TrivialA {};
struct TrivialB {};
struct ComposedOfTrivials {
  TrivialA a;
  TrivialB b;
};

void trivially_movable_doesnt_suggest() {
  {
    NoCopyMoveData Val;
    NoCopyMoveData Val2{Val};
  }
  {
    NoCopyMove Val;
    NoCopyMove Val2{Val};
  }
  {
    DefaultedCopyMove Val;
    DefaultedCopyMove Val2{Val};
  }
  {
    ComposedOfTrivials Val;
    ComposedOfTrivials Val2{Val};
  }
}

void implicit_move_ctor_with_triival() {
  struct ImplicitMoveCtor {
    TrivialA A;
    NoMove B;

    // The check triggers below since the implicitly generated move constructor
    // is not trivial. It's non-trivially movable since NoMove is not trivally
    // copyable/movable.
    //
    // Suggesting std::move doesn't really improve the performance of the code.
    // Perhaps in this situation, ImplicitMoveCtor should delete its move 
    // constructor if it's just going to be the same as the copy constructor.
  };

  ImplicitMoveCtor Val;
  ImplicitMoveCtor Val2{Val};
  // CHECK-MESSAGES: :[[@LINE-1]]:25: warning: parameter 'Val' is copied on last use, consider moving it instead [performance-unnecessary-copy-on-last-use]
  // CHECK-FIXES: ImplicitMoveCtor Val2{std::move(Val)};
}

void containers_are_movable() {
  {
    std::vector<int> Vs;

    std::vector<int> Vs2{Vs};
    // CHECK-MESSAGES: :[[@LINE-1]]:26: warning: parameter 'Vs' is copied on last use, consider moving it instead [performance-unnecessary-copy-on-last-use]
    // CHECK-FIXES: std::vector<int> Vs2{std::move(Vs)};
  }

  {
    std::vector<HasMove> Vs;

    std::vector<HasMove> Vs2{Vs};
    // CHECK-MESSAGES: :[[@LINE-1]]:30: warning: parameter 'Vs' is copied on last use, consider moving it instead [performance-unnecessary-copy-on-last-use]

    // CHECK-FIXES: std::vector<HasMove> Vs2{std::move(Vs)};
  }

  {
    std::vector<NoMove> Vs;
    std::vector<NoMove> Vs2{Vs};
    // CHECK-MESSAGES: :[[@LINE-1]]:29: warning: parameter 'Vs' is copied on last use, consider moving it instead [performance-unnecessary-copy-on-last-use]

    // CHECK-FIXES: std::vector<NoMove> Vs2{std::move(Vs)};
  }

  {
    std::vector<int> Vs;

    std::vector<int> Vs2;
    Vs2 = Vs;
    // CHECK-MESSAGES: :[[@LINE-1]]:11: warning: parameter 'Vs' is copied on last use, consider moving it instead [performance-unnecessary-copy-on-last-use]
    // CHECK-FIXES: Vs2 = std::move(Vs);
  }
}

static HasMove FileStatic;
HasMove FileGlobal;
void non_atomic_not_matched() {
  static HasMove Static;
  value_receiver(Static);

  thread_local HasMove ThreadLocal;
  value_receiver(ThreadLocal);

  extern HasMove Extern;
  value_receiver(Extern);

  value_receiver(FileStatic);
  value_receiver(FileGlobal);
}
