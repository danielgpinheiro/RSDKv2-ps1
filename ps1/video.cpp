/*
 * PS1 STR video playback (MDEC + VLC).
 *
 * Streams a psxavenc `-t strv` file (BS v2, 2048-byte sectors, 10 sectors/frame)
 * via the stdio FILE* shim, Huffman-decodes each frame with DecDCTvlcStart2, then
 * the MDEC hardware does RLE+IDCT+YUV->RGB into a 15bpp BGR555 buffer.
 */
#include "video.hh"
#include "mdec.hh"

#include <stdio.h>
#include <string.h>

#define ISO_SECTOR            2048
#define STR_SECTORS_PER_FRAME 10
#define STR_FRAME_SIZE        (STR_SECTORS_PER_FRAME * ISO_SECTOR)   // 20480
#define STR_BS_CHUNK          (ISO_SECTOR - 32)                     // 2016
#define STR_BS_MAX            (STR_SECTORS_PER_FRAME * STR_BS_CHUNK) // 20160

// Macroblock grid for 256x160 (16x16 blocks) = 160 macroblocks = 960 DCT blocks.
// Worst-case uncompressed RLE: 960 blocks * 66 halfwords = 63360 halfwords = 31680 words.
#define STR_MAX_RLE_WORDS 0x8000 // 32768 words (128 KB), headroom over 31680.

static FILE *s_str = nullptr;

static uint8_t    s_frame[STR_FRAME_SIZE]; // raw 10-sector frame
static uint8_t    s_bs[STR_BS_MAX];        // header-stripped BS payload
static uint32_t   s_rle[STR_MAX_RLE_WORDS];// VLC output: MDEC(1) cmd + RLE halfwords
static uint16_t   s_video[256 * 160];      // decoded 15bpp BGR555
static DECDCTTAB  s_vlcTable;              // 34 KB Huffman table
static VLC_Context s_vlcCtx;

static int s_width = 256;
static int s_height = 160;
static int s_frameCount = 0;
static int s_frameIndex = 0;

bool PS1VideoInit() {
    DecDCTReset(0);
    DecDCTvlcBuild(&s_vlcTable);
    return true;
}

bool PS1VideoOpen(const char *path) {
    s_str = fopen(path, "rb");
    if (!s_str)
        return false;

    // Read the first frame's header to learn the geometry.
    if (fread(s_frame, 1, STR_FRAME_SIZE, s_str) != STR_FRAME_SIZE) {
        fclose(s_str);
        s_str = nullptr;
        return false;
    }
    fseek(s_str, 0, SEEK_SET);

    s_width  = (s_frame[0x10] | (s_frame[0x11] << 8));
    s_height = (s_frame[0x12] | (s_frame[0x13] << 8));

    // Frame count from the file size (10 sectors per frame).
    fseek(s_str, 0, SEEK_END);
    long size = ftell(s_str);
    fseek(s_str, 0, SEEK_SET);
    s_frameCount = (int)(size / STR_FRAME_SIZE);
    s_frameIndex = 0;

    return true;
}

bool PS1VideoNextFrame() {
    if (!s_str)
        return false;

    size_t got = fread(s_frame, 1, STR_FRAME_SIZE, s_str);
    if (got < 32)
        return false; // EOF or truncated

    uint32_t bytesUsed = s_frame[0x0C] | (s_frame[0x0D] << 8) | (s_frame[0x0E] << 16) | (s_frame[0x0F] << 24);
    if (bytesUsed > STR_BS_MAX)
        bytesUsed = STR_BS_MAX;

    // Strip the 32-byte sector headers: 2016 payload bytes per sector.
    for (int i = 0; i < STR_SECTORS_PER_FRAME; i++)
        memcpy(s_bs + i * STR_BS_CHUNK, s_frame + i * ISO_SECTOR + 32, STR_BS_CHUNK);

    // Huffman decode -> MDEC RLE buffer (s_rle[0] = MDEC(1) command word).
    if (DecDCTvlcStart2(&s_vlcCtx, s_rle, 0, (const uint32_t *) s_bs) < 0)
        return false;

    // Hardware decode: RLE + IDCT + YUV->RGB.
    DecDCTin(s_rle, DECDCT_MODE_16BPP);
    DecDCTout((uint32_t *) s_video, (size_t)(s_width * s_height / 2));
    DecDCToutSync(0);

    s_frameIndex++;
    return true;
}

void PS1VideoClose() {
    if (s_str) {
        fclose(s_str);
        s_str = nullptr;
    }
}

uint16_t *PS1VideoPixels() { return s_video; }
int       PS1VideoWidth()  { return s_width; }
int       PS1VideoHeight() { return s_height; }
