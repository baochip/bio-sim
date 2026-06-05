# bio-sim

A Verilator harness for the `bio_bdma` block. The host CPU is not modelled;
everything it would do (load code, poke control registers, start cores) is
reduced to APB transactions on the block's slave ports, driven from C++. The
RTL is used unmodified and built with `+define+SIM`.

## Directory layout

```
bio-sim/
├── rtl/                  # bio-bdma RTL tree, taken from baochip-1x repo
│   ├── bio_bdma.sv
│   ├── bio_bdma_wrapper.sv   # <-- DUT (flattened Verilog ports)
│   ├── picorv32.v ...
│   └── lib/
│       ├── template.sv   # Various libs and configs
│       └── ...
├── sim/
│   └── sim_main.cpp      # the harness (clocks, APB transactor, loader, trace)
├── configs/
│   ├── smoke.json        # no firmware; just the cfginfo self-test
│   └── hello_world.json  # example of toggle-a-pin
├── rtl.f                 # Verilator file list
├── Makefile
├── simulate              # Local build & run (not containerized)
├── container-run         # Runs the simulation from a container
└── README.md
```

The containerized version contains a pre-build of the BIO model as an
executable.

## Build & run (standalone, your preinstalled Verilator)

```bash
make build                     # fetches json.hpp, runs verilator, compiles
make run CONFIG=configs/smoke.json
# or:
./simulate configs/smoke.json
```

`make build` depends on the single-header `nlohmann/json` into `sim/json.hpp`
(one-time, needs network). If the build host is offline, fetch it manually:
```bash
curl -fsSL https://raw.githubusercontent.com/nlohmann/json/v3.11.3/single_include/nlohmann/json.hpp -o sim/json.hpp
```

## Smoke Test

`configs/smoke.json` runs no firmware. After reset the harness reads
`sfr_cfginfo` at offset `0x04` over the main APB port and checks it equals
`0x10000408` (the register hardwires `{16'd4096, 8'd4, 8'd8}`). A `PASS`
confirms that the build resolved, the clocking/reset are sane, and the APB transactor
completes a real transaction.

## Config schema (work in progress)

```jsonc
{
  "fclk_mhz": 700,              // pclk is DERIVED: /16 if >=700 MHz else /8
  "firmware": "firmware.bin",  // optional; little-endian, loaded word by word
  "load_core": 0,              // which core's IMEM to load (0..3)

  "registers": [               // applied in order, after load, before start
    { "name": "sfr_config", "value": "0x000" },        // by name (validated)
    { "offset": "0x6C", "value": "0x0", "port": "sfr" } // raw offset escape hatch
  ],

  "start": { "cores": [0], "restart": true, "clkdiv_restart": true },
  "run":   { "max_cycles": 2000000, "stop_on_trap": true },
  "trace": { "file": "trace.fst", "format": "fst" }
}
```

