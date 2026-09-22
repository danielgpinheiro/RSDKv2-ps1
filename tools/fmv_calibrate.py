import numpy as np, struct, re, sys
exec(open("/Users/danielpinheiro/.hermes/cache/scratch/fmv_decode.py").read().split("# ---- Test")[0])

str_path = "/Users/danielpinheiro/Files/Projetos_Infaliveis/PS1/RSDKv2-ps1/build/iso/Data/Sprites/Videos/Intro.str"
data = open(str_path, 'rb').read()

def get_frame_blocks(fn):
    frame = data[fn*20480:(fn+1)*20480]
    bs = b''.join(frame[i*2048+32:(i+1)*2048] for i in range(10))
    return decode_frame(bs, 0)[0]

def assemble(blocks, scale, K, orient):
    Y  = np.zeros((160,256)); Cb = np.zeros((80,128)); Cr = np.zeros((80,128))
    for mb in range(160):
        fx = mb//10; fy = mb%10
        for t in range(6):
            F = blocks[mb*6+t]
            s = (scale.T @ F @ scale) / K if orient=='T' else (scale @ F @ scale.T) / K
            s = s + 128.0
            if t==0: Cr[fy*8:(fy+1)*8, fx*8:(fx+1)*8]=s
            elif t==1: Cb[fy*8:(fy+1)*8, fx*8:(fx+1)*8]=s
            elif t==2: Y[fy*16:fy*16+8, fx*16:fx*16+8]=s
            elif t==3: Y[fy*16:fy*16+8, fx*16+8:fx*16+16]=s
            elif t==4: Y[fy*16+8:fy*16+16, fx*16:fx*16+8]=s
            else: Y[fy*16+8:fy*16+16, fx*16+8:fx*16+16]=s
    return Y, Cb, Cr

def load_src_y4m(fn):
    y4m = open("/tmp/intro.y4m","rb").read()
    W,H = 256,160
    pos = y4m.index(b"FRAME")
    for f in range(fn+1):
        nl = y4m.index(b"\n", pos); pos = nl+1
    y = np.frombuffer(y4m[pos:pos+W*H],dtype=np.uint8).reshape(H,W).astype(np.float64); pos+=W*H
    cb = np.frombuffer(y4m[pos:pos+(W//2)*(H//2)],dtype=np.uint8).reshape(H//2,W//2).astype(np.float64); pos+=(W//2)*(H//2)
    cr = np.frombuffer(y4m[pos:pos+(W//2)*(H//2)],dtype=np.uint8).reshape(H//2,W//2).astype(np.float64)
    return y, cb, cr

# Calibrate K on frame 0 (DC-only, works in emulator)
b0 = get_frame_blocks(0)
sy0, scb0, scr0 = load_src_y4m(0)
for orient in ['T','N']:
    for K in [2**26, 2**28, 2**30, 2**32]:
        Y, Cb, Cr = assemble(b0, dct_scale, K, orient)
        m = (np.abs(Y-sy0).mean()*4 + np.abs(Cb-scb0).mean() + np.abs(Cr-scr0).mean())/6
        print(f"frame0 orient={orient} K=2^{round(np.log2(K))} -> MAE {m:.1f}")

# Now frame 119 with the best config
for orient, K in [('T', 2**32), ('N', 2**32), ('T', 2**30), ('N', 2**30)]:
    b = get_frame_blocks(119)
    sy, scb, scr = load_src_y4m(119)
    Y, Cb, Cr = assemble(b, dct_scale, K, orient)
    my = np.abs(Y-sy).mean(); mc = np.abs(Cb-scb).mean(); mr = np.abs(Cr-scr).mean()
    print(f"frame119 orient={orient} K=2^{round(np.log2(K))} -> Y {my:.1f} Cb {mc:.1f} Cr {mr:.1f}")
