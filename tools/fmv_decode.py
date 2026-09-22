import numpy as np, struct, re, sys

# ---- Parse tables from ps1/mdec.cpp ----
src = open("/Users/danielpinheiro/Files/Projetos_Infaliveis/PS1/RSDKv2-ps1/ps1/mdec.cpp").read()

def grab(pattern):
    m = re.search(pattern, src, re.S)
    assert m, pattern
    return m.group(1)

# _compressed_table
ct = re.findall(r'0x[0-9a-fA-F]{8}', grab(r'_compressed_table\[TABLE_LENGTH\] = \{(.*?)\};'))
compressed = [int(x, 16) for x in ct]
print("compressed table entries:", len(compressed))

# iq_y
iq = [int(x) for x in re.findall(r'\b\d+\b', grab(r'\.iq_y = \{(.*?)\},'))]
print("iq_y:", len(iq), iq[:8])

# dct scale matrix
dct_txt = grab(r'\.dct = \{(.*?)\n    \}')
S = {'S0':0x5a82,'S1':0x7d8a,'S2':0x7641,'S3':0x6a6d,'S4':0x5a82,'S5':0x471c,'S6':0x30fb,'S7':0x18f8}
dct_vals = []
for tok in re.findall(r'-?S\d', dct_txt):
    neg = tok.startswith('-')
    dct_vals.append(-S[tok.lstrip('-')] if neg else S[tok])
dct_scale = np.array(dct_vals, dtype=np.float64).reshape(8,8)
print("dct scale matrix:\n", dct_scale.astype(int))

# ---- Decompress the VLC table (DecDCTvlcBuild) ----
ac = []
for v in compressed:
    val = v & 0x001fffff
    for _ in range((v >> 21) + 1):
        ac.append(val)
ac = np.array(ac, dtype=np.uint32)
print("decompressed VLC table size:", len(ac), " (expect 8192+512=8704)")

# ---- VLC decode (DecDCTvlcContinue2, byte-faithful) ----
def vlc_decode(bs, quant_scale, remaining):
    # bs = bytes of bitstream (after 8-byte BS header)
    words = [struct.unpack('<I', bs[i:i+4])[0] for i in range(0, len(bs)-len(bs)%4, 4)]
    words += [0, 0]  # pad
    idx = 2  # ctx->input = &input[2]
    window = ((words[0] << 16) | (words[0] >> 16)) & 0xffffffff
    next_window = ((words[1] << 16) | (words[1] >> 16)) & 0xffffffff
    bit_offset = 32
    block_index = 0
    coeff_index = 0
    out = []
    max_size = remaining
    while max_size > 0:
        max_size -= 1
        if coeff_index:
            if (window >> 30) == 0b10:
                out.append(0xfe00)
                window = (window << 2) & 0xffffffff
                bit_offset -= 2
                coeff_index = -1
                block_index += 1
                if block_index > 5: block_index = 0
            elif (window >> 26) == 0b000001:
                out.append((window >> 10) & 0xffff)
                window = (window << 22) & 0xffffffff
                bit_offset -= 22
            elif (window >> 24):
                v = int(ac[window >> 19])
                window = (window << (v >> 16)) & 0xffffffff
                bit_offset -= (v >> 16)
                out.append(v & 0xffff)
            else:
                v = int(ac[8192 + (window >> 15)])
                window = (window << (v >> 16)) & 0xffffffff
                bit_offset -= (v >> 16)
                out.append(v & 0xffff)
        else:
            value = window >> 22  # _get_bits_unsigned(10)
            if value == 0x1ff:
                break
            out.append((value | quant_scale) & 0xffff)
            window = (window << 10) & 0xffffffff
            bit_offset -= 10
        coeff_index += 1
        if bit_offset < 0:
            window = (next_window << (-bit_offset)) & 0xffffffff
            bit_offset += 32
            next_window = ((words[idx] << 16) | (words[idx] >> 16)) & 0xffffffff
            idx += 1
        window = (window | (next_window >> bit_offset)) & 0xffffffff
    return out

# ---- rl_decode_block + idct (psx-spx reference) ----
zagzig = [0,1,8,16,9,2,3,10,17,24,32,25,18,11,4,5,12,19,26,33,40,48,41,34,27,20,13,6,7,14,21,28,35,42,49,56,57,50,43,36,29,22,15,23,30,37,44,51,58,59,52,45,38,31,39,46,53,60,61,54,47,55,62,63]

def signed10(n):
    n &= 0x3ff
    return n - 1024 if n >= 512 else n

def decode_frame(bs, quant_scale_raw):
    # bs = full BS frame bytes (8-byte header + bitstream)
    header = bs[:8]
    mdec0 = struct.unpack('<I', header[0:4])[0]
    qs_raw = struct.unpack('<H', header[4:6])[0]
    remaining = (mdec0 & 0xffff) * 2
    qs = (qs_raw & 63) << 10
    rle = vlc_decode(bs[8:], qs, remaining)
    # dequantize per block
    blocks = []  # each 8x8 raster
    k = 0
    n = len(rle)
    i = 0
    while i < n:
        blk = np.zeros((8,8), dtype=np.float64)
        # first halfword = DC
        if i >= n: break
        hw = rle[i]
        if hw == 0xfe00:
            i += 1
            continue
        q_scale = (hw >> 10) & 0x3f
        dc = signed10(hw) * iq[0]
        blk.flat[zagzig[0]] = dc
        i += 1
        kk = 0
        while kk <= 63:
            if i >= n: break
            hw = rle[i]
            if hw == 0xfe00:
                i += 1
                break
            run = (hw >> 10) & 0x3f
            lvl = signed10(hw)
            kk = kk + run + 1
            if kk <= 63:
                if q_scale == 0:
                    val = lvl * 2
                    blk.flat[kk] = val
                else:
                    val = (lvl * iq[kk] * q_scale + 4) // 8
                    blk.flat[zagzig[kk]] = val
            i += 1
        blocks.append(blk)
    return blocks, remaining, qs_raw

# ---- Test: decode frame 119, compare with source ----
str_path = "/Users/danielpinheiro/Files/Projetos_Infaliveis/PS1/RSDKv2-ps1/build/iso/Data/Sprites/Videos/Intro.str"
data = open(str_path, 'rb').read()
FRAME = 20480
frame119 = data[119*FRAME:(119+1)*FRAME]
# strip 32-byte sector headers
bs = b''.join(frame119[i*2048+32:(i+1)*2048] for i in range(10))
blocks, remaining, qs_raw = decode_frame(bs, 0)
print("frame 119: blocks decoded =", len(blocks), "remaining(halfwords) =", remaining, "quant_scale =", qs_raw)
