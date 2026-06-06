#!/usr/bin/env python3
"""I2S stimulus for bio-sim, two ways.

Generates a standard Philips I2S waveform (continuous BCLK, WS low=left /
high=right, MSB first, data delayed one BCLK after the WS edge, sampled on the
rising BCLK edge) carrying a full-swing sine on both channels, and either:

  --file CONFIG    write a complete JSONC config (load + clock + start +
                   inject + run/fifo_drain). Run it with ./container-run CONFIG.
  --port N         drive a *running* sim's driven serve socket live: send the
                   inject events, then run/fifo_drain in lock-step and print the
                   decoded samples. System setup (load/clock/start + a driven
                   `serve`) is done by the sim-side config, e.g.
                   ./container-run --port N configs/i2s_serve.jsonc

I2S timing lives in clock cycles, so this always uses deterministic injection;
the same edges are produced whether written to a file or streamed over TCP.

Pins: BCLK=0 WS=1 SD=2 (override below). Decoded samples read from fifo3.
"""
import argparse, json, math, socket, sys

ap = argparse.ArgumentParser()
out = ap.add_mutually_exclusive_group(required=True)
out.add_argument("--file", metavar="CONFIG", help="write a JSONC config to this path ('-' = stdout)")
out.add_argument("--port", type=int, help="stream to a running driven serve socket on this port")
ap.add_argument("--host", default="127.0.0.1")
ap.add_argument("--freq",  type=float, default=1000.0, help="sine frequency, Hz (default 1000)")
ap.add_argument("--rate",  type=int,   default=44100,  help="sample rate, Hz")
ap.add_argument("--bits",  type=int,   default=16,     help="bits per channel")
ap.add_argument("--fclk",  type=float, default=700e6,  help="fclk in Hz")
ap.add_argument("--frames", type=int,  default=64,     help="stereo samples to generate")
ap.add_argument("--frames-per-drain", type=int, default=2, help="drain fifo3 every N frames")
ap.add_argument("--align", choices=["i2s","left"], default="i2s")
ap.add_argument("--bclk-pin", type=int, default=0)
ap.add_argument("--ws-pin",   type=int, default=1)
ap.add_argument("--sd-pin",   type=int, default=2)
ap.add_argument("--read-bank", type=int, default=3)
ap.add_argument("--firmware", default="i2s_decode.bin")
ap.add_argument("--fclk-mhz", type=float, default=700.0)
ap.add_argument("--trace", default="i2s.fst")
a = ap.parse_args()

bclk_freq = a.rate * a.bits * 2
period = round(a.fclk / bclk_freq)
half   = period // 2
slots_per_frame = a.bits * 2

def sample(n):
    v = int(round(32767 * math.sin(2*math.pi*a.freq*n / a.rate)))
    return max(-32768, min(32767, v)) & 0xFFFF

# per-slot logical bits (left then right, MSB first) and ws (0 left / 1 right)
logic_bits, logic_ws, expected = [], [], []
for f in range(a.frames):
    s = sample(f)
    expected.append((s ^ 0x8000) - 0x8000)
    for ch_ws in (0, 1):
        for b in range(a.bits):
            logic_bits.append((s >> (a.bits-1-b)) & 1)
            logic_ws.append(ch_ws)

def wire_sd(i):
    if a.align == "left":
        return logic_bits[i]
    return logic_bits[i-1] if i > 0 else 0

# inject events on slot/edge boundaries (emit ws/sd only on change)
events = []
last_ws = last_sd = None
total_slots = len(logic_bits)
for i in range(total_slots):
    t0 = i * period
    events.append((t0, a.bclk_pin, 0))
    ws, sd = logic_ws[i], wire_sd(i)
    if ws != last_ws: events.append((t0, a.ws_pin, ws)); last_ws = ws
    if sd != last_sd: events.append((t0, a.sd_pin, sd)); last_sd = sd
    events.append((t0+half, a.bclk_pin, 1))

# chunked run lengths (so fifo3 is drained before it can overflow)
chunk_cycles = a.frames_per_drain * slots_per_frame * period
total_cycles = total_slots * period
chunks = []
covered = 0
while covered < total_cycles:
    chunks.append(chunk_cycles); covered += chunk_cycles
chunks.append(8 * period)   # margin so the last sample lands before the final drain

# ------------------------------------------------------------------ file mode
if a.file:
    commands = [
        {"cmd": "load",  "core": 0, "bin": a.firmware},
        {"cmd": "clock", "core": 0, "style": "frac", "freq_hz": int(a.fclk)},
        {"cmd": "start", "cores": [0]},
        {"cmd": "inject", "events": [{"cycle": c, "pin": p, "value": v} for (c,p,v) in events]},
    ]
    for ch in chunks:
        commands.append({"cmd": "run", "cycles": ch, "stop_on_trap": False})
        commands.append({"cmd": "fifo_drain", "bank": a.read_bank})
    cfg = {"fclk_mhz": a.fclk_mhz, "trace": {"file": a.trace}, "commands": commands}

    hdr  = f"// I2S {a.freq:.0f} Hz sine, {a.rate} Hz {a.bits}-bit stereo, align={a.align}\n"
    hdr += f"// BCLK={bclk_freq} Hz ({period} cyc/period), pins BCLK={a.bclk_pin} WS={a.ws_pin} SD={a.sd_pin}\n"
    hdr += f"// expected samples: {expected}\n"
    f = sys.stdout if a.file == "-" else open(a.file, "w")
    f.write(hdr); json.dump(cfg, f, indent=2); f.write("\n")
    if f is not sys.stdout:
        f.close(); print(f"wrote {a.file}  ({len(events)} inject events, {len(chunks)} drains)", file=sys.stderr)
    sys.exit(0)

# ------------------------------------------------------------------ port mode
print(f"expected samples (sine): {expected}")
s = socket.create_connection((a.host, a.port))
f = s.makefile("r")
print("  <-", f.readline().strip())            # ready banner

for (c, p, v) in events:                        # ship the whole waveform
    s.sendall(f"inject {c} {p} {v}\n".encode())

got = []
for ch in chunks:
    s.sendall(f"run {ch}\n".encode())
    while True:                                  # wait for the run ack
        l = f.readline()
        if not l: sys.exit("connection closed during run")
        l = l.strip()
        if l.startswith("ran"): break
        if l.startswith("#"):   print("  <-", l)
    s.sendall(f"fifo_drain {a.read_bank}\n".encode())
    l = f.readline().strip()
    while not l.startswith("drain"):
        print("  <-", l); l = f.readline().strip()
    count = int(l.split()[2])
    for _ in range(count):
        hexv = int(f.readline().strip().split()[1], 0)
        s16 = hexv & 0xFFFF
        s16 = s16 - 0x10000 if s16 >= 0x8000 else s16
        got.append(s16)
        print(f"  decoded[{len(got)-1}] = {s16}")

s.sendall(b"stop\n")
s.close()
print(f"\ndone: {len(got)} samples decoded. compare against the expected sine above.")