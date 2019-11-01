#include <devastator/diagnostic.hxx>
#include <devastator/opnew.hxx>

#if DEVA_WORLD
  #include <devastator/world.hxx>
#endif

#include <csignal>
#include <mutex>

const char *const deva::git_version = DEVA_GIT_VERSION;

namespace {
  std::mutex lock_;
}

#if DEVA_GASNET_PRESENT
  #include <external/gasnetex.h>
  
  extern "C" {
    volatile int deva_frozen;
  }

  void deva::dbgbrk(bool *aborting) {
    gasnett_freezeForDebuggerNow(&deva_frozen, "deva_frozen");
  }
#else
  void deva::dbgbrk(bool *aborting) {
    std::raise(SIGINT);
  }
#endif

void deva::assert_failed(const char *file, int line, const char *msg) {
  std::stringstream ss;

  ss << std::string(50, '/') << '\n';
  #if DEVA_WORLD
    ss << "ASSERT FAILED rank="<<deva::rank_me()<<" at="<<file<<':'<<line<<'\n';
  #else
    ss << "ASSERT FAILED at="<<file<<':'<<line<<'\n';
  #endif
  if(msg != nullptr && '\0' != msg[0])
    ss << '\n' << msg << '\n';
  
  #if DEVA_GASNET_PRESENT
    if(0 == gasnett_getenv_int_withdefault("GASNET_FREEZE_ON_ERROR", 0, 0)) {
      ss << "\n"
        "To freeze during these errors so you can attach a debugger, rerun "
        "the program with GASNET_FREEZE_ON_ERROR=1 in the environment.\n";
    }
  #endif

  ss << std::string(50, '/') << '\n';
  
  lock_.lock();
  std::cerr << ss.str();
  lock_.unlock();
  
  bool aborting = true;
  deva::dbgbrk(&aborting);
  if(aborting) std::abort();
}

deva::say::say() {
  lock_.lock();
  #if DEVA_WORLD
    std::cerr<<"["<<deva::rank_me()<<"] ";
  #endif
}

deva::say::~say() {
  std::cerr<<"\n";
  lock_.unlock();
}

deva::datarow deva::describe() {
  datarow ans;

  ans &= datarow::x("git", DEVA_GIT_VERSION);
  ans &= datarow::x("opnew", DEVA_OPNEW);
  
  #if DEVA_WORLD
    ans &= datarow::x("world", DEVA_WORLD_THREADS ? "threads" : "gasnet" );
    ans &= datarow::x("ranks", deva::rank_n);
    #if DEVA_WORLD_GASNET
      ans &= datarow::x("procs", deva::process_n);
      ans &= datarow::x("workers", deva::worker_n);
    #endif
  #endif

  ans &= datarow::x("tmsg", DEVA_THREADS_SPSC ? "spsc" : DEVA_THREADS_MPSC ? "mpsc" : "");

  #if DEVA_THREADS_SPSC
    ans &= datarow::x("tsigbits", DEVA_THREADS_SPSC_BITS);
  #endif

  return ans;
}
