#!/usr/bin/env python3
"""Audio evidence from SPU capture buffers (phase 5).

The SPU writes its CD/XA input (L at 0x000, R at 0x400: 512 x s16 at 44.1 kHz, a ring) and voices
1/3 (0x800 / 0xC00) into the first 4 KB of SPU RAM. Dump them from GDB through the port's debug
service (set g_ps1SpuDumpAddr = 0, g_ps1SpuDumpLen = 4096; next frame: dump binary memory ...
&TileGfx[0] &TileGfx[4096]) and find where the captured samples sit in a reference track.

Reference: any audio ffmpeg reads (Ogg/WAV/.xa via --xa CHANNEL for Music.xa, decoded exactly
as the drive does with xa_adpcm.py). Files on the loose disc tree may be XOR 0xFF (handled).

PCSX-Redux caveats (docs/04): under -interpreter the emulated CD runs ~20x slower than the host
audio thread that drains the XA FIFO, so many captures are exact zeros ("silent") and a capture's
timestamp can lag by up to ~0.25 s; voice captures hold only the last ~45 samples. A capture that
has audio still identifies the track and its position (correlation > 0.9).

Usage: capture_match.py CAPTURE.bin REFERENCE [--xa CHANNEL] [--source cd|v1|v3] [--from S] [--to S]
"""
import argparse, os, subprocess, sys, tempfile
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))


def load_reference(path, xa_channel):
    if xa_channel is not None:
        import xa_adpcm
        L, _ = xa_adpcm.decode_sectors(list(xa_adpcm.channel_sectors(open(path, 'rb').read(), xa_channel)))
        t = np.arange(0, len(L) / 37800, 1 / 44100)
        return np.interp(t, np.arange(len(L)) / 37800, L)
    d = open(path, 'rb').read()
    if d[:4] not in (b'OggS', b'RIFF'):
        d = bytes(b ^ 0xFF for b in d)
    with tempfile.TemporaryDirectory() as tmp:
        src = os.path.join(tmp, 'in' + os.path.splitext(path)[1])
        open(src, 'wb').write(d)
        raw = subprocess.run(['ffmpeg', '-loglevel', 'error', '-i', src, '-ac', '1', '-ar', '44100', '-f', 's16le', '-'],
                             capture_output=True, check=True).stdout
    return np.frombuffer(raw, dtype='<i2').astype(float)


def match(cap, ref):
    ref = ref - ref.mean()
    e = None
    best = (0.0, 0)
    for rot in range(0, len(cap), 4):
        seg = np.roll(cap, -rot)[16:len(cap) - 16]
        seg = seg - seg.mean()
        ns = np.linalg.norm(seg)
        if ns == 0 or len(ref) < len(seg):
            return None
        c = np.correlate(ref, seg, mode='valid')
        if e is None:
            e = np.sqrt(np.convolve(ref ** 2, np.ones(len(seg)), mode='valid'))
        r = c / (ns * np.maximum(e, 1e-9))
        k = int(np.argmax(r))
        if r[k] > best[0]:
            best = (float(r[k]), k)
    return best


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('capture')
    ap.add_argument('reference')
    ap.add_argument('--xa', type=int, default=None, help='reference is Music.xa: decode this channel')
    ap.add_argument('--source', default='cd', choices=('cd', 'v1', 'v3'))
    ap.add_argument('--from', dest='t0', type=float, default=0)
    ap.add_argument('--to', dest='t1', type=float, default=None)
    a = ap.parse_args()
    off = {'cd': 0, 'v1': 0x800, 'v3': 0xC00}[a.source]
    cap = np.frombuffer(open(a.capture, 'rb').read()[off:off + 0x400], dtype='<i2').astype(float)
    nz = np.count_nonzero(cap)
    if a.source != 'cd':
        cap = cap[:nz] if nz else cap  # PCSX-Redux keeps only the latest mix chunk
    ref = load_reference(a.reference, a.xa)
    lo = int(a.t0 * 44100)
    hi = len(ref) if a.t1 is None else min(len(ref), int(a.t1 * 44100))
    if cap.std() < 1:
        print('%s: silent capture (PCSX-Redux XA FIFO empty at dump time, see docstring)' % a.capture)
        return
    b = match(cap, ref[lo:hi])
    print('%s: rms %.0f | best correlation %.4f at %.3f s of %s' % (a.capture, cap.std(), b[0], (lo + b[1]) / 44100,
                                                                  os.path.basename(a.reference)))


if __name__ == '__main__':
    main()
