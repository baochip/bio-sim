// bio-sim : Verilator harness for the bio_bdma block
// ---------------------------------------------------------------------------
// DUT  : bio_bdma_wrapper  (flattened Verilog ports; built with +define+SIM)
// Model: one fast clock `fclk` (fanned to aclk/hclk/dmaclk inside the wrapper)
//        plus `pclk`, derived by integer division of fclk (see PCLK divider).
//
// What this harness does, in order:
//   1. parse a JSON config
//   2. reset the DUT, tie off the (unused) DMA master inputs
//   3. self-test: read sfr_cfginfo @0x04, expect 0x10000408
//   4. load firmware into a core's IMEM over its apbs_imem APB port
//   5. apply the `registers` poke list over the main SFR APB port
//   6. compose+write sfr_ctrl @0x00 to start the requested cores
//   7. run for max_cycles fclk cycles, optionally stopping on trap
//   8. write the FST waveform
//
// The host CPU is NOT modelled. Everything it would do is reduced to APB
// transactions on the block's slave ports, which is exactly how silicon is
// brought up. No RTL is modified.
// ---------------------------------------------------------------------------

#include "Vbio_bdma_wrapper.h"
#include "verilated.h"
#include "verilated_fst_c.h"
#include "json.hpp"            // nlohmann/json single header (fetched by Makefile)

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

using json = nlohmann::json;

// --------------------------------------------------------------------------
// SFR name -> byte offset, on the MAIN APB port (apbs/PADDR).
// Extracted from the apb_* register declarations in bio_bdma.sv. Status
// (read-only) registers are flagged so a write to one can be warned about.
// --------------------------------------------------------------------------
struct SfrInfo { uint32_t offset; int width; bool readonly; };

static const std::map<std::string, SfrInfo> SFR = {
    {"sfr_ctrl",            {0x00, 12, false}},
    {"sfr_cfginfo",         {0x04, 32, true }},
    {"sfr_config",          {0x08, 10, false}},
    {"sfr_flevel",          {0x0C, 16, true }},
    {"sfr_txf0",            {0x10, 32, false}},
    {"sfr_txf1",            {0x14, 32, false}},
    {"sfr_txf2",            {0x18, 32, false}},
    {"sfr_txf3",            {0x1C, 32, false}},
    {"sfr_rxf0",            {0x20, 32, true }},
    {"sfr_rxf1",            {0x24, 32, true }},
    {"sfr_rxf2",            {0x28, 32, true }},
    {"sfr_rxf3",            {0x2C, 32, true }},
    {"sfr_elevel",          {0x30, 32, false}},
    {"sfr_etype",           {0x34, 24, false}},
    {"sfr_event_set",       {0x38, 24, false}},
    {"sfr_event_clr",       {0x3C, 24, false}},
    {"sfr_event_status",    {0x40, 32, true }},
    {"sfr_extclock",        {0x44, 24, false}},
    {"sfr_fifo_clr",        {0x48,  4, false}},
    {"sfr_qdiv0",           {0x50, 32, false}},
    {"sfr_qdiv1",           {0x54, 32, false}},
    {"sfr_qdiv2",           {0x58, 32, false}},
    {"sfr_qdiv3",           {0x5C, 32, false}},
    {"sfr_sync_bypass",     {0x60, 32, false}},
    {"sfr_io_oe_inv",       {0x64, 32, false}},
    {"sfr_io_o_inv",        {0x68, 32, false}},
    {"sfr_io_i_inv",        {0x6C, 32, false}},
    {"sfr_irqmask_0",       {0x70, 32, false}},
    {"sfr_irqmask_1",       {0x74, 32, false}},
    {"sfr_irqmask_2",       {0x78, 32, false}},
    {"sfr_irqmask_3",       {0x7C, 32, false}},
    {"sfr_irq_edge",        {0x80,  4, false}},
    {"sfr_dbg_padout",      {0x84, 32, true }},
    {"sfr_dbg_padoe",       {0x88, 32, true }},
    {"sfr_dbg0",            {0x90, 13, true }},
    {"sfr_dbg1",            {0x94, 13, true }},
    {"sfr_dbg2",            {0x98, 13, true }},
    {"sfr_dbg3",            {0x9C, 13, true }},
    {"sfr_mem_gutter",      {0xA0, 32, false}},
    {"sfr_peri_gutter",     {0xA4, 32, false}},
    {"sfr_dmareq_map",      {0xB0, 32, false}},
    {"sfr_filter_base_0",   {0xE0, 20, false}},
    {"sfr_filter_bounds_0", {0xE4, 20, false}},
    {"sfr_filter_base_1",   {0xE8, 20, false}},
    {"sfr_filter_bounds_1", {0xEC, 20, false}},
    {"sfr_filter_base_2",   {0xF0, 20, false}},
    {"sfr_filter_bounds_2", {0xF4, 20, false}},
    {"sfr_filter_base_3",   {0xF8, 20, false}},
    {"sfr_filter_bounds_3", {0xFC, 20, false}},
};

// Known good constant wired into sfr_cfginfo: {16'd4096, 8'd4, 8'd8}
static const uint32_t CFGINFO_EXPECT = (4096u << 16) | (4u << 8) | 8u; // 0x10000408

// --------------------------------------------------------------------------
// An APB slave port, expressed as setters/getters bound to wrapper signals.
// One of these per port: main "sfr", "imem0".."imem3", "fifo0".."fifo3".
// --------------------------------------------------------------------------
struct ApbPort {
    std::function<void(uint32_t)> set_paddr;
    std::function<void(uint32_t)> set_pwdata;
    std::function<void(uint8_t)>  set_pstrb;
    std::function<void(uint8_t)>  set_pprot;
    std::function<void(uint8_t)>  set_psel;
    std::function<void(uint8_t)>  set_penable;
    std::function<void(uint8_t)>  set_pwrite;
    std::function<void(uint8_t)>  set_apbactive;
    std::function<uint32_t()>     get_prdata;
    std::function<uint8_t()>      get_pready;
};

// Bind an ApbPort to a wrapper signal group. PFX is the port prefix token:
// empty for the main SFR port (PADDR), IM0_ for imem0, FP0_ for fifo0, etc.
#define MK_APB_PORT(PFX)                                                       \
    ApbPort{                                                                   \
        [&](uint32_t v){ dut->PFX##PADDR     = v; },                          \
        [&](uint32_t v){ dut->PFX##PWDATA    = v; },                          \
        [&](uint8_t  v){ dut->PFX##PSTRB     = v; },                          \
        [&](uint8_t  v){ dut->PFX##PPROT     = v; },                          \
        [&](uint8_t  v){ dut->PFX##PSEL      = v; },                          \
        [&](uint8_t  v){ dut->PFX##PENABLE   = v; },                          \
        [&](uint8_t  v){ dut->PFX##PWRITE    = v; },                          \
        [&](uint8_t  v){ dut->PFX##APBACTIVE = v; },                          \
        [&](){ return (uint32_t)dut->PFX##PRDATA; },                          \
        [&](){ return (uint8_t) dut->PFX##PREADY; }                           \
    }

// --------------------------------------------------------------------------
// The simulation engine.
// --------------------------------------------------------------------------
class Sim {
public:
    Vbio_bdma_wrapper* dut;
    VerilatedFstC*     tfp = nullptr;
    VerilatedContext*  ctx;

    uint64_t time_ps    = 0;
    uint64_t half_count = 0;
    uint32_t fclk_half_ps;     // fclk half-period, in ps (display only)
    uint32_t pclk_div;         // 8 or 16: fclk cycles per pclk half-period... see toggle
    bool     tracing = false;

    std::map<std::string, ApbPort> ports;

    Sim(VerilatedContext* c, double fclk_mhz) : ctx(c) {
        dut = new Vbio_bdma_wrapper{c};

        // fclk period in ps (display realism only; ratio is what matters).
        double period_ps = 1.0e6 / fclk_mhz;          // 1/MHz == us == 1e6 ps
        fclk_half_ps = (uint32_t)(period_ps / 2.0 + 0.5);
        if (fclk_half_ps == 0) fclk_half_ps = 1;

        // Fixed-ratio divider: >=700 MHz -> /16, else /8 (per spec; 350-700 -> /8).
        pclk_div = (fclk_mhz >= 700.0) ? 16 : 8;

        bind_ports();
        idle_all_apb();
        benign_dma_inputs();

        dut->cmatpg  = 0;
        dut->cmbist  = 0;
        dut->sramtrm = 0;
        dut->fclk    = 0;
        dut->pclk    = 0;
        dut->hclk    = 0;       // unused inside the wrapper, but keep it defined
        dut->resetn  = 0;
        dut->gpio_in = 0;
    }

    ~Sim() {
        if (tfp) { tfp->close(); delete tfp; }
        delete dut;
    }

    void open_trace(const std::string& path) {
        Verilated::traceEverOn(true);
        tfp = new VerilatedFstC;
        dut->trace(tfp, 99);
        tfp->open(path.c_str());
        tracing = true;
    }

    void bind_ports() {
        ports["sfr"]   = MK_APB_PORT();        // main SFR port: bare PADDR/PSEL/...
        ports["imem0"] = MK_APB_PORT(IM0_);
        ports["imem1"] = MK_APB_PORT(IM1_);
        ports["imem2"] = MK_APB_PORT(IM2_);
        ports["imem3"] = MK_APB_PORT(IM3_);
        ports["fifo0"] = MK_APB_PORT(FP0_);
        ports["fifo1"] = MK_APB_PORT(FP1_);
        ports["fifo2"] = MK_APB_PORT(FP2_);
        ports["fifo3"] = MK_APB_PORT(FP3_);
    }

    void idle_all_apb() {
        for (auto& kv : ports) {
            ApbPort& p = kv.second;
            p.set_psel(0); p.set_penable(0); p.set_pwrite(0);
            p.set_paddr(0); p.set_pwdata(0); p.set_pstrb(0);
            p.set_pprot(0); p.set_apbactive(0);
        }
    }

    // The block routes addresses >= 0x1000_0000 to the AXI/AHB masters. Our
    // hello-world programs stay inside the 4 KB IMEM, so the masters never
    // activate -- we just hold their inputs at benign values so nothing is X.
    void benign_dma_inputs() {
        dut->aw_ready = 1; dut->ar_ready = 1; dut->w_ready = 1;
        dut->r_valid = 0; dut->r_data = 0; dut->r_resp = 0;
        dut->r_last = 0;  dut->r_id = 0;   dut->r_user = 0;
        dut->b_valid = 0; dut->b_resp = 0; dut->b_id = 0; dut->b_user = 0;
        dut->hrdata = 0;  dut->hready = 1; dut->hresp = 0;
        dut->hruser = 0;  dut->hreadym = 1;
    }

    // ---- time stepping -------------------------------------------------
    void eval_dump() {
        dut->eval();
        if (tracing) tfp->dump(time_ps);
    }

    // Advance one fclk half-period; toggle fclk every step and pclk every
    // `pclk_div` steps so that fclk:pclk == pclk_div exactly.
    void half_step() {
        time_ps += fclk_half_ps;
        dut->fclk = !dut->fclk;
        dut->hclk = dut->fclk;
        if (++half_count % pclk_div == 0) dut->pclk = !dut->pclk;
        eval_dump();
    }

    void advance_to_pclk_posedge() {
        uint8_t prev = dut->pclk;
        for (;;) {
            half_step();
            if (prev == 0 && dut->pclk == 1) return;
            prev = dut->pclk;
        }
    }

    void run_fclk_cycles(uint64_t n) {
        for (uint64_t i = 0; i < 2 * n; ++i) half_step();
    }

    // ---- reset ---------------------------------------------------------
    void reset(int pclk_cycles = 16) {
        dut->resetn = 0;
        for (int i = 0; i < pclk_cycles; ++i) advance_to_pclk_posedge();
        dut->resetn = 1;
        for (int i = 0; i < pclk_cycles; ++i) advance_to_pclk_posedge();
    }

    // ---- APB3 transactions (driven in the pclk domain) -----------------
    // Returns read data; for writes the return value is ignored.
    uint32_t apb_xact(ApbPort& p, uint32_t addr, uint32_t data, bool write) {
        // SETUP phase
        advance_to_pclk_posedge();
        p.set_psel(1); p.set_penable(0); p.set_pwrite(write ? 1 : 0);
        p.set_paddr(addr); p.set_pwdata(write ? data : 0);
        p.set_pstrb(write ? 0xF : 0x0); p.set_pprot(0); p.set_apbactive(1);

        // slave samples SETUP at this edge
        advance_to_pclk_posedge();
        p.set_penable(1);

        // ACCESS phase: wait for PREADY at a rising edge
        uint32_t rdata = 0;
        int guard = 0;
        for (;;) {
            advance_to_pclk_posedge();
            if (p.get_pready()) { rdata = p.get_prdata(); break; }
            if (++guard > 4096) {
                fprintf(stderr, "[apb] TIMEOUT waiting for PREADY @0x%03x\n", addr);
                break;
            }
        }
        // teardown
        p.set_psel(0); p.set_penable(0); p.set_pwrite(0); p.set_apbactive(0);
        advance_to_pclk_posedge();
        return rdata;
    }

    void     apb_write(ApbPort& p, uint32_t a, uint32_t d) { apb_xact(p, a, d, true); }
    uint32_t apb_read (ApbPort& p, uint32_t a)             { return apb_xact(p, a, 0, false); }
};

// --------------------------------------------------------------------------
// helpers
// --------------------------------------------------------------------------
static uint32_t parse_u32(const json& v) {
    if (v.is_number_unsigned()) return (uint32_t)v.get<uint64_t>();
    if (v.is_number_integer())  return (uint32_t)v.get<int64_t>();
    if (v.is_string()) {
        std::string s = v.get<std::string>();
        return (uint32_t)std::stoul(s, nullptr, 0);   // honours 0x prefix
    }
    throw std::runtime_error("expected a number or hex string");
}

static std::vector<uint32_t> read_bin_words(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open firmware: " + path);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)),
                                std::istreambuf_iterator<char>());
    while (bytes.size() % 4) bytes.push_back(0);       // pad to word boundary
    std::vector<uint32_t> words(bytes.size() / 4);
    for (size_t i = 0; i < words.size(); ++i)
        words[i] =  (uint32_t)bytes[4*i]        | ((uint32_t)bytes[4*i+1] << 8)
                 | ((uint32_t)bytes[4*i+2] << 16)| ((uint32_t)bytes[4*i+3] << 24);
    return words;
}

// --------------------------------------------------------------------------
int main(int argc, char** argv) {
    VerilatedContext ctx;
    ctx.commandArgs(argc, argv);

    if (argc < 2) {
        fprintf(stderr, "usage: %s <config.json>\n", argv[0]);
        return 2;
    }

    json cfg;
    try {
        std::ifstream cf(argv[1]);
        if (!cf) { fprintf(stderr, "cannot open config: %s\n", argv[1]); return 2; }
        cf >> cfg;
    } catch (const std::exception& e) {
        fprintf(stderr, "config parse error: %s\n", e.what());
        return 2;
    }

    double fclk_mhz = cfg.value("fclk_mhz", 700.0);
    Sim sim(&ctx, fclk_mhz);

    // tracing
    if (cfg.contains("trace")) {
        std::string tf = cfg["trace"].value("file", std::string("trace.fst"));
        sim.open_trace(tf);
    }
    printf("[cfg] fclk=%.3f MHz  pclk=fclk/%u (%.3f MHz)\n",
           fclk_mhz, sim.pclk_div, fclk_mhz / sim.pclk_div);

    sim.reset();

    // ---- self-test: read the hardwired cfginfo constant ----------------
    uint32_t cfginfo = sim.apb_read(sim.ports["sfr"], SFR.at("sfr_cfginfo").offset);
    printf("[selftest] sfr_cfginfo @0x04 = 0x%08x (expect 0x%08x) -> %s\n",
           cfginfo, CFGINFO_EXPECT, cfginfo == CFGINFO_EXPECT ? "PASS" : "FAIL");

    // ---- firmware load over the imem APB port --------------------------
    int load_core = cfg.value("load_core", 0);
    if (cfg.contains("firmware")) {
        std::string fw = cfg["firmware"].get<std::string>();
        auto words = read_bin_words(fw);
        std::string imem_port = "imem" + std::to_string(load_core);
        printf("[load] %s -> core %d (%zu words) via %s\n",
               fw.c_str(), load_core, words.size(), imem_port.c_str());
        for (size_t i = 0; i < words.size(); ++i)
            sim.apb_write(sim.ports[imem_port], (uint32_t)(i * 4), words[i]);
    }

    // ---- apply the register poke list ----------------------------------
    if (cfg.contains("registers")) {
        for (const auto& r : cfg["registers"]) {
            uint32_t value = parse_u32(r.at("value"));
            std::string port = r.value("port", std::string("sfr"));
            uint32_t offset;
            if (r.contains("name")) {
                const std::string nm = r["name"].get<std::string>();
                auto it = SFR.find(nm);
                if (it == SFR.end())
                    throw std::runtime_error("unknown register name: " + nm);
                offset = it->second.offset;
                if (r.contains("offset") && parse_u32(r["offset"]) != offset)
                    throw std::runtime_error("name/offset mismatch for " + nm);
                if (it->second.readonly)
                    fprintf(stderr, "[poke] WARN %s is read-only; write ignored by RTL\n", nm.c_str());
            } else if (r.contains("offset")) {
                offset = parse_u32(r["offset"]);
                if (port == "sfr") {
                    bool known = false;
                    for (auto& kv : SFR) if (kv.second.offset == offset) known = true;
                    if (!known)
                        fprintf(stderr, "[poke] WARN raw offset 0x%03x not in SFR map\n", offset);
                }
            } else {
                throw std::runtime_error("register entry needs 'name' or 'offset'");
            }
            printf("[poke] %s @0x%03x <= 0x%08x\n", port.c_str(), offset, value);
            sim.apb_write(sim.ports[port], offset, value);
        }
    }

    // ---- compose + write sfr_ctrl to start cores -----------------------
    // sfr_ctrl @0x00 = {clkdiv_restart[3:0], restart[3:0], en[3:0]}
    if (cfg.contains("start")) {
        const auto& st = cfg["start"];
        uint32_t en = 0, restart = 0, clkdiv = 0;
        for (const auto& c : st.value("cores", std::vector<int>{})) {
            en |= (1u << c);
            if (st.value("restart", true))        restart |= (1u << c);
            if (st.value("clkdiv_restart", true)) clkdiv  |= (1u << c);
        }
        uint32_t ctrl = (clkdiv << 8) | (restart << 4) | en;
        printf("[start] sfr_ctrl @0x00 <= 0x%03x (en=0x%x restart=0x%x clkdiv=0x%x)\n",
               ctrl, en, restart, clkdiv);
        sim.apb_write(sim.ports["sfr"], SFR.at("sfr_ctrl").offset, ctrl);
    }

    // ---- signal monitors -----------------------------------------------
    // Watch top-level wrapper signals and print edges. Configured as:
    //   "monitor": [ {"signal":"gpio_out","bit":3}, {"signal":"irq"} ]
    // Supported signals are the flattened wrapper ports below; "bit" is
    // optional (omit to watch the whole word).
    struct Monitor {
        std::string name; int bit; bool has_bit;
        std::function<uint32_t()> get; uint32_t last;
    };
    std::map<std::string, std::function<uint32_t()>> mon_sources = {
        {"gpio_out", [&]{ return (uint32_t)sim.dut->gpio_out; }},
        {"gpio_in",  [&]{ return (uint32_t)sim.dut->gpio_in;  }},
        {"gpio_dir", [&]{ return (uint32_t)sim.dut->gpio_dir; }},
        {"irq",      [&]{ return (uint32_t)sim.dut->irq;      }},
    };
    std::vector<Monitor> monitors;
    if (cfg.contains("monitor")) {
        for (const auto& m : cfg["monitor"]) {
            std::string sig = m.at("signal").get<std::string>();
            auto it = mon_sources.find(sig);
            if (it == mon_sources.end()) {
                fprintf(stderr, "[mon] WARN unknown signal '%s' (have: gpio_out/gpio_in/gpio_dir/irq)\n",
                        sig.c_str());
                continue;
            }
            Monitor mon;
            mon.name = sig;
            mon.has_bit = m.contains("bit");
            mon.bit = mon.has_bit ? m["bit"].get<int>() : -1;
            mon.get = it->second;
            mon.last = mon.get();
            monitors.push_back(mon);
            fprintf(stderr, "[mon] watching %s%s\n", sig.c_str(),
                   mon.has_bit ? (" bit " + std::to_string(mon.bit)).c_str() : "");
        }
    }
    auto sample_monitors = [&]() {
        for (auto& mon : monitors) {
            uint32_t cur = mon.get();
            if (mon.has_bit) {
                uint32_t a = (mon.last >> mon.bit) & 1, b = (cur >> mon.bit) & 1;
                if (a != b)
                    fprintf(stderr, "[mon] %10llu ps  %s[%d]: %u -> %u\n",
                           (unsigned long long)sim.time_ps, mon.name.c_str(), mon.bit, a, b);
            } else if (cur != mon.last) {
                fprintf(stderr, "[mon] %10llu ps  %s: 0x%08x -> 0x%08x\n",
                       (unsigned long long)sim.time_ps, mon.name.c_str(), mon.last, cur);
            }
            mon.last = cur;
        }
    };

    // ---- run -----------------------------------------------------------
    uint64_t max_cycles  = cfg.contains("run") ? cfg["run"].value("max_cycles", (uint64_t)1000000)
                                               : (uint64_t)1000000;
    bool     stop_on_trap = cfg.contains("run") ? cfg["run"].value("stop_on_trap", true) : true;

    printf("[run] up to %llu fclk cycles%s\n",
           (unsigned long long)max_cycles, stop_on_trap ? " (stop on trap)" : "");

    const uint64_t POLL = 2000;     // poll trap / refresh bar every ~2000 cycles
    const bool show_bar = (max_cycles > 0);
    auto   t_start  = std::chrono::steady_clock::now();
    auto   t_last   = t_start;
    uint64_t done = 0;
    bool trapped = false;

    auto draw_bar = [&](uint64_t cyc) {
        double frac = max_cycles ? (double)cyc / (double)max_cycles : 0.0;
        if (frac > 1.0) frac = 1.0;
        int W = 30, fill = (int)(frac * W);
        double secs = std::chrono::duration<double>(
                          std::chrono::steady_clock::now() - t_start).count();
        double rate = secs > 0 ? cyc / secs : 0.0;          // sim cycles / wall sec
        double eta  = rate > 0 ? (max_cycles - cyc) / rate : 0.0;
        fprintf(stderr, "\r[run] [%.*s%*s] %3.0f%%  %llu/%llu cyc  %.0f cyc/s  ETA %.0fs   ",
                fill, "##############################", W - fill, "",
                frac * 100.0, (unsigned long long)cyc, (unsigned long long)max_cycles,
                rate, eta);
        fflush(stderr);
    };

    while (done < max_cycles) {
        uint64_t chunk = std::min<uint64_t>(POLL, max_cycles - done);
        for (uint64_t i = 0; i < chunk; ++i) {
            sim.half_step(); sim.half_step();   // one fclk cycle
            if (!monitors.empty()) sample_monitors();
        }
        done += chunk;

        if (stop_on_trap) {
            uint32_t dbg0 = sim.apb_read(sim.ports["sfr"], SFR.at("sfr_dbg0").offset);
            if (dbg0 & (1u << 12)) { trapped = true; }
        }

        // refresh the bar once a second
        auto now = std::chrono::steady_clock::now();
        if (show_bar &&
            std::chrono::duration<double>(now - t_last).count() > 1.0) {
            draw_bar(done);
            t_last = now;
        }
        if (trapped) break;
    }

    if (show_bar) { draw_bar(done); fprintf(stderr, "\n"); }
    if (trapped)
        printf("[run] core0 trap at ~%llu cycles\n", (unsigned long long)done);

    sim.run_fclk_cycles(64);        // flush a few cycles into the trace
    printf("[done] sim_time = %llu ps  (%llu cycles run)\n",
           (unsigned long long)sim.time_ps, (unsigned long long)done);
    return 0;
}