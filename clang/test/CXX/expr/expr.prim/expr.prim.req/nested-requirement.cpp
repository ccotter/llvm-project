// RUN: %clang_cc1 -std=c++2a -x c++ %s -verify

namespace SubstitutionFailureNestedRequires {
template<class T>  concept True = true;
template<class T>  concept False = false;

struct S { double value; };

template<class T> constexpr bool NotAConceptTrue = true;
template <class T>
concept SFinNestedRequires = requires (T x) {
    // SF in a non-concept specialisation should also be evaluated to false.
   requires NotAConceptTrue<decltype(x.value)> || NotAConceptTrue<T>;
};
static_assert(SFinNestedRequires<int>);
static_assert(SFinNestedRequires<S>);
template <class T>
void foo() requires SFinNestedRequires<T> {}
void bar() {
  foo<int>();
  foo<S>();
}
namespace ErrorExpressions_NotSF {
template<typename T> struct X { static constexpr bool value = T::value; }; // #X_Value
struct True { static constexpr bool value = true; };
struct False { static constexpr bool value = false; };
template<typename T> concept C = true;
template<typename T> concept F = false;

template<typename T> requires requires(T) { requires C<T> || X<T>::value; } void foo();

template<typename T> requires requires(T) { requires C<T> && X<T>::value; } void bar(); // #bar
template<typename T> requires requires(T) { requires F<T> || (X<T>::value && C<T>); } void baz();

void func() {
  foo<True>();
  foo<False>();
  foo<int>();

  bar<True>();
  bar<False>();
  // expected-error@-1 {{no matching function for call to 'bar'}}
  // expected-note@#bar {{while substituting template arguments into constraint expression here}}
  // expected-note@#bar {{while checking the satisfaction of nested requirement requested here}}
  // expected-note@#bar {{candidate template ignored: constraints not satisfied [with T = False]}}
  // expected-note@#bar {{because 'X<False>::value' evaluated to false}}

  bar<int>();
  // expected-note@-1 {{while checking constraint satisfaction for template 'bar<int>' required here}} \
  // expected-note@-1 {{in instantiation of function template specialization}}
  // expected-note@#bar {{in instantiation of static data member}}
  // expected-note@#bar {{in instantiation of requirement here}}
  // expected-note@#bar {{while checking the satisfaction of nested requirement requested here}}
  // expected-note@#bar {{while substituting template arguments into constraint expression here}}
  // expected-error@#X_Value {{type 'int' cannot be used prior to '::' because it has no members}}
}
}
}
