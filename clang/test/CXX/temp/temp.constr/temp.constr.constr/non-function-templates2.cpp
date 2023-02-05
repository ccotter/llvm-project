// RUN: %clang_cc1 -std=c++2a -x c++ -verify %s

template<typename T>
struct AA
{
    template<typename U>
    static constexpr int getB() requires (sizeof(U) == sizeof(int)) { // expected-note{{candidate template ignored: constraints not satisfied [with U = int[2]]}}
        return 2;
    }

    static auto foo2()
    {
        return AA<T>::getB<T[2]>(); // expected-error{{no matching function for call to 'getB'}}
    }
};

constexpr auto x2 = AA<int>::foo2(); // expected-note{{in instantiation of member function 'AA<int>::foo2' requested here}}
