#!/usr/bin/env python3
"""Analyze QEMU's WAV capture of guest audio. QEMU writes only while a guest
stream runs, so test tones are back to back: split them by dominant
frequency/level in 100 ms blocks, then report each segment's duration, exact
frequency (FFT over the segment) and RMS per channel."""
import sys, struct
import numpy as np

raw = open(sys.argv[1], 'rb').read()
# QEMU only finalizes the RIFF sizes on a clean exit: read the fmt chunk, take the rest as data.
ch, rate = struct.unpack_from('<HI', raw, 22)
sw = struct.unpack_from('<H', raw, 34)[0] // 8
body = raw[44:]
body = body[:len(body) // (ch * sw) * ch * sw]
data = np.frombuffer(body, dtype=np.int16 if sw == 2 else np.int32).reshape(-1, ch).astype(np.float64)
n = len(data)
print(f"file: {n} frames, {n/rate:.2f} s, {rate} Hz, {ch} ch, {sw*8} bit")

blk = rate // 10
def describe(x):
    rms = np.sqrt(np.mean(x ** 2))
    if rms < 20:
        return 0, rms
    spec = np.abs(np.fft.rfft(x * np.hanning(len(x))))
    return (np.argmax(spec[1:]) + 1) * rate / len(x), rms

labels = []
for i in range(n // blk):
    x = data[i*blk:(i+1)*blk]
    key = []
    for c in range(ch):
        f, rms = describe(x[:, c])
        key.append((round(f / 50) * 50, int(np.log2(rms + 1) * 2)))  # 50 Hz bins, 3 dB level bins
    labels.append(tuple(key))
segs = []
for i, k in enumerate(labels):
    if segs and segs[-1][2] == k:
        segs[-1][1] = i + 1
    else:
        segs.append([i, i + 1, k])
# absorb 1-block glitches (block boundaries straddling two tones)
segs = [s for s in segs if s[1] - s[0] > 1]
for k, (a, b, _) in enumerate(segs):
    seg = data[a*blk:b*blk]
    parts = []
    for c in range(ch):
        f, rms = describe(seg[:, c])
        parts.append(f"ch{c} {f:7.1f} Hz rms {rms:7.0f}" if f else f"ch{c}  silent  rms {rms:7.0f}")
    print(f"segment {k+1:2}: t={a*blk/rate:6.2f}s dur={(b-a)*blk/rate:5.2f}s  " + "   ".join(parts))
