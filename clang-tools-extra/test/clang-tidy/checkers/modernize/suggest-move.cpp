// RUN: %check_clang_tidy %s modernize-suggest-move %t

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

void last_use_suggests() {
  {
    HasMove Val;
    HasMove Val2{Val};
    // CHECK-MESSAGES: :[[@LINE-1]]:18: warning: use std::move to avoid copy [modernize-suggest-move]
    // CHECK-FIXES: HasMove Val2{std::move(Val)};
  }

  {
    HasMove Val;
    HasMove Val2{Val};
    HasMove Val3{Val};
    // CHECK-MESSAGES: :[[@LINE-1]]:18: warning: use std::move to avoid copy [modernize-suggest-move]
    // CHECK-FIXES: HasMove Val3{std::move(Val)};
  }
}

void const_value_doesnt_suggest() {
  const HasMove Val;
  HasMove Val2{Val};
}

void non_movable_doesnt_suggest() {
  NoMove Val;
  NoMove Val2{Val};
}

void containers_are_movable() {
  {
    std::vector<int> Vs;

    std::vector<int> Vs2{Vs};
    // CHECK-MESSAGES: :[[@LINE-1]]:26: warning: use std::move to avoid copy [modernize-suggest-move]
    // CHECK-FIXES: std::vector<int> Vs2{std::move(Vs)};
  }

  {
    std::vector<HasMove> Vs;

    std::vector<HasMove> Vs2{Vs};
    // CHECK-MESSAGES: :[[@LINE-1]]:30: warning: use std::move to avoid copy [modernize-suggest-move]
    // CHECK-FIXES: std::vector<HasMove> Vs2{std::move(Vs)};
  }

  {
    std::vector<NoMove> Vs;
    std::vector<NoMove> Vs2{Vs};
    // CHECK-MESSAGES: :[[@LINE-1]]:29: warning: use std::move to avoid copy [modernize-suggest-move]
    // CHECK-FIXES: std::vector<NoMove> Vs2{std::move(Vs)};
  }

  {
    std::vector<int> Vs;

    std::vector<int> Vs2;
    Vs2 = Vs;
    // CHECK-MESSAGES: :[[@LINE-1]]:11: warning: use std::move to avoid copy [modernize-suggest-move]
    // CHECK-FIXES: Vs2 = std::move(Vs);
  }
}

void do_escape(std::vector<int>*);

void escaped_values_do_not_trigger() {
  {
    std::vector<int> Vs;
    do_escape(&Vs);
    std::vector<int> Vs2{Vs};
  }
}

void use_after_does_not_suggest_move() {
  {
    std::vector<int> Vs;
    std::vector<int> Vs2{Vs};
    if (Vs.size()) { }
  }
  {
    std::vector<int> Vs;
    std::vector<int> Vs2;
    Vs2 = Vs;
    if (Vs.size()) { }
  }
}
