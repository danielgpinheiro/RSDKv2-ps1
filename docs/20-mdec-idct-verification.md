# FMV MDEC — IDCT/VLC decode verified by software (2026-09-22)

## Conclusion
The C++ VLC + dequant + IDCT pipeline is **correct**. A pure-Python byte-faithful
decoder of `Intro.str` frame 119 reproduces the source Y plane to **MAE 10.3**
(Cb 5.6, Cr 9.5). So the Stage-3 driver (`ps1/mdec.cpp` + `ps1/video.cpp`) decodes
correctly; any residual emulator mismatch is in the MDEC *hardware* path
(registers/DMA/output ordering), not the bitstream decode.

## The two facts that unblocked it
1. **MDEC IDCT** = `scale.T @ F @ scale / 2^32`, where `scale` is the MDEC(3) matrix
   `dct[y*8+x] = 2·cos(π(2x+1)y/16)·2^14` (row 0 = √2·2^14 = 23170), indexed
   `scale[frequency][spatial]`. It is the **transpose** form, normalized by **2^32**
   (2^15·2^15 coefficient scale × the butterfly's /4). Right form → frame-0 MAE ≈ 0,
   frame-119 MAE ≈ 10. Wrong orientation or K (2^26/2^28/2^30) → MAE 100+.
2. **VLC table layout** — `DECDCTTAB.ac00[i]` is `ac[8192 + i]`, i.e. the `ac00`
   table is the tail of the flat 8704-entry array (offset 8192), *not* `ac[i]`.
   This was the bug in the Python diagnostic (the C++ `->ac00[...]` was already correct).

## Software decoder
`/Users/danielpinheiro/.hermes/cache/scratch/fmv_decode.py` (VLC+dequant+IDCT,
byte-faithful port of `ps1/mdec.cpp`) + `fmv_calibrate.py` (K/orientation sweep) +
`fmv_compare.py` / `fmv_shot_check.py`. Reuse before touching the hardware path.

## Still open
Emulator screenshot of frame 119 currently comes back **all black** (0 non-zero px);
the Lua `takeScreenShot()` writes `.raw` but `.meta` is empty (`ss.width` is nil in
this PCSX-Redux build) and no `syscall_puts` output reaches `/tmp/pcsx_run.log`.
So the earlier "frame 119 MAE 73.27" could not be re-confirmed this session — the
self-test video is not drawing. Next: get a non-black frame-119 screenshot (serial
capture / boot check / longer decode delay), then compare its luminance against the
software Y plane to settle whether the 73.27 was real or a stale measurement.
