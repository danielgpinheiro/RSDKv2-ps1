#!/usr/bin/env python3
"""Repack Data.bin: add .gfx siblings for every .gif sprite/tile.

VFS layout (matches LoadFile in RSDKv2):
  header   : headerSize(u32 LE) + dirCount(u8) + [nameLen(u8) + name + offset(u32 LE)] * dirCount,
             padded to headerSize bytes.
  blocks   : for each dir (in dir-list order), at `offset + headerSize`:
             [nameLen(u8) + name + size(u32 LE) + data(size bytes)] * files.
  offsets  : offset[0]=0; offset[i] = offset[i-1] + block_size[i-1]  (relative to header end).
"""
import struct, io, os
from PIL import Image

SRC = 'build/iso/Data.bin'
DST = 'build/iso/Data.bin'
BAK = 'build/Data.bin.bak'  # outside build/iso: never shipped on the disc


def parse(data):
    headerSize = struct.unpack('<I', data[0:4])[0]
    dirCount = data[4]
    pos = 5
    dirs = []
    for _ in range(dirCount):
        nameLen = data[pos]; pos += 1
        name = data[pos:pos + nameLen].decode('latin1'); pos += nameLen
        offset = struct.unpack('<I', data[pos:pos + 4])[0]; pos += 4
        dirs.append([name, offset])
    return headerSize, dirs


def blocks(data, headerSize, dirs):
    out = []  # list of (dirname, [(fname, bytes)])
    for i, (name, offset) in enumerate(dirs):
        start = offset + headerSize
        nxt = None
        for j in range(i + 1, len(dirs)):
            nxt = dirs[j][1] + headerSize
            break
        end = nxt if nxt else len(data)
        p = start
        files = []
        while p < end - 8:
            nameLen = data[p]; p += 1
            if nameLen == 0 or nameLen > 64:
                break
            fname = data[p:p + nameLen].decode('latin1'); p += nameLen
            size = struct.unpack('<I', data[p:p + 4])[0]; p += 4
            files.append((fname, data[p:p + size]))
            p += size
        out.append((name, files))
    return out


def gif_to_gfx(raw):
    im = Image.open(io.BytesIO(raw)).convert('P')
    w, h = im.size
    px = list(im.getdata())
    if 0xFF in px:
        return None  # RLE escape collision — skip (e.g. RetroLogo.gif)
    out = bytearray()
    out += struct.pack('>H', w)
    out += struct.pack('>H', h)
    out += b'\x00' * (255 * 3)
    # RLE
    n = len(px)
    i = 0
    while i < n:
        v = px[i]
        j = i
        while j < n and px[j] == v and (j - i) < 255:
            j += 1
        run = j - i
        if run >= 3 and v != 0xFF:
            out += bytes([0xFF, v, run])
            i = j
        else:
            out.append(v)
            i += 1
    out += bytes([0xFF, 0xFF])
    return bytes(out)


def main():
    data = open(SRC, 'rb').read()
    headerSize, dirs = parse(data)
    blks = blocks(data, headerSize, dirs)

    # add .gfx siblings for every .gif (sprites + tile strip)
    added = 0
    for dirname, files in blks:
        newfiles = []
        for fname, fdata in files:
            if fname.lower().endswith('.gif') and ('/Sprites/' in dirname or fname == '16x16Tiles.gif'):
                gfxname = fname.rsplit('.', 1)[0] + '.gfx'
                gfx = gif_to_gfx(bytes(b ^ 0xFF for b in fdata))  # fdata is XOR'd; decode GIF, emit raw .gfx
                if gfx is not None:
                    newfiles.append((gfxname, bytes(b ^ 0xFF for b in gfx)))  # re-XOR for VFS storage
                    added += 1
        files.extend(newfiles)

    # rebuild
    out = bytearray()
    # header (dir list) — build after we know offsets, but offsets are cumulative
    # first compute block sizes and offsets
    offsets = []
    running = 0
    for dirname, files in blks:
        offsets.append(running)
        for fname, fdata in files:
            running += 1 + len(fname.encode('latin1')) + 4 + len(fdata)
    # write header
    out += struct.pack('<I', headerSize)
    out += bytes([len(dirs)])
    for (dirname, offset0), offset in zip(dirs, offsets):
        nb = dirname.encode('latin1')
        out += bytes([len(nb)]) + nb + struct.pack('<I', offset)
    while len(out) < headerSize:
        out += b'\x00'
    # write blocks
    for dirname, files in blks:
        for fname, fdata in files:
            nb = fname.encode('latin1')
            out += bytes([len(nb)]) + nb + struct.pack('<I', len(fdata)) + fdata

    os.replace(SRC, BAK)
    with open(DST, 'wb') as f:
        f.write(out)
    print(f'wrote {DST} ({len(out)} bytes), added {added} .gfx files, backed up to {BAK}')


if __name__ == '__main__':
    main()
