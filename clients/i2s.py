#!/usr/bin/env python3
"""I2S stimulus for bio-sim.

Two roles, selected with --role:

  mic    (default)  Simulate an I2S microphone (a SLAVE). The bio core is the
                    master: it drives BCLK and WS, and the mic only reacts --
                    it watches BCLK/WS and shifts sample data out onto SD.
                    Reactive, so it runs only over a `driven` serve socket
                    (--port). Per the agreed convention, the mic stays idle
                    until the FIRST FALLING EDGE of WS, then begins emitting
                    valid data (MSB first, one BCLK of delay, Philips I2S).

  source            The original behaviour: generate BCLK + WS + data and feed
                    all three to the core (the core is the receiver). Supports
                    --file (write a JSONC config) or --port (drive a sim live).

Pins (defaults): BCLK=0 WS=1 SD=2. In `mic` role BCLK/WS are core OUTPUTS the
script reads from gpio_out, and SD is a core INPUT the script drives via gpio_in
-- so your master firmware must set BCLK/WS as outputs and SD as an input.
"""
import argparse, json, math, socket, sys

ap = argparse.ArgumentParser()
ap.add_argument("--role", choices=["mic", "source"], default="mic",
                help="mic = reactive I2S slave (default); source = generate clk/ws/data")
ap.add_argument("--file", metavar="CONFIG", help="(source only) write JSONC config to this path ('-' = stdout)")
ap.add_argument("--port", type=int, help="driven serve port to drive live")
ap.add_argument("--host", default="127.0.0.1")
ap.add_argument("--freq",  type=float, default=1000.0, help="sine frequency, Hz")
ap.add_argument("--rate",  type=int,   default=44100,  help="sample rate, Hz")
ap.add_argument("--bits",  type=int,   default=24,     help="bits per channel")
ap.add_argument("--fclk",  type=float, default=700e6,  help="fclk in Hz")
ap.add_argument("--frames", type=int,  default=16,     help="stereo samples (mic default kept modest: socket round-trips)")
ap.add_argument("--frames-per-drain", type=int, default=2, help="(source) drain fifo every N frames")
ap.add_argument("--bclk-pin", type=int, default=2)
ap.add_argument("--ws-pin",   type=int, default=1)
ap.add_argument("--sd-pin",   type=int, default=3)
ap.add_argument("--read-bank", type=int, default=-1, help="if >=0, fifo_drain this bank at the end to read what the master captured")
ap.add_argument("--firmware", default="i2s_decode.bin")
ap.add_argument("--fclk-mhz", type=float, default=700.0)
ap.add_argument("--trace", default="i2s.fst")
# I2S framing knobs -- tweak to match your master firmware's convention
ap.add_argument("--bit-delay", type=int, default=1, help="BCLK periods of delay after WS edge before MSB (Philips=1)")
ap.add_argument("--ws-left",  choices=["low","high"], default="low", help="WS level that selects the LEFT channel (Philips: low)")
ap.add_argument("--bit-order", choices=["msb","lsb"], default="msb")
ap.add_argument("--data-edge", choices=["falling","rising"], default="falling", help="BCLK edge on which the mic updates SD")
ap.add_argument("--channel", choices=["left","right","both"], default="both", help="which slot(s) the mic drives data on")
ap.add_argument("--step", type=int, default=0, help="(mic) cycles per poll step; 0 = auto (~BCLK/8)")
ap.add_argument("--align", choices=["i2s","left"], default="i2s", help="(source) framing")
a = ap.parse_args()

BCLK_FREQ = a.rate * a.bits * 2
PERIOD = round(a.fclk / BCLK_FREQ)            # fclk cycles per BCLK period
HALF   = PERIOD // 2
SLOTS_PER_FRAME = a.bits * 2
FULL = (1 << a.bits) - 1

def sample_word(frame):
    """Unsigned two's-complement sample for a given stereo-frame index."""
    peak = (1 << (a.bits - 1)) - 1
    v = int(round(peak * math.sin(2 * math.pi * a.freq * frame / a.rate)))
    v = max(-(1 << (a.bits - 1)), min(peak, v))
    return v & FULL

def signed(v):
    return v - (1 << a.bits) if v >= (1 << (a.bits - 1)) else v

# --------------------------------------------------------------- validation
if a.role == "mic" and a.port is None:
    sys.exit("mic role is reactive and requires --port (a driven serve socket).")
if a.role == "mic" and a.file:
    sys.exit("mic role cannot be precomputed to a file; use --port. (--file is source-only.)")
if a.role == "source" and a.port is None and not a.file:
    sys.exit("source role needs either --file CONFIG or --port N.")

# ============================================================ SOURCE role ===
# (unchanged generator: the script owns the clock and emits BCLK/WS/SD)
if a.role == "source":
    logic_bits, logic_ws, expected = [], [], []
    for f in range(a.frames):
        s = sample_word(f); expected.append(signed(s))
        for ch_ws in (0, 1):
            for b in range(a.bits):
                logic_bits.append((s >> (a.bits - 1 - b)) & 1)
                logic_ws.append(ch_ws)
    def wire_sd(i):
        if a.align == "left": return logic_bits[i]
        return logic_bits[i - 1] if i > 0 else 0
    events = []; last_ws = last_sd = None
    for i in range(len(logic_bits)):
        t0 = i * PERIOD
        events.append((t0, a.bclk_pin, 0))
        ws, sd = logic_ws[i], wire_sd(i)
        if ws != last_ws: events.append((t0, a.ws_pin, ws)); last_ws = ws
        if sd != last_sd: events.append((t0, a.sd_pin, sd)); last_sd = sd
        events.append((t0 + HALF, a.bclk_pin, 1))
    total = len(logic_bits) * PERIOD
    chunk = a.frames_per_drain * SLOTS_PER_FRAME * PERIOD
    chunks, covered = [], 0
    while covered < total: chunks.append(chunk); covered += chunk
    chunks.append(8 * PERIOD)

    if a.file:
        cmds = [{"cmd": "load", "core": 0, "bin": a.firmware},
                {"cmd": "clock", "core": 0, "style": "frac", "freq_hz": int(a.fclk)},
                {"cmd": "start", "cores": [0]},
                {"cmd": "inject", "events": [{"cycle": c, "pin": p, "value": v} for c, p, v in events]}]
        for ch in chunks:
            cmds.append({"cmd": "run", "cycles": ch, "stop_on_trap": False})
            cmds.append({"cmd": "fifo_drain", "bank": a.read_bank if a.read_bank >= 0 else 3})
        cfg = {"fclk_mhz": a.fclk_mhz, "trace": {"file": a.trace}, "commands": cmds}
        hdr = (f"// SOURCE: I2S {a.freq:.0f} Hz, {a.rate} Hz {a.bits}-bit, align={a.align}\n"
               f"// expected samples: {expected}\n")
        f = sys.stdout if a.file == "-" else open(a.file, "w")
        f.write(hdr); json.dump(cfg, f, indent=2); f.write("\n")
        if f is not sys.stdout:
            f.close(); print(f"wrote {a.file} ({len(events)} events)", file=sys.stderr)
        sys.exit(0)

    # source over socket
    print(f"expected samples: {[f'{x:x}' for x in expected]}")
    s = socket.create_connection((a.host, a.port)); f = s.makefile("r")
    print("  <-", f.readline().strip())
    for c, p, v in events: s.sendall(f"inject {c} {p} {v}\n".encode())
    bank = a.read_bank if a.read_bank >= 0 else 3
    for ch in chunks:
        s.sendall(f"run {ch}\n".encode())
        while True:
            l = f.readline()
            if not l: sys.exit("connection closed")
            if l.strip().startswith("ran"): break
        s.sendall(f"fifo_drain {bank}\n".encode())
        l = f.readline().strip()
        while not l.startswith("drain"): l = f.readline().strip()
        for _ in range(int(l.split()[2])):
            hexv = int(f.readline().strip().split()[1], 0)
            print(f"  decoded {signed(hexv & FULL)}")
    s.sendall(b"stop\n"); s.close()
    sys.exit(0)

# =============================================================== MIC role ===
# Reactive I2S slave. The core drives BCLK/WS (read from gpio_out); we drive SD
# (gpio_in). We poll in lock-step, so timing is deterministic regardless of
# socket latency; the only requirement is to step finer than BCLK toggles.
WS_LEFT   = 0 if a.ws_left == "low" else 1
STEP      = a.step if a.step > 0 else max(1, PERIOD // 8)
DATA_FALL = (a.data_edge == "falling")
# generous cap so we bail instead of hanging if the master never clocks
MAX_CYCLES = (SLOTS_PER_FRAME * PERIOD) * (a.frames + 4)

s = socket.create_connection((a.host, a.port)); f = s.makefile("r")
banner = f.readline().strip()
print("  <-", banner)
if "driven" not in banner:
    sys.exit("not a driven serve socket -- the mic needs mode:driven on the sim side.")

def cmd(line):
    s.sendall((line + "\n").encode())

def run(n):
    cmd(f"run {n}")
    while True:
        l = f.readline()
        if not l: sys.exit("connection closed during run")
        if l.strip().startswith("ran"): return

def get_pins():
    cmd("get gpio_out")
    while True:
        l = f.readline()
        if not l: sys.exit("connection closed during get")
        l = l.strip()
        if l.startswith("val"):
            w = int(l.split()[2], 0)
            return (w >> a.bclk_pin) & 1, (w >> a.ws_pin) & 1

def bit_of(word, idx):
    return (word >> (a.bits - 1 - idx)) & 1 if a.bit_order == "msb" else (word >> idx) & 1

def drives_this_channel(is_left):
    return a.channel == "both" or (a.channel == "left") == is_left

# state machine -------------------------------------------------------------
prev_bclk, prev_ws = get_pins()
waiting   = True          # idle until first WS falling edge
cur_sd    = 0
word      = 0
falls     = 0             # data-update edges since last WS edge
frame     = -1            # stereo-frame index (advanced when a LEFT slot opens)
captured  = 0             # left slots seen (== frames emitted)
cycles    = 0
sent_log  = []

cmd(f"set {a.sd_pin} 0")
while captured < a.frames and cycles < MAX_CYCLES:
    cmd(f"set {a.sd_pin} {cur_sd}")
    run(STEP); cycles += STEP
    bclk, ws = get_pins()

    # ---- WS edge: a new channel slot begins ----
    if ws != prev_ws:
        if waiting:
            if prev_ws == 1 and ws == 0:      # FIRST FALLING EDGE -> start
                waiting = False
        if not waiting:
            is_left = (ws == WS_LEFT)
            if is_left:
                frame += 1
                captured += 1
            word  = sample_word(max(frame, 0)) if drives_this_channel(is_left) else 0
            print(f"word: 0x{word:x}")
            falls = 0
            if drives_this_channel(is_left):
                sent_log.append((("L" if is_left else "R"), signed(word)))

    # ---- BCLK data-update edge: present the next bit ----
    edge = (prev_bclk == 1 and bclk == 0) if DATA_FALL else (prev_bclk == 0 and bclk == 1)
    if not waiting and edge:
        idx = falls - a.bit_delay
        cur_sd = bit_of(word, idx) if 0 <= idx < a.bits else 0
        falls += 1

    prev_bclk, prev_ws = bclk, ws

cmd("stop"); s.close()

if cycles >= MAX_CYCLES and captured == 0:
    sys.exit(f"\nno WS activity on pins BCLK={a.bclk_pin}/WS={a.ws_pin} after {cycles} cycles.\n"
             f"is the master firmware running and driving BCLK/WS as outputs?")
print(f"\nmic done: emitted {len(sent_log)} channel-slots over {cycles} cycles "
      f"(step={STEP}, BCLK~{BCLK_FREQ} Hz / {PERIOD} cyc).")
print("sent (channel, value):", sent_log[:16], "..." if len(sent_log) > 16 else "")
print("expected sine:", [signed(sample_word(i)) for i in range(min(a.frames, 16))])