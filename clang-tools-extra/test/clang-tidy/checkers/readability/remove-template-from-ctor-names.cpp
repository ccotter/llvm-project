// RUN: %check_clang_tidy %s readability-remove-template-from-ctor-names %t

template <class T>
struct Bad {
  Bad<T>(const Bad<T>&);
  // CHECK-MESSAGES: :[[@LINE-1]]:3: warning: type 'Bad<T>' should not specify template parameters [readability-remove-template-from-ctor-names]
  // CHECK-FIXES: Bad(const Bad<T>&);

  Bad<T>(Bad&&);
  // CHECK-MESSAGES: :[[@LINE-1]]:3: warning: type 'Bad<T>' should not specify template parameters [readability-remove-template-from-ctor-names]
  // CHECK-FIXES: Bad(Bad&&);

  Bad/**/<T>();
  // CHECK-MESSAGES: :[[@LINE-1]]:3: warning: type 'Bad<T>' should not specify template parameters [readability-remove-template-from-ctor-names]
  // CHECK-FIXES: Bad/**/();

  Bad<T>& operator=(const Bad&);
  // HECK-MESSAGES: :[[@LINE-1]]:3: warning: type 'Bad<T>' should not specify template parameters [readability-remove-template-from-ctor-names]
};

template <class T>
struct Good {
  Good();
  Good(const Good&);
  void foo() {
    Good g;
  }
};

template <class T, bool> struct Good2;
template <class T> struct Good2<T, true> {
  static void some_uses() {
    Good2<int, true> good;
    auto good2 = good;
  }
};
