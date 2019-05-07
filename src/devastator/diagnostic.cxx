#include <devastator/diagnostic.hxx>
#include <devastator/world.hxx>

#include <csignal>
#include <mutex>

namespace {
  std::mutex lock_;
}

#if WORLD_GASNET
  #include <gasnetex.h>
  #include <gasnet_tools.h>

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
  ss << "ASSERT FAILED rank="<<deva::rank_me()<<" at="<<file<<':'<<line<<'\n';
  if(msg != nullptr && '\0' != msg[0])
    ss << '\n' << msg << '\n';
  
  #if WORLD_GASNET
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
  std::cerr<<"["<<deva::rank_me()<<"] ";
}

deva::say::~say() {
  std::cerr<<"\n";
  lock_.unlock();
}
