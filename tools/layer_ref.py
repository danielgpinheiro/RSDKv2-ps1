#!/usr/bin/env python3
"""Pixel-exact reference for the PS1 tile-layer renderer (phase 3c).

Renders the stage tile layers the way the PC engine's DrawHLineScrollLayer does (per line:
HParallax line scroll, BG Y parallax + autoscroll, vertical/horizontal wrap, visual plane,
X/Y flips, index 0 transparent), in DrawStageGfx order, from the stage files plus a state
dump taken by tools/layer_capture.sh. Reference indices are mapped through the master CLUT
dumped from VRAM (0,240) and compared with both framebuffers.

Usage: layer_ref.py STAGE_DIR STATE_FILE VRAM_BIN [OUT_PREFIX] [--layers 0,1,2,3] [--tint]
  STAGE_DIR   e.g. build/iso/Data/Stages/SSZ  (16x16Tiles.raw, 128x128Tiles.bin,
              Backgrounds.bin, Act<N>.bin)
  STATE_FILE  "S name value" lines + HParallax dump (layer_capture.sh)
  --layers    layer slots to render (default: all active); e.g. 2,3 = FG only
  --tint      the frame had a full-screen DrawTintRect(tint table 0): expect clut[tint[i]]
              (STATE_FILE.tint = TintLookupTable1 dump)
  --circle GIF  title card colour circle (TitleCard.txt state 9): tinted outside the window
              and on the opaque texels of the scaled mask (frame 111 of GIF at 256,256; PC
              DrawScaledTintMask stepping), from S tcScale
Prints per framebuffer: drawn pixels, mismatches, undrawn-pixel colours. Writes
OUT_PREFIX_ref.png and OUT_PREFIX_diff.png (red = mismatch) when OUT_PREFIX is given.
"""
import os, struct, sys

W, H = 320, 240
CHUNK_W = 0x100          # TILELAYER_CHUNK_W (PS1)
SCROLL_MAX = 0x10 * 128  # TILELAYER_SCROLL_MAX (PS1)
PARALLAX_COUNT = 0x100
PS1_LAYER_COUNT = 4


def cdiv_mod(a, b):
    """C remainder (sign of the dividend)."""
    r = abs(a) % abs(b)
    return -r if a < 0 else r


def load_stage(d, act):
    raw = open(os.path.join(d, '16x16Tiles.raw'), 'rb').read()
    tilegfx = raw[4 + 768:]
    t = open(os.path.join(d, '128x128Tiles.bin'), 'rb').read()
    tiles = []
    for i in range(len(t) // 3):
        e0, e1 = t[3 * i], t[3 * i + 1]
        e0 &= 0x3F
        plane = e0 >> 4
        e0 &= 0xF
        direction = e0 >> 2
        e0 &= 3
        tiles.append((e1 + (e0 << 8), direction, plane))
    layouts = {}
    a = open(os.path.join(d, 'Act%d.bin' % act), 'rb').read()
    n = a[0]
    i = 1 + n
    active = list(a[i:i + 4])
    mid = a[i + 4]
    xs, ys = a[i + 5], a[i + 6]
    i += 7
    fg = [[0] * xs for _ in range(ys)]
    for y in range(ys):
        for x in range(xs):
            fg[y][x] = (a[i] << 8) + a[i + 1]
            i += 2
    layouts[0] = dict(xsize=xs, ysize=ys, tiles=fg, lineScroll=[0] * SCROLL_MAX, pf=0)
    b = open(os.path.join(d, 'Backgrounds.bin'), 'rb').read()
    j = 0

    def rb():
        nonlocal j
        v = b[j]
        j += 1
        return v
    lc = rb()
    hc = rb()
    hp = [(rb(), rb(), rb()) for _ in range(hc)]
    vc = rb()
    [(rb(), rb(), rb()) for _ in range(vc)]
    for L in range(1, lc + 1):
        xs, ys, ty, pf, sp = rb(), rb(), rb(), rb(), rb()
        ls = []
        while True:
            c = rb()
            if c == 0xFF:
                c1 = rb()
                if c1 == 0xFF:
                    break
                cnt = rb()
                ls += [c1] * (cnt - 1)
            else:
                ls.append(c)
        ls = (ls + [0] * SCROLL_MAX)[:SCROLL_MAX]
        grid = [[rb() for _ in range(xs)] for _ in range(ys)]
        layouts[L] = dict(xsize=xs, ysize=ys, tiles=grid, lineScroll=ls, pf=pf, type=ty)
    return tilegfx, tiles, layouts, active, mid, hp


def load_state(path):
    st, hp = {}, None
    for line in open(path):
        p = line.split()
        if len(p) >= 3 and p[0] == 'S':
            st[p[1]] = int(p[2], 0)
    hpf = path + '.hparallax'
    if os.path.exists(hpf):
        raw = open(hpf, 'rb').read()
        ints = struct.unpack_from('<%di' % (4 * PARALLAX_COUNT), raw, 0)
        hp = dict(parallaxFactor=ints[0:256], scrollSpeed=ints[256:512], scrollPos=ints[512:768],
                  linePos=ints[768:1024], deform=raw[4096:4096 + 256], entryCount=raw[4096 + 256])
    return st, hp


def render(stage, st, hp, slots):
    tilegfx, tiles, layouts, active, mid, _ = stage
    X, Y = st['XScrollOffset'], st['YScrollOffset']
    img = [[-1] * W for _ in range(H)]
    warn = []
    flips = set()  # (slot, layer line tile row, px tile, tile, dir) of visible flipped tiles
    for slot in range(4):
        li = active[slot]
        if li >= PS1_LAYER_COUNT or slot not in slots:
            continue
        L = layouts.get(li)
        if not L or not L['xsize'] or not L['ysize']:
            continue
        above = 1 if slot >= mid else 0
        width, height = L['xsize'], L['ysize']
        full_w = width << 7
        if li:  # BG layer: Y parallax + autoscroll (scrollPos is the value after this frame's +=)
            full_h = height << 7
            yScroll = Y * L['pf'] >> 6
            yoff = cdiv_mod(yScroll + (st['layerScrollPos%d' % li] >> 16), full_h)
            linePos = []
            for i in range(PARALLAX_COUNT):
                if i < hp['entryCount']:
                    lp = (X * hp['parallaxFactor'][i] >> 7) + (hp['scrollPos'][i] >> 16)
                    linePos.append(cdiv_mod(lp, full_w))
                else:
                    linePos.append(hp['linePos'][i])
            deform_used = any(hp['deform'][i] for i in range(hp['entryCount']))
        else:
            yoff = Y
            linePos = [X] * PARALLAX_COUNT
            deform_used = False
        if deform_used:
            warn.append('layer %d: deformation flags set (not modelled)' % li)
        tileYPos = cdiv_mod(yoff, height << 7)
        if tileYPos < 0:
            tileYPos += height << 7
        for y in range(H):
            ly = (tileYPos + y) % (height << 7)
            idx = L['lineScroll'][ly]
            chunkX = linePos[idx]
            if chunkX < 0:
                chunkX += full_w
            if chunkX >= full_w:
                chunkX -= full_w
            chunkY, tileY, ty16 = ly >> 7, (ly & 0x7F) >> 4, ly & 0xF
            row = img[y]
            for x in range(W):
                px = (chunkX + x) % full_w
                ch = L['tiles'][chunkY][px >> 7]
                t, dr, pl = tiles[(ch << 6) + ((px & 0x7F) >> 4) + 8 * tileY]
                if pl != above:
                    continue
                if dr:
                    flips.add((slot, ly >> 4, px >> 4, t, dr))
                tx = px & 0xF
                tyy = ty16
                if dr & 1:
                    tx = 15 - tx
                if dr & 2:
                    tyy = 15 - tyy
                v = tilegfx[(t << 8) + tyy * 16 + tx]
                if v:
                    row[x] = v
    return img, warn, flips


def circle_tint_mask(gif, scale):
    """TitleCard.txt state 9: DrawTintRect x4 around a centred window + DrawScaledTintMask of
    frame 111 (256x256 at 256,256, pivot -128,-128) at (160,120) -- PC maths."""
    from PIL import Image
    t0, t5 = 128 * scale >> 9, 256 * scale >> 9
    x1, y1 = 160 - t0, 120 - t0
    x2, y2 = x1 + t5, y1 + t5
    tinted = [[not (x1 <= x < x2 and y1 <= y < y2) for x in range(W)] for y in range(H)]
    im = Image.open(gif)
    spr = im.load()
    ts = 4 * scale
    width = height = 256
    sprX = sprY = 256
    trueX = 160 - (ts * 128 >> 11)
    trueY = 120 - (ts * 128 >> 11)
    width = ts * width >> 11
    height = ts * height >> 11
    f32 = lambda v: struct.unpack('<f', struct.pack('<f', v))[0]  # C (float) cast
    fs = int(f32(f32(2048.0 / f32(ts)) * 2048.0))
    fs16 = ((fs + 32768) & 0xFFFF) - 32768                     # C (short) cast
    rx = ry = 0
    if width + trueX > W:
        width = W - trueX
    if trueX < 0:
        sprX += trueX * -fs >> 11
        rx = ((trueX & 0xFFFF) * -fs16) & 0x7FF
        width += trueX
        trueX = 0
    if height + trueY > H:
        height = H - trueY
    if trueY < 0:
        sprY += trueY * -fs >> 11
        ry = ((trueY & 0xFFFF) * -fs16) & 0x7FF
        height += trueY
        trueY = 0
    if width <= 0 or height <= 0:
        return tinted
    gy = sprY
    for yy in range(height):
        gx = sprX
        rxx = rx
        for xx in range(width):
            if spr[gx, gy] != 0:
                tinted[trueY + yy][trueX + xx] = True
            o = fs + rxx
            gx += o >> 11
            rxx = o & 0x7FF
        o = fs + ry
        gy += o >> 11
        ry = o & 0x7FF
    return tinted


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    if '--circle' in sys.argv:
        args = [a for a in args if a != sys.argv[sys.argv.index('--circle') + 1]]
    slots = {0, 1, 2, 3}
    if '--layers' in sys.argv:
        slots = {int(s) for s in sys.argv[sys.argv.index('--layers') + 1].split(',')}
        args = [a for a in args if a != sys.argv[sys.argv.index('--layers') + 1]]
    stage_dir, state_file, vram_file = args[:3]
    out = args[3] if len(args) > 3 else None
    st, hp = load_state(state_file)
    stage = load_stage(stage_dir, st.get('ActNo', 1))
    img, warn, flips = render(stage, st, hp, slots)
    edge = {'u0': 0, 'u240': 0, 'v0': 0, 'v240': 0}
    for (_, _, _, t, dr) in flips:
        u, v = (t & 15) << 4, ((t & 255) >> 4) << 4
        edge['u0'] += (dr & 1) and u == 0
        edge['u240'] += not (dr & 1) and u == 240
        edge['v0'] += (dr & 2) and v == 0
        edge['v240'] += not (dr & 2) and v == 240
    print('FLIPS visible=%d X=%d Y=%d XY=%d edge=%s' % (len(flips), sum(f[4] == 1 for f in flips),
          sum(f[4] == 2 for f in flips), sum(f[4] == 3 for f in flips), edge))
    for w in warn:
        print('WARN', w)
    vram = struct.unpack('<%dH' % (1024 * 512), open(vram_file, 'rb').read())
    clut = [vram[240 * 1024 + i] for i in range(256)]
    if '--tint' in sys.argv or '--circle' in sys.argv:
        tint = open(state_file + '.tint', 'rb').read()
        tinted = [[True] * W for _ in range(H)]
        if '--circle' in sys.argv:
            tinted = circle_tint_mask(sys.argv[sys.argv.index('--circle') + 1], st['tcScale'])
        img = [[(tint[i] if tinted[y][x] else i) if i >= 0 else -1 for x, i in enumerate(r)] for y, r in enumerate(img)]
        clut[0] = clut[0] or 0x8000  # a tinted pixel is never transparent
    best = None
    for fb, y0 in (('fb0', 0), ('fb1', 256)):
        drawn = mism = 0
        und = {}
        diffs = []
        for y in range(H):
            for x in range(W):
                got = vram[(y0 + y) * 1024 + x] & 0x7FFF
                i = img[y][x]
                if i < 0:
                    und[got] = und.get(got, 0) + 1
                    continue
                drawn += 1
                if got != clut[i] & 0x7FFF:
                    mism += 1
                    diffs.append((x, y))
        top = sorted(und.items(), key=lambda kv: -kv[1])[:3]
        print('%s drawn=%d mismatch=%d undrawn=%d undrawn_colours=%s' % (
            fb, drawn, mism, sum(und.values()), ['%04x:%d' % kv for kv in top]))
        if best is None or mism < best[1]:
            best = (fb, mism, diffs)
    print('BEST %s mismatch=%d first=%s' % (best[0], best[1], best[2][:8]))
    if out:
        from PIL import Image

        def rgb(v):
            return ((v & 31) << 3, ((v >> 5) & 31) << 3, ((v >> 10) & 31) << 3)
        ref = Image.new('RGB', (W, H))
        ref.putdata([rgb(clut[i]) if i >= 0 else (0, 0, 0) for r in img for i in r])
        ref.save(out + '_ref.png')
        dif = ref.convert('L').convert('RGB')
        for (x, y) in best[2]:
            dif.putpixel((x, y), (255, 0, 0))
        dif.save(out + '_diff.png')


if __name__ == '__main__':
    main()
