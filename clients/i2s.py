#!/usr/bin/env python3
"""Generate a deterministic I2S stimulus config for bio-sim.

Why a generator and not a live socket client: I2S timing is defined in clock
cycles (a 44.1 kHz / 16-bit stereo stream needs a ~1.41 MHz bit clock, ~496
fclk cycles per BCLK period at 700 MHz). That can only be driven by
cycle-timestamped injection, not by the real-time `set` socket path whose edge
timing would be set by TCP arrival jitter. So this script emits a JSONC config
of `inject` events plus interleaved `run` / `fifo_drain` commands, which the
batch executor replays exactly the same way every run.

Standard Philips I2S is produced: continuous BCLK, WS low = left / high =
right, MSB first, data delayed one BCLK after the WS edge, sampled on the
rising BCLK edge. Use --align left for left-justified (no 1-bit delay) if your
decoder expects that.

  ./clients/i2s.py > configs/i2s.jsonc
  ./container-run configs/i2s.jsonc      # then inspect waveform/i2s.fst

Pins (override as needed):  BCLK=0  WS=1  SD=2.  Decoded samples read from fifo3.
"""
import argparse, math, sys, json

ap = argparse.ArgumentParser()
ap.add_argument("--freq",  type=float, default=1000.0, help="sine frequency, Hz (default 1000)")
ap.add_argument("--rate",  type=int,   default=44100,  help="sample rate, Hz (default 44100)")
ap.add_argument("--bits",  type=int,   default=16,     help="bits per channel (default 16)")
ap.add_argument("--fclk",  type=float, default=700e6,  help="fclk in Hz (default 700e6)")
ap.add_argument("--frames", type=int,  default=64,     help="stereo samples to generate (default 64)")
ap.add_argument("--frames-per-drain", type=int, default=2, help="drain fifo3 every N frames (default 2)")
ap.add_argument("--align", choices=["i2s","left"], default="i2s", help="bit alignment (default i2s)")
ap.add_argument("--bclk-pin", type=int, default=0)
ap.add_argument("--ws-pin",   type=int, default=1)
ap.add_argument("--sd-pin",   type=int, default=2)
ap.add_argument("--read-bank", type=int, default=3)
ap.add_argument("--firmware", default="i2s_decode.bin", help="BIO decoder .bin (you provide this)")
ap.add_argument("--fclk-mhz", type=float, default=700.0)
ap.add_argument("--trace", default="i2s.fst")
a = ap.parse_args()

bclk_freq = a.rate * a.bits * 2
period = round(a.fclk / bclk_freq)            # fclk cycles per BCLK period
half   = period // 2
slots_per_frame = a.bits * 2

# --- build the per-slot logical bit stream -------------------------------
# logical: 16 left bits (MSB first) then 16 right bits; ws 0 for left, 1 for right
def sample(n):
    v = int(round(32767 * math.sin(2*math.pi*a.freq*n / a.rate)))
    return max(-32768, min(32767, v)) & 0xFFFF

logic_bits, logic_ws, expected = [], [], []
for f in range(a.frames):
    s = sample(f)
    expected.append((f, (s ^ 0x8000) - 0x8000))   # signed, for the comment block
    for ch_ws in (0, 1):                            # left then right (same sine)
        for b in range(a.bits):                     # MSB first
            logic_bits.append((s >> (a.bits-1-b)) & 1)
            logic_ws.append(ch_ws)

# wire SD: I2S delays data one slot relative to WS; left-justified does not.
def wire_sd(i):
    if a.align == "left":
        return logic_bits[i]
    return logic_bits[i-1] if i > 0 else 0

# --- emit inject events on slot/edge boundaries --------------------------
events = []
last_ws = last_sd = None
total_slots = len(logic_bits)
for i in range(total_slots):
    t0 = i * period
    events.append({"cycle": t0,        "pin": a.bclk_pin, "value": 0})   # BCLK falling
    ws, sd = logic_ws[i], wire_sd(i)
    if ws != last_ws:
        events.append({"cycle": t0, "pin": a.ws_pin, "value": ws}); last_ws = ws
    if sd != last_sd:
        events.append({"cycle": t0, "pin": a.sd_pin, "value": sd}); last_sd = sd
    events.append({"cycle": t0+half,   "pin": a.bclk_pin, "value": 1})   # BCLK rising (sample)

# --- build the command list ----------------------------------------------
commands = [
    {"cmd": "load",  "core": 0, "bin": a.firmware},
    {"cmd": "clock", "core": 0, "style": "frac", "freq_hz": int(a.fclk)},  # >= fclk -> bypass (full speed)
    {"cmd": "start", "cores": [0]},
    {"cmd": "inject", "events": events},
]
chunk_cycles = a.frames_per_drain * slots_per_frame * period
covered = 0
total_cycles = total_slots * period
while covered < total_cycles:
    commands.append({"cmd": "run", "cycles": chunk_cycles, "stop_on_trap": False})
    commands.append({"cmd": "fifo_drain", "bank": a.read_bank})
    covered += chunk_cycles
commands.append({"cmd": "run", "cycles": 8*period, "stop_on_trap": False})   # margin
commands.append({"cmd": "fifo_drain", "bank": a.read_bank})

cfg = {"fclk_mhz": a.fclk_mhz, "trace": {"file": a.trace}, "commands": commands}

# --- print as JSONC with an expected-sample comment block ----------------
hdr  = f"// I2S stimulus: {a.freq:.0f} Hz sine, {a.rate} Hz, {a.bits}-bit stereo\n"
hdr += f"// BCLK={bclk_freq} Hz ({period} fclk cycles/period), pins BCLK={a.bclk_pin} WS={a.ws_pin} SD={a.sd_pin}\n"
hdr += f"// alignment={a.align}, {a.frames} frames, {len(events)} inject events\n"
hdr += "// expected decoded samples (frame: signed16) -- compare to fifo_drain output:\n"
for f, v in expected:
    hdr += f"//   frame {f:3d}: {v}\n"
sys.stdout.write(hdr)
json.dump(cfg, sys.stdout, indent=2)
sys.stdout.write("\n")