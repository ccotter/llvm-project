#pragma once

namespace __tsan {

struct IFuzzingScheduler
{
    virtual void SynchronizationPoint() = 0;
    virtual void UnblockOne(int new_state) = 0;
    virtual int GetCurrentState() = 0;
    virtual void SetCurrentState(int new_state) = 0;
    virtual void SetBlocking(bool IsBlocking) = 0;

    virtual int SynchronizationPoint_MutexLock(void* m) = 0;
    virtual int SynchronizationPoint_MutexTryLock(void* m) = 0;
    virtual int SynchronizationPoint_MutexUnlock(void* m) = 0;
    virtual int SynchronizationPoint_CondWait(void* c, void* m) = 0;
    virtual int SynchronizationPoint_CondNotifyOne(void* c) = 0;
    virtual int SynchronizationPoint_CondNotifyAll(void* c) = 0;
    virtual int SynchronizationPoint_JoinThread(void* th, void** ret) = 0;
    virtual int SynchronizationPoint_DetachThread(void* th) = 0;
    virtual void SynchronizationPoint_InitThread(void* th) = 0;
    virtual void SynchronizationPoint_CreateThread(void* th) = 0;
    virtual void SynchronizationPoint_ExitThread() = 0;

    virtual void SynchronizationPoint_MutexInit(void* m, bool recursive) = 0;
    virtual void SynchronizationPoint_CondInit(void* c) = 0;
};

IFuzzingScheduler& GetFuzzingScheduler();

struct ScopedFuzzingSchedulerBlocked {
  ScopedFuzzingSchedulerBlocked() {
    GetFuzzingScheduler().SetBlocking(true);
  }
  ~ScopedFuzzingSchedulerBlocked() {
    GetFuzzingScheduler().SetBlocking(false);
  }
};

}  // namespace __tsan
