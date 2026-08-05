# nec_rx

Receives NEC infrared frames on one pin. The core clock is an external clock on
that pin, so every quantum is a rising edge - the end of a burst. The program
measures the interval since the previous edge, classifies it, and pushes one
word per decoded frame to FIFO0.

## Host interface

At startup the host pushes six words to FIFO0: `1 << pin`, then the five
interval boundaries in BIO clock ticks. Sending the boundaries rather than
baking them in keeps decode correct at whatever the BIO clock turns out to be.

Each frame comes back as one FIFO0 word - little-endian
`[address lo, address hi, command, ~command]`, or `0xffffffff` when the remote
is repeating (button held). The FIFO is 8 deep, so it buffers about a second of
button presses.

## Intervals

An interval is a gap plus the 560us burst that ends it, which keeps the four
symbols far enough apart to separate with fixed boundaries:

| interval | meaning | nominal |
|---|---|---|
| below 700us | noise | - |
| 700-1685us | bit 0 | 1120us |
| 1685-2530us | bit 1 | 2250us |
| 2530-3935us | repeat frame | 2810us |
| 3935-6500us | leader | 5060us |
| above 6500us | idle between frames | - |

Bits arrive LSB first and shift in at bit 31, so a completed frame reads out in
the byte order above.

## Building and running

```
cd sw
python3 -m ziglang build "-Dmodule=nec_rx"
cd ..
./container-run configs/nec_rx.jsonc
```

`configs/nec_rx.jsonc` injects one frame (address `85 fe`, command `43`) plus a
repeat, then drains FIFO0. A good run ends with:

```
[fifo_drain]   [0] = 0xbc43fe85  (s16=-379)
[fifo_drain]   [1] = 0xffffffff  (s16=-1)
```

`clients/nec.py` regenerates the config for other codes:

```
python3 clients/nec.py --command 0x46 --repeats 3 --file configs/nec_rx.jsonc
```

The config declares an `fclk_mhz` of 7, so a 67.5ms frame is 470k cycles instead
of the 47M it would be at 700MHz - seconds of Verilator rather than minutes. The
boundaries are computed in ticks from the same figure, so they scale with it.
