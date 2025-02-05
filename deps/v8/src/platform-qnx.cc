// Copyright 2012 the V8 project authors. All rights reserved.
// Additional changes copyright © 2011-2012 Research In Motion Limited.
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
//       notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
//       copyright notice, this list of conditions and the following
//       disclaimer in the documentation and/or other materials provided
//       with the distribution.
//     * Neither the name of Google Inc. nor the names of its
//       contributors may be used to endorse or promote products derived
//       from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

// Platform specific code for QNX goes here. For the POSIX comaptible parts
// the implementation is in platform-posix.cc.

#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <stdlib.h>
#include <ucontext.h>

#include <sys/types.h>  // mmap & munmap
#include <sys/mman.h>   // mmap & munmap
#include <sys/stat.h>   // open
#include <fcntl.h>      // open
#include <unistd.h>     // sysconf
#include <strings.h>    // index
#include <errno.h>
#include <stdarg.h>
#include <sys/procfs.h>
#include <sys/syspage.h>

#undef MAP_TYPE

#include "v8.h"

#include "platform-posix.h"
#include "platform.h"
#include "v8threads.h"
#include "vm-state-inl.h"


namespace v8 {
namespace internal {

static const pthread_t kNoThread = (pthread_t) 0;


double ceiling(double x) {
  return ceil(x);
}


static Mutex* limit_mutex = NULL;


void OS::SetUp() {
  // Seed the random number generator. We preserve microsecond resolution.
  uint64_t seed = Ticks() ^ (getpid() << 16);
  srandom(static_cast<unsigned int>(seed));
  limit_mutex = CreateMutex();

#ifdef __arm__
  // When running on ARM hardware check that the EABI used by V8 and
  // by the C code is the same.
  bool hard_float = OS::ArmUsingHardFloat();
  if (hard_float) {
#if !USE_EABI_HARDFLOAT
    PrintF("ERROR: Binary compiled with -mfloat-abi=hard but without "
           "-DUSE_EABI_HARDFLOAT\n");
    exit(1);
#endif
  } else {
#if USE_EABI_HARDFLOAT
    PrintF("ERROR: Binary not compiled with -mfloat-abi=hard but with "
           "-DUSE_EABI_HARDFLOAT\n");
    exit(1);
#endif
  }
#endif
// TODO QNX
//  SignalSender::SetUp();
}


void OS::PostSetUp() {
  POSIXPostSetUp();
}


uint64_t OS::CpuFeaturesImpliedByPlatform() {
  return 0;  // QNX runs on anything.
}


#ifdef __arm__
bool OS::ArmCpuHasFeature(CpuFeature feature) {
  switch (feature) {
    case VFP3:
      // All shipping devices currently support this and QNX has no easy way to
      // determine this at runtime.
      return true;
    case ARMv7:
      return (SYSPAGE_ENTRY(cpuinfo)->flags & ARM_CPU_FLAG_V7) != 0;
    default:
      UNREACHABLE();
  }

  return false;
}

CpuImplementer OS::GetCpuImplementer() {
    return ARM_IMPLEMENTER;
}

// Simple helper function to detect whether the C code is compiled with
// option -mfloat-abi=hard. The register d0 is loaded with 1.0 and the register
// pair r0, r1 is loaded with 0.0. If -mfloat-abi=hard is pased to GCC then
// calling this will return 1.0 and otherwise 0.0.
static void ArmUsingHardFloatHelper() {
  asm("mov r0, #0");
#if defined(__VFP_FP__) && !defined(__SOFTFP__)
  // Load 0x3ff00000 into r1 using instructions available in both ARM
  // and Thumb mode.
  asm("mov r1, #3");
  asm("mov r2, #255");
  asm("lsl r1, r1, #8");
  asm("orr r1, r1, r2");
  asm("lsl r1, r1, #20");
  // For vmov d0, r0, r1 use ARM mode.
#ifdef __thumb__
  asm volatile(
    "@   Enter ARM Mode  \n\t"
    "    adr r3, 1f      \n\t"
    "    bx  r3          \n\t"
    "    .ALIGN 4        \n\t"
    "    .ARM            \n"
    "1:  vmov d0, r0, r1 \n\t"
    "@   Enter THUMB Mode\n\t"
    "    adr r3, 2f+1    \n\t"
    "    bx  r3          \n\t"
    "    .THUMB          \n"
    "2:                  \n\t");
#else
  asm("vmov d0, r0, r1");
#endif  // __thumb__
#endif  // defined(__VFP_FP__) && !defined(__SOFTFP__)
  asm("mov r1, #0");
}


bool OS::ArmUsingHardFloat() {
  // Cast helper function from returning void to returning double.
  typedef double (*F)();
  F f = FUNCTION_CAST<F>(FUNCTION_ADDR(ArmUsingHardFloatHelper));
  return f() == 1.0;
}
#endif  // def __arm__


int OS::ActivationFrameAlignment() {
#ifdef V8_TARGET_ARCH_ARM
  // On EABI ARM targets this is required for fp correctness in the
  // runtime system.
  return 8;
#endif
  // With gcc 4.4 the tree vectorization optimizer can generate code
  // that requires 16 byte alignment such as movdqa on x86.
  return 16;
}


void OS::ReleaseStore(volatile AtomicWord* ptr, AtomicWord value) {
#if defined(V8_TARGET_ARCH_ARM) && defined(__arm__)
  // Only use on ARM hardware.
  MemoryBarrier();
#else
  __asm__ __volatile__("" : : : "memory");
  // An x86 store acts as a release barrier.
#endif
  *ptr = value;
}


const char* OS::LocalTimezone(double time) {
  if (isnan(time)) return "";
  time_t tv = static_cast<time_t>(floor(time/msPerSecond));
  struct tm* t = localtime(&tv);
  if (NULL == t) return "";
  return t->tm_zone;
}


double OS::LocalTimeOffset() {
  time_t tv = time(NULL);
  struct tm* t = localtime(&tv);
  // tm_gmtoff includes any daylight savings offset, so subtract it.
  return static_cast<double>(t->tm_gmtoff * msPerSecond -
                             (t->tm_isdst > 0 ? 3600 * msPerSecond : 0));
}


// We keep the lowest and highest addresses mapped as a quick way of
// determining that pointers are outside the heap (used mostly in assertions
// and verification).  The estimate is conservative, ie, not all addresses in
// 'allocated' space are actually allocated to our heap.  The range is
// [lowest, highest), inclusive on the low and and exclusive on the high end.
static void* lowest_ever_allocated = reinterpret_cast<void*>(-1);
static void* highest_ever_allocated = reinterpret_cast<void*>(0);


static void UpdateAllocatedSpaceLimits(void* address, int size) {
  ASSERT(limit_mutex != NULL);
  ScopedLock lock(limit_mutex);

  lowest_ever_allocated = Min(lowest_ever_allocated, address);
  highest_ever_allocated =
      Max(highest_ever_allocated,
          reinterpret_cast<void*>(reinterpret_cast<char*>(address) + size));
}


bool OS::IsOutsideAllocatedSpace(void* address) {
  return address < lowest_ever_allocated || address >= highest_ever_allocated;
}


size_t OS::AllocateAlignment() {
  return sysconf(_SC_PAGESIZE);
}


void* OS::Allocate(const size_t requested,
                   size_t* allocated,
                   bool is_executable) {
  const size_t msize = RoundUp(requested, sysconf(_SC_PAGESIZE));
  int prot = PROT_READ | PROT_WRITE | (is_executable ? PROT_EXEC : 0);
  void* addr = GetRandomMmapAddr();
  void* mbase = mmap(addr, msize, prot, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (mbase == MAP_FAILED) {
    LOG(i::Isolate::Current(),
        StringEvent("OS::Allocate", "mmap failed"));
    return NULL;
  }
  *allocated = msize;
  UpdateAllocatedSpaceLimits(mbase, msize);
  return mbase;
}


void OS::Free(void* address, const size_t size) {
  // TODO(1240712): munmap has a return value which is ignored here.
  int result = munmap(address, size);
  USE(result);
  ASSERT(result == 0);
}


void OS::Sleep(int milliseconds) {
  unsigned int ms = static_cast<unsigned int>(milliseconds);
  usleep(1000 * ms);
}


void OS::Abort() {
  // Redirect to std abort to signal abnormal program termination.
  if (FLAG_break_on_abort) {
    DebugBreak();
  }
  abort();
}


void OS::DebugBreak() {
// TODO(lrn): Introduce processor define for runtime system (!= V8_ARCH_x,
//  which is the architecture of generated code).
#if (defined(__arm__) || defined(__thumb__))
# if defined(CAN_USE_ARMV5_INSTRUCTIONS)
  asm("bkpt 0");
# endif
#else
  asm("int $3");
#endif
}


class PosixMemoryMappedFile : public OS::MemoryMappedFile {
 public:
  PosixMemoryMappedFile(FILE* file, void* memory, int size)
    : file_(file), memory_(memory), size_(size) { }
  virtual ~PosixMemoryMappedFile();
  virtual void* memory() { return memory_; }
  virtual int size() { return size_; }
 private:
  FILE* file_;
  void* memory_;
  int size_;
};


OS::MemoryMappedFile* OS::MemoryMappedFile::open(const char* name) {
  FILE* file = fopen(name, "r+");
  if (file == NULL) return NULL;

  fseek(file, 0, SEEK_END);
  int size = ftell(file);

  void* memory =
      mmap(OS::GetRandomMmapAddr(),
           size,
           PROT_READ | PROT_WRITE,
           MAP_SHARED,
           fileno(file),
           0);
  return new PosixMemoryMappedFile(file, memory, size);
}


OS::MemoryMappedFile* OS::MemoryMappedFile::create(const char* name, int size,
    void* initial) {
  FILE* file = fopen(name, "w+");
  if (file == NULL) return NULL;
  int result = fwrite(initial, size, 1, file);
  if (result < 1) {
    fclose(file);
    return NULL;
  }
  void* memory =
      mmap(OS::GetRandomMmapAddr(),
           size,
           PROT_READ | PROT_WRITE,
           MAP_SHARED,
           fileno(file),
           0);
  return new PosixMemoryMappedFile(file, memory, size);
}


PosixMemoryMappedFile::~PosixMemoryMappedFile() {
  if (memory_) munmap(memory_, size_);
  fclose(file_);
}


void OS::LogSharedLibraryAddresses() {
  procfs_mapinfo *mapinfos = NULL, *mapinfo;
  int proc_fd, num, i;

  struct {
    procfs_debuginfo info;
    char buff[PATH_MAX];
  } map;

  char buf[PATH_MAX + 1];
  sprintf(buf, "/proc/%d/as", getpid());

  if ((proc_fd = open(buf, O_RDONLY)) == -1) {
    close(proc_fd);
    return;
  }

  /* Get the number of map entrys.  */
  if (devctl(proc_fd, DCMD_PROC_MAPINFO, NULL, 0, &num) != EOK) {
    close(proc_fd);
    return;
  }

  mapinfos =(procfs_mapinfo *)malloc(num * sizeof(procfs_mapinfo));
  if (mapinfos == NULL) {
    close(proc_fd);
    return;
  }

  /* Fill the map entrys.  */
  if (devctl(proc_fd, DCMD_PROC_PAGEDATA, mapinfos, num * sizeof(procfs_mapinfo), &num) != EOK) {
    free(mapinfos);
    close(proc_fd);
    return;
  }

  i::Isolate* isolate = ISOLATE;

  for (i = 0; i < num; i++) {
    mapinfo = mapinfos + i;
    if (mapinfo->flags & MAP_ELF) {
      map.info.vaddr = mapinfo->vaddr;
      if (devctl(proc_fd, DCMD_PROC_MAPDEBUG, &map, sizeof(map), 0) != EOK)
	    continue;

	  LOG(isolate, SharedLibraryEvent(map.info.path, mapinfo->vaddr, mapinfo->vaddr + mapinfo->size));
	}
  }
  free(mapinfos);
  close(proc_fd);
}


static const char kGCFakeMmap[] = "/tmp/__v8_gc__";


void OS::SignalCodeMovingGC() {
  // Support for ll_prof.py.
  int size = sysconf(_SC_PAGESIZE);
  FILE* f = fopen(kGCFakeMmap, "w+");
  void* addr = mmap(OS::GetRandomMmapAddr(),
                    size,
                    PROT_READ | PROT_EXEC,
                    MAP_PRIVATE,
                    fileno(f),
                    0);
  ASSERT(addr != MAP_FAILED);
  munmap(addr, size);
  fclose(f);
}


int OS::StackWalk(Vector<OS::StackFrame> frames) {
	// Not supported without additional libbacktrace on QNX.
  return 0;
}


// Constants used for mmap.
static const int kMmapFd = -1;
static const int kMmapFdOffset = 0;

VirtualMemory::VirtualMemory() : address_(NULL), size_(0) { }

VirtualMemory::VirtualMemory(size_t size) {
  address_ = ReserveRegion(size);
  size_ = size;
}


VirtualMemory::VirtualMemory(size_t size, size_t alignment)
    : address_(NULL), size_(0) {
  ASSERT(IsAligned(alignment, static_cast<intptr_t>(OS::AllocateAlignment())));
  size_t request_size = RoundUp(size + alignment,
                                static_cast<intptr_t>(OS::AllocateAlignment()));
  void* reservation = mmap(OS::GetRandomMmapAddr(),
                           request_size,
                           PROT_NONE,
                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_LAZY,
                           kMmapFd,
                           kMmapFdOffset);
  if (reservation == MAP_FAILED) return;

  Address base = static_cast<Address>(reservation);
  Address aligned_base = RoundUp(base, alignment);
  ASSERT_LE(base, aligned_base);

  // Unmap extra memory reserved before and after the desired block.
  if (aligned_base != base) {
    size_t prefix_size = static_cast<size_t>(aligned_base - base);
    OS::Free(base, prefix_size);
    request_size -= prefix_size;
  }

  size_t aligned_size = RoundUp(size, OS::AllocateAlignment());
  ASSERT_LE(aligned_size, request_size);

  if (aligned_size != request_size) {
    size_t suffix_size = request_size - aligned_size;
    OS::Free(aligned_base + aligned_size, suffix_size);
    request_size -= suffix_size;
  }

  ASSERT(aligned_size == request_size);

  address_ = static_cast<void*>(aligned_base);
  size_ = aligned_size;
}


VirtualMemory::~VirtualMemory() {
  if (IsReserved()) {
    bool result = ReleaseRegion(address(), size());
    ASSERT(result);
    USE(result);
  }
}


bool VirtualMemory::IsReserved() {
  return address_ != NULL;
}


void VirtualMemory::Reset() {
  address_ = NULL;
  size_ = 0;
}


bool VirtualMemory::Commit(void* address, size_t size, bool is_executable) {
  return CommitRegion(address, size, is_executable);
}


bool VirtualMemory::Uncommit(void* address, size_t size) {
  return UncommitRegion(address, size);
}


bool VirtualMemory::Guard(void* address) {
  OS::Guard(address, OS::CommitPageSize());
  return true;
}


void* VirtualMemory::ReserveRegion(size_t size) {
  void* result = mmap(OS::GetRandomMmapAddr(),
                      size,
                      PROT_NONE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_LAZY,
                      kMmapFd,
                      kMmapFdOffset);

  if (result == MAP_FAILED) return NULL;

  return result;
}


bool VirtualMemory::CommitRegion(void* base, size_t size, bool is_executable) {
  int prot = PROT_READ | PROT_WRITE | (is_executable ? PROT_EXEC : 0);
  if (MAP_FAILED == mmap(base,
                         size,
                         prot,
                         MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED,
                         kMmapFd,
                         kMmapFdOffset)) {
    return false;
  }

  UpdateAllocatedSpaceLimits(base, size);
  return true;
}


bool VirtualMemory::UncommitRegion(void* base, size_t size) {
  return mmap(base,
              size,
              PROT_NONE,
              MAP_PRIVATE | MAP_ANONYMOUS | MAP_LAZY | MAP_FIXED,
              kMmapFd,
              kMmapFdOffset) != MAP_FAILED;
}


bool VirtualMemory::ReleaseRegion(void* base, size_t size) {
  return munmap(base, size) == 0;
}


class Thread::PlatformData : public Malloced {
 public:
  PlatformData() : thread_(kNoThread) {}

  pthread_t thread_;  // Thread handle for pthread.
};

Thread::Thread(const Options& options)
    : data_(new PlatformData()),
      stack_size_(options.stack_size()) {
  set_name(options.name());
}


Thread::~Thread() {
  delete data_;
}


static void* ThreadEntry(void* arg) {
  Thread* thread = reinterpret_cast<Thread*>(arg);
  // This is also initialized by the first argument to pthread_create() but we
  // don't know which thread will run first (the original thread or the new
  // one) so we initialize it here too.
#ifdef PR_SET_NAME
  prctl(PR_SET_NAME,
        reinterpret_cast<unsigned long>(thread->name()),  // NOLINT
        0, 0, 0);
#endif
  thread->data()->thread_ = pthread_self();
  ASSERT(thread->data()->thread_ != kNoThread);
  thread->Run();
  return NULL;
}


void Thread::set_name(const char* name) {
  strncpy(name_, name, sizeof(name_));
  name_[sizeof(name_) - 1] = '\0';
}


void Thread::Start() {
  pthread_attr_t* attr_ptr = NULL;
  pthread_attr_t attr;
  if (stack_size_ > 0) {
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, static_cast<size_t>(stack_size_));
    attr_ptr = &attr;
  }
  int result = pthread_create(&data_->thread_, attr_ptr, ThreadEntry, this);
  CHECK_EQ(0, result);
  ASSERT(data_->thread_ != kNoThread);
}


void Thread::Join() {
  pthread_join(data_->thread_, NULL);
}


Thread::LocalStorageKey Thread::CreateThreadLocalKey() {
  pthread_key_t key;
  int result = pthread_key_create(&key, NULL);
  USE(result);
  ASSERT(result == 0);
  return static_cast<LocalStorageKey>(key);
}


void Thread::DeleteThreadLocalKey(LocalStorageKey key) {
  pthread_key_t pthread_key = static_cast<pthread_key_t>(key);
  int result = pthread_key_delete(pthread_key);
  USE(result);
  ASSERT(result == 0);
}


void* Thread::GetThreadLocal(LocalStorageKey key) {
  pthread_key_t pthread_key = static_cast<pthread_key_t>(key);
  return pthread_getspecific(pthread_key);
}


void Thread::SetThreadLocal(LocalStorageKey key, void* value) {
  pthread_key_t pthread_key = static_cast<pthread_key_t>(key);
  pthread_setspecific(pthread_key, value);
}


void Thread::YieldCPU() {
  sched_yield();
}


class QNXMutex : public Mutex {
 public:
  QNXMutex() {
    pthread_mutexattr_t attrs;
    int result = pthread_mutexattr_init(&attrs);
    ASSERT(result == 0);
    result = pthread_mutexattr_settype(&attrs, PTHREAD_MUTEX_RECURSIVE);
    ASSERT(result == 0);
    result = pthread_mutex_init(&mutex_, &attrs);
    ASSERT(result == 0);
    USE(result);
  }

  virtual ~QNXMutex() { pthread_mutex_destroy(&mutex_); }

  virtual int Lock() {
    int result = pthread_mutex_lock(&mutex_);
    return result;
  }

  virtual int Unlock() {
    int result = pthread_mutex_unlock(&mutex_);
    return result;
  }

  virtual bool TryLock() {
    int result = pthread_mutex_trylock(&mutex_);
    // Return false if the lock is busy and locking failed.
    if (result == EBUSY) {
      return false;
    }
    ASSERT(result == 0);  // Verify no other errors.
    return true;
  }

 private:
  pthread_mutex_t mutex_;   // Pthread mutex for POSIX platforms.
};


Mutex* OS::CreateMutex() {
  return new QNXMutex();
}


class QNXSemaphore : public Semaphore {
 public:
  explicit QNXSemaphore(int count) {  sem_init(&sem_, 0, count); }
  virtual ~QNXSemaphore() { sem_destroy(&sem_); }

  virtual void Wait();
  virtual bool Wait(int timeout);
  virtual void Signal() { sem_post(&sem_); }
 private:
  sem_t sem_;
};


void QNXSemaphore::Wait() {
  while (true) {
    int result = sem_wait(&sem_);
    if (result == 0) return;  // Successfully got semaphore.
    CHECK(result == -1 && errno == EINTR);  // Signal caused spurious wakeup.
  }
}


#ifndef TIMEVAL_TO_TIMESPEC
#define TIMEVAL_TO_TIMESPEC(tv, ts) do {                            \
    (ts)->tv_sec = (tv)->tv_sec;                                    \
    (ts)->tv_nsec = (tv)->tv_usec * 1000;                           \
} while (false)
#endif


bool QNXSemaphore::Wait(int timeout) {
  const long kOneSecondMicros = 1000000;  // NOLINT

  // Split timeout into second and nanosecond parts.
  struct timeval delta;
  delta.tv_usec = timeout % kOneSecondMicros;
  delta.tv_sec = timeout / kOneSecondMicros;

  struct timeval current_time;
  // Get the current time.
  if (gettimeofday(&current_time, NULL) == -1) {
    return false;
  }

  // Calculate time for end of timeout.
  struct timeval end_time;
  timeradd(&current_time, &delta, &end_time);

  struct timespec ts;
  TIMEVAL_TO_TIMESPEC(&end_time, &ts);
  // Wait for semaphore signalled or timeout.
  while (true) {
    int result = sem_timedwait(&sem_, &ts);
    if (result == 0) return true;  // Successfully got semaphore.
    if (result == -1 && errno == ETIMEDOUT) return false;  // Timeout.
    CHECK(result == -1 && errno == EINTR);  // Signal caused spurious wakeup.
  }
}


Semaphore* OS::CreateSemaphore(int count) {
  return new QNXSemaphore(count);
}


static int GetThreadID() {
  pthread_t thread_id = pthread_self();
  return thread_id;
}


static void ProfilerSignalHandler(int signal, siginfo_t* info, void* context) {
  USE(info);
  if (signal != SIGPROF) return;
  Isolate* isolate = Isolate::UncheckedCurrent();
  if (isolate == NULL || !isolate->IsInitialized() || !isolate->IsInUse()) {
    // We require a fully initialized and entered isolate.
    return;
  }
  if (v8::Locker::IsActive() &&
      !isolate->thread_manager()->IsLockedByCurrentThread()) {
    return;
  }

  Sampler* sampler = isolate->logger()->sampler();
  if (sampler == NULL || !sampler->IsActive()) return;

  TickSample sample_obj;
  TickSample* sample = CpuProfiler::TickSampleEvent(isolate);
  if (sample == NULL) sample = &sample_obj;

  // Extracting the sample from the context is extremely machine dependent.
  ucontext_t* ucontext = reinterpret_cast<ucontext_t*>(context);
  mcontext_t& mcontext = ucontext->uc_mcontext;
  sample->state = isolate->current_vm_state();
#if V8_HOST_ARCH_IA32
  sample->pc = reinterpret_cast<Address>(mcontext.cpu.eip);
  sample->sp = reinterpret_cast<Address>(mcontext.cpu.esp);
  sample->fp = reinterpret_cast<Address>(mcontext.cpu.ebp);
#elif V8_HOST_ARCH_X64
  sample->pc = reinterpret_cast<Address>(mcontext.cpu.rip);
  sample->sp = reinterpret_cast<Address>(mcontext.cpu.rsp);
  sample->fp = reinterpret_cast<Address>(mcontext.cpu.rbp);
#elif V8_HOST_ARCH_ARM
  sample->pc = reinterpret_cast<Address>(mcontext.cpu.gpr[ARM_REG_PC]);
  sample->sp = reinterpret_cast<Address>(mcontext.cpu.gpr[ARM_REG_SP]);
  sample->fp = reinterpret_cast<Address>(mcontext.cpu.gpr[ARM_REG_FP]);
#endif
  sampler->SampleStack(sample);
  sampler->Tick(sample);
}


class Sampler::PlatformData : public Malloced {
 public:
  PlatformData() : vm_tid_(GetThreadID()) {}

  int vm_tid() const { return vm_tid_; }

 private:
  const int vm_tid_;
};


class SignalSender : public Thread {
 public:
  enum SleepInterval {
    HALF_INTERVAL,
    FULL_INTERVAL
  };

  static const int kSignalSenderStackSize = 32 * KB;

  explicit SignalSender(int interval)
      : Thread(Thread::Options("SignalSender", kSignalSenderStackSize)),
        vm_tgid_(getpid()),
        interval_(interval) {}

  static void SetUp() { if (!mutex_) mutex_ = OS::CreateMutex(); }
  static void TearDown() { delete mutex_; }

  static void InstallSignalHandler() {
    struct sigaction sa;
    sa.sa_sigaction = ProfilerSignalHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_SIGINFO;
    signal_handler_installed_ =
        (sigaction(SIGPROF, &sa, &old_signal_handler_) == 0);
  }

  static void RestoreSignalHandler() {
    if (signal_handler_installed_) {
      sigaction(SIGPROF, &old_signal_handler_, 0);
      signal_handler_installed_ = false;
    }
  }

  static void AddActiveSampler(Sampler* sampler) {
    ScopedLock lock(mutex_);
    SamplerRegistry::AddActiveSampler(sampler);
    if (instance_ == NULL) {
      // Start a thread that will send SIGPROF signal to VM threads,
      // when CPU profiling will be enabled.
      instance_ = new SignalSender(sampler->interval());
      instance_->Start();
    } else {
      ASSERT(instance_->interval_ == sampler->interval());
    }
  }

  static void RemoveActiveSampler(Sampler* sampler) {
    ScopedLock lock(mutex_);
    SamplerRegistry::RemoveActiveSampler(sampler);
    if (SamplerRegistry::GetState() == SamplerRegistry::HAS_NO_SAMPLERS) {
      RuntimeProfiler::StopRuntimeProfilerThreadBeforeShutdown(instance_);
      delete instance_;
      instance_ = NULL;
      RestoreSignalHandler();
    }
  }

  // Implement Thread::Run().
  virtual void Run() {
    SamplerRegistry::State state;
    while ((state = SamplerRegistry::GetState()) !=
           SamplerRegistry::HAS_NO_SAMPLERS) {
      bool cpu_profiling_enabled =
          (state == SamplerRegistry::HAS_CPU_PROFILING_SAMPLERS);
      bool runtime_profiler_enabled = RuntimeProfiler::IsEnabled();
      if (cpu_profiling_enabled && !signal_handler_installed_) {
        InstallSignalHandler();
      } else if (!cpu_profiling_enabled && signal_handler_installed_) {
        RestoreSignalHandler();
      }
      // When CPU profiling is enabled both JavaScript and C++ code is
      // profiled. We must not suspend.
      if (!cpu_profiling_enabled) {
        if (rate_limiter_.SuspendIfNecessary()) continue;
      }
      if (cpu_profiling_enabled && runtime_profiler_enabled) {
        if (!SamplerRegistry::IterateActiveSamplers(&DoCpuProfile, this)) {
          return;
        }
        Sleep(HALF_INTERVAL);
        if (!SamplerRegistry::IterateActiveSamplers(&DoRuntimeProfile, NULL)) {
          return;
        }
        Sleep(HALF_INTERVAL);
      } else {
        if (cpu_profiling_enabled) {
          if (!SamplerRegistry::IterateActiveSamplers(&DoCpuProfile,
                                                      this)) {
            return;
          }
        }
        if (runtime_profiler_enabled) {
          if (!SamplerRegistry::IterateActiveSamplers(&DoRuntimeProfile,
                                                      NULL)) {
            return;
          }
        }
        Sleep(FULL_INTERVAL);
      }
    }
  }

  static void DoCpuProfile(Sampler* sampler, void* raw_sender) {
    if (!sampler->IsProfiling()) return;
    SignalSender* sender = reinterpret_cast<SignalSender*>(raw_sender);
    sender->SendProfilingSignal(sampler->platform_data()->vm_tid());
  }

  static void DoRuntimeProfile(Sampler* sampler, void* ignored) {
    if (!sampler->isolate()->IsInitialized()) return;
    sampler->isolate()->runtime_profiler()->NotifyTick();
  }

  void SendProfilingSignal(int tid) {
    if (!signal_handler_installed_) return;
    pthread_kill(tid, SIGPROF);
  }

  void Sleep(SleepInterval full_or_half) {
    // Convert ms to us and subtract 100 us to compensate delays
    // occuring during signal delivery.
    useconds_t interval = interval_ * 1000 - 100;
    if (full_or_half == HALF_INTERVAL) interval /= 2;
    int result = usleep(interval);
#ifdef DEBUG
    if (result != 0 && errno != EINTR) {
      fprintf(stderr,
              "SignalSender usleep error; interval = %u, errno = %d\n",
              interval,
              errno);
      ASSERT(result == 0 || errno == EINTR);
    }
#endif
    USE(result);
  }

  const int vm_tgid_;
  const int interval_;
  RuntimeProfilerRateLimiter rate_limiter_;

  // Protects the process wide state below.
  static Mutex* mutex_;
  static SignalSender* instance_;
  static bool signal_handler_installed_;
  static struct sigaction old_signal_handler_;

  DISALLOW_COPY_AND_ASSIGN(SignalSender);
};


Mutex* SignalSender::mutex_ = OS::CreateMutex();
SignalSender* SignalSender::instance_ = NULL;
struct sigaction SignalSender::old_signal_handler_;
bool SignalSender::signal_handler_installed_ = false;


void OS::TearDown() {
  SignalSender::TearDown();
  delete limit_mutex;
}


Sampler::Sampler(Isolate* isolate, int interval)
    : isolate_(isolate),
      interval_(interval),
      profiling_(false),
      active_(false),
      samples_taken_(0) {
  data_ = new PlatformData;
}


Sampler::~Sampler() {
  ASSERT(!IsActive());
  delete data_;
}


void Sampler::Start() {
  ASSERT(!IsActive());
  SetActive(true);
  SignalSender::AddActiveSampler(this);
}


void Sampler::Stop() {
  ASSERT(IsActive());
  SignalSender::RemoveActiveSampler(this);
  SetActive(false);
}


} }  // namespace v8::internal
