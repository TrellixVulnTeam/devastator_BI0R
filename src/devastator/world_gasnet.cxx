#include "world_gasnet.hxx"
#include "intrusive_map.hxx"
#include "opnew.hxx"

#include <gasnet.h>

#include <atomic>
#include <cstdlib>
#include <string>
#include <thread>
#include <tuple>

#include <unistd.h>
#include <sched.h>
#include <fcntl.h>

using namespace std;

using world::worker_n;
using world::remote_out_message;

__thread int world::rank_me_ = 0xdeadbeef;

alignas(64)
int world::process_rank_lo_ = 0xdeadbeef;
int world::process_rank_hi_ = 0xdeadbeef;

namespace {
  int process_me;
  
  gex_TM_t the_team;
  
  std::atomic<bool> leave_pump{false};
}

alignas(64)
tmsg::channels_r<worker_n> world::remote_send_chan_r;
tmsg::channels_w<1> world::remote_send_chan_w[worker_n];
tmsg::channels_r<1> world::remote_recv_chan_r[worker_n];
tmsg::channels_w<worker_n> world::remote_recv_chan_w;

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

void world::run(upcxx::function_ref<void()> fn) {
  static bool inited = false;

  if(!inited) {
    inited = true;
    init_gasnet();
    process_rank_lo_ = process_me*worker_n;
    process_rank_hi_ = (process_me+1)*worker_n;
  }
  
  tmsg::run([&]() {
    int tme = tmsg::thread_me();

    static thread_local bool inited = false;
    
    if(!inited) {
      inited = true;
      
      if(tme == 0) {
        for(int w=0; w < worker_n; w++)
          remote_recv_chan_w.connect(w, remote_recv_chan_r[w]);
      }
      else
        remote_send_chan_w[tme-1].connect(0, remote_send_chan_r);

      #if 0
        for(int i=0; i < process_n; i++) {
          if(i == process_me) {
            static std::mutex mu;
            mu.lock();
            std::cerr<<"[pid "<<getpid()<<" t "<<tme<<"] watch *((uintptr_t*)"<<&opnew::my_ts.bins[29].held_pools.top<<")&1\n";
            mu.unlock();
            tmsg::barrier(false);
            if(tme == 0)
              gasnett_freezeForDebuggerErr();
          }
          if(tme==0){
            gasnet_barrier_notify(0,0);
            gasnet_barrier_wait(0,0);
          }
          tmsg::barrier(false);
        }
      #endif
      
      tmsg::barrier(/*do_progress=*/false);
    }

    if(tme == 0) {
      rank_me_ = -process_me - 1;
      master_pump();
    }
    else {
      rank_me_ = process_rank_lo_ + tme-1;
      fn();
      
      world::barrier(/*do_progress=*/false);
      
      if(tme == 1)
        leave_pump.store(true, std::memory_order_release);
    }
  });
}

namespace {
  void init_gasnet() {
    #if GASNET_CONDUIT_SMP
      setenv("GASNET_PSHM_NODES", std::to_string(world::process_n).c_str(), 1);
    #elif GASNET_CONDUIT_ARIES
      // Everyone carves out 1GB and shares it evenly across peers
      setenv("GASNET_NETWORKDEPTH_SPACE", std::to_string((1<<30)/world::process_n).c_str(), 1);
    #endif
    
    int ok;
    gex_Client_t client;
    gex_EP_t endpoint;
    gex_Segment_t segment;
    
    ok = gex_Client_Init(
      &client, &endpoint, &the_team, "devastator", nullptr, nullptr, 0
    );
    ASSERT_ALWAYS(ok == GASNET_OK);

    ASSERT_ALWAYS(world::process_n == gex_TM_QuerySize(the_team));
    process_me = gex_TM_QueryRank(the_team);

    if(0) {
      int fd = open(("err."+std::to_string(process_me)).c_str(), O_CREAT|O_TRUNC|O_RDWR, 0666);
      ASSERT(fd >= 0);
      dup2(fd, 2);
    }
    
    gex_AM_Entry_t am_table[] = {
      {id_am_recv, (void(*)())am_recv, GEX_FLAG_AM_MEDIUM | GEX_FLAG_AM_REQUEST, 1, nullptr, "am_recv"}
    };
    ok = gex_EP_RegisterHandlers(endpoint, am_table, sizeof(am_table)/sizeof(am_table[0]));
    ASSERT_ALWAYS(ok == GASNET_OK);

    gasnet_barrier_notify(0, GASNET_BARRIERFLAG_ANONYMOUS);
    ok = gasnet_barrier_wait(0, GASNET_BARRIERFLAG_ANONYMOUS);
    ASSERT_ALWAYS(ok == GASNET_OK);
  }
}

void world::progress() {
  bool did_something = tmsg::progress_noyield();

  int wme = tmsg::thread_me() - 1;
  did_something |= remote_send_chan_w[wme].cleanup();
  did_something |= remote_recv_chan_r[wme].receive(
    [](tmsg::message *m) {
      auto *ms = static_cast<remote_in_messages*>(m);

      upcxx::parcel_reader r{ms};
      r.pop(ms->header_size, 1);
      
      int n = ms->count;
      while(n--) {
        r.pop(0, 8);
        upcxx::command_execute(r);
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

namespace {
  std::atomic<unsigned> barrier_done{0};
  
  void barrier_defer_try(unsigned done_value) {
    tmsg::send(0, [=]() {
      if(GASNET_OK == gasnet_barrier_try(0, GASNET_BARRIERFLAG_ANONYMOUS))
        barrier_done.store(done_value, std::memory_order_release);
      else
        barrier_defer_try(done_value);
    });
  }
}

void world::barrier(bool do_progress) {
  static std::atomic<int> c[2]{{0}, {0}};
  static thread_local unsigned epoch = 0;
  
  int bump = epoch & 2 ? -1 : 1;
  int end = epoch & 2 ? 0 : worker_n;
  
  if(c[epoch & 1].fetch_add(bump) + bump != end) {
    while(c[epoch & 1].load(std::memory_order_acquire) != end)
      world::progress();
  }
  else {
    unsigned epoch1 = epoch + 1;
    tmsg::send(0, [=]() {
      gasnet_barrier_notify(0, GASNET_BARRIERFLAG_ANONYMOUS);
      barrier_defer_try(epoch1);
    });
  }

  while(barrier_done.load(std::memory_order_acquire) != epoch+1) {
    if(do_progress)
      world::progress();
    else
      sched_yield();
  }

  epoch += 1;
}

namespace {
  void am_master(gex_Token_t, void *buf, size_t buf_size) {
    upcxx::parcel_reader r{buf};
    upcxx::command_execute(r);
  }

  struct alignas(8) am_worker_header {
    uint16_t worker:14, has_part_head:1, has_part_tail:1;
    uint16_t middle_msg_n;
    int32_t middle_size8;
  };

  struct alignas(8) am_worker_part_header {
    uint32_t nonce;
    int32_t total_size8;
    int32_t part_size8;
    int32_t offset8;
  };
  
  struct alignas(8) remote_in_chunked_message: remote_in_messages {
    int proc_from;
    uint32_t nonce;
    int worker;
    int32_t waiting_size8;
    
    static remote_in_chunked_message*& next_of(remote_in_chunked_message *me) {
      return *reinterpret_cast<remote_in_chunked_message**>(&me->next);
    }
    static pair<int,uint32_t> key_of(remote_in_chunked_message *me) {
      return {me->proc_from, me->nonce};
    }
    static size_t hash_of(pair<int,uint32_t> const &key) {
      return size_t(key.first)*0xdeadbeef + key.second;
    }
  };

  intrusive_map<
      remote_in_chunked_message, pair<int,uint32_t>,
      remote_in_chunked_message::next_of,
      remote_in_chunked_message::key_of,
      remote_in_chunked_message::hash_of>
    chunked_by_key;
  
  void recv_part(int proc_from, int worker, upcxx::parcel_reader &r) {
    am_worker_part_header hdr = r.pop_trivial_aligned<am_worker_part_header>();

    chunked_by_key.visit(
      /*key*/{proc_from, hdr.nonce},
      [&](remote_in_chunked_message *m) {
        size_t part_size = 8*size_t(hdr.part_size8);
        size_t total_size = 8*size_t(hdr.total_size8);
        size_t offset = 8*size_t(hdr.offset8);
        
        if(m == nullptr) {
          void *mem = operator new(sizeof(remote_in_chunked_message) + total_size);
          m = new(mem) remote_in_chunked_message;
          ASSERT((void*)m == mem);
          
          m->header_size = sizeof(remote_in_chunked_message);
          m->count = 1;
          
          m->proc_from = proc_from;
          m->nonce = hdr.nonce;
          m->worker = worker;
          m->waiting_size8 = hdr.total_size8;
        }
        
        std::memcpy((char*)(m+1) + offset, r.pop(part_size, 8), part_size);

        m->waiting_size8 -= hdr.part_size8;
        
        if(m->waiting_size8 == 0) {
          world::remote_recv_chan_w.send(worker, m);
          m = nullptr; // removes m from table
        }
        return m;
      }
    );
  }
  
  void am_recv(gex_Token_t tok, void *buf, size_t buf_size, gex_AM_Arg_t worker_n) {
    int proc_from; {
      gex_Token_Info_t info;
      gex_Token_Info(tok, &info, GEX_TI_SRCRANK);
      proc_from = info.gex_srcrank;
    }
    
    upcxx::parcel_reader r{buf};
    
    while(worker_n--) {
      am_worker_header hdr = r.pop_trivial_aligned<am_worker_header>();
      
      if(hdr.has_part_head)
        recv_part(proc_from, hdr.worker, r);

      if(hdr.middle_msg_n != 0) {
        size_t size = 8*size_t(hdr.middle_size8);
        
        upcxx::parcel_layout ub;
        ub.add_trivial_aligned<remote_in_messages>();
        ub.add_bytes(size, 8);
        
        void *buf = operator new(ub.size());
        upcxx::parcel_writer w{buf};
        remote_in_messages *m = w.put_trivial_aligned<remote_in_messages>({});

        m->header_size = sizeof(remote_in_messages);
        m->count = hdr.middle_msg_n;
        std::memcpy(w.put(size, 8), r.pop(size, 8), size);

        //say()<<"rrecv send w="<<worker<<" mn="<<msg_n;
        world::remote_recv_chan_w.send(hdr.worker, m);
      }

      if(hdr.has_part_tail)
        recv_part(proc_from, hdr.worker, r);
    }
  }

  void master_pump() {
    struct bundle {
      int next = -2; // -2=not in list, -1=none, 0 <= table index
      int workers_present = 0;
      size_t size8 = 0;

      // if of[w].offset8 != 0 then the head message has been partially sent that much.
      struct worker_t {
        // messages are in a singly-linked (using remote_out_message::bundle_next) circular list.
        remote_out_message *tail;
        int32_t offset8;
        uint32_t nonce;
      } of[world::worker_n] = {/*{nullptr,0,0}...*/};
    };
    
    std::unique_ptr<bundle[]> bun_table{ new bundle[world::process_n] };
    int bun_head = -1;

    uint32_t nonce_bump = 0;
    
    while(!leave_pump.load(std::memory_order_relaxed)) {
      gasnet_AMPoll();
      
      bool did_something = tmsg::progress_noyield();
      
      did_something |= world::remote_recv_chan_w.cleanup();

      // Non-bundling algorithm. One message from a worker = one AM.
      #if 0
        #error "Not updated to use new partial-message format"
        
        did_something |= world::remote_send_chan_r.receive(
          [&](tmsg::message *m) {
            auto *rm = static_cast<remote_out_message*>(m);
            int proc = rm->rank / world::worker_n;
            int wrkr = rm->rank % world::worker_n;

            alignas(8) char buf[16<<10];
            upcxx::parcel_writer w{buf};
            w.put_trivial_aligned<uint16_t>(wrkr);
            w.put_trivial_aligned<uint16_t>(1);
            w.put_trivial_aligned<int32_t>(rm->size8);
            std::memcpy(w.put(8*rm->size8, 8), rm+1, 8*rm->size8);
            
            //say()<<"gex_AM_RequestMedium1";
            gex_AM_RequestMedium1(
              the_team, proc,
              id_am_worker, buf, w.size(),
              GEX_EVENT_NOW, /*flags*/0,
              /*rank_n*/1
            );
          }
        );
      // Bundling algorithm.
      #else 
        did_something |= world::remote_send_chan_r.receive_batch(
          // lambda called to receive each message
          [&](tmsg::message *m) {
            auto *rm = static_cast<remote_out_message*>(m);
            int p = rm->rank / world::worker_n;
            int w = rm->rank % world::worker_n;
            //say()<<"rsend to p="<<p<<" w="<<w<<" sz="<<rm->size8;

            bundle *bun = &bun_table[p];
            bun->size8 += rm->size8;
            
            if(bun->of[w].tail == nullptr) {
              bun->workers_present += 1;

              rm->bundle_next = rm;
              bun->of[w].tail = rm;
            }
            else {
              rm->bundle_next = bun->of[w].tail->bundle_next;
              bun->of[w].tail->bundle_next = rm;
              bun->of[w].tail = rm;
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
                  /*max_length*/bun->workers_present*(2*sizeof(am_worker_part_header) + sizeof(am_worker_header)) + 8*bun->size8,
                  /*lc_opt*/nullptr,
                  /*flags*/GEX_FLAG_IMMEDIATE,
                  /*numargs*/1);

                if(sd != GEX_AM_SRCDESC_NO_OP) {
                  void *am_buf = gex_AM_SrcDescAddr(sd);
                  size_t am_len = gex_AM_SrcDescSize(sd);

                  upcxx::parcel_writer w{am_buf};
                  int committed_workers = 0;
                  
                  for(int worker=0; worker < world::worker_n; worker++) {
                    remote_out_message *rm_tail = bun->of[worker].tail;

                    if(rm_tail != nullptr) {
                      remote_out_message *rm = rm_tail->bundle_next;
                      
                      upcxx::parcel_layout laytmp = w.layout();
                      laytmp.add_trivial_aligned<am_worker_header>();
                      if(laytmp.size() >= am_len) goto am_full;
                      
                      committed_workers += 1;

                      auto *hdr = w.put_trivial_aligned<am_worker_header>({});
                      hdr->worker = worker;
                      hdr->has_part_head = 0;
                      hdr->has_part_tail = 0;
                      hdr->middle_msg_n = 0;
                      hdr->middle_size8 = 0;
                      
                      if(bun->of[worker].offset8 != 0) {
                        int32_t offset8 = bun->of[worker].offset8;

                        laytmp.add_trivial_aligned<am_worker_part_header>();
                        if(laytmp.size() >= am_len) goto am_full;

                        hdr->has_part_head = 1;
                        auto *part = w.put_trivial_aligned<am_worker_part_header>({});
                        part->nonce = bun->of[worker].nonce;
                        part->total_size8 = rm->size8;
                        part->part_size8 = std::min<int32_t>(rm->size8 - offset8, am_len/8 - w.size()/8);
                        part->offset8 = offset8;
                        
                        bun->of[worker].offset8 += part->part_size8;
                        bun->size8 -= part->part_size8;
                        
                        std::memcpy(w.put(8*part->part_size8, 8), (char*)(rm+1) + 8*offset8, 8*part->part_size8);

                        if(bun->of[worker].offset8 == rm->size8) {
                          bun->of[worker].offset8 = 0;
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
                        laytmp = w.layout();
                        laytmp.add_bytes(8*rm->size8, 8);
                        
                        if(uint16_t(hdr->middle_msg_n + 1) == 0)
                          goto am_full;
                        
                        if(laytmp.size() > am_len) {
                          laytmp = w.layout();
                          laytmp.add_trivial_aligned<am_worker_part_header>();
                          
                          if(am_len >= laytmp.size() + 64) {
                            hdr->has_part_tail = 1;
                            auto *part = w.put_trivial_aligned<am_worker_part_header>({});
                            part->nonce = nonce_bump++;
                            part->total_size8 = rm->size8;
                            part->part_size8 = am_len/8 - w.size()/8;
                            part->offset8 = 0;

                            bun->of[worker].offset8 = part->part_size8;
                            bun->of[worker].nonce = part->nonce;
                            bun->size8 -= part->part_size8;
                            ASSERT(part->part_size8 < rm->size8);
                            
                            std::memcpy(w.put(8*part->part_size8, 8), rm + 1, 8*part->part_size8);
                          }

                          goto am_full;
                        }
                        
                        hdr->middle_msg_n += 1;
                        hdr->middle_size8 += rm->size8;
                        bun->size8 -= rm->size8;
                        
                        std::memcpy(w.put(8*rm->size8, 8), rm + 1, 8*rm->size8);

                        // pop rm from head of list
                        if(rm == rm_tail) break;
                        rm = rm->bundle_next;
                        rm_tail->bundle_next = rm;
                        goto rm_not_null;
                      }
                      
                      // depleted all messages to worker
                      bun->of[worker].tail = nullptr;
                      bun->workers_present -= 1;
                    }
                  }

                  if(true) { // depleted all messages to proc
                    ASSERT(bun->size8 == 0);
                    bun->next = -2; // not in list
                  }
                  else { // messages still remain to proc
                  am_full:
                    ASSERT(bun->size8 != 0);
                    bun->next = bun_head;
                    bun_head = proc;
                  }

                  //say()<<"sending AM to "<<proc<<" sz="<<committed_size/8<<" msgs="<<committed_msgs;
                  gex_AM_CommitRequestMedium1(sd, id_am_recv, w.size(), committed_workers);

                  proc = proc_next;
                }
                
                if(proc == -1)
                  break; // all messages in this batch have been sent
                
                gasnet_AMPoll();
              }
            }
          }
        );
      #endif
      
      static thread_local int consecutive_nothings = 0;

      if(did_something)
        consecutive_nothings = 0;
      else if(++consecutive_nothings == 10) {
        consecutive_nothings = 0;
        sched_yield();
      }
    }

    leave_pump.store(false, std::memory_order_relaxed);
    
    ASSERT_ALWAYS(bun_head == -1); // No messages in flight when run() terminates
  }
}
