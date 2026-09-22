import numpy as np, struct, re, sys
exec(open("/Users/danielpinheiro/.hermes/cache/scratch/fmv_decode.py").read().split("# ---- Test")[0])

# ---- Re-extract frame 119 + decode (self-contained) ----
str_path = "/Users/danielpinheiro/Files/Projetos_Infaliveis/PS1/RSDKv2-ps1/build/iso/Data/Sprites/Videos/Intro.str"
data = open(str_path, 'rb').read()
frame119 = data[119*20480:(119+1)*20480]
bs = b''.join(frame119[i*2048+32:(i+1)*2048] for i in range(10))
blocks, remaining, qs_raw = decode_frame(bs, 0)
print("frame 119 blocks:", len(blocks), "quant_scale:", qs_raw)


# ---- Assemble the decoded frame 119 into Y/Cb/Cr planes ----
scale = dct_scale  # 8x8 float, row-major dct[y][x]

def idct(F):
    return (scale @ F @ scale.T) / (2**28)

Y  = np.zeros((160, 256), dtype=np.float64)
Cb = np.zeros((80, 128), dtype=np.float64)
Cr = np.zeros((80, 128), dtype=np.float64)

for mb in range(160):
    fx = mb // 10
    fy = mb % 10
    for t in range(6):
        blk = blocks[mb*6 + t]
        s = idct(blk) + 128.0  # undo the -128 level shift
        if t == 0:      Cr[fy*8:(fy+1)*8, fx*8:(fx+1)*8] = s
        elif t == 1:    Cb[fy*8:(fy+1)*8, fx*8:(fx+1)*8] = s
        elif t == 2:    Y[fy*16:fy*16+8, fx*16:fx*16+8] = s
        elif t == 3:    Y[fy*16:fy*16+8, fx*16+8:fx*16+16] = s
        elif t == 4:    Y[fy*16+8:fy*16+16, fx*16:fx*16+8] = s
        else:           Y[fy*16+8:fy*16+16, fx*16+8:fx*16+16] = s

# ---- Load the Y4M source frame 119 ----
y4m = open("/tmp/intro.y4m", "rb").read()
# find the FRAME marker + parse the header
hdr_end = y4m.index(b"FRAME") + 5
while y4m[hdr_end] in b" \t": hdr_end += 1
hdr_end += len(b"W256 H160 F15:1 Ip A0:0 C420jpeg XYSCSS=420JPEG\n")  # rough; find newline instead
# better: parse frame headers generically
def parse_y4m(data, frame_idx):
    p = data.index(b"FRAME\n")
    # header up to first \n after FRAME
    # YUV420p: Y plane W*H, then Cb (W/2)*(H/2), Cr
    W, H = 256, 160
    pos = 0
    # skip stream header (before first FRAME)
    f0 = data.index(b"FRAME")
    pos = f0
    for f in range(frame_idx + 1):
        nl = data.index(b"\n", pos)
        pos = nl + 1  # skip the FRAME line
    y = np.frombuffer(data[pos:pos+W*H], dtype=np.uint8).reshape(H, W).astype(np.float64)
    pos += W*H
    cb = np.frombuffer(data[pos:pos+(W//2)*(H//2)], dtype=np.uint8).reshape(H//2, W//2).astype(np.float64)
    pos += (W//2)*(H//2)
    cr = np.frombuffer(data[pos:pos+(W//2)*(H//2)], dtype=np.uint8).reshape(H//2, W//2).astype(np.float64)
    return y, cb, cr

sy, scb, scr = parse_y4m(y4m, 119)
print("source frame 119 loaded:", sy.shape, scb.shape, scr.shape)

# ---- Compare (MAE) ----
def mae(a, b): return np.abs(a - b).mean()

print("Y  MAE:", round(mae(Y, sy), 2))
print("Cb MAE:", round(mae(Cb, scb), 2))
print("Cr MAE:", round(mae(Cr, scr), 2))
print("overall MAE (Y+Cb+Cr weighted):", round((mae(Y,sy)*4 + mae(Cb,scb) + mae(Cr,scr))/6, 2))
