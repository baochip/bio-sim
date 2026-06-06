// bio-sim : Verilator harness for the bio_bdma block
// ---------------------------------------------------------------------------
// DUT  : bio_bdma_wrapper  (flattened Verilog ports; built with +define+SIM)
// Model: one fast clock `fclk` (fanned to aclk/hclk/dmaclk inside the wrapper)
//        plus `pclk`, derived by integer division of fclk.
//
// COMMAND-EXECUTOR MODEL
// ----------------------
// The run is an ordered list of COMMANDS executed against the sim. A JSON
// config is just a batch script of commands; later, a socket can feed the
// same commands live. The two front-ends share one execution path.
//
// Back-compat: the original flat schema (firmware/registers/start/run/monitor)
// is desugared into the same command list, so existing configs run unchanged.
//
// Commands:
//   load       {core, bin}                       load firmware into a core IMEM
//   poke       {name|offset, value, port?}        APB write (register)
//   peek       {name|offset, port?}               APB read (logged)
//   fifo_write {bank, data:[...]|value, via?}      push word(s) to a TX FIFO
//   fifo_read  {bank, count?, via?}                pop word(s) from an RX FIFO
//   start      {cores:[..], restart?, clkdiv_restart?}   compose+write sfr_ctrl
//   monitor    {signal, bit?}                      watch a top-level signal edge
//   inject     {events:[{cycle,pin,value}...]}      schedule timestamped pin events
//   run|delay  {cycles, stop_on_trap?}             advance N fclk cycles
//
// "via" selects the FIFO path: "sfr" (default, main port) or "alias" (the
// per-bank apbs_fifo page). The register OFFSET is identical either way; only
// the port differs (alias bank n -> port fifo{n}).
//
// The host CPU is NOT modelled; everything it does is APB on the slave ports.
// No RTL is modified.
// ---------------------------------------------------------------------------

#include "Vbio_bdma_wrapper.h"
#include "verilated.h"
#include "verilated_fst_c.h"
#include "json.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

// real-time serve mode (POSIX sockets + a reader thread)
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <deque>
#include <mutex>
#include <sstream>
#include <thread>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

using json = nlohmann::json;

// Set by SIGINT so the serve loop unwinds to the normal FST-close path instead
// of the process being killed mid-trace.
static std::atomic<bool> g_stop{false};
static void on_sigint(int) { g_stop = true; }

// mkdir -p for a (possibly nested) directory path; ignores already-exists.
static void mkdir_p(const std::string& dir) {
    std::string acc;
    for (size_t i = 0; i < dir.size(); ++i) {
        acc.push_back(dir[i]);
        if (dir[i] == '/' || i + 1 == dir.size())
            if (!acc.empty() && acc != "/") ::mkdir(acc.c_str(), 0777);
    }
}

// --------------------------------------------------------------------------
// SFR name -> byte offset on the MAIN APB port. readonly registers warn on poke.
// --------------------------------------------------------------------------
struct SfrInfo { uint32_t offset; int width; bool readonly; };

static const std::map<std::string, SfrInfo> SFR = {
    {"sfr_ctrl",{0x00,12,false}}, {"sfr_cfginfo",{0x04,32,true}},
    {"sfr_config",{0x08,10,false}}, {"sfr_flevel",{0x0C,16,true}},
    {"sfr_txf0",{0x10,32,false}}, {"sfr_txf1",{0x14,32,false}},
    {"sfr_txf2",{0x18,32,false}}, {"sfr_txf3",{0x1C,32,false}},
    {"sfr_rxf0",{0x20,32,true}}, {"sfr_rxf1",{0x24,32,true}},
    {"sfr_rxf2",{0x28,32,true}}, {"sfr_rxf3",{0x2C,32,true}},
    {"sfr_elevel",{0x30,32,false}}, {"sfr_etype",{0x34,24,false}},
    {"sfr_event_set",{0x38,24,false}}, {"sfr_event_clr",{0x3C,24,false}},
    {"sfr_event_status",{0x40,32,true}}, {"sfr_extclock",{0x44,24,false}},
    {"sfr_fifo_clr",{0x48,4,false}},
    {"sfr_qdiv0",{0x50,32,false}}, {"sfr_qdiv1",{0x54,32,false}},
    {"sfr_qdiv2",{0x58,32,false}}, {"sfr_qdiv3",{0x5C,32,false}},
    {"sfr_sync_bypass",{0x60,32,false}}, {"sfr_io_oe_inv",{0x64,32,false}},
    {"sfr_io_o_inv",{0x68,32,false}}, {"sfr_io_i_inv",{0x6C,32,false}},
    {"sfr_irqmask_0",{0x70,32,false}}, {"sfr_irqmask_1",{0x74,32,false}},
    {"sfr_irqmask_2",{0x78,32,false}}, {"sfr_irqmask_3",{0x7C,32,false}},
    {"sfr_irq_edge",{0x80,4,false}}, {"sfr_dbg_padout",{0x84,32,true}},
    {"sfr_dbg_padoe",{0x88,32,true}},
    {"sfr_dbg0",{0x90,13,true}}, {"sfr_dbg1",{0x94,13,true}},
    {"sfr_dbg2",{0x98,13,true}}, {"sfr_dbg3",{0x9C,13,true}},
    {"sfr_mem_gutter",{0xA0,32,false}}, {"sfr_peri_gutter",{0xA4,32,false}},
    {"sfr_dmareq_map",{0xB0,32,false}},
    {"sfr_filter_base_0",{0xE0,20,false}}, {"sfr_filter_bounds_0",{0xE4,20,false}},
    {"sfr_filter_base_1",{0xE8,20,false}}, {"sfr_filter_bounds_1",{0xEC,20,false}},
    {"sfr_filter_base_2",{0xF0,20,false}}, {"sfr_filter_bounds_2",{0xF4,20,false}},
    {"sfr_filter_base_3",{0xF8,20,false}}, {"sfr_filter_bounds_3",{0xFC,20,false}},
};

static const uint32_t CFGINFO_EXPECT = (4096u << 16) | (4u << 8) | 8u; // 0x10000408

// --------------------------------------------------------------------------
struct ApbPort {
    std::function<void(uint32_t)> set_paddr, set_pwdata;
    std::function<void(uint8_t)>  set_pstrb, set_pprot, set_psel,
                                  set_penable, set_pwrite, set_apbactive;
    std::function<uint32_t()>     get_prdata;
    std::function<uint8_t()>      get_pready;
};

#define MK_APB_PORT(PFX)                                                       \
    ApbPort{                                                                   \
        [&](uint32_t v){ dut->PFX##PADDR     = v; },                           \
        [&](uint32_t v){ dut->PFX##PWDATA    = v; },                           \
        [&](uint8_t  v){ dut->PFX##PSTRB     = v; },                           \
        [&](uint8_t  v){ dut->PFX##PPROT     = v; },                           \
        [&](uint8_t  v){ dut->PFX##PSEL      = v; },                           \
        [&](uint8_t  v){ dut->PFX##PENABLE   = v; },                           \
        [&](uint8_t  v){ dut->PFX##PWRITE    = v; },                           \
        [&](uint8_t  v){ dut->PFX##APBACTIVE = v; },                           \
        [&](){ return (uint32_t)dut->PFX##PRDATA; },                           \
        [&](){ return (uint8_t) dut->PFX##PREADY; }                            \
    }

// --------------------------------------------------------------------------
// The simulation engine: clocks, APB transactor, trace, monitors, injection.
// --------------------------------------------------------------------------
class Sim {
public:
    Vbio_bdma_wrapper* dut;
    VerilatedFstC*     tfp = nullptr;
    VerilatedContext*  ctx;

    uint64_t time_ps = 0, half_count = 0, cycle = 0;
    uint32_t fclk_half_ps, pclk_div;
    uint64_t fclk_hz;
    bool     tracing = false;

    std::map<std::string, ApbPort> ports;

    struct Monitor { std::string name; int bit; bool has_bit;
                     std::function<uint32_t()> get; uint32_t last; };
    std::vector<Monitor> monitors;

    struct Inject { uint64_t at_cycle; int pin; int value; };
    std::vector<Inject> injects;
    size_t inject_cursor = 0;
    bool   inject_dirty  = false;

    Sim(VerilatedContext* c, double fclk_mhz) : ctx(c) {
        dut = new Vbio_bdma_wrapper{c};
        double period_ps = 1.0e6 / fclk_mhz;
        fclk_half_ps = (uint32_t)(period_ps / 2.0 + 0.5);
        if (!fclk_half_ps) fclk_half_ps = 1;
        pclk_div = (fclk_mhz >= 700.0) ? 16 : 8;
        fclk_hz  = (uint64_t)(fclk_mhz * 1.0e6 + 0.5);
        bind_ports(); idle_all_apb(); benign_dma_inputs();
        dut->cmatpg=0; dut->cmbist=0; dut->sramtrm=0;
        dut->fclk=0; dut->pclk=0; dut->hclk=0; dut->resetn=0; dut->gpio_in=0;
    }
    ~Sim() { if (tfp) { tfp->close(); delete tfp; } delete dut; }

    void open_trace(const std::string& p) {
        // Bare filenames go under waveform/; explicit paths are honored as-is.
        std::string out = p;
        if (out.find('/') == std::string::npos) out = "waveform/" + out;
        auto slash = out.find_last_of('/');
        if (slash != std::string::npos) mkdir_p(out.substr(0, slash));
        Verilated::traceEverOn(true);
        tfp = new VerilatedFstC; dut->trace(tfp, 99); tfp->open(out.c_str());
        tracing = true;
        printf("[trace] writing %s\n", out.c_str());
    }

    void bind_ports() {
        ports["sfr"]=MK_APB_PORT();
        ports["imem0"]=MK_APB_PORT(IM0_); ports["imem1"]=MK_APB_PORT(IM1_);
        ports["imem2"]=MK_APB_PORT(IM2_); ports["imem3"]=MK_APB_PORT(IM3_);
        ports["fifo0"]=MK_APB_PORT(FP0_); ports["fifo1"]=MK_APB_PORT(FP1_);
        ports["fifo2"]=MK_APB_PORT(FP2_); ports["fifo3"]=MK_APB_PORT(FP3_);
    }
    void idle_all_apb() {
        for (auto& kv : ports) { ApbPort& p=kv.second;
            p.set_psel(0); p.set_penable(0); p.set_pwrite(0); p.set_paddr(0);
            p.set_pwdata(0); p.set_pstrb(0); p.set_pprot(0); p.set_apbactive(0); }
    }
    void benign_dma_inputs() {
        dut->aw_ready=1; dut->ar_ready=1; dut->w_ready=1;
        dut->r_valid=0; dut->r_data=0; dut->r_resp=0; dut->r_last=0;
        dut->r_id=0; dut->r_user=0; dut->b_valid=0; dut->b_resp=0;
        dut->b_id=0; dut->b_user=0; dut->hrdata=0; dut->hready=1;
        dut->hresp=0; dut->hruser=0; dut->hreadym=1;
    }

    // FIFO register offset is identical on the main port and the alias page.
    static uint32_t fifo_off(int bank, bool tx) { return (tx?0x10u:0x20u)+4u*bank; }

    // ---- time stepping ----
    void eval_dump() { dut->eval(); if (tracing) tfp->dump(time_ps); }
    void half_step() {
        time_ps += fclk_half_ps;
        dut->fclk = !dut->fclk; dut->hclk = dut->fclk;
        if (++half_count % pclk_div == 0) dut->pclk = !dut->pclk;
        eval_dump();
    }
    void advance_to_pclk_posedge() {
        uint8_t prev = dut->pclk;
        for (;;) { half_step(); if (prev==0 && dut->pclk==1) return; prev=dut->pclk; }
    }
    void run_fclk_cycles(uint64_t n) { for (uint64_t i=0;i<2*n;++i) half_step(); }

    void reset(int pclk_cycles=16) {
        dut->resetn=0; for(int i=0;i<pclk_cycles;++i) advance_to_pclk_posedge();
        dut->resetn=1; for(int i=0;i<pclk_cycles;++i) advance_to_pclk_posedge();
    }

    // ---- APB3 transaction (pclk domain) ----
    uint32_t apb_xact(ApbPort& p, uint32_t addr, uint32_t data, bool write) {
        advance_to_pclk_posedge();
        p.set_psel(1); p.set_penable(0); p.set_pwrite(write?1:0);
        p.set_paddr(addr); p.set_pwdata(write?data:0);
        p.set_pstrb(write?0xF:0x0); p.set_pprot(0); p.set_apbactive(1);
        advance_to_pclk_posedge(); p.set_penable(1);
        uint32_t rdata=0; int guard=0;
        for (;;) {
            advance_to_pclk_posedge();
            if (p.get_pready()) { rdata=p.get_prdata(); break; }
            if (++guard>4096) { fprintf(stderr,"[apb] TIMEOUT @0x%03x\n",addr); break; }
        }
        p.set_psel(0); p.set_penable(0); p.set_pwrite(0); p.set_apbactive(0);
        advance_to_pclk_posedge();
        return rdata;
    }
    void     apb_write(ApbPort& p, uint32_t a, uint32_t d) { apb_xact(p,a,d,true); }
    uint32_t apb_read (ApbPort& p, uint32_t a)             { return apb_xact(p,a,0,false); }

    // ---- monitors ----
    void add_monitor(const std::string& name, int bit, bool has_bit) {
        std::function<uint32_t()> g;
        if      (name=="gpio_out") g=[this]{return (uint32_t)dut->gpio_out;};
        else if (name=="gpio_in")  g=[this]{return (uint32_t)dut->gpio_in; };
        else if (name=="gpio_dir") g=[this]{return (uint32_t)dut->gpio_dir;};
        else if (name=="irq")      g=[this]{return (uint32_t)dut->irq;     };
        else { fprintf(stderr,"[mon] WARN unknown signal '%s'\n",name.c_str()); return; }
        monitors.push_back({name,bit,has_bit,g,g()});
        printf("[mon] watching %s%s\n", name.c_str(),
               has_bit ? (" bit "+std::to_string(bit)).c_str() : "");
    }
    // optional sink for machine-readable event lines (set during serve mode)
    std::function<void(const std::string&)> event_sink;

    void sample_monitors() {
        for (auto& m : monitors) {
            uint32_t cur=m.get();
            if (m.has_bit) {
                uint32_t a=(m.last>>m.bit)&1, b=(cur>>m.bit)&1;
                if (a!=b) {
                    printf("[mon] cyc=%llu (%llu ps)  %s[%d]: %u -> %u\n",
                        (unsigned long long)cycle,(unsigned long long)time_ps,
                        m.name.c_str(),m.bit,a,b);
                    if (event_sink) {
                        char e[96]; snprintf(e,sizeof e,"evt %llu %s %d %u",
                            (unsigned long long)cycle,m.name.c_str(),m.bit,b);
                        event_sink(e);
                    }
                }
            } else if (cur!=m.last) {
                printf("[mon] cyc=%llu (%llu ps)  %s: 0x%08x -> 0x%08x\n",
                    (unsigned long long)cycle,(unsigned long long)time_ps,
                    m.name.c_str(),m.last,cur);
                if (event_sink) {
                    char e[96]; snprintf(e,sizeof e,"evt %llu %s * 0x%08x",
                        (unsigned long long)cycle,m.name.c_str(),cur);
                    event_sink(e);
                }
            }
            m.last=cur;
        }
    }

    // ---- injection ----
    void set_gpio_in_bit(int pin, int val) {
        uint32_t g=dut->gpio_in;
        if (val) g|=(1u<<pin); else g&=~(1u<<pin);
        dut->gpio_in=g;
    }
    void schedule_inject(uint64_t rel_cycle, int pin, int val) {
        injects.push_back({cycle+rel_cycle, pin, val});
        inject_dirty = true;        // sort lazily before the next application
    }
    void apply_due_injects() {
        if (inject_dirty) {
            // Only the not-yet-applied tail can be out of order; newly scheduled
            // events always have at_cycle >= current cycle >= everything applied.
            std::stable_sort(injects.begin()+inject_cursor, injects.end(),
                [](const Inject&a,const Inject&b){return a.at_cycle<b.at_cycle;});
            inject_dirty = false;
        }
        while (inject_cursor < injects.size() && injects[inject_cursor].at_cycle <= cycle) {
            const Inject& e = injects[inject_cursor++];
            set_gpio_in_bit(e.pin, e.value);
        }
    }

    // one fclk cycle, with injection applied first and monitors sampled after
    void tick() { apply_due_injects(); half_step(); half_step(); cycle++; sample_monitors(); }

    void draw_bar(uint64_t done, uint64_t total,
                  std::chrono::steady_clock::time_point t0) {
        double frac = total ? (double)done/(double)total : 0.0; if (frac>1) frac=1;
        int W=30, fill=(int)(frac*W);
        double secs=std::chrono::duration<double>(std::chrono::steady_clock::now()-t0).count();
        double rate=secs>0?done/secs:0, eta=rate>0?(total-done)/rate:0;
        fprintf(stderr,"\r[run] [%.*s%*s] %3.0f%%  %llu/%llu cyc  %.0f cyc/s  ETA %.0fs   ",
                fill,"##############################",W-fill,"",frac*100.0,
                (unsigned long long)done,(unsigned long long)total,rate,eta);
        fflush(stderr);
    }

    // advance N fclk cycles; returns true if core0 trapped (when stop_on_trap)
    bool run(uint64_t cycles, bool stop_on_trap) {
        const uint64_t POLL=2000; const bool bar=(cycles>0);
        auto t0=std::chrono::steady_clock::now(), tl=t0;
        uint64_t done=0; bool trapped=false;
        while (done<cycles) {
            tick(); done++;
            if (done%POLL==0) {
                if (stop_on_trap) {
                    uint32_t dbg0=apb_read(ports["sfr"], SFR.at("sfr_dbg0").offset);
                    if (dbg0 & (1u<<12)) trapped=true;
                }
                auto now=std::chrono::steady_clock::now();
                if (bar && std::chrono::duration<double>(now-tl).count()>0.1) { draw_bar(done,cycles,t0); tl=now; }
            }
            if (trapped) break;
        }
        if (bar) { draw_bar(done,cycles,t0); fprintf(stderr,"\n"); }
        return trapped;
    }

    // ---- real-time serve mode -----------------------------------------
    int client_fd = -1;

    void send_line(const std::string& s) {
        if (client_fd < 0) return;
        std::string m = s; m.push_back('\n');
        ::send(client_fd, m.data(), m.size(), MSG_NOSIGNAL);
    }
    uint32_t read_signal(const std::string& n) {
        if (n=="gpio_out") return (uint32_t)dut->gpio_out;
        if (n=="gpio_in")  return (uint32_t)dut->gpio_in;
        if (n=="gpio_dir") return (uint32_t)dut->gpio_dir;
        if (n=="irq")      return (uint32_t)dut->irq;
        return 0;
    }
    // returns true if the client asked to stop
    bool handle_serve_line(const std::string& line) {
        std::istringstream is(line); std::string op; is >> op;
        if (op.empty() || op[0]=='#') return false;
        if (op=="set") {
            int pin, val;
            if (is>>pin>>val) set_gpio_in_bit(pin, val?1:0);
            return false;
        }
        if (op=="get") {
            std::string sig;
            if (is>>sig) { char b[96]; snprintf(b,sizeof b,"val %s 0x%08x",sig.c_str(),read_signal(sig)); send_line(b); }
            return false;
        }
        if (op=="stop" || op=="quit") return true;
        send_line("# err unknown command: " + op);
        return false;
    }

    // Listen on `port`, accept one client, then free-run: apply inbound `set`
    // commands at cycle boundaries (latest-value-wins) and stream monitored
    // output transitions back as `evt` lines. Runs until the client sends
    // stop, disconnects, SIGINT fires, or max_cycles (0 = unlimited) is hit.
    void serve(int port, bool wait_for_client, uint64_t max_cycles, uint64_t min_dwell) {
        int lfd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (lfd < 0) { perror("[serve] socket"); return; }
        int yes=1; ::setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
        sockaddr_in addr{}; addr.sin_family=AF_INET;
        addr.sin_addr.s_addr=INADDR_ANY; addr.sin_port=htons((uint16_t)port);
        if (::bind(lfd,(sockaddr*)&addr,sizeof addr) < 0) { perror("[serve] bind"); ::close(lfd); return; }
        if (::listen(lfd,1) < 0) { perror("[serve] listen"); ::close(lfd); return; }
        printf("[serve] listening on 0.0.0.0:%d  (fclk=%llu Hz)\n", port,(unsigned long long)fclk_hz);
        fflush(stdout);

        int cfd = -1;
        if (wait_for_client) {
            pollfd pfd{lfd, POLLIN, 0};
            while (!g_stop) {
                int r = ::poll(&pfd, 1, 200);
                if (r>0 && (pfd.revents & POLLIN)) { cfd = ::accept(lfd,nullptr,nullptr); break; }
                if (r<0 && errno!=EINTR) { perror("[serve] poll"); break; }
            }
        }
        if (cfd < 0) { ::close(lfd); printf("[serve] no client; stopping\n"); return; }
        client_fd = cfd;
        printf("[serve] client connected\n"); fflush(stdout);
        send_line("# bio-sim ready");

        std::mutex mtx; std::deque<std::string> inq; std::atomic<bool> closed{false};
        std::thread reader([&]{
            std::string buf; char tmp[1024];
            while (!g_stop && !closed) {
                ssize_t n = ::recv(cfd, tmp, sizeof tmp, 0);
                if (n <= 0) { closed = true; break; }
                buf.append(tmp, n);
                size_t pos;
                while ((pos=buf.find('\n')) != std::string::npos) {
                    std::string line = buf.substr(0,pos); buf.erase(0,pos+1);
                    if (!line.empty() && line.back()=='\r') line.pop_back();
                    std::lock_guard<std::mutex> lk(mtx); inq.push_back(line);
                }
            }
        });

        event_sink = [this](const std::string& s){ send_line(s); };

        auto t_beat = std::chrono::steady_clock::now();
        uint64_t ran=0, last_input_cycle=0; bool local_stop=false, primed=false;
        while (!g_stop && !closed && !local_stop && (max_cycles==0 || ran<max_cycles)) {
            // Process at most one queued command per tick. `set` commands are
            // paced: each input change is held >= min_dwell cycles before the
            // next is applied. Without this, a fast burst of keypresses arrives
            // between two clock edges and collapses 1->0->1 into a single net
            // change that the program never gets to sample. Non-`set` commands
            // (get/stop) are processed immediately.
            std::string line; bool have=false, is_set=false;
            {
                std::lock_guard<std::mutex> lk(mtx);
                if (!inq.empty()) {
                    is_set = inq.front().rfind("set",0)==0;
                    if (!is_set || !primed || (cycle - last_input_cycle) >= min_dwell) {
                        line = inq.front(); inq.pop_front(); have=true;
                    }
                }
            }
            if (have) {
                local_stop |= handle_serve_line(line);
                if (is_set) { last_input_cycle = cycle; primed = true; }
            }
            tick(); ran++;
            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration<double>(now - t_beat).count() > 2.0) {
                fprintf(stderr,"\r[serve] running, cyc=%llu   ",(unsigned long long)cycle); fflush(stderr);
                t_beat = now;
            }
        }

        event_sink = nullptr;
        send_line("# bye");
        closed = true;
        ::shutdown(cfd, SHUT_RDWR);
        reader.join();
        ::close(cfd); ::close(lfd); client_fd = -1;
        fprintf(stderr,"\n");
        printf("[serve] stopped after %llu cycles\n",(unsigned long long)ran);
    }

    // ---- driven (lock-step, deterministic) serve mode -----------------
    // The sim advances ONLY on `run` commands and replies to reads, so an
    // external model can drive timestamped stimulus and read results back with
    // full determinism (wall-clock / socket jitter cannot affect the result).
    // Protocol (newline text):
    //   inject <relcycle> <pin> <val>   schedule a pin edge (no reply)
    //   run <n>                         advance n cycles -> reply: ran <n>
    //   fifo_drain <bank>               -> reply: drain <bank> <count>, then
    //                                     <count> lines: sample 0x........
    //   fifo_read  <bank> <count>       same shape, explicit count
    //   set <pin> <val>                 immediate input (no reply)
    //   get <signal>                    -> reply: val <signal> 0x........
    //   stop                            end session
    int serve_accept(int port, bool wait_for_client, int* lfd_out) {
        int lfd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (lfd < 0) { perror("[serve] socket"); return -1; }
        int yes=1; ::setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
        sockaddr_in addr{}; addr.sin_family=AF_INET;
        addr.sin_addr.s_addr=INADDR_ANY; addr.sin_port=htons((uint16_t)port);
        if (::bind(lfd,(sockaddr*)&addr,sizeof addr) < 0) { perror("[serve] bind"); ::close(lfd); return -1; }
        if (::listen(lfd,1) < 0) { perror("[serve] listen"); ::close(lfd); return -1; }
        printf("[serve] listening on 0.0.0.0:%d\n", port); fflush(stdout);
        int cfd=-1;
        if (wait_for_client) {
            pollfd pfd{lfd, POLLIN, 0};
            while (!g_stop) {
                int r=::poll(&pfd,1,200);
                if (r>0 && (pfd.revents&POLLIN)) { cfd=::accept(lfd,nullptr,nullptr); break; }
                if (r<0 && errno!=EINTR) { perror("[serve] poll"); break; }
            }
        }
        *lfd_out = lfd; return cfd;
    }

    bool handle_driven_line(const std::string& line) {
        std::istringstream is(line); std::string op; is >> op;
        if (op.empty() || op[0]=='#') return false;
        if (op=="inject") { uint64_t cyc; int pin,val;
            if (is>>cyc>>pin>>val) schedule_inject(cyc,pin,val); return false; }
        if (op=="run") { uint64_t n=0; is>>n; run(n,false);
            char b[64]; snprintf(b,sizeof b,"ran %llu",(unsigned long long)n); send_line(b); return false; }
        if (op=="set") { int pin,val; if (is>>pin>>val) set_gpio_in_bit(pin,val); return false; }
        if (op=="get") { std::string s; if (is>>s) {
            char b[96]; snprintf(b,sizeof b,"val %s 0x%08x",s.c_str(),read_signal(s)); send_line(b);} return false; }
        if (op=="fifo_drain" || op=="fifo_read") {
            int bank=0; is>>bank;
            uint32_t flevel=apb_read(ports["sfr"], SFR.at("sfr_flevel").offset);
            uint32_t level=(flevel>>(4*bank))&0xF, n=level;
            if (op=="fifo_read") { uint32_t want; if (is>>want) n=want; }
            char hdr[48]; snprintf(hdr,sizeof hdr,"drain %d %u",bank,n); send_line(hdr);
            uint32_t off=fifo_off(bank,false);
            for (uint32_t i=0;i<n;++i) {
                uint32_t v=apb_read(ports["sfr"],off);
                char b[48]; snprintf(b,sizeof b,"sample 0x%08x",v); send_line(b);
            }
            return false;
        }
        if (op=="stop" || op=="quit") return true;
        send_line("# err unknown command: " + op);
        return false;
    }

    void serve_driven(int port, bool wait_for_client) {
        int lfd=-1; int cfd=serve_accept(port, wait_for_client, &lfd);
        if (cfd < 0) { if (lfd>=0) ::close(lfd); printf("[serve] no client; stopping\n"); return; }
        client_fd=cfd;
        printf("[serve] driven client connected\n"); fflush(stdout);
        send_line("# bio-sim ready (driven)");

        std::string bufd; std::string line;
        auto next_line = [&](std::string& out)->bool {
            for (;;) {
                size_t pos=bufd.find('\n');
                if (pos!=std::string::npos) {
                    out=bufd.substr(0,pos); bufd.erase(0,pos+1);
                    if (!out.empty() && out.back()=='\r') out.pop_back();
                    return true;
                }
                pollfd pfd{cfd, POLLIN, 0};
                int r=::poll(&pfd,1,200);
                if (g_stop) return false;
                if (r>0 && (pfd.revents&POLLIN)) {
                    char tmp[2048]; ssize_t n=::recv(cfd,tmp,sizeof tmp,0);
                    if (n<=0) return false;
                    bufd.append(tmp,n);
                }
            }
        };
        while (!g_stop) { if (!next_line(line)) break; if (handle_driven_line(line)) break; }

        send_line("# bye");
        ::shutdown(cfd,SHUT_RDWR); ::close(cfd); ::close(lfd); client_fd=-1;
        printf("[serve] driven session ended (cyc=%llu)\n",(unsigned long long)cycle);
    }
};

// --------------------------------------------------------------------------
// helpers
// --------------------------------------------------------------------------
static uint32_t parse_u32(const json& v) {
    if (v.is_number_unsigned()) return (uint32_t)v.get<uint64_t>();
    if (v.is_number_integer())  return (uint32_t)v.get<int64_t>();
    if (v.is_string())          return (uint32_t)std::stoul(v.get<std::string>(),nullptr,0);
    throw std::runtime_error("expected number or hex string");
}
static uint64_t parse_u64(const json& v) {
    if (v.is_number_unsigned()) return v.get<uint64_t>();
    if (v.is_number_integer())  return (uint64_t)v.get<int64_t>();
    if (v.is_string())          return std::stoull(v.get<std::string>(),nullptr,0);
    throw std::runtime_error("expected number or hex string");
}
static std::vector<uint32_t> read_bin_words(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open firmware: " + path);
    std::vector<uint8_t> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    while (b.size()%4) b.push_back(0);
    std::vector<uint32_t> w(b.size()/4);
    for (size_t i=0;i<w.size();++i)
        w[i]=(uint32_t)b[4*i]|((uint32_t)b[4*i+1]<<8)|((uint32_t)b[4*i+2]<<16)|((uint32_t)b[4*i+3]<<24);
    return w;
}

// resolve a register reference (name and/or offset, with a port) -> (port,offset)
struct Target { std::string port; uint32_t offset; };
static Target resolve_reg(const json& r) {
    std::string port = r.value("port", std::string("sfr"));
    uint32_t offset;
    if (r.contains("name")) {
        const std::string nm = r["name"].get<std::string>();
        auto it = SFR.find(nm);
        if (it==SFR.end()) throw std::runtime_error("unknown register name: "+nm);
        offset = it->second.offset;
        if (r.contains("offset") && parse_u32(r["offset"])!=offset)
            throw std::runtime_error("name/offset mismatch for "+nm);
        if (it->second.readonly)
            fprintf(stderr,"[poke] WARN %s is read-only; write ignored by RTL\n",nm.c_str());
    } else if (r.contains("offset")) {
        offset = parse_u32(r["offset"]);
        if (port=="sfr") {
            bool known=false; for (auto& kv:SFR) if (kv.second.offset==offset) known=true;
            if (!known) fprintf(stderr,"[poke] WARN raw offset 0x%03x not in SFR map\n",offset);
        }
    } else throw std::runtime_error("register entry needs 'name' or 'offset'");
    return {port, offset};
}

static std::string fifo_port(const json& c, int bank) {
    std::string via = c.value("via", std::string("sfr"));
    return (via=="alias") ? ("fifo"+std::to_string(bank)) : "sfr";
}

// ---- clock divider math, ported from the Rust BIO setup reference ----------
// qdiv encodes div_int (16b) and div_frac (8b, /256). Effective divisor is
// div_int + div_frac/256; freq = fclk / divisor. div_int=div_frac=0 => bypass.
static uint32_t compute_freq(uint64_t fclk_hz, uint16_t div_int, uint8_t div_frac) {
    uint32_t divisor = (uint32_t)div_int * 256u + div_frac;
    if (divisor == 0) return (uint32_t)fclk_hz;
    uint64_t numerator = fclk_hz * 256ull;
    return (uint32_t)((numerator + divisor/2) / divisor);   // round to nearest
}
struct Dividers { uint16_t div_int; uint8_t div_frac; uint32_t actual; };
static Dividers compute_dividers(uint64_t fclk_hz, uint32_t target, bool allow_frac) {
    if (target >= fclk_hz) return {0, 0, (uint32_t)fclk_hz};   // bypass
    auto err = [](uint32_t a, uint32_t b){ return a>b ? a-b : b-a; };
    if (allow_frac) {
        uint64_t total = (fclk_hz * 256ull) / target;
        uint64_t maxd  = 65535ull * 256 + 255;
        if (total > maxd) return {65535, 255, compute_freq(fclk_hz, 65535, 255)};
        uint64_t d1 = total, d2 = std::min(total + 1, maxd);
        uint16_t i1=(uint16_t)(d1/256), i2=(uint16_t)(d2/256);
        uint8_t  f1=(uint8_t)(d1%256),  f2=(uint8_t)(d2%256);
        uint32_t q1=compute_freq(fclk_hz,i1,f1), q2=compute_freq(fclk_hz,i2,f2);
        return err(q1,target) <= err(q2,target) ? Dividers{i1,f1,q1} : Dividers{i2,f2,q2};
    } else {
        uint64_t ideal = fclk_hz / target; if (ideal < 1) ideal = 1;
        if (ideal > 65535) return {65535, 0, compute_freq(fclk_hz, 65535, 0)};
        uint16_t d1=(uint16_t)ideal, d2=(uint16_t)std::min(ideal+1, (uint64_t)65535);
        uint32_t q1=compute_freq(fclk_hz,d1,0), q2=compute_freq(fclk_hz,d2,0);
        return err(q1,target) <= err(q2,target) ? Dividers{d1,0,q1} : Dividers{d2,0,q2};
    }
}

// --------------------------------------------------------------------------
static void execute_command(Sim& sim, const json& c) {
    const std::string cmd = c.value("cmd", std::string(""));

    if (cmd=="load") {
        int core = c.value("core", 0);
        std::string bin = c.at("bin").get<std::string>();
        auto words = read_bin_words(bin);
        std::string port = "imem"+std::to_string(core);
        printf("[load] %s -> core %d (%zu words)\n", bin.c_str(), core, words.size());
        for (size_t i=0;i<words.size();++i) sim.apb_write(sim.ports[port],(uint32_t)(i*4),words[i]);

    } else if (cmd=="poke") {
        Target t = resolve_reg(c);
        uint32_t v = parse_u32(c.at("value"));
        printf("[poke] %s @0x%03x <= 0x%08x\n", t.port.c_str(), t.offset, v);
        sim.apb_write(sim.ports[t.port], t.offset, v);

    } else if (cmd=="peek") {
        Target t = resolve_reg(c);
        uint32_t v = sim.apb_read(sim.ports[t.port], t.offset);
        printf("[peek] %s @0x%03x = 0x%08x\n", t.port.c_str(), t.offset, v);

    } else if (cmd=="fifo_write") {
        int bank = c.at("bank").get<int>();
        std::string port = fifo_port(c, bank);
        uint32_t off = Sim::fifo_off(bank, true);
        std::vector<uint32_t> words;
        if (c.contains("data")) for (auto& d : c["data"]) words.push_back(parse_u32(d));
        else if (c.contains("value")) words.push_back(parse_u32(c["value"]));
        printf("[fifo_write] bank %d via %s @0x%03x (%zu words)\n",
               bank, port.c_str(), off, words.size());
        for (auto w : words) sim.apb_write(sim.ports[port], off, w);

    } else if (cmd=="fifo_read") {
        int bank = c.at("bank").get<int>();
        std::string port = fifo_port(c, bank);
        uint32_t off = Sim::fifo_off(bank, false);
        int n = c.value("count", 1);
        printf("[fifo_read] bank %d via %s @0x%03x (%d words)\n", bank, port.c_str(), off, n);
        for (int i=0;i<n;++i) {
            uint32_t v = sim.apb_read(sim.ports[port], off);
            printf("[fifo_read]   [%d] = 0x%08x\n", i, v);
        }

    } else if (cmd=="fifo_drain") {
        // Read sfr_flevel for this bank; if non-empty, pop up to `max` words
        // (default: all available). Prints each as hex and as a signed 16-bit
        // sample, which is handy for confirming decoded audio.
        int bank = c.at("bank").get<int>();
        std::string port = fifo_port(c, bank);
        uint32_t flevel = sim.apb_read(sim.ports[port], SFR.at("sfr_flevel").offset);
        uint32_t level  = (flevel >> (4*bank)) & 0xF;
        uint32_t want   = c.contains("max") ? parse_u32(c["max"]) : level;
        uint32_t n      = std::min(level, want);
        uint32_t off    = Sim::fifo_off(bank, false);
        if (n == 0) {
            printf("[fifo_drain] bank %d empty\n", bank);
        } else {
            printf("[fifo_drain] bank %d level=%u, reading %u\n", bank, level, n);
            for (uint32_t i=0;i<n;++i) {
                uint32_t v = sim.apb_read(sim.ports[port], off);
                printf("[fifo_drain]   [%u] = 0x%08x  (s16=%d)\n", i, v, (int)(int16_t)(v & 0xFFFF));
            }
        }

    } else if (cmd=="start") {
        uint32_t en=0,restart=0,clkdiv=0;
        for (auto& core : c.value("cores", std::vector<int>{})) {
            en |= (1u<<core);
            if (c.value("restart", true))        restart |= (1u<<core);
            if (c.value("clkdiv_restart", true)) clkdiv  |= (1u<<core);
        }
        uint32_t ctrl=(clkdiv<<8)|(restart<<4)|en;
        printf("[start] sfr_ctrl @0x00 <= 0x%03x (en=0x%x restart=0x%x clkdiv=0x%x)\n",
               ctrl,en,restart,clkdiv);
        sim.apb_write(sim.ports["sfr"], SFR.at("sfr_ctrl").offset, ctrl);

    } else if (cmd=="monitor") {
        sim.add_monitor(c.at("signal").get<std::string>(),
                        c.value("bit",-1), c.contains("bit"));

    } else if (cmd=="inject") {
        for (auto& e : c.at("events"))
            sim.schedule_inject(parse_u64(e.at("cycle")),
                                e.at("pin").get<int>(), e.at("value").get<int>());
        printf("[inject] scheduled %zu event(s)\n", c["events"].size());

    } else if (cmd=="clock") {
        int core = c.at("core").get<int>();
        if (core < 0 || core > 3) throw std::runtime_error("clock: core must be 0..3");
        uint32_t qoff = SFR.at("sfr_qdiv0").offset + 4u*core;   // 0x50 + 4*core
        uint32_t eoff = SFR.at("sfr_extclock").offset;          // 0x44
        std::string style = c.value("style", std::string("frac"));

        if (style=="external") {
            int pin = c.at("pin").get<int>();
            sim.apb_write(sim.ports["sfr"], qoff, 0);           // disable divider
            uint32_t e = sim.apb_read(sim.ports["sfr"], eoff);  // RMW extclock
            int shift = 4 + 5*core;                             // extclk_gpio_<core>
            e &= ~(0x1Fu << shift);
            e |= ((uint32_t)(pin & 0x1F) << shift);
            e |= (1u << core);                                  // use_extclk[core]=1
            sim.apb_write(sim.ports["sfr"], eoff, e);
            printf("[clock] core %d <= external pin %d  (extclock=0x%06x)\n",
                   core, pin, e & 0xFFFFFFu);
        } else {
            Dividers d;
            uint32_t target = 0;
            if (style=="fixed") {
                uint16_t di = (uint16_t)parse_u32(c.at("div_int"));
                uint8_t  df = c.contains("div_frac") ? (uint8_t)parse_u32(c["div_frac"]) : 0;
                d = {di, df, compute_freq(sim.fclk_hz, di, df)};
            } else {                                            // "frac" (default) or "int"
                target = parse_u32(c.at("freq_hz"));
                d = compute_dividers(sim.fclk_hz, target, style != "int");
            }
            uint32_t sfr_value = ((uint32_t)d.div_int << 16) | ((uint32_t)d.div_frac << 8);
            sim.apb_write(sim.ports["sfr"], qoff, sfr_value);
            uint32_t e = sim.apb_read(sim.ports["sfr"], eoff);  // leave external mode
            e &= ~(1u << core);
            sim.apb_write(sim.ports["sfr"], eoff, e);
            if (style=="fixed")
                printf("[clock] core %d <= fixed div_int=%u div_frac=%u  (qdiv=0x%08x)  actual=%u Hz\n",
                       core, d.div_int, d.div_frac, sfr_value, d.actual);
            else {
                long ppm = target ? (long)((double)((long)d.actual - (long)target) * 1e6 / target) : 0;
                printf("[clock] core %d <= %u Hz (%s)  div_int=%u div_frac=%u  (qdiv=0x%08x)  actual=%u Hz (%+ld ppm)\n",
                       core, target, style.c_str(), d.div_int, d.div_frac, sfr_value, d.actual, ppm);
            }
        }

    } else if (cmd=="io_config") {
        // Mirrors setup_io_config. NOTE: unlike the Rust IoConfig struct (whose
        // omitted fields default to 0 and are always written in Overwrite mode),
        // this verb only touches the inversion/sync registers you name, so it
        // composes without clobbering. 'mapped' targets the external IOX mux,
        // which isn't part of this DUT, so it's ignored here.
        std::string m = c.value("mode", std::string("overwrite"));
        auto apply = [&](const char* nm, uint32_t off){
            if (!c.contains(nm)) return;
            uint32_t v = parse_u32(c[nm]), out;
            if      (m=="overwrite") out = v;
            else if (m=="set")       out = v | sim.apb_read(sim.ports["sfr"], off);
            else if (m=="clear")     out = (~v) & sim.apb_read(sim.ports["sfr"], off);
            else throw std::runtime_error("io_config: mode must be overwrite|set|clear");
            sim.apb_write(sim.ports["sfr"], off, out);
            printf("[io_config] %-11s @0x%02x <= 0x%08x (mode=%s)\n", nm, off, out, m.c_str());
        };
        apply("i_inv",       SFR.at("sfr_io_i_inv").offset);
        apply("o_inv",       SFR.at("sfr_io_o_inv").offset);
        apply("oe_inv",      SFR.at("sfr_io_oe_inv").offset);
        apply("sync_bypass", SFR.at("sfr_sync_bypass").offset);
        if (c.contains("mapped"))
            fprintf(stderr,"[io_config] NOTE 'mapped' targets the IOX mux (not in this DUT); ignored\n");
        if (c.contains("snap_inputs") || c.contains("snap_outputs")) {
            uint32_t cfg = sim.apb_read(sim.ports["sfr"], SFR.at("sfr_config").offset);
            if (c.contains("snap_inputs")) {
                uint32_t core = (uint32_t)c["snap_inputs"].get<int>() & 3;
                cfg &= ~((1u<<5)|(3u<<3));  cfg |= (1u<<5)|(core<<3);   // quantum + which (input)
            }
            if (c.contains("snap_outputs")) {
                uint32_t core = (uint32_t)c["snap_outputs"].get<int>() & 3;
                cfg &= ~((1u<<2)|(3u<<0));  cfg |= (1u<<2)|(core<<0);   // quantum + which (output)
            }
            sim.apb_write(sim.ports["sfr"], SFR.at("sfr_config").offset, cfg);
            printf("[io_config] sfr_config @0x08 <= 0x%03x (snap)\n", cfg & 0x3FF);
        }

    } else if (cmd=="fifo_event") {
        // Mirrors setup_fifo_event_triggers. event_offset = fifo*2 + slot (0..7).
        int which = c.at("fifo").get<int>();
        int slot  = c.value("slot", 0);
        int level = c.at("level").get<int>();
        if (which<0||which>3) throw std::runtime_error("fifo_event: fifo must be 0..3");
        if (slot<0||slot>1)   throw std::runtime_error("fifo_event: slot must be 0 or 1");
        int off = which*2 + slot;
        uint32_t el = sim.apb_read(sim.ports["sfr"], SFR.at("sfr_elevel").offset);
        el &= ~(0xFu << (4*off));
        el |=  ((uint32_t)(level & 0xF) << (4*off));     // ELEVEL nibble for this slot
        sim.apb_write(sim.ports["sfr"], SFR.at("sfr_elevel").offset, el);

        bool lt=c.value("less_than",false), gt=c.value("greater_than",false), eq=c.value("equal_to",false);
        uint32_t mask=1u<<off, setb=0, clrb=0xFFFFFFFFu;
        if (lt) setb|=mask;        else clrb&=~mask;
        if (eq) setb|=(mask<<8);   else clrb&=~(mask<<8);
        if (gt) setb|=(mask<<16);  else clrb&=~(mask<<16);
        uint32_t et = sim.apb_read(sim.ports["sfr"], SFR.at("sfr_etype").offset);
        et &= clrb; et |= setb;
        sim.apb_write(sim.ports["sfr"], SFR.at("sfr_etype").offset, et);
        printf("[fifo_event] fifo %d slot %d (off %d) level %d  lt=%d gt=%d eq=%d  (elevel=0x%08x etype=0x%06x)\n",
               which, slot, off, level, lt, gt, eq, el, et & 0xFFFFFFu);

    } else if (cmd=="irq") {
        // Mirrors setup_irq_config. mask is a raw 32-bit IrqMask value.
        int which = c.at("which").get<int>();
        if (which<0||which>3) throw std::runtime_error("irq: which must be 0..3");
        uint32_t mask = parse_u32(c.at("mask"));
        uint32_t moff = SFR.at("sfr_irqmask_0").offset + 4u*which;     // 0x70 + 4*which
        sim.apb_write(sim.ports["sfr"], moff, mask);
        bool edge = c.value("edge_triggered", false);
        uint32_t e = sim.apb_read(sim.ports["sfr"], SFR.at("sfr_irq_edge").offset);
        if (edge) e |= (1u<<which); else e &= ~(1u<<which);
        sim.apb_write(sim.ports["sfr"], SFR.at("sfr_irq_edge").offset, e);
        printf("[irq] line %d mask=0x%08x edge=%d  (irq_edge=0x%x)\n", which, mask, edge, e & 0xF);

    } else if (cmd=="serve") {
        int port = c.value("port", 5555);
        bool wfc = c.value("wait_for_client", true);
        std::string mode = c.value("mode", std::string("realtime"));
        if (mode=="driven") {
            sim.serve_driven(port, wfc);
        } else {
            uint64_t maxc = c.contains("max_cycles") ? parse_u64(c["max_cycles"]) : 0;
            uint64_t dwell = c.contains("min_dwell") ? parse_u64(c["min_dwell"]) : 2000;
            sim.serve(port, wfc, maxc, dwell);
        }

    } else if (cmd=="run" || cmd=="delay") {
        uint64_t cyc = c.contains("cycles")     ? parse_u64(c["cycles"])
                     : c.contains("max_cycles") ? parse_u64(c["max_cycles"])   // legacy
                     : 1000000;
        bool sot = (cmd=="run") && c.value("stop_on_trap", true);
        printf("[run] up to %llu cycles%s\n",(unsigned long long)cyc, sot?" (stop on trap)":"");
        if (sim.run(cyc, sot)) printf("[run] core0 trap at ~%llu cycles\n",(unsigned long long)sim.cycle);

    } else {
        fprintf(stderr,"[cmd] WARN ignoring unknown command '%s'\n", cmd.c_str());
    }
}

// Build the command list: explicit "commands" array, else desugar the legacy
// flat schema into the same command sequence (so old configs are unchanged).
static json build_commands(const json& cfg) {
    if (cfg.contains("commands")) return cfg["commands"];
    json out = json::array();
    if (cfg.contains("monitor"))
        for (auto& m : cfg["monitor"]) { json c=m; c["cmd"]="monitor"; out.push_back(c); }
    if (cfg.contains("firmware")) {
        json c; c["cmd"]="load"; c["core"]=cfg.value("load_core",0); c["bin"]=cfg["firmware"];
        out.push_back(c);
    }
    if (cfg.contains("registers"))
        for (auto& r : cfg["registers"]) { json c=r; c["cmd"]="poke"; out.push_back(c); }
    if (cfg.contains("start")) { json c=cfg["start"]; c["cmd"]="start"; out.push_back(c); }
    if (cfg.contains("run"))   { json c=cfg["run"];   c["cmd"]="run";   out.push_back(c); }
    else { json c; c["cmd"]="run"; c["cycles"]=0; out.push_back(c); }
    return out;
}

// --------------------------------------------------------------------------
int main(int argc, char** argv) {
    VerilatedContext ctx;
    ctx.commandArgs(argc, argv);
    std::signal(SIGINT, on_sigint);   // clean unwind from serve mode -> FST close
    if (argc < 2) { fprintf(stderr,"usage: %s <config.json>\n", argv[0]); return 2; }

    json cfg;
    try {
        std::ifstream cf(argv[1]);
        if (!cf) { fprintf(stderr,"cannot open config: %s\n", argv[1]); return 2; }
        // ignore_comments=true allows // line and /* */ block comments (a JSONC
        // superset). Strict JSON still parses unchanged.
        cfg = json::parse(cf, /*callback=*/nullptr, /*allow_exceptions=*/true,
                          /*ignore_comments=*/true);
    } catch (const std::exception& e) {
        fprintf(stderr,"config parse error: %s\n", e.what()); return 2;
    }

    double fclk_mhz = cfg.value("fclk_mhz", 700.0);
    Sim sim(&ctx, fclk_mhz);
    if (cfg.contains("trace"))
        sim.open_trace(cfg["trace"].value("file", std::string("trace.fst")));
    printf("[cfg] fclk=%.3f MHz  pclk=fclk/%u (%.3f MHz)\n",
           fclk_mhz, sim.pclk_div, fclk_mhz/sim.pclk_div);

    sim.reset();

    uint32_t cfginfo = sim.apb_read(sim.ports["sfr"], SFR.at("sfr_cfginfo").offset);
    printf("[selftest] sfr_cfginfo @0x04 = 0x%08x (expect 0x%08x) -> %s\n",
           cfginfo, CFGINFO_EXPECT, cfginfo==CFGINFO_EXPECT ? "PASS":"FAIL");

    try {
        for (const auto& c : build_commands(cfg)) execute_command(sim, c);
    } catch (const std::exception& e) {
        fprintf(stderr,"[cmd] ERROR: %s\n", e.what());
        sim.run_fclk_cycles(64);
        return 1;
    }

    sim.run_fclk_cycles(64);
    printf("[done] sim_time = %llu ps  (%llu cycles)\n",
           (unsigned long long)sim.time_ps, (unsigned long long)sim.cycle);
    return 0;
}