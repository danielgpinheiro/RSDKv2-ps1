import numpy as np, struct, os

# ---- Software decode of frame 119 -> Y plane (luminance) ----
exec(open("/Users/danielpinheiro/.hermes/cache/scratch/fmv_decode.py").read().split("# ---- Test")[0])
exec(open("/Users/danielpinheiro/.hermes/cache/scratch/fmv_calibrate.py").read().split("# Calibrate")[0])
b = get_frame_blocks(119)
Y, Cb, Cr = assemble(b, dct_scale, 2**32, 'T')
print("software frame119 Y mean:", round(Y.mean(),1), "std:", round(Y.std(),1))

# ---- Read the emulator screenshot (BGR555, 320x239) ----
shot = "/tmp/ps1_shot.raw"
if not os.path.exists(shot):
    print("NO SCREENSHOT at", shot)
else:
    raw = open(shot, 'rb').read()
    print("screenshot bytes:", len(raw), "-> px:", len(raw)//2)
    px = np.frombuffer(raw, dtype='<u2').reshape(239, 320)  # little-endian 16-bit BGR555
    # video is 256x160 at (32,40)
    v = px[40:40+160, 32:32+256].astype(np.int32)
    r = (v & 0x1F) << 3
    g = ((v >> 5) & 0x1F) << 3
    bb = ((v >> 10) & 0x1F) << 3
    lum = 0.299*r + 0.587*g + 0.114*bb
    print("screenshot frame119 lum mean:", round(lum.mean(),1), "std:", round(lum.std(),1))

    # compare luminance (structure) vs software Y
    mae_y = np.abs(lum - Y).mean()
    print("LUMINANCE MAE (structure check):", round(mae_y, 2))

    # also try shifted bbox in case of off-by-N
    for dy in (-2,-1,0,1,2):
        for dx in (-2,-1,0,1,2):
            vv = px[40+dy:40+dy+160, 32+dx:32+dx+256].astype(np.int32)
            rr = (vv & 0x1F) << 3
            gg = ((vv >> 5) & 0x1F) << 3
            bb2 = ((vv >> 10) & 0x1F) << 3
            ll = 0.299*rr + 0.587*gg + 0.114*bb2
            m = np.abs(ll - Y).mean()
            if m < mae_y:
                mae_y = m
                print(f"  better at dx={dx} dy={dy}: MAE {m:.2f}")
    print("best LUMINANCE MAE:", round(mae_y, 2))
