// RUN: %check_clang_tidy %s -std=c++17 performance-unnecessary-copy-on-last-use %t
// RUN: %check_clang_tidy %s -std=c++11 performance-unnecessary-copy-on-last-use %t
// CHECK-FIXES: #include <utility>

struct Movable {
  Movable() = default;
  Movable(Movable const &) = default;
  Movable(Movable &&) = default;
  Movable &operator=(Movable const &) = default;
  Movable &operator=(Movable &&) = default;
  ~Movable();

  void memberUse() {}
};

struct Copyable {
  Copyable() = default;
  Copyable(Copyable const &) = default;
  Copyable(Copyable &&) = default;
  Copyable &operator=(Copyable const &) = default;
  Copyable &operator=(Copyable &&) = default;
  ~Copyable() = default; 

  void memberUse() {}
};
// static_assert(!std::is_trivially_copyable_v<Movable>, "Movable must not be trivially copyable");

void valueReceiver(Movable Mov);
void constRefReceiver(Movable const &Mov);

void valueTester() {
  Movable Mov{};
  valueReceiver(Mov);
  valueReceiver(Mov);
  // CHECK-MESSAGES: [[@LINE-1]]:17: warning: parameter 'Mov' is copied on last use, consider moving it instead [performance-unnecessary-copy-on-last-use]
  // CHECK-FIXES: valueReceiver(std::move(Mov));
  Mov = Movable{};
  valueReceiver(Mov);
  // CHECK-MESSAGES: [[@LINE-1]]:17: warning: parameter 'Mov' is copied on last use, consider moving it instead [performance-unnecessary-copy-on-last-use]
  // CHECK-FIXES: valueReceiver(std::move(Mov));
}

void testUsageInBranch(bool Splitter) {
  Movable Mov{};

  valueReceiver(Mov);
  if(Splitter){
    Mov.memberUse();
  } else {
    Mov = Movable{};
  }
  valueReceiver(Mov);
  // CHECK-MESSAGES: [[@LINE-1]]:17: warning: parameter 'Mov' is copied on last use, consider moving it instead [performance-unnecessary-copy-on-last-use]
  // CHECK-FIXES: valueReceiver(std::move(Mov));

  if(Splitter){
    Mov = Movable{};
  } else {
    Mov = Movable{};
  }
  valueReceiver(Mov);
  Mov.memberUse();
}

void testExplicitCopy() {
  Movable Mov{};
  constRefReceiver(Movable{Mov});
  // CHECK-MESSAGES: [[@LINE-1]]:28: warning: parameter 'Mov' is copied on last use, consider moving it instead [performance-unnecessary-copy-on-last-use]
  // CHECK-FIXES: constRefReceiver(Movable{std::move(Mov)});
}

Movable testReturn() {
  Movable Mov{};
  return Mov; // no warning, copy elision
}

Movable testReturn2(Movable && Mov, bool F) {
  return F? Mov: Movable{}; 
  // CHECK-MESSAGES: [[@LINE-1]]:13: warning: parameter 'Mov' is copied on last use, consider moving it instead [performance-unnecessary-copy-on-last-use] 
  // CHECK-FIXES: return F? std::move(Mov): Movable{};
}

void rValReferenceTester(Movable&& Mov) {
  valueReceiver(Mov);
  valueReceiver(Mov);
  // CHECK-MESSAGES: [[@LINE-1]]:17: warning: parameter 'Mov' is copied on last use, consider moving it instead [performance-unnecessary-copy-on-last-use]
  // CHECK-FIXES: valueReceiver(std::move(Mov));
  Mov = Movable{};
  valueReceiver(Mov);
  // CHECK-MESSAGES: [[@LINE-1]]:17: warning: parameter 'Mov' is copied on last use, consider moving it instead [performance-unnecessary-copy-on-last-use]
  // CHECK-FIXES: valueReceiver(std::move(Mov));
}

void referenceTester(Movable& Mov) {
  valueReceiver(Mov);
  valueReceiver(Mov);
  Mov = Movable{};
  valueReceiver(Mov);
}

void pointerTester(Movable* Mov) {
  valueReceiver(*Mov);
  valueReceiver(*Mov);
  *Mov = Movable{};
  valueReceiver(*Mov);
}

// Replacements in expansions from macros or of their parameters are buggy, so we don't fix them.
// Todo (future): The source location of macro parameters might be fixed in the future
#define FUN(Mov) valueReceiver((Mov))
void falseMacroExpansion() {
  Movable Mov;
  FUN(Mov);
  // CHECK-MESSAGES: [[@LINE-1]]:7: warning: parameter 'Mov' is copied on last use, consider moving it instead [performance-unnecessary-copy-on-last-use] 
  // CHECK-FIXES: FUN(Mov);
}

template <class T>
struct RemoveRef{
  using type = T;
};

template <class T>
struct RemoveRef<T&&>{
  using type = T;
};

template <class T>
struct RemoveRef<T&>{
  using type = T;
};

template<class T>
using RemoveRefT = typename RemoveRef<T>::type;

template <class Movable>
void initSomething(Movable&& Mov) {
  valueReceiver(Mov);
  valueReceiver(Mov);
  // CHECK-MESSAGES: [[@LINE-1]]:17: warning: parameter 'Mov' may be copied on last use, consider forwarding it instead [performance-unnecessary-copy-on-last-use] 
  Mov = RemoveRefT<Movable>{};
  valueReceiver(Mov);
  // CHECK-MESSAGES: [[@LINE-1]]:17: warning: parameter 'Mov' may be copied on last use, consider forwarding it instead [performance-unnecessary-copy-on-last-use]
}

void initSomethingVal(Movable const& Mov) {
  initSomething<Movable>(Movable{Mov}); 
}

void initSomethingRValRef(Movable const& Mov) {
  initSomething<Movable&&>(Movable{Mov}); 
}

void initSomethingRef(Movable& Mov) {
  initSomething<Movable&>(Mov); 
}

// no "automatic - storage" tests:

void staticTester() {
  static Movable MovStatic;
  valueReceiver(MovStatic);
  valueReceiver(MovStatic);
  MovStatic = Movable{};
  valueReceiver(MovStatic);
}

void threadLocalTester() {
  thread_local Movable MovThreadLocal;
  valueReceiver(MovThreadLocal);
  valueReceiver(MovThreadLocal);
  MovThreadLocal = Movable{};
  valueReceiver(MovThreadLocal);
}

void externTester() {
  extern Movable MovExtern;
  valueReceiver(MovExtern);
  valueReceiver(MovExtern);
  MovExtern = Movable{};
  valueReceiver(MovExtern);
}

Movable MovGlobal;
void globalTester() {
  valueReceiver(MovGlobal);
  valueReceiver(MovGlobal);
  MovGlobal = Movable{};
  valueReceiver(MovGlobal);
}

// lambda tests:
void lambdaCaptureRefTester() {
  Movable Mov{};
  auto Lambda = [&Mov](){
    Mov.memberUse();
  };
  Lambda();
}

void lambdaCaptureValueTester() {
  Movable Mov{};
  auto Lambda = [Mov]() mutable {
    // CHECK-MESSAGES: [[@LINE-1]]:18: warning: parameter 'Mov' is copied on last use, consider moving it instead [performance-unnecessary-copy-on-last-use]
    // CHECK-FIXES: auto Lambda = [Mov]() mutable {
    // Note: No fix, because a fix requires c++14.
    Mov.memberUse();
    };
  Lambda();
}

/* Todo (improvement): currently the following is not fixed. 
        A differentiation between init-params in lambdas is required.
*/
void lambdaCaptureValueTester2() {
  Movable Mov{};
  auto Lambda = [Mov = Mov]() mutable {
    // CHECK-MESSAGES: [[@LINE-1]]:24: warning: parameter 'Mov' is copied on last use, consider moving it instead [performance-unnecessary-copy-on-last-use]
    // NOCHECK-FIXES: auto Lambda = [Mov = std::move(Mov)]() mutable {
    // Note: No fix, because a fix requires c++14.
    Mov.memberUse();
  };
  Lambda();
}

void lambdaCaptureValueTester3() {
  Movable Mov{};
  auto Lambda = [=]() mutable {
    // CHECK-MESSAGES: [[@LINE-1]]:18: warning: parameter 'Mov' is copied on last use, consider moving it instead [performance-unnecessary-copy-on-last-use]
    // CHECK-FIXES: auto Lambda = [=]() mutable {
    // NOCHECK-FIXES: auto Lambda = [=, Mov = std::move(Mov)]() mutable {
    // Note: No fix, because a fix requires c++14.
    Mov.memberUse();
  };
  Lambda();
}

/* 
Todo (improvement):
Currently the check does not find last usages of fields of local objects.

struct MoveOwner{
  Movable Mov{};
};

void testFieldMove(){
  MoveOwner Owner{};
  valueReceiver(Owner.Mov);
  Owner.Mov = Movable{};
  valueReceiver(Owner.Mov);
  // DISABLED-CHECK-MESSAGES: [[@LINE-1]]:17: warning: parameter 'Owner.Mov' is copied on last use, consider moving it instead [performance-unnecessary-copy-on-last-use]
  // DISABLED-CHECK-FIXES: valueReceiver(std::move(Owner.Mov));
}
*/

/*
Todo (improvement):
Currently a heuristic detection of guards is not implemented, so this test is disabled
But before this is done, the check should not be used for automatic fixing

using NoMove = std::shared_ptr<std::lock_guard<std::mutex>>;

void shareMutex(NoMove Nmov);

void testNoMove(std::mutex& M, int& I) {
    NoMove Nmov = std::make_shared<std::lock_guard<std::mutex>>(M);
    shareMutex(Nmov);
    I = 42;
}
*/
