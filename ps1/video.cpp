/*
 * PS1 STR video playback (MDEC + VLC).
 *
 * Streams a psxavenc `-t strv` file (BS v2, 2048-byte sectors, 10 sectors/frame) with
 * the continuous CD reader (cdrom_stream.hh), Huffman-decodes each frame with
 * DecDCTvlcStart2, then the MDEC hardware does RLE+IDCT+YUV->RGB into a 15bpp BGR555
 * buffer. Every sector's STR header (magic, sector index, frame number) is checked, so
 * a duplicated or missing sector can never shift the bitstream.
 */
#include "video.hh"
#include "mdec.hh"
#include "cdrom_stream.hh"

#include <stdio.h>
#include <string.h>

#include <psyqo/alloc.h>

#define ISO_SECTOR            2048
#define STR_SECTORS_PER_FRAME 10
#define STR_BS_CHUNK          (ISO_SECTOR - 32)                     // 2016
#define STR_BS_MAX            (STR_SECTORS_PER_FRAME * STR_BS_CHUNK) // 20160
#define STR_MAGIC             0x0160
#define STR_RING_SECTORS      20                                    // 40 KB read-ahead (2 frames)

// MDEC RLE output per frame = BS header mdec0 low 16 bits (+1 command word). Measured
// over all 934 frames of Intro.str: max 13,056 words. 0x3800 = 14,336 words (+9.8%);
// frames that would exceed it are skipped (counted in g_ps1VideoRleOverflow).
#define STR_MAX_RLE_WORDS 0x3800
#define STR_PIXELS        (256 * 160)

volatile uint32_t g_ps1VideoFrames = 0; // frames decoded
volatile uint32_t g_ps1VideoResync = 0; // sectors skipped because their STR header didn't match
volatile uint32_t g_ps1VideoRleOverflow = 0; // frames skipped: RLE bigger than the buffer
volatile uint32_t g_ps1VideoHeapBytes   = 0; // bytes currently allocated for FMV playback

static FILE *s_str = nullptr;

// All FMV buffers live on the heap only while a video is open (docs/26 B1): ~235 KB that
// the game doesn't carry after the intro. Allocated largest-first to limit fragmentation.
static uint16_t   *s_video    = nullptr; // decoded 15bpp BGR555 frame (80 KB)
static uint8_t    *s_ring     = nullptr; // CD read-ahead ring (40 KB)
static uint32_t   *s_rle      = nullptr; // VLC output: MDEC(1) cmd + RLE halfwords (56 KB)
static DECDCTTAB  *s_vlcTable = nullptr; // Huffman table (34 KB)
static uint8_t    *s_bs       = nullptr; // header-stripped BS payload (20 KB)
static VLC_Context s_vlcCtx;

static int s_width = 256;
static int s_height = 160;
static int s_frameCount = 0;
static int s_nextFrame  = 1; // STR frame numbers are 1-based

struct StrSectorHeader {
    uint16_t magic;
    uint16_t type;
    uint16_t sectorIndex;
    uint16_t sectorCount;
    uint32_t frame;
    uint32_t bytesUsed;
};

static const StrSectorHeader *hdr(uint32_t i) { return (const StrSectorHeader *)PS1StreamSector(i); }

bool PS1VideoInit() {
    DecDCTReset(0);
    return true;
}

static void freeBuffers() {
    psyqo_free(s_bs);
    psyqo_free(s_vlcTable);
    psyqo_free(s_rle);
    psyqo_free(s_ring);
    psyqo_free(s_video);
    s_bs = nullptr;
    s_vlcTable = nullptr;
    s_rle = nullptr;
    s_ring = nullptr;
    s_video = nullptr;
    g_ps1VideoHeapBytes = 0;
}

static bool allocBuffers() {
    s_video    = (uint16_t *)psyqo_malloc(STR_PIXELS * sizeof(uint16_t));
    s_ring     = (uint8_t *)psyqo_malloc(STR_RING_SECTORS * ISO_SECTOR);
    s_rle      = (uint32_t *)psyqo_malloc(STR_MAX_RLE_WORDS * sizeof(uint32_t));
    s_vlcTable = (DECDCTTAB *)psyqo_malloc(sizeof(DECDCTTAB));
    s_bs       = (uint8_t *)psyqo_malloc(STR_BS_MAX);
    if (!s_video || !s_ring || !s_rle || !s_vlcTable || !s_bs) {
        freeBuffers();
        return false;
    }
    g_ps1VideoHeapBytes = STR_PIXELS * sizeof(uint16_t) + STR_RING_SECTORS * ISO_SECTOR +
                          STR_MAX_RLE_WORDS * sizeof(uint32_t) + sizeof(DECDCTTAB) + STR_BS_MAX;
    memset(s_video, 0, STR_PIXELS * sizeof(uint16_t));
    DecDCTvlcBuild(s_vlcTable);
    return true;
}

bool PS1VideoOpen(const char *path) {
    s_str = fopen(path, "rb");
    if (!s_str)
        return false;
    if (!allocBuffers()) { // not enough heap: skip the video gracefully
        fclose(s_str);
        s_str = nullptr;
        return false;
    }
    uint32_t sectors = s_str->size / ISO_SECTOR;
    s_frameCount = (int)(sectors / STR_SECTORS_PER_FRAME);
    s_nextFrame  = 1;
    // Geometry is fixed by the encoder (psxavenc -s 256x160).
    s_width  = 256;
    s_height = 160;
    return PS1StreamOpen(s_str->lba, sectors, s_ring, STR_RING_SECTORS);
}

int PS1VideoNextFrame() {
    if (!s_str)
        return -1;
    PS1StreamPump();
    if (PS1StreamError())
        return -1;
    if (s_nextFrame > s_frameCount)
        return -1;

    // Resync: the next frame must start with sector 0 of frame s_nextFrame.
    while (PS1StreamAvailable()) {
        const StrSectorHeader *h = hdr(0);
        if (h->magic == STR_MAGIC && h->sectorIndex == 0 && (int)h->frame >= s_nextFrame) {
            s_nextFrame = (int)h->frame; // a lost frame is skipped, never misaligned
            break;
        }
        PS1StreamConsume(1);
        g_ps1VideoResync = g_ps1VideoResync + 1;
    }
    if (PS1StreamAvailable() < STR_SECTORS_PER_FRAME)
        return PS1StreamEnded() ? -1 : 0; // still reading — keep showing the current frame

    // All 10 sectors must belong to this frame, in order.
    for (int i = 0; i < STR_SECTORS_PER_FRAME; i++) {
        const StrSectorHeader *h = hdr(i);
        if (h->magic != STR_MAGIC || h->sectorIndex != i || (int)h->frame != s_nextFrame) {
            PS1StreamConsume(1); // drop the bad head and resync next time
            g_ps1VideoResync = g_ps1VideoResync + 1;
            return 0;
        }
    }

    // Strip the 32-byte sector headers: 2016 payload bytes per sector.
    for (int i = 0; i < STR_SECTORS_PER_FRAME; i++)
        memcpy(s_bs + i * STR_BS_CHUNK, PS1StreamSector(i) + 32, STR_BS_CHUNK);
    PS1StreamConsume(STR_SECTORS_PER_FRAME);
    PS1StreamPump();

    // RLE size of this frame (BS header mdec0 low 16 bits) + the MDEC(1) command word.
    if ((((const uint32_t *)s_bs)[0] & 0xffff) + 1 > STR_MAX_RLE_WORDS) {
        g_ps1VideoRleOverflow = g_ps1VideoRleOverflow + 1;
        s_nextFrame++;
        return 0;
    }

    // Huffman decode -> MDEC RLE buffer (s_rle[0] = MDEC(1) command word).
    if (DecDCTvlcStart2(&s_vlcCtx, s_rle, 0, (const uint32_t *)s_bs) < 0)
        return -1;

    // Hardware decode: RLE + IDCT + YUV->RGB. Decoding is synchronous, so a single
    // pixel buffer suffices (the caller uploads it after this returns).
    DecDCTin(s_rle, DECDCT_MODE_16BPP);
    DecDCTout((uint32_t *)s_video, (size_t)(s_width * s_height / 2));
    DecDCToutSync(0);

    s_nextFrame++;
    g_ps1VideoFrames = g_ps1VideoFrames + 1;
    return 1;
}

void PS1VideoClose() {
    PS1StreamClose();
    if (s_str) {
        fclose(s_str);
        s_str = nullptr;
    }
    freeBuffers();
}

uint16_t *PS1VideoPixels() { return s_video; }
int       PS1VideoWidth()  { return s_width; }
int       PS1VideoHeight() { return s_height; }
