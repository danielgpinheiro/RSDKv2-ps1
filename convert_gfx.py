#!/usr/bin/env python3
"""Convert RSDKv2 GIF sprites/tiles to the engine's native .gfx RLE format.

.gfx format (matches LoadGFXFile in RSDKv2/Sprite.cpp):
  width  : 2 bytes big-endian
  height : 2 bytes big-endian
  palette: 255 x 3 bytes (discarded by the loader; sprites use MasterPalette.act)
  pixels : RLE, top-down, left-right
      0xFF 0xFF            -> end
      0xFF <value> <count> -> emit <value> x <count>
      any other byte       -> literal

Output: a new Data.bin with every .gif sprite (and the 16x16Tiles.gif tile
strip) replaced by a .gfx file of the same name, plus the original name kept
as a 0-size .gif stub? No -- we just ADD .gfx siblings and remap in the loader.
This script only EMITS .gfx files into build/gfx-out/ for inspection + repack.

`convert_gfx.py --loose build/iso/Data` instead writes a .gfx next to every sprite .gif
in the loose tree that has none; index 0xFF is remapped to the nearest palette colour.
"""
import struct, io, sys
from PIL import Image

DATA = 'build/iso/Data.bin'
OUT = 'build/gfx-out'


def parse_vfs(data):
    headerSize = struct.unpack('<I', data[0:4])[0]
    dirCount = data[4]
    pos = 5
    dirs = []
    for _ in range(dirCount):
        nameLen = data[pos]; pos += 1
        name = data[pos:pos + nameLen].decode('latin1', errors='replace'); pos += nameLen
        offset = struct.unpack('<I', data[pos:pos + 4])[0]; pos += 4
        dirs.append((name, offset))
    return headerSize, dirs


def list_dir(data, headerSize, dirs, dirname):
    d = dict(dirs)[dirname]
    offs = sorted(o for _, o in dirs)
    nxt = None
    for o in offs:
        if o > d:
            nxt = o
            break
    end = nxt if nxt else len(data)
    p = d + headerSize
    out = []
    while p < end - 8:
        nameLen = data[p]; p += 1
        if nameLen == 0 or nameLen > 64:
            break
        name = data[p:p + nameLen].decode('latin1'); p += nameLen
        size = struct.unpack('<I', data[p:p + 4])[0]; p += 4
        out.append((name, size, p))
        p += size
    return out


def read_file(data, headerSize, dirs, dirname, filename):
    for name, size, off in list_dir(data, headerSize, dirs, dirname):
        if name == filename:
            return bytes(b ^ 0xFF for b in data[off:off + size]), size
    return None, 0


def rle_encode(pixels):
    out = bytearray()
    n = len(pixels)
    i = 0
    while i < n:
        v = pixels[i]
        # find run length
        j = i
        while j < n and pixels[j] == v and (j - i) < 255:
            j += 1
        run = j - i
        if run >= 3 and v != 0xFF:
            out += bytes([0xFF, v, run])
            i = j
        else:
            # literal(s): emit up to the run boundary (or single)
            # for a run of 1 or 2 (or v==0xFF), emit literals
            out.append(v)
            i += 1
    out += bytes([0xFF, 0xFF])  # end
    return bytes(out)


def remap_ff(im, px):
    """Index 0xFF is the .gfx RLE escape. Replace it with the nearest other palette entry
    (never 0 = transparent). Returns (pixels, (target, rgb_from, rgb_to, count)) or (px, None)."""
    n = px.count(0xFF)
    if n == 0:
        return px, None
    pal = im.getpalette() or []
    pal = pal + [0] * (768 - len(pal))
    src = pal[765:768]
    best, bestd = None, None
    for i in range(1, 255):
        c = pal[i * 3:i * 3 + 3]
        d = sum((a - b) ** 2 for a, b in zip(c, src))
        if bestd is None or d < bestd:
            best, bestd = i, d
    return [best if v == 0xFF else v for v in px], (best, src, pal[best * 3:best * 3 + 3], n)


def gif_to_gfx(raw, allow_remap=False):
    im = Image.open(io.BytesIO(raw))
    if im.mode != 'P':
        im = im.convert('P')
    w, h = im.size
    px = list(im.getdata())
    if allow_remap:
        px, remap = remap_ff(im, px)
        if remap:
            t, a, b, n = remap
            print(f'    remap 0xFF {tuple(a)} -> {t} {tuple(b)} ({n} px)')
    assert 0xFF not in px, "0xFF pixel present — RLE escape collision"
    out = bytearray()
    out += struct.pack('>H', w)  # width BE
    out += struct.pack('>H', h)  # height BE
    out += b'\x00' * (255 * 3)   # palette placeholder (discarded)
    out += rle_encode(px)
    return bytes(out)


def roundtrip_ok(gfx, w, h):
    body = gfx[4 + 255 * 3:]
    count, p = 0, 0
    while p < len(body):
        b = body[p]; p += 1
        if b == 0xFF:
            b1 = body[p]; p += 1
            if b1 == 0xFF:
                break
            count += body[p]; p += 1
        else:
            count += 1
    return count == w * h


def convert_loose(root):
    """Write a .gfx sibling for every sprite .gif in a loose Data/ tree that has none
    (tile strips are handled by convert_tiles.py)."""
    import os
    for dirpath, _, files in os.walk(root):
        for fn in sorted(files):
            if not fn.lower().endswith('.gif') or fn == '16x16Tiles.gif':
                continue
            src = os.path.join(dirpath, fn)
            dst = src[:-4] + '.gfx'
            if os.path.exists(dst):
                continue
            print(f'  {src}')
            raw = open(src, 'rb').read()
            gfx = gif_to_gfx(raw, allow_remap=True)
            w, h = struct.unpack('>HH', gfx[0:4])
            assert roundtrip_ok(gfx, w, h), dst
            open(dst, 'wb').write(gfx)
            print(f'    {w}x{h} gif={len(raw)}B -> gfx={len(gfx)}B')


def main():
    if len(sys.argv) == 3 and sys.argv[1] == '--loose':
        convert_loose(sys.argv[2])
        return
    data = open(DATA, 'rb').read()
    headerSize, dirs = parse_vfs(data)
    import os
    os.makedirs(OUT, exist_ok=True)

    targets = [
        ('Data/Sprites/NexusGlobals/', 'Display.gif'),
        ('Data/Sprites/NexusGlobals/', 'Items.gif'),
        ('Data/Sprites/SSZ/', 'Gimmicks.gif'),
        ('Data/Sprites/SSZ/', 'Shield.gif'),
        ('Data/Sprites/SSZ/', 'Enemies.gif'),
        ('Data/Sprites/SSZ/', 'ZipLine.gif'),
        ('Data/Stages/SSZ/', '16x16Tiles.gif'),
    ]
    for dirname, fn in targets:
        raw, size = read_file(data, headerSize, dirs, dirname, fn)
        if raw is None:
            print(f'  {fn}: NOT FOUND', file=sys.stderr)
            continue
        gfx = gif_to_gfx(raw)
        outname = fn.rsplit('.', 1)[0] + '.gfx'
        path = os.path.join(OUT, outname)
        with open(path, 'wb') as f:
            f.write(gfx)
        # round-trip sanity: decode the RLE back and compare pixel count
        w = struct.unpack('>H', gfx[0:2])[0]
        h = struct.unpack('>H', gfx[2:4])[0]
        body = gfx[2 + 2 + 255 * 3:]
        decoded = []
        p = 0
        while p < len(body):
            b = body[p]; p += 1
            if b == 0xFF:
                b1 = body[p]; p += 1
                if b1 == 0xFF:
                    break
                c = body[p]; p += 1
                decoded += [b1] * c
            else:
                decoded.append(b)
        ok = len(decoded) == w * h
        print(f'  {fn}: {w}x{h} gif={size}B -> gfx={len(gfx)}B ({outname}) roundtrip_px={len(decoded)} ok={ok}')


if __name__ == '__main__':
    main()
