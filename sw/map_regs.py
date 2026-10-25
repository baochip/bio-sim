#!/usr/bin/env python3
"""
Substitute RISC-V ABI/C-style register names with BIO register names
"""

import re
import sys

# Full ABI -> absolute register mapping
ABI_TO_ABS = {
    "zero": "x0",
    "ra":   "x1",
    "sp":   "x2",
    "gp":   "x3",
    "tp":   "x4",
    "t0":   "x5",
    "t1":   "x6",
    "t2":   "x7",
    "s0":   "x8",   # also fp
    "fp":   "x8",
    "s1":   "x9",
    "a0":   "x10",
    "a1":   "x11",
    "a2":   "x12",
    "a3":   "x13",
    "a4":   "x14",
    "a5":   "x15",
    "a6":   "fifo0",
    "a7":   "fifo1",
    "s2":   "fifo2",
    "s3":   "fifo3",
    "s4":   "quant",
    "s5":   "gpio",
    "s6":   "gpset",
    "s7":   "gpclrn",
    "s8":   "dirout",
    "s9":   "dirin",
    "s10":  "gpmask",
    "s11":  "evmask",
    "t3":   "evset",
    "t4":   "evclr",
    "t5":   "evwait",
    "t6":   "idclk",
}

# Regex: match ABI names only when surrounded by non-alphanumeric/underscore chars
# Sorted by length descending so e.g. "s10" matches before "s1"
_PATTERN = re.compile(
    r'(?<![a-zA-Z0-9_])(' +
    '|'.join(re.escape(k) for k in sorted(ABI_TO_ABS, key=len, reverse=True)) +
    r')(?![a-zA-Z0-9_:])'
)


def substitute(line: str) -> str:
    # Don't touch comments or string literals—stop at '#' or '"'
    comment_start = len(line)
    for marker in ('#', '"'):
        idx = line.find(marker)
        if idx != -1:
            comment_start = min(comment_start, idx)

    code_part    = line[:comment_start]
    comment_part = line[comment_start:]

    replaced = _PATTERN.sub(lambda m: ABI_TO_ABS[m.group(1)], code_part)
    return replaced + comment_part


def process(text: str) -> str:
    return "\n".join(substitute(line) for line in text.splitlines())


if __name__ == "__main__":
    if len(sys.argv) == 1 or sys.argv[1] in ("-h", "--help"):
        print("Usage: riscv_regs.py <input.s> [output.s]")
        print("       cat input.s | riscv_regs.py -")
        sys.exit(0)

    src = sys.argv[1]
    dst = sys.argv[2] if len(sys.argv) > 2 else None

    if src == "-":
        text = sys.stdin.read()
    else:
        with open(src) as f:
            text = f.read()

    result = process(text)

    if dst:
        with open(dst, "w") as f:
            f.write(result)
        print(f"Written to {dst}")
    else:
        print(result)

