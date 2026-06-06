#!/usr/bin/env python3
"""Interactive keyboard client for bio-sim serve mode.

Press SPACE to toggle a GPIO input pin in real time. Ctrl-C exits.
Output transitions streamed back by the simulation print as they arrive.

  ./clients/interactive.py [port] [pin]
"""
import socket, sys, termios, threading, tty

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 5555
PIN  = int(sys.argv[2]) if len(sys.argv) > 2 else 0

if not sys.stdin.isatty():
    sys.exit("error: this client needs an interactive terminal (a TTY)")

s = socket.create_connection(("127.0.0.1", PORT))

def reader():
    """Print everything the simulation sends (output edges, banners, replies)."""
    f = s.makefile("r")
    for line in f:
        line = line.rstrip("\n")
        if line.startswith("evt"):          # evt <cycle> <signal> <bit> <val>
            print("   <- output:", line)
        else:
            print("   <-", line)

threading.Thread(target=reader, daemon=True).start()

print(f"connected to 127.0.0.1:{PORT}, driving gpio_in[{PIN}]")
print("SPACE = toggle pin,  Ctrl-C = exit")

val = 0
fd  = sys.stdin.fileno()
old = termios.tcgetattr(fd)
try:
    # cbreak: keys arrive immediately (no Enter, no echo) but ISIG stays on,
    # so Ctrl-C still raises KeyboardInterrupt rather than arriving as a byte.
    tty.setcbreak(fd)
    while True:
        ch = sys.stdin.read(1)
        if ch == "":                        # stdin closed
            break
        if ch == " ":
            val ^= 1
            s.sendall(f"set {PIN} {val}\n".encode())
            print(f"-> set gpio_in[{PIN}] = {val}")
        # any other key is ignored
except KeyboardInterrupt:
    pass
finally:
    termios.tcsetattr(fd, termios.TCSADRAIN, old)
    try:
        s.sendall(b"stop\n")
    except OSError:
        pass
    s.close()
    print("\nstopped")