#!/usr/bin/env python3
"""Minimal real-time client for bio-sim serve mode.

Drives one GPIO input pin up/down once per second and prints the output
transitions the simulation streams back. This is the language-agnostic seam:
it speaks newline-delimited text over TCP, so the same protocol works from
Rust, C, shell + nc, etc.

  ./clients/toggle.py [port] [pin]
"""
import socket, sys, threading, time

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 5555
PIN  = int(sys.argv[2]) if len(sys.argv) > 2 else 0

s = socket.create_connection(("127.0.0.1", PORT))
print(f"connected to 127.0.0.1:{PORT}, driving gpio_in[{PIN}]")

def reader():
    f = s.makefile("r")
    for line in f:
        line = line.rstrip("\n")
        if line.startswith("evt"):          # evt <cycle> <signal> <bit> <val>
            print("   <- output:", line)
        else:                                # banners (#) and `val` replies
            print("   <-", line)

threading.Thread(target=reader, daemon=True).start()

try:
    val = 0
    while True:
        val ^= 1
        s.sendall(f"set {PIN} {val}\n".encode())
        print(f"-> set gpio_in[{PIN}] = {val}")
        time.sleep(1.0)
except KeyboardInterrupt:
    s.sendall(b"stop\n")
    time.sleep(0.2)
    s.close()
    print("\nstopped")
