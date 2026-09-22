#!/usr/bin/env python3
"""Dump Intro.rsv frames as a YUV4MPEG2 (.y4m) stream for psxavenc."""
import sys, struct
import numpy as np

sys.path.insert(0, "/Users/danielpinheiro/Files/Projetos_Infaliveis/PS1/RSDKv2-ps1")
from convert_fmv import parse_rsv, rgb_to_ycbcr

def main():
    frames = parse_rsv("/Users/danielpinheiro/Files/Projetos_Infaliveis/PS1/RSDKv2-ps1/build/iso/Data/Sprites/Videos/Intro.rsv")
    w, h = frames[0][2], frames[0][3]
    out = open("/tmp/intro.y4m", "wb")
    out.write(f"YUV4MPEG2 W{w} H{h} F15:1 Ip A1:1 C420jpeg\n".encode())
    for i, (palette, indices, ww, hh) in enumerate(frames):
        idx = np.frombuffer(indices, dtype=np.uint8).reshape(hh, ww)
        rgb = np.zeros((hh, ww, 3), dtype=np.uint8)
        for c in range(3):
            lut = np.zeros(256, dtype=np.uint8)
            for k in range(0x80):
                lut[k] = palette[k * 3 + c]
            rgb[:, :, c] = lut[idx]
        ycbcr = rgb_to_ycbcr(rgb)
        y = ycbcr[:, :, 0]
        cb = ycbcr[0::2, 0::2, 1]
        cr = ycbcr[0::2, 0::2, 2]
        out.write(b"FRAME\n")
        out.write(y.astype(np.uint8).tobytes())
        out.write(cb.astype(np.uint8).tobytes())
        out.write(cr.astype(np.uint8).tobytes())
    out.close()
    print(f"wrote /tmp/intro.y4m ({len(frames)} frames, {w}x{h})")

if __name__ == "__main__":
    main()
