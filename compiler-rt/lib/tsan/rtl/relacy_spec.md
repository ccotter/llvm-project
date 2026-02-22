# Goal

Replicate Relacy's random and full scheduler algos.

To do this, we need to
 - intercept interesting sync points, defined below.
 - Implement a scheduler class which controls all interleavings.
 - create public TSAN C interface `__tsan_simulate` (described below)

We should FIRST only implement the random algo. Full search is a long term goal,
but we will not worry about actually implementing it yet. Just keep this in mind
that we should design generic solutions for future evolution.

Relacy uses fibers, but we should use real threads like TSAN does. TSAN already has
machinery to detect thread races with its own vector clock detection.

## Limitations

The solution should only worry about pthread* and atomic ops on linux. Ignore
libdispatch and other languages like Go.

We should assume the code under test does not rely on external APIs like network IO,
timed waits, or other non-deterministic APIs like random / file reads. If the code
under test calls these APIS, we can exhibit undefined behavior.

### Detect if other threads exist

If other threads exist when `__tsan_simulate` is invoked, the simulation should detect
this and print an error, only running the callback once.

`__tsan_simulate` should return non-zero in this case.

### Invoking unsupported posix interceptors

While a simulation is active, if an interceptor is not supported, the simulation should
report an error and bail out.

`__tsan_simulate` should return non-zero in this case.

### Hitting max depth

If the simulation hits the maximum depth limit (controlled by `simulate_max_depth` flag),
the simulation should report this condition and immediately exit. No further iterations
should be executed.

`__tsan_simulate` should return non-zero (specifically 3) in this case.

### Return codes

`__tsan_simulate` returns an integer status code:
- `0` - Success: all iterations completed without errors
- `1` - Error: pre-existing threads detected
- `2` - Error: unsupported interceptor called during simulation
- `3` - Error: max depth limit hit during simulation

## Relacy reference

Relacy is a runtime thread race detector. It provides an alternate implementation of
standard library functions like pthread_* and std::mutex/atomic/conditiob_variable etc.
This contrasts with TSAN's approach of compiler instrumentation of all memory accesses
for non-atomic (stack variable, global variable) reads/writes and atomic (std::atomic)
reads/writes. Relacy only sees pthread* calls because it intercepts them via its own
impls, and only sees non-atomic variables via rl::var, or sees atomics via rl::atomic
or std::atomic.

Relacy does not run real OS threads; it simulates threads as fibers all on a single OS
thread. This allows the Relacy runtime to fully control the thread interleaving.

Relacy supports a couple different scheduling algos. One is random, and another is
a full graph search (depth first lexicographic enumeration).

Relacy lives in /workarea/llvm-project/compiler-rt/lib/tsan/rtl/relacy. Please read
relacy/AGENTS.md in this directory for an overview of the project.

## TSAN vs Relacy

TSAN works on any program; is instruments the compilation by inserting hooks to atomic
ops and all memory accesses. Its runtime intercepts pthread* APIs calls. TSAN in its
current form only executes whichever thread execution path the OS and CPU choose, which
is not in TSAN's control

As described earlier, Relacy controls the full interleaving. It interceps sync points like
atomic ops and pthread_* APIs calls, and switches to different fibers according to what
the random or full scheduler say.

For TSAn's new simulate feature, we will NOT be using fibers; instead, we will continue
using the OS threads. We will ensure exactly one thread can run at once. Other threads
will wait using internal mutex/cv impl. For example, if there are 4 threads running,
3 should be blocked on an internal TSAN mutex, with only one running. When the running
thread reaches a sync point, it should check if another thread should run. If another
thread should run, the running thread should mark itself as not runnable, and wake
the other thread to run and put itself to sleep.

## Proposed solution

Create a C-callable API entrypoint for unit tests of lock-free data structures

```

struct mpsq_q { ... };

void test_multi_producer_single_consumer_queue(void* arg) {
  (void)args; // Unused;

  mpsq_q q;

  // Test case spawns multiple threads, with producers pushing items
  // to the queue, and one consumer consuming, with various assertions.
  ...
}

int main() {
  __tsan_simulate(test_multi_producer_single_consumer_queue, nullptr);
  return 0;
}
```

Another simple example

```
void test_two_threads(void* arg) {
  int x{0};
  std::thread t1{[&] {
    ++x;
  }};
  std::thread t2{[] {
    ++x;
  }};

  t1.join();
  t2.join();

  assert(x == 2);
}

int main() {
  __tsan_simulate(test_two_threads, nullptr);
  return 0;
}
```

__tsan_simulate will invoke the callback with argument N times, exploring
the thread interleaving graph space.

## TSAN flags

TSAN_OPTIONS=...

There should be flags for
 - simulate_scheduler=<random|...> - which scheduler algo to choose (only random supported for now)
 - simulate_iterations=N - number of iterations (see Relacy test_params)
 - simulate_max_depth=N - max depth (see Relacy test_params)

## Comparison with Relacy

```
struct test_two_threads : rl::test_suite<test_two_threads, 2>
{
  int x{0};

  void thread(unsigned index)
  {
    if (index == 0) {
      ++x;
    } else {
      ++x;
    }
  }

  void after() {
    RL_ASSERT(x == 2);
  }
};
```

## Non-atomic ops

Ignore non-atomic ops for now. In the future we might want to use annotations
to insert sync/schedule points on certain non-atomic memory accesses / variable
accesses, but we do not care about that for now.

## To Build

cd /workarea/llvm-project/build
ninja tsan -j 32

To build a test executable with the new API

/workarea/llvm-project/build/bin/clang++ -fsanitize=thread foo.cpp -o foo

And to run,

TSAN_OPTIONS=simulate_scheduler=random ./foo

## Test cases needed

 - fsanitize-thread-simulate-main
   and, that it conflicts with -Wl,--wrap
 - simulation calls an unsupported API
 - deadlock detection
 - paper1, rare_ref
 - simulate existing threads are running

### TODO

 - std::atomic::wait/notify_* do not work currently. Although it varies by platform,
   OSes like Linux will implement with with futex, which is invisible to TSAN. TSAN
   simulation will not be told when a thread is about to wait and be blocked, and
   the simulation requires being told which threads are blocked so it can ensure
   another runnable thread can execute.
   Possible solutions (none are great)
     - Update the Transform llvm opt pass to intercept atomic:::wait/notify_* calls
     - Provide tsan_interface.h hooks for user code or even libstdc++/libc++ to
       instrument when the calls happen within atomic::wait//notify_*.
