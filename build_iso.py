#!/usr/bin/env python3
"""Build a minimal ISO9660 image (DATA.BIN + PSX.EXE) for PCSX-Redux.

Layout: sectors 0-15 system area, 16 PVD, 17 terminator, 18-19 path tables,
20 root dir, 21+ file extents. No CD-XA, no license, no dummy — just enough
for the psyqo CD-ROM device to find DATA.BIN and PSX.EXE.
"""
import struct, os

DATA_BIN = 'build/iso/Data.bin'
PSX_EXE = 'build/iso/PSX.EXE'
OUT = 'build/game.iso'
SECTOR = 2048


def both16(v):
    return struct.pack('<H', v) + struct.pack('>H', v)


def both32(v):
    return struct.pack('<I', v) + struct.pack('>I', v)


def dir_record(name, extent, size, flags, name_len=None):
    if name_len is None:
        name_len = len(name)
    pad = 1 if name_len % 2 == 0 else 0
    rec_len = 33 + name_len + pad
    rec = bytearray(rec_len)
    rec[0] = rec_len
    rec[2:10] = both32(extent)
    rec[10:18] = both32(size)
    rec[18:25] = bytes(7)  # recording date/time (zeros ok)
    rec[25] = flags
    rec[26:28] = bytes(2)  # file unit size
    rec[28:30] = bytes(2)  # interleave gap
    rec[30:34] = both16(1)  # volume seq number
    rec[32] = name_len
    rec[33:33 + name_len] = name.encode('ascii')
    return bytes(rec)


def main():
    data_bin = open(DATA_BIN, 'rb').read()
    psx_exe = open(PSX_EXE, 'rb').read()

    # sector assignment
    pvd_lba = 16
    term_lba = 17
    lpath_lba = 18
    mpath_lba = 19
    root_lba = 20
    data_lba = 21
    data_size = len(data_bin)
    data_sectors = (data_size + SECTOR - 1) // SECTOR
    psx_lba = data_lba + data_sectors
    psx_size = len(psx_exe)
    psx_sectors = (psx_size + SECTOR - 1) // SECTOR

    # root directory records: '.', '..', DATA.BIN, PSX.EXE
    root_recs = bytearray()
    root_recs += dir_record('\x00', root_lba, 2048, 0x02, name_len=1)
    root_recs += dir_record('\x01', root_lba, 2048, 0x02, name_len=1)
    root_recs += dir_record('DATA.BIN;1', data_lba, data_size, 0x00)
    root_recs += dir_record('PSX.EXE;1', psx_lba, psx_size, 0x00)
    root_recs = root_recs.ljust(SECTOR, b'\x00')

    # path tables (L and M) — one entry for the root directory
    lpath = bytearray()
    lpath += bytes([1])  # len of dir id
    lpath += bytes([0])  # ext attr len
    lpath += struct.pack('<I', root_lba)  # extent LBA (LE)
    lpath += struct.pack('<H', 1)  # parent dir number
    lpath += b'\x00'  # dir id
    lpath = lpath.ljust(SECTOR, b'\x00')

    mpath = bytearray()
    mpath += bytes([1, 0])
    mpath += struct.pack('>I', root_lba)  # extent LBA (BE)
    mpath += struct.pack('>H', 1)
    mpath += b'\x00'
    mpath = mpath.ljust(SECTOR, b'\x00')

    # PVD
    pvd = bytearray(SECTOR)
    pvd[0] = 1  # type = PVD
    pvd[1:6] = b'CD001'
    pvd[6] = 1  # version
    pvd[8:40] = b'RSDKV2'.ljust(32, b' ')  # system id
    pvd[40:72] = b'RSDKV2'.ljust(32, b' ')  # volume id
    pvd[80:88] = struct.pack('<I', 0)  # volume space size (unused)
    pvd[120:124] = struct.pack('<I', 1)  # volume set size LE
    pvd[124:128] = struct.pack('>I', 1)  # volume set size BE
    pvd[128:132] = struct.pack('<H', 1)  # volume seq LE
    pvd[132:136] = struct.pack('>H', 1)  # volume seq BE
    pvd[136:140] = struct.pack('<H', SECTOR)  # logical block size LE
    pvd[140:144] = struct.pack('>H', SECTOR)  # logical block size BE
    total_sectors = psx_lba + psx_sectors
    pvd[80:88] = struct.pack('<I', total_sectors) + struct.pack('>I', total_sectors)  # volume space
    # path table sizes
    pvd[132 - 0 + 0:132]  # no-op
    pvd[132:136]  # already done
    # path table location + size (little-endian path table at 18)
    pvd[140 + 0:140]  # noop
    # L path table (offset 140)
    pvd[140:144]  # already block size LE
    # per ECMA-119: path table size at 132, L path table loc at 140, M path table loc at 148
    pvd[132:136]  # placeholder
    # rewrite cleanly below
    pvd = bytearray(SECTOR)
    pvd[0] = 1
    pvd[1:6] = b'CD001'
    pvd[6] = 1
    pvd[8:40] = b'RSDKV2'.ljust(32, b' ')
    pvd[40:72] = b'RSDKV2'.ljust(32, b' ')
    pvd[80:88] = struct.pack('<I', total_sectors) + struct.pack('>I', total_sectors)
    pvd[120:124] = struct.pack('<I', 1)
    pvd[124:128] = struct.pack('>I', 1)
    pvd[128:132] = struct.pack('<H', 1)
    pvd[132:136] = struct.pack('>H', 1)
    pvd[136:140] = struct.pack('<H', SECTOR)
    pvd[140:144] = struct.pack('>H', SECTOR)
    pvd[132:136] = struct.pack('<H', len(lpath)) + struct.pack('>H', len(lpath))  # path table size
    pvd[140:144] = struct.pack('<I', lpath_lba)  # L path table loc
    pvd[144:148] = struct.pack('<I', 0)  # optional L path table
    pvd[148:152] = struct.pack('>I', mpath_lba)  # M path table loc
    pvd[152:156] = struct.pack('>I', 0)  # optional M path table
    # root directory record at offset 156
    pvd[156:156 + len(dir_record('\x00', root_lba, 2048, 0x02, name_len=1))] = \
        dir_record('\x00', root_lba, 2048, 0x02, name_len=1)

    term = bytearray(SECTOR)
    term[0] = 255
    term[1:6] = b'CD001'
    term[6] = 1

    out = bytearray()
    out += bytes(16 * SECTOR)  # system area
    out += pvd
    out += term
    out += lpath
    out += mpath
    out += root_recs
    out += data_bin.ljust(data_sectors * SECTOR, b'\x00')
    out += psx_exe.ljust(psx_sectors * SECTOR, b'\x00')

    with open(OUT, 'wb') as f:
        f.write(out)
    print(f'wrote {OUT} ({len(out)} bytes): DATA.BIN@{data_lba} ({data_size}B), PSX.EXE@{psx_lba} ({psx_size}B)')


if __name__ == '__main__':
    main()
