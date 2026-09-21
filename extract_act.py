import struct, sys

data = open('build/iso/Data.bin', 'rb').read()
print(f"Data.bin size = {len(data)} bytes")

# Header: 4-byte LE headerSize, 1-byte dirCount
headerSize = struct.unpack('<I', data[0:4])[0]
dirCount = data[4]
print(f"headerSize={headerSize} dirCount={dirCount}")

# Directory list
pos = 5
dirs = []
for d in range(dirCount):
    nameLen = data[pos]; pos += 1
    name = data[pos:pos+nameLen].decode('latin1', errors='replace'); pos += nameLen
    offset = struct.unpack('<I', data[pos:pos+4])[0]; pos += 4
    dirs.append((name, offset))
print("Directories:")
for n, o in dirs:
    print(f"  '{n}' -> offset {o}")

def find_file(dirname, filename):
    # find directory
    dir_offset = None
    for n, o in dirs:
        if n == dirname:
            dir_offset = o
            break
    if dir_offset is None:
        return None
    # file list at dir_offset + headerSize
    p = dir_offset + headerSize
    while True:
        nameLen = data[p]; p += 1
        name = data[p:p+nameLen].decode('latin1', errors='replace'); p += nameLen
        size = struct.unpack('<I', data[p:p+4])[0]; p += 4
        if name == filename:
            return p, size
        p += size  # skip file data
        if p >= len(data):
            return None

# Extract Act1.bin from Data/Stages/NIntro (directory names carry a trailing slash)
r = find_file('Data/Stages/NIntro/', 'Act1.bin')
if r is None:
    print("Act1.bin NOT FOUND")
    sys.exit(1)
off, size = r
raw = bytes(b ^ 0xFF for b in data[off:off+size])  # file data is XOR'd with 0xFF
print(f"\nAct1.bin size={size}, first 64 bytes:")
print(' '.join(f'{b:02x}' for b in raw[:64]))
print('ascii:', ''.join(chr(b) if 32 <= b < 127 else '.' for b in raw[:64]))

# Parse title card + following fields like LoadActLayout does
p = 0
length = raw[p]; p += 1
title = raw[p:p+length].decode('latin1', errors='replace'); p += length
print(f"\ntitleCard length={length} text='{title}'")
activeLayers = list(raw[p:p+4]); p += 4
midPoint = raw[p]; p += 1
xsize = raw[p]; p += 1
ysize = raw[p]; p += 1
print(f"activeTileLayers={activeLayers} midPoint={midPoint} xsize={xsize} ysize={ysize}")
p += xsize * ysize * 2  # skip tiles
typenameCnt = raw[p]; p += 1
for i in range(typenameCnt):
    nameLen = raw[p]; p += 1 + nameLen
objectCount = (raw[p] << 8) + raw[p+1]; p += 2
print(f"typenameCnt={typenameCnt} objectCount={objectCount}")
print(f"object bytes: {' '.join(f'{b:02x}' for b in raw[p:p+6])}")

# Backgrounds.bin
r2 = find_file('Data/Stages/NIntro/', 'Backgrounds.bin')
if r2:
    off2, size2 = r2
    raw2 = bytes(b ^ 0xFF for b in data[off2:off2+size2])
    layerCount = raw2[0]
    print(f"\nBackgrounds.bin size={size2} layerCount={layerCount} (first 16 bytes: {' '.join(f'{b:02x}' for b in raw2[:16])})")
