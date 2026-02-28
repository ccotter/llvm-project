// RUN: %clang_cc1 -fsyntax-only -verify -std=c++20 %s

// Test that clang does not crash when a member pointer NTTP references a member
// on an enum type (which cannot have members).

template <auto _Ptr>
class intrusive_queue;

template <class T>
struct AT {
    T t_;
};

template <class _Node, AT<_Node*> _Node::* _Next>
class intrusive_queue<_Next> {};

struct cc {
    AT<void*> next_{nullptr}; // expected-note {{'cc::next_' declared here}}
};

enum something {};

void foo() {
    intrusive_queue<&something::next_> command_queue_; // expected-error {{no member named 'next_' in 'something'; did you mean 'cc::next_'?}}
}
