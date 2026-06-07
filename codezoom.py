#!/usr/bin/env python3

import argparse
import curses
import socket
import select
import sys
import time
import signal

# This requires a GTKwave built from https://github.com/baochip/gtkwave/tree/udp-send

def parse_args():
    parser = argparse.ArgumentParser(
        description="Daemon that listens to a patched GTKWave to display code disassembly listings"
    )
    parser.add_argument(
        "--file", required=False, help="The file to zoom around", type=str, default="./listings/load.lst"
    )
    parser.add_argument(
        "--port", required=False, help="The port to listen on", type=int, default=6502
    )
    parser.add_argument(
        "--signal", required=False, help="Name of the program counter signal", type=str, default="dbg_pc"
    )
    return parser.parse_args()

def _graceful_quit(signum, frame):
    try:
        curses.endwin()
    except Exception:
        pass
    sys.exit(0)

def main(stdscr, args):
    curses.noecho()
    curses.cbreak()
    stdscr.keypad(True)
    stdscr.nodelay(True)

    if hasattr(signal, "SIGTERM"):
        signal.signal(signal.SIGTERM, _graceful_quit)

    udp_socket = socket.socket(family=socket.AF_INET, type=socket.SOCK_DGRAM)
    udp_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        udp_socket.bind(("127.0.0.1", args.port))
    except OSError as e:
        raise SystemExit(f"Failed to bind UDP socket on port {args.port}: {e}") from e

    try:
        with open(args.file, 'r') as file_handle:
            user_text = file_handle.readlines()
    except OSError as e:
        raise SystemExit(f"Failed to open file '{args.file}': {e}") from e

    rows, cols = stdscr.getmaxyx()

    text_offset = 0
    stdscr.addstr(0, 0, "listening")
    stdscr.refresh()
    time.sleep(1)

    string = None
    nstring = None
    try:
        while True:
            readable, _writeable, _exceptional = select.select([udp_socket], [], [], 0.1)
            if readable:
                data = readable[0].recv(64)
                if data and data[0] == 1: # name type
                    strlen = data[1]
                    nstring = data[2:2 + strlen].decode('utf-8', errors='replace')
                    sigmatch = nstring.startswith(args.signal)
                    if sigmatch and string:
                        # Find the line number that contains this string
                        for index, line in enumerate(user_text):
                            if line.lower().lstrip().startswith(string.lower()):
                                text_offset = index
                                break

                        # Centre the matched line on the screen
                        rows, cols = stdscr.getmaxyx()
                        start_line = max(0, text_offset - rows // 2)

                        stdscr.clear()
                        for i in range(rows - 1):
                            line_index = start_line + i
                            if line_index >= len(user_text):
                                break
                            line_text = user_text[line_index].rstrip()
                            attr = curses.A_REVERSE if line_index == text_offset else curses.A_NORMAL
                            try:
                                stdscr.addstr(i, 0, line_text, attr)
                            except curses.error:
                                pass  # Ignore lines that don't fit (e.g. at terminal edge)
                    try:
                        if nstring and string:
                            debug = f"Packet: {nstring} {string}"
                            stdscr.addstr(rows - 1, 0, debug.ljust(cols - 1), curses.A_REVERSE)
                    except curses.error:
                        pass

                if data and data[0] == 2:  # value type
                    strlen = data[1]
                    string = data[2:2 + strlen].decode('utf-8', errors='replace')
                    try:
                        offset = int(string, 16)
                        string = hex(offset).lstrip('0x') or '0'
                    except ValueError:
                        pass

            stdscr.refresh()

            try:
                key = stdscr.getkey()
                if key == 'q':
                    break
            except curses.error:
                pass  # No key pressed; nodelay raises an error instead of returning None

    finally:
        udp_socket.close()


if __name__ == "__main__":
    args = parse_args()
    try:
        curses.wrapper(main, args)
    except KeyboardInterrupt:
        pass  # Clean exit on Ctrl+C; curses.wrapper has already restored the terminal
    except SystemExit as e:
        sys.exit(e)