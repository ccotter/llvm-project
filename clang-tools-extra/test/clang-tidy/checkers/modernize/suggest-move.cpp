// RUN: %check_clang_tidy %s modernize-suggest-move %t

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

void containers() {
  std::vector<int> Vs;

  std::vector<int> Vs2{Vs};
  // CHECK-MESSAGES: :[[@LINE-1]]:6: warning: use std::move to avoid copy [modernize-suggest-move]
  // CHECK-FIXES: std::vector<int> Vs2{Vs};
}

void containers_that_dont_trigger() {
  std::vector<int> Vs;

  std::vector<int> Vs2{Vs};

  if (Vs.size()) { }
}
