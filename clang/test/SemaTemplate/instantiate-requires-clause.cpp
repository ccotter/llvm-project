// RUN: %clang_cc1 -std=c++2a -x c++ %s -Wno-unused-value -verify


template <class X>
static constexpr bool Check = true;

template<typename T>
struct S {
	template<typename U>
	static constexpr auto f(U const index2) requires(index2, true, Check<U>) {
		return true;
	}
};

static_assert(S<void>::f(1));
