// RUN: %check_clang_tidy %s cppcoreguidelines-avoid-unknown-calls-with-mutex-held %t

// NOLINTBEGIN
namespace std {

template <class Mutex>
class unique_lock {
public:
  unique_lock() noexcept;
  explicit unique_lock(Mutex &m);
  void unlock();
  Mutex* release() noexcept;
  Mutex* mutex() const noexcept;
  void swap(unique_lock& other) noexcept;
};

class mutex {
public:
  mutex() noexcept;
  ~mutex();
  mutex(const mutex &) = delete;
  mutex &operator=(const mutex &) = delete;

  void lock();
  void unlock();
};

class recursive_mutex {
public:
  recursive_mutex() noexcept;
  ~recursive_mutex();
  recursive_mutex(const recursive_mutex &) = delete;
  recursive_mutex &operator=(const recursive_mutex &) = delete;
};

} // namespace std
// NOLINTEND

std::mutex m;
std::recursive_mutex rm;

struct Obj {
  Obj();
  Obj(const Obj&);
  Obj(Obj&&) noexcept;
  Obj& operator=(const Obj&);
  Obj& operator=(Obj&&) noexcept;
  virtual ~Obj();
  virtual void vmethod();

  void unlock();
};

void lock_held_in_virtual_call(Obj* obj) {
  std::unique_lock<std::mutex> lk(m);
  obj->vmethod();
  // CHECK-MESSAGES: :[[@LINE-1]]:3: warning: virtual call while mutex held [cppcoreguidelines-avoid-unknown-calls-with-mutex-held]

  if (true) {
    obj->vmethod();
    // CHECK-MESSAGES: :[[@LINE-1]]:5: warning: virtual call while mutex held [cppcoreguidelines-avoid-unknown-calls-with-mutex-held]
  }

  if (true) {
    obj->unlock(); // Not mutex unlock

    obj->vmethod();
    // CHECK-MESSAGES: :[[@LINE-1]]:5: warning: virtual call while mutex held [cppcoreguidelines-avoid-unknown-calls-with-mutex-held]
  }

  if (true) {
    std::unique_lock<std::mutex> lk2;
    lk2.unlock(); // lk not unlocked

    obj->vmethod();
    // CHECK-MESSAGES: :[[@LINE-1]]:5: warning: virtual call while mutex held [cppcoreguidelines-avoid-unknown-calls-with-mutex-held]
  }
}

void lock_held_vcall_last_stmt_in_block(Obj* obj) {
  std::unique_lock<std::mutex> lk(m);
  obj->vmethod();
  // CHECK-MESSAGES: :[[@LINE-1]]:3: warning: virtual call while mutex held [cppcoreguidelines-avoid-unknown-calls-with-mutex-held]
}

void virtual_call_without_lock(Obj* obj) {
  {
    std::unique_lock<std::mutex> lk(m);
  }
  obj->vmethod();
}

void lock_with_potentially_unlocking_call(Obj* obj) {
  {
    std::unique_lock<std::mutex> lk(m);
    lk.unlock();
    obj->vmethod();
  }
  
  {
    std::unique_lock<std::mutex> lk(m);
    lk.mutex()->unlock();
    obj->vmethod();
  }

  {
    std::unique_lock<std::mutex> lk(m);
    std::unique_lock<std::mutex> lk2;
    lk.swap(lk2);
    obj->vmethod();
  }
}
