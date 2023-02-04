// RUN: %clang_cc1 -std=c++20 -verify %s
// RUN: %clang_cc1 -std=c++2b -verify %s

namespace PR52909a {

template<class> constexpr bool B = true;
template<class T> concept True = B<T>;

template <class T>
int foo(T t) requires requires { // expected-note {{candidate template ignored: constraints not satisfied}}
    {t.begin} -> True; // expected-note {{because 't.begin' would be invalid: reference to non-static member function must be called}}
}
{}

struct A { int begin(); };
auto x = foo(A()); // expected-error {{no matching function for call to 'foo'}}

} // namespace PR52909a
