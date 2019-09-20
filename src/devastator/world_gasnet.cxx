#include <devastator/world_gasnet.hxx>
#include <devastator/intrusive_map.hxx>
#include <devastator/opnew.hxx>

#include <external/gasnetex.h>

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <string>
#include <thread>
#include <tuple>

#include <unistd.h>
#include <sched.h>
#include <fcntl.h>

namespace tmsg = deva::tmsg;

using namespace std;

using deva::worker_n;
using deva::remote_out_message;

using upcxx::detail::command;
using upcxx::detail::serialization_reader;
using upcxx::detail::serialization_writer;

__thread int deva::rank_me_ = 0xdeadbeef;

alignas(64)
int deva::process_me_ = 0xdeadbeef;
int deva::process_rank_lo_ = 0xdeadbeef;
int deva::process_rank_hi_ = 0xdeadbeef;

namespace {
  gex_TM_t the_team;
  
  std::atomic<bool> leave_pump{false};
}

alignas(64)
tmsg::channels_r<tmsg::thread_n> deva::remote_send_chan_r;
tmsg::channels_w<1> deva::remote_send_chan_w[tmsg::thread_n];
tmsg::channels_r<1> deva::remote_recv_chan_r[tmsg::thread_n];
tmsg::channels_w<tmsg::thread_n> deva::remote_recv_chan_w;

namespace {
  enum {
    id_am_recv = GEX_AM_INDEX_BASE
  };
  
  void am_recv(gex_Token_t, void *buf, size_t buf_size, gex_AM_Arg_t worker_n);
  
  void init_gasnet();
  void master_pump();

  struct remote_in_messages: tmsg::message {
    int header_size;
    int count;
  };
}

void deva::run(upcxx::detail::function_ref<void()> fn) {
  static bool inited = false;

  if(!inited) {
    inited = true;
    init_gasnet();
    process_rank_lo_ = deva::process_me_*worker_n;
    process_rank_hi_ = (deva::process_me_+1)*worker_n;
  }
  
  tmsg::run([&]() {
    int tme = tmsg::thread_me();

    static thread_local bool inited = false;
    
    if(!inited) {
      inited = true;
      
      if(tme == 0) {
        for(int t=0; t < tmsg::thread_n; t++)
          remote_recv_chan_w.connect(t, remote_recv_chan_r[t]);
      }
      remote_send_chan_w[tme].connect(0, remote_send_chan_r);

      #if 0
        for(int i=0; i < process_n; i++) {
          if(i == deva::process_me_) {
            static std::mutex mu;
            mu.lock();
            std::cerr<<"[pid "<<getpid()<<" t "<<tme<<"] watch *((uintptr_t*)"<<&opnew::my_ts.bins[29].held_pools.top<<")&1\n";
            mu.unlock();
            tmsg::barrier(/*deaf=*/true);
            if(tme == 0)
              gasnett_freezeForDebuggerErr();
          }
          if(tme==0){
            gasnet_barrier_notify(0,0);
            gasnet_barrier_wait(0,0);
          }
          tmsg::barrier(/*deaf=*/true);
        }
      #endif
      
      tmsg::barrier(/*deaf=*/true);
    }

    if(tme == 0) {
      rank_me_ = -(1 + deva::process_me_);
      master_pump();
    }
    else {
      rank_me_ = process_rank_lo_ + tme-1;
      fn();
      
      deva::barrier(/*deaf=*/true);
      
      if(tme == 1)
        leave_pump.store(true, std::memory_order_release);
    }
  });
}

namespace {
  void init_gasnet() {
    #if GASNET_CONDUIT_SMP
      setenv("GASNET_PSHM_NODES", std::to_string(deva::process_n).c_str(), 1);
    #elif GASNET_CONDUIT_ARIES
      { // Everyone carves out some GBs and shares them evenly across peers
        size_t space = std::max<size_t>(512<<20, size_t((512<<20)*(std::log(deva::process_n)/std::log(2))));
        setenv("GASNET_NETWORKDEPTH_SPACE", std::to_string(space/deva::process_n).c_str(), 1);

        // disable this disable since the default (16k) is insanely high given our
        // preference towards fat processes.
        //setenv("GASNET_GNI_AM_RVOUS_CUTOVER", "0", /*overwrite=*/0); // disable the scalable algo since we know we have the space
      }
    #endif
    
    int ok;
    gex_Client_t client;
    gex_EP_t endpoint;
    //gex_Segment_t segment;
    
    ok = gex_Client_Init(
      &client, &endpoint, &the_team, "devastator", nullptr, nullptr, 0
    );
    DEVA_ASSERT_ALWAYS(ok == GASNET_OK);

    DEVA_ASSERT_ALWAYS(deva::process_n == gex_TM_QuerySize(the_team));
    deva::process_me_ = gex_TM_QueryRank(the_team);

    if(0) {
      int fd = open(("err."+std::to_string(deva::process_me_)).c_str(), O_CREAT|O_TRUNC|O_RDWR, 0666);
      DEVA_ASSERT(fd >= 0);
      dup2(fd, 2);
    }
    
    gex_AM_Entry_t am_table[] = {
      {id_am_recv, (void(*)())am_recv, GEX_FLAG_AM_MEDIUM | GEX_FLAG_AM_REQUEST, 1, nullptr, "am_recv"}
    };
    ok = gex_EP_RegisterHandlers(endpoint, am_table, sizeof(am_table)/sizeof(am_table[0]));
    DEVA_ASSERT_ALWAYS(ok == GASNET_OK);

    gasnet_barrier_notify(0, GASNET_BARRIERFLAG_ANONYMOUS);
    ok = gasnet_barrier_wait(0, GASNET_BARRIERFLAG_ANONYMOUS);
    DEVA_ASSERT_ALWAYS(ok == GASNET_OK);
  }
}

namespace {
  bool burst_remote_recv(bool deaf) {
    int tme = tmsg::thread_me();
    bool did_something = deva::remote_send_chan_w[tme].cleanup();
    
    if(!deaf) {
      did_something |= deva::remote_recv_chan_r[tme].receive(
        [](tmsg::message *m) {
          auto *ms = static_cast<remote_in_messages*>(m);
          
          serialization_reader r(ms);
          r.unplace(ms->header_size, 1);
          
          int n = ms->count;
          while(n--) {
            r.unplace(0, 8);
            command::execute(r);
          }
        }
      );
    }

    return did_something;
  }
}

void deva::progress(bool spinning, bool deaf) {
  bool did_something = tmsg::progress(deaf);

  did_something |= burst_remote_recv(deaf);
  
  #if GASNET_CONDUIT_SMP || GASNET_CONDUIT_UDP
    static thread_local int nothings = 0;
    
    if(!spinning || did_something)
      nothings = 0;
    else if(++nothings == 10) {
      nothings = 0;
      sched_yield();
    }
  #endif
}

namespace {
  tmsg::barrier_state_global<tmsg::thread_n-1> wbar_g_;
  thread_local tmsg::barrier_state_local<tmsg::thread_n-1> wbar_l_;
  std::atomic<uint64_t> bigbar_epoch_{0};
  
  void barrier_defer_try(uint64_t e) {
    tmsg::send(0, [=]() {
      if(GASNET_OK == gasnet_barrier_try(0, GASNET_BARRIERFLAG_ANONYMOUS))
        bigbar_epoch_.store(e, std::memory_order_release);
      else
        barrier_defer_try(e);
    });
  }
}

void deva::barrier(bool deaf) {
  int wme = tmsg::thread_me() - 1;
  
  wbar_l_.begin(wbar_g_, wme);

  while(!wbar_l_.try_end(wbar_g_, wme))
    deva::progress(/*spinning=*/true, deaf);

  uint64_t e = wbar_l_.epoch64();

  if(wme == 0) {
    tmsg::send(0, [=]() {
      gasnet_barrier_notify(0, GASNET_BARRIERFLAG_ANONYMOUS);
      barrier_defer_try(e);
    });
  }

  while(bigbar_epoch_.load(std::memory_order_relaxed) != e)
    deva::progress(/*spinning=*/true, deaf);
}

void deva::bcast_remote_sends_(int proc_root, void const *cmd, size_t cmd_size) {
  DEVA_ASSERT(tmsg::thread_me() == 0);
  int t_me = 0;
  
  int p_me = process_me_ - proc_root;
  if(p_me < 0) p_me += process_n;

  int p_ub; {
    int p_lb = 0;
    int p_mid = 0;
    p_ub = process_n;

    while(true) {
      if(p_me < p_mid)
        p_ub = p_mid;
      else if(p_me > p_mid)
        p_lb = p_mid;
      else
        break;
      p_mid = p_lb + (p_ub - p_lb)/2;
    }
  }
  
  while(true) {
    int p_mid = p_me + (p_ub - p_me)/2;
    
    // Send-to-self is stop condition.
    if(p_mid == p_me)
      break;

    int proc_mid = proc_root + p_mid;
    if(process_n <= proc_mid) proc_mid -= process_n;
    
    auto *m = remote_out_message::make(-1-proc_mid, cmd, cmd_size);
    remote_send_chan_w[t_me].send(0, m);
    
    p_ub = p_mid;
  }
}

namespace {
  void am_master(gex_Token_t, void *buf, size_t buf_size) {
    serialization_reader r(buf);
    command::execute(r);
  }

  struct alignas(8) am_thread_header {
    #if DEBUG
      uint32_t deadbeef = 0xdeadbeef;
    #endif
    uint16_t thread:14, has_part_head:1, has_part_tail:1;
    uint16_t middle_msg_n;
    int32_t middle_size8;
  };

  struct alignas(8) am_thread_part_header {
    #if DEBUG
      uint32_t deadbeef = 0xdeadbeef;
    #endif
    uint32_t nonce;
    int32_t total_size8;
    int32_t part_size8;
    int32_t offset8;
  };
  
  struct alignas(8) remote_in_chunked_message: remote_in_messages {
    int proc_from;
    uint32_t nonce;
    int thread;
    int32_t waiting_size8;
    
    static tmsg::message*& next_of(tmsg::message *me) {
      return me->next;
    }
    static pair<int,uint32_t> key_of(tmsg::message *me0) {
      auto *me = static_cast<remote_in_chunked_message*>(me0);
      return {me->proc_from, me->nonce};
    }
    static size_t hash_of(pair<int,uint32_t> const &key) {
      return size_t(key.first)*0xdeadbeef + key.second;
    }
  };

  deva::intrusive_map<
      tmsg::message, pair<int,uint32_t>,
      remote_in_chunked_message::next_of,
      remote_in_chunked_message::key_of,
      remote_in_chunked_message::hash_of>
    chunked_by_key;
  
  void recv_part(int proc_from, int thread, upcxx::detail::serialization_reader &r) {
    am_thread_part_header hdr = r.template pop_trivial<am_thread_part_header>();
    DEVA_ASSERT(hdr.deadbeef == 0xdeadbeef);
    
    chunked_by_key.visit(
      /*key*/{proc_from, hdr.nonce},
      [&](tmsg::message *m0) {
        auto *m = static_cast<remote_in_chunked_message*>(m0);
        size_t part_size = 8*size_t(hdr.part_size8);
        size_t total_size = 8*size_t(hdr.total_size8);
        size_t offset = 8*size_t(hdr.offset8);
        
        if(m == nullptr) {
          void *mem = operator new(sizeof(remote_in_chunked_message) + total_size);
          m = new(mem) remote_in_chunked_message;
          DEVA_ASSERT((void*)m == mem);
          
          m->header_size = sizeof(remote_in_chunked_message);
          m->count = 1;
          
          m->proc_from = proc_from;
          m->nonce = hdr.nonce;
          m->thread = thread;
          m->waiting_size8 = hdr.total_size8;
        }
        
        std::memcpy((char*)(m+1) + offset, r.unplace(part_size, 8), part_size);

        m->waiting_size8 -= hdr.part_size8;
        
        if(m->waiting_size8 == 0) {
          deva::remote_recv_chan_w.send(thread, m);
          m = nullptr; // removes m from table
        }
        return m;
      }
    );
  }
  
  void am_recv(gex_Token_t tok, void *buf, size_t buf_size, gex_AM_Arg_t thread_popn) {
    int proc_from; {
      gex_Token_Info_t info;
      gex_Token_Info(tok, &info, GEX_TI_SRCRANK);
      proc_from = info.gex_srcrank;
    }
    
    DEVA_ASSERT(0 <= thread_popn && thread_popn <= tmsg::thread_n);
    
    upcxx::detail::serialization_reader r(buf);
    
    while(thread_popn--) {
      am_thread_header hdr = r.pop_trivial<am_thread_header>();
      DEVA_ASSERT(hdr.deadbeef == 0xdeadbeef);
      
      if(hdr.has_part_head)
        recv_part(proc_from, hdr.thread, r);

      if(hdr.middle_msg_n != 0) {
        size_t size = 8*size_t(hdr.middle_size8);
        
        auto ub = upcxx::storage_size_of<remote_in_messages>()
                  .cat(size, 8);
        
        void *buf = operator new(ub.size);
        upcxx::detail::serialization_writer</*bounded=*/true> w(buf);

        auto *m = w.place_new<remote_in_messages>();
        m->header_size = sizeof(remote_in_messages);
        m->count = hdr.middle_msg_n;

        std::memcpy(w.place(size, 8), r.unplace(size, 8), size);
        //say()<<"rrecv send w="<<thread<<" mn="<<msg_n;
        deva::remote_recv_chan_w.send(hdr.thread, m);
      }

      if(hdr.has_part_tail)
        recv_part(proc_from, hdr.thread, r);
    }
  }

  void master_pump() {
    struct bundle {
      int next = -2; // -2=not in list, -1=none, 0 <= table index
      int thread_popn = 0;
      size_t size8 = 0;

      // if of[t].offset8 != 0 then the head message has been partially sent that much.
      struct thread_t {
        // messages are in a singly-linked (using remote_out_message::bundle_next) circular list.
        remote_out_message *tail;
        int32_t offset8;
        uint32_t nonce;
      } of[tmsg::thread_n] = {/*{nullptr,0,0}...*/};
    };
    
    std::unique_ptr<bundle[]> bun_table{ new bundle[deva::process_n] };
    int bun_head = -1;
    
    uint32_t nonce_bump = 0;
    
    while(!leave_pump.load(std::memory_order_relaxed)) {
      gasnet_AMPoll();
      
      bool did_something = tmsg::progress();
      
      did_something |= deva::remote_recv_chan_w.cleanup();
      
      did_something |= burst_remote_recv(false);
      
      did_something |= deva::remote_send_chan_r.receive_batch(
        // lambda called to receive each message
        [&](tmsg::message *m) {
          auto *rm = static_cast<remote_out_message*>(m);
          
          int p, t;
          if(rm->rank >= 0) {
            p = rm->rank / deva::worker_n;
            t = 1 + (rm->rank % deva::worker_n);
          }
          else {
            p = -(rm->rank + 1);
            t = 0;
          }
          
          //say()<<"rsend to p="<<p<<" w="<<w<<" sz="<<rm->size8;

          bundle *bun = &bun_table[p];
          bun->size8 += rm->size8;
          
          if(bun->of[t].tail == nullptr) {
            bun->thread_popn += 1;
            rm->bundle_next = rm;
            bun->of[t].tail = rm;
          }
          else {
            rm->bundle_next = bun->of[t].tail->bundle_next;
            bun->of[t].tail->bundle_next = rm;
            bun->of[t].tail = rm;
          }
          
          if(bun->next == -2) { // not in list
            bun->next = bun_head;
            bun_head = p;
          }
        },
        // lambda called once after batch of receival lambdas
        [&]() {
          while(bun_head != -1) {
            int proc = bun_head;
            bun_head = -1;
            
            while(true) {
              bundle *bun = &bun_table[proc];
              int proc_next = bun->next;
              
              gex_AM_SrcDesc_t sd = gex_AM_PrepareRequestMedium(
                the_team, /*rank*/proc,
                /*client_buf*/nullptr,
                /*min_length*/0,
                /*max_length*/bun->thread_popn*(2*sizeof(am_thread_part_header) + sizeof(am_thread_header)) + 8*bun->size8,
                /*lc_opt*/nullptr,
                /*flags*/GEX_FLAG_IMMEDIATE,
                /*numargs*/1);

              if(sd != GEX_AM_SRCDESC_NO_OP) {
                void *am_buf = gex_AM_SrcDescAddr(sd);
                size_t am_len = gex_AM_SrcDescSize(sd);
                
                #if 0 && GASNET_CONDUIT_ARIES // no longer needed
                  #warning "GASNet-Ex AM workaround in effect."
                  am_len = std::min<size_t>(GASNETC_GNI_MAX_MEDIUM, am_len);
                #endif
                
                upcxx::detail::serialization_writer</*bounded=*/true> w(am_buf);
                int thread_popn_sent = 0;
                
                for(int t=0; t < tmsg::thread_n; t++) {
                  remote_out_message *rm_tail = bun->of[t].tail;

                  if(rm_tail != nullptr) {
                    remote_out_message *rm = rm_tail->bundle_next;
                    
                    upcxx::storage_size<> laytmp = {w.size(), w.align()};
                    laytmp = laytmp.cat_size_of<am_thread_header>();
                    if(laytmp.size >= am_len) goto am_full;
                    
                    thread_popn_sent += 1;

                    auto *hdr = w.place_new<am_thread_header>();
                    hdr->thread = t;
                    hdr->has_part_head = 0;
                    hdr->has_part_tail = 0;
                    hdr->middle_msg_n = 0;
                    hdr->middle_size8 = 0;
                    
                    if(bun->of[t].offset8 != 0) {
                      int32_t offset8 = bun->of[t].offset8;

                      laytmp = laytmp.cat_size_of<am_thread_part_header>();
                      if(laytmp.size >= am_len) goto am_full;

                      hdr->has_part_head = 1;
                      auto *part = w.place_new<am_thread_part_header>();
                      part->nonce = bun->of[t].nonce;
                      part->total_size8 = rm->size8;
                      part->part_size8 = std::min<int32_t>(rm->size8 - offset8, am_len/8 - w.size()/8);
                      part->offset8 = offset8;
                      
                      bun->of[t].offset8 += part->part_size8;
                      bun->size8 -= part->part_size8;
                      
                      std::memcpy(w.place(8*part->part_size8, 8),
                                  (char*)(rm+1) + 8*offset8,
                                  8*part->part_size8);

                      if(bun->of[t].offset8 == rm->size8) {
                        bun->of[t].offset8 = 0;
                        // pop rm from head of list
                        if(rm == rm_tail)
                          rm = nullptr;
                        else {
                          rm = rm->bundle_next;
                          rm_tail->bundle_next = rm;
                        }
                      }
                      else
                        goto am_full;
                    }
                    
                    while(rm != nullptr) {
                    rm_not_null:
                      laytmp = {w.size(), w.align()};
                      laytmp = laytmp.cat(8*size_t(rm->size8), 8);
                      
                      if(uint16_t(hdr->middle_msg_n + 1) == 0)
                        goto am_full;
                      
                      if(laytmp.size > am_len) {
                        laytmp = {w.size(), w.align()};
                        laytmp = laytmp.cat_size_of<am_thread_part_header>();
                        
                        if(am_len >= laytmp.size + 64) {
                          hdr->has_part_tail = 1;
                          auto *part = w.place_new<am_thread_part_header>();
                          part->nonce = nonce_bump++;
                          part->total_size8 = rm->size8;
                          part->part_size8 = am_len/8 - w.size()/8;
                          part->offset8 = 0;

                          bun->of[t].offset8 = part->part_size8;
                          bun->of[t].nonce = part->nonce;
                          bun->size8 -= part->part_size8;
                          DEVA_ASSERT(part->part_size8 < rm->size8, "part->part_size8="<<part->part_size8<<" rm->size8="<<rm->size8);
                          
                          std::memcpy(w.place(8*part->part_size8, 8),
                                      rm + 1,
                                      8*part->part_size8);
                        }

                        goto am_full;
                      }
                      
                      hdr->middle_msg_n += 1;
                      hdr->middle_size8 += rm->size8;
                      bun->size8 -= rm->size8;
                      
                      std::memcpy(w.place(8*rm->size8, 8), rm + 1, 8*rm->size8);

                      // pop rm from head of list
                      if(rm == rm_tail) break;
                      rm = rm->bundle_next;
                      rm_tail->bundle_next = rm;
                      goto rm_not_null;
                    }
                    
                    // depleted all messages to thread
                    bun->of[t].tail = nullptr;
                    bun->thread_popn -= 1;
                  }
                }

                if(true) { // depleted all messages to proc
                  DEVA_ASSERT(bun->size8 == 0);
                  bun->next = -2; // not in list
                }
                else { // messages still remain to proc
                am_full:
                  DEVA_ASSERT(bun->size8 != 0);
                  bun->next = bun_head;
                  bun_head = proc;
                }

                //say()<<"sending AM to "<<proc<<" sz="<<committed_size/8<<" msgs="<<committed_msgs;
                gex_AM_CommitRequestMedium1(sd, id_am_recv, w.size(), thread_popn_sent);

                proc = proc_next;
              }
              
              if(proc == -1)
                break; // all messages in this batch have been sent
              
              gasnet_AMPoll();
            }
          }
        }
      );
      
      static thread_local int consecutive_nothings = 0;

      if(did_something)
        consecutive_nothings = 0;
      else if(++consecutive_nothings == 10) {
        consecutive_nothings = 0;
        sched_yield();
      }
    }

    leave_pump.store(false, std::memory_order_relaxed);
    
    DEVA_ASSERT_ALWAYS(bun_head == -1); // No messages in flight when run() terminates
  }
}
