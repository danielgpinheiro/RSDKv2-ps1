"""SPU-ADPCM decoder (psx-spx "SPU ADPCM Samples") for build-time checks and evidence."""
import numpy as np

FILTERS = [(0, 0), (60, 0), (115, -52), (98, -55), (122, -60)]


def decode(data):
    out = []
    s1 = s2 = 0
    for b in range(0, len(data) - 15, 16):
        blk = data[b:b + 16]
        shift, flt = blk[0] & 0x0F, min(blk[0] >> 4, 4)
        f0, f1 = FILTERS[flt]
        for i in range(28):
            nib = (blk[2 + i // 2] >> ((i & 1) * 4)) & 0xF
            if nib >= 8:
                nib -= 16
            s = ((nib << 12) >> shift) + ((s1 * f0 + s2 * f1 + 32) >> 6)
            s = max(-32768, min(32767, s))
            out.append(s)
            s2, s1 = s1, s
        if blk[1] & 1 and not blk[1] & 2:  # end without repeat
            break
    return np.array(out, dtype=float)
