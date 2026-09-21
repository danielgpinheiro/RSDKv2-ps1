#!/usr/bin/env python3
"""Add build/precompiled.bin into Data.bin as Data/Scripts/Precompiled.bin."""
import struct

DATA = 'build/iso/Data.bin'
PRECOMP = 'build/precompiled.bin'


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
    out = []
    for i, (name, offset) in enumerate(dirs):
        start = offset + headerSize
        nxt = dirs[i + 1][1] + headerSize if i + 1 < len(dirs) else len(data)
        p = start
        files = []
        while p < nxt - 8:
            nameLen = data[p]; p += 1
            if nameLen == 0 or nameLen > 64:
                break
            fname = data[p:p + nameLen].decode('latin1'); p += nameLen
            size = struct.unpack('<I', data[p:p + 4])[0]; p += 4
            files.append((fname, data[p:p + size]))
            p += size
        out.append((name, files))
    return out


def rebuild(headerSize, dirs, blks):
    out = bytearray()
    offsets = []
    running = 0
    for dirname, files in blks:
        offsets.append(running)
        for fname, fdata in files:
            running += 1 + len(fname.encode('latin1')) + 4 + len(fdata)
    out += struct.pack('<I', headerSize)
    out += bytes([len(dirs)])
    for (dirname, _off0), off in zip(dirs, offsets):
        nb = dirname.encode('latin1')
        out += bytes([len(nb)]) + nb + struct.pack('<I', off)
    while len(out) < headerSize:
        out += b'\x00'
    for dirname, files in blks:
        for fname, fdata in files:
            nb = fname.encode('latin1')
            out += bytes([len(nb)]) + nb + struct.pack('<I', len(fdata)) + fdata
    return bytes(out)


def main():
    data = open(DATA, 'rb').read()
    headerSize, dirs = parse(data)
    blks = blocks(data, headerSize, dirs)
    precomp = open(PRECOMP, 'rb').read()
    precomp_xor = bytes(b ^ 0xFF for b in precomp)
    for dirname, files in blks:
        if dirname == 'Data/Scripts/':
            files.append(('Precompiled.bin', precomp_xor))
            break
    out = rebuild(headerSize, dirs, blks)
    open(DATA, 'wb').write(out)
    print(f'added Precompiled.bin ({len(precomp)} bytes) -> Data.bin ({len(out)} bytes)')


if __name__ == '__main__':
    main()
