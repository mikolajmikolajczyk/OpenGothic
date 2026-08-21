#include "workers.h"
#include <cstdlib>
#include <cstdio>

#if defined(__PS4__)
#include <orbis/libkernel.h>
// ⚠ ps4_log AND NOT ac_orbis_note. This used to hand-declare `extern "C" void ac_orbis_note(const
// char*)` - a symbol private to Mesa's PS4 arm (ac_orbis_drm.c) - from game code, with no header
// between them. Game code reaching into the graphics driver for a log channel is backwards, and a
// hand-written declaration means a signature change upstream is a link error at best.
//
// ps4_log is the overlay's channel and what the rest of the port already uses.
#include "ps4_app.h"
#endif
#include "utils/string_frm.h"

#include <Tempest/Platform>
#include <Tempest/Log>

#if defined(__WINDOWS__)
#include <windows.h>
#include <processthreadsapi.h>
#endif

#if defined(__GNUC__)
#include <pthread.h>
#endif

#if defined(_MSC_VER)
void Workers::setThreadName(const char* threadName) {
  const DWORD MS_VC_EXCEPTION = 0x406D1388;
  DWORD dwThreadID = GetCurrentThreadId();
#pragma pack(push,8)
  struct THREADNAME_INFO {
    DWORD  dwType = 0x1000; // Must be 0x1000.
    LPCSTR szName;          // Pointer to name (in user addr space).
    DWORD  dwThreadID;      // Thread ID (-1=caller thread).
    DWORD  dwFlags;         // Reserved for future use, must be zero.
    };
#pragma pack(pop)

  THREADNAME_INFO info = {};
  info.szName     = threadName;
  info.dwThreadID = dwThreadID;
  info.dwFlags    = 0;

  __try {
    RaiseException(MS_VC_EXCEPTION, 0, sizeof(info)/sizeof(ULONG_PTR), (ULONG_PTR*)&info );
    }
  __except(EXCEPTION_EXECUTE_HANDLER) {
    }
  }
#elif defined(__WINDOWS__)
void Workers::setThreadName(const char* threadName) {
#if defined(__GNUC__)
  pthread_setname_np(pthread_self(), threadName);
#endif
  auto k32 = GetModuleHandleA("Kernel32");
  auto fn  = GetProcAddress(k32, "SetThreadDescription");
  if(fn==nullptr)
    return;

  // nsight does not care about pthread_setname_np
  WCHAR wname[64] = {};
  for(size_t i=0; i<63 && threadName[i]; ++i)
    wname[i] = WCHAR(threadName[i]);
  auto SetThreadDescription = reinterpret_cast<HRESULT(WINAPI*)(HANDLE,PCWSTR)>(fn);
  SetThreadDescription(GetCurrentThread(), wname);
  }
#elif defined(__GNUC__) && !defined(__clang__)
void Workers::setThreadName(const char* threadName){
  pthread_setname_np(pthread_self(), threadName);
  }
#elif defined(__OSX__)
void Workers::setThreadName(const char* threadName){
  pthread_setname_np(threadName);
  }
#else
void Workers::setThreadName(const char* threadName) { (void)threadName; }
#endif

using namespace Tempest;

const size_t Workers::taskPerThread = 128;
const size_t Workers::taskPerStep   = 16;

Workers::Workers() {
  size_t id=0;
  for(auto& i:th) {
    i = std::thread([this,id]() noexcept {
      threadFunc(id);
      });
    ++id;
    }
  }

Workers::~Workers() {
  running  = false;
  workSet  = nullptr;
  workSize = MAX_THREADS;
  execWork(minWorkSize<void,void>());
  for(auto& i:th)
    i.join();
  }

Workers &Workers::inst() {
  static Workers w;
  return w;
  }

uint8_t Workers::maxThreads() {
  int32_t th = int32_t(std::thread::hardware_concurrency());
#if defined(__PS4__)
  /* ⚠ hardware_concurrency() RETURNS 1 ON THIS CONSOLE. Measured, not assumed - it was printed to
   * the driver's log and read back.
   *
   * The standard permits that: the function is a hint and may return 0 when the answer is unknown.
   * The consequence here is not a hint. Every parallelFor and parallelTasks in this title has been
   * running SERIALLY on the calling thread for the whole life of the PS4 port, on a machine with
   * six cores available to it. Animation alone was 27.2 ms of a 61.6 ms frame while the entire
   * process burned 0.87 cores; those two numbers could only ever be reconciled this way.
   *
   * The kernel knows the real answer. scePthreadGetaffinity reports which cores this process may
   * run on, which is the question that actually matters - not how many the chip has, but how many
   * this title is allowed. The mask is logged either way, because a wrong answer here would quietly
   * oversubscribe rather than fail.
   *
   * ⚠ AND THIS TURNS ON CONCURRENCY THAT HAS NEVER RUN HERE. If OpenGothic's parallel sections
   * carry a latent race, this is the change that wakes it. That is a reason to watch for new
   * misbehaviour, not a reason to stay serial on six cores. */
  /* ⚠ AND NOTHING HERE ASKS FOR A STACK SIZE, deliberately. This block used to rewrite the pool
   * onto pthread_create purely to request 1 MB, because std::thread cannot. orbis-compat's
   * pthread_create interposer raises anything below the MAIN THREAD's stack, which is 2048 KiB
   * here - so the 1 MB request was itself being raised, and a plain std::thread reaches the same
   * 2 MB by the same path. Confirmed on hardware: "16 created, 16 raised to 2048 KiB". */
  {
  uint64_t mask = 0;
  scePthreadGetaffinity(scePthreadSelf(), &mask);
  const int cores = __builtin_popcountll(mask);

  static bool said = false;
  if(!said) {
    char buf[192];
    snprintf(buf,sizeof(buf),
             "Workers: hardware_concurrency() = %d (a lie on this console), affinity mask 0x%llx = "
             "%d cores, MAX_THREADS = %d",
             int(th), (unsigned long long)mask, cores, int(MAX_THREADS));
    ps4_log("%s",buf);
    said = true;
    }

  if(cores>th)
    th = cores;

  /* ⚠ AND THE NUMBER HAS TO BE A KNOB, because six was not better than one.
   *
   * Animation did fall from 27.2 ms to 9.5 - the pool works - but the frame went from 61.6 ms to
   * 77.2, because the GPU's own time grew by about 15 ms at the same moment. Six Jaguar cores
   * working flat out compete with the GPU for the same memory, and on this port every surface the
   * GPU touches is on the CACHE-COHERENT bus, which is the one the CPUs are on.
   *
   * So the useful measurement is the curve, not the endpoint: OG_WORKERS=1,2,3,4,6 says where the
   * two costs cross. Unset means whatever the kernel allows. */
  if(const char* w = getenv("OG_WORKERS")) {
    const int n = atoi(w);
    if(n>0) {
      th = n;
      char buf[96];
      snprintf(buf,sizeof(buf),"Workers: OG_WORKERS=%d overrides the pool size",n);
      ps4_log("%s",buf);
      }
    }
  }
#endif
  if(th<=0)
    th = 1;
  if(th>MAX_THREADS)
    return MAX_THREADS;
  return uint8_t(th);
  }

void Workers::threadFunc(size_t id) {
  {
  string_frm tname("Workers [",int(id),"]");
  setThreadName(tname.c_str());
  }

  while(true) {
    {
    std::unique_lock<std::mutex> lck(sync);
    workWait.wait(lck, [this]() { return workTbd>0; });
    --workTbd;
    }

    if(!running) {
      taskDone.fetch_add(1);
      return;
      }

    if(workSet==nullptr) {
      auto idx = progressIt.fetch_add(1);
      workFunc(workSet+idx, 1);
      } else {
      taskLoop();
      }

    taskDone.fetch_add(1);
    // if(size_t(taskDone.fetch_add(1)+1)==taskCount)
    //   std::this_thread::yield();
    }
  }

uint32_t Workers::taskLoop() {
  uint32_t count = 0;
  while(true) {
    size_t b = size_t(progressIt.fetch_add(taskPerStep));
    size_t e = std::min(b+taskPerStep, workSize);
    if(e<=b)
      break;

    void* d = workSet + b*workEltSize;
    workFunc(d,e-b);
    count += uint32_t(e-b);
    }
  return count;
  }

void Workers::execWork(uint32_t& minElts) {
  if(workSize==0)
    return;

  if(workSet!=nullptr) {
    const auto maxTheads = maxThreads();
    taskCount = uint32_t((workSize+taskPerThread-1)/taskPerThread);
    taskCount--; // main thread also do tasks
    if(taskCount>maxTheads)
      taskCount = maxTheads;
    if(taskCount<=0)
      taskCount = 1;
    } else {
    taskCount = uint32_t(workSize);
    }

  if(running && taskCount==1) {
    workFunc(workSet, workSize);
    return;
    }

  minElts = std::max<uint32_t>(minElts, taskPerThread);

  if(workSet!=nullptr && workSize<=minElts && true) {
    workFunc(workSet, workSize);
    if(minElts > workSize*2)
      minElts = 0;
    return;
    }

  progressIt.store(0);
  taskDone.store(0);

  {
  std::unique_lock<std::mutex> lck(sync);
  workTbd = int32_t(taskCount);
  }
  workWait.notify_all();

  uint32_t cnt = 0;
  if(workSet==nullptr) {
    std::this_thread::yield();
    } else {
    cnt = taskLoop(); (void)cnt;
    }

  while(true) {
    int expect = int(taskCount);
    if(taskDone.load()==expect) {
      taskDone.store(0);
      break;
      }
    std::this_thread::yield();
    }
  }
