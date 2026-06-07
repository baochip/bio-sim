#!/usr/bin/env python3
"""biowave — launch gtkwave + codezoom together for a named test.

Usage:
    python3 biowave.py test-ws2812
    python3 biowave.py test-ws2812 --port 6510
    python3 biowave.py test-ws2812 --fst path/to/other.fst

Path convention (all relative to the current directory), derived from NAME:
    waveform/NAME.fst        the waveform
    sw/NAME/NAME.dis         the disassembly
    gtkw/bio.gtkw            the (shared) layout file

Any of these can be overridden with a flag. The port defaults to 6502 and can
be overridden with --port or the BIOWAVE_PORT environment variable.

Lifecycle:
    gtkwave runs in the background (GUI). codezoom runs in the foreground and
    owns the terminal. If gtkwave exits, codezoom is told to quit. If codezoom
    exits first (you hit 'q'), gtkwave is left running.

Cross-platform:
    Runs on Linux and Windows from the same file. proc.terminate() maps to
    SIGTERM on POSIX (which codezoom catches and cleans up curses) and to
    TerminateProcess on Windows. The only platform-specific step is the
    terminal-restore fallback at the end, which is a no-op off POSIX.
"""

import argparse
import os
import subprocess
import sys
import threading
from pathlib import Path

DEFAULT_PORT = 6502
GTKW_LAYOUT = "gtkw/bio.gtkw"


def derive_paths(name: str) -> dict[str, Path]:
    return {
        "waveform": Path("waveform") / f"{name}.fst",
        "disassembly": Path("sw") / name / f"{name}.dis",
        "gtkw layout": Path(GTKW_LAYOUT),
    }


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Launch gtkwave + codezoom for a named test."
    )
    parser.add_argument("name", help="test name, e.g. test-ws2812")
    parser.add_argument(
        "--port",
        type=int,
        default=int(os.environ.get("BIOWAVE_PORT", DEFAULT_PORT)),
        help=f"relay port (default {DEFAULT_PORT}, or $BIOWAVE_PORT)",
    )
    parser.add_argument("--fst", help="override the waveform path")
    parser.add_argument("--dis", help="override the disassembly path")
    parser.add_argument("--gtkw", help="override the gtkw layout path")
    parser.add_argument(
        "--gtkwave-bin",
        default=None,
        help="path to a gtkwave binary (e.g. a downloaded AppImage or mingw "
             "build not yet on PATH); defaults to searching PATH for 'gtkwave'",
    )
    parser.add_argument(
        "--codezoom",
        default="./codezoom.py",
        help="path to codezoom.py (default ./codezoom.py)",
    )
    args = parser.parse_args()

    paths = derive_paths(args.name)
    fst = Path(args.fst) if args.fst else paths["waveform"]
    dis = Path(args.dis) if args.dis else paths["disassembly"]
    gtkw = Path(args.gtkw) if args.gtkw else paths["gtkw layout"]

    missing = [
        (label, p)
        for label, p in (("waveform", fst), ("disassembly", dis), ("gtkw layout", gtkw))
        if not p.exists()
    ]
    if missing:
        for label, p in missing:
            print(f"biowave: {label} not found: {p}", file=sys.stderr)
        return 1

    # 1. gtkwave in the background. It's a GUI, so send its chatter to the void
    #    rather than to the terminal where it would corrupt codezoom's display.
    gtkwave_bin = args.gtkwave_bin or "gtkwave"
    gtkwave = subprocess.Popen(
        [gtkwave_bin, str(fst), "-a", str(gtkw), "-u", f"127.0.0.1:{args.port}"],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )

    # 2. codezoom in the foreground. With no stdio redirection it inherits the
    #    terminal, so its curses UI works normally.
    codezoom = subprocess.Popen(
        [sys.executable, args.codezoom, "--file", str(dis)],
    )

    # 3. Watcher thread: when gtkwave dies, tell codezoom to quit. On a side
    #    thread so it never touches the terminal. terminate() is SIGTERM on
    #    POSIX (codezoom catches it) and TerminateProcess on Windows.
    def watch_gtkwave() -> None:
        gtkwave.wait()
        if codezoom.poll() is None:
            codezoom.terminate()

    threading.Thread(target=watch_gtkwave, daemon=True).start()

    # 4. Block until codezoom exits (user quit, or the watcher killed it).
    try:
        codezoom.wait()
    except KeyboardInterrupt:
        codezoom.terminate()
        codezoom.wait()

    # 5. codezoom is gone; gtkwave is deliberately left running (in the q-to-quit
    #    case). With codezoom's SIGTERM handler this is now just a fallback for a
    #    hard kill; harmless if redundant. No-op off POSIX.
    if os.name == "posix" and sys.stdout.isatty():
        subprocess.run(["stty", "sane"])

    return 0


if __name__ == "__main__":
    raise SystemExit(main())