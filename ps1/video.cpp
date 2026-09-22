/*
 * PS1 STR video playback (MDEC + VLC).
 *
 * Streams a psxavenc `-t strv` file (BS v2, 2048-byte sectors, 10 sectors/frame)
 * via the CD-ROM asynchronously (double-buffered), Huffman-decodes each frame
 * with DecDCTvlcStart2, then the MDEC hardware does RLE+IDCT+YUV->RGB into a
 * 15bpp BGR555 buffer.
 */
#include "video.hh"
#include "mdec.hh"

#include <stdio.h>
#include <string.h>
#include <psyqo/cdrom-device.hh>
#include <psyqo/gpu.hh>

extern psyqo::CDRomDevice *g_ps1CDRom;
extern psyqo::GPU          *g_ps1GPU;

#define ISO_SECTOR            2048
#define STR_SECTORS_PER_FRAME 10
#define STR_FRAME_SIZE        (STR_SECTORS_PER_FRAME * ISO_SECTOR)   // 20480
#define STR_BS_CHUNK          (ISO_SECTOR - 32)                     // 2016
#define STR_BS_MAX            (STR_SECTORS_PER_FRAME * STR_BS_CHUNK) // 20160

// Macroblock grid for 256x160 (16x16 blocks) = 160 macroblocks = 960 DCT blocks.
// Worst-case uncompressed RLE: 960 blocks * 66 halfwords = 63360 halfwords = 31680 words.
#define STR_MAX_RLE_WORDS 0x8000 // 32768 words (128 KB), headroom over 31680.

static FILE *s_str = nullptr;

// Double-buffered raw frame + pixel output. The async CD read fills s_frame[s_readIdx]
// while s_video[s_dispIdx] is on screen; the two swap when a frame is decoded.
static uint8_t     s_frame[2][STR_FRAME_SIZE];
static uint8_t     s_bs[STR_BS_MAX];        // header-stripped BS payload (scratch)
static uint32_t    s_rle[STR_MAX_RLE_WORDS];// VLC output: MDEC(1) cmd + RLE halfwords (scratch)
static uint16_t    s_video[2][256 * 160];   // decoded 15bpp BGR555 (ping-pong)
static DECDCTTAB   s_vlcTable;              // 34 KB Huffman table
static VLC_Context s_vlcCtx;

static int s_width = 256;
static int s_height = 160;
static int s_frameCount = 0;
static int s_frameIndex = 0; // the frame being read into s_frame[s_readIdx]
static int s_readIdx  = 0;   // the buffer the async read fills
static int s_dispIdx  = 1;   // the buffer on screen
static volatile bool s_readDone = false;
static volatile bool s_readOk   = false;
static bool s_eof = false;

bool PS1VideoInit() {
    DecDCTReset(0);
    DecDCTvlcBuild(&s_vlcTable);
    return true;
}

static void queueRead() {
    uint32_t lba = s_str->lba + (uint32_t)s_frameIndex * STR_SECTORS_PER_FRAME;
    s_readDone = false;
    // Asynchronous: the callback fires from the main loop (pumpCallbacks) when the
    // CD-ROM DMA finishes, so the read of frame N+1 overlaps the display of frame N.
    g_ps1CDRom->readSectors(lba, STR_SECTORS_PER_FRAME, s_frame[s_readIdx],
                            [](bool ok) {
                                s_readOk   = ok;
                                s_readDone = true;
                            });
}

bool PS1VideoOpen(const char *path) {
    s_str = fopen(path, "rb");
    if (!s_str)
        return false;

    s_frameCount = (int)(s_str->size / STR_FRAME_SIZE);
    s_frameIndex = 0;
    s_readIdx    = 0;
    s_dispIdx    = 1;
    s_eof        = false;
    // Geometry is fixed by the encoder (psxavenc -s 256x160).
    s_width  = 256;
    s_height = 160;
    queueRead();
    return true;
}

int PS1VideoNextFrame() {
    if (!s_str)
        return -1;
    if (!s_readDone)
        return 0; // read still in flight — keep showing the current frame
    if (!s_readOk) {
        s_eof = true; // read failed (truncated / past end)
        return -1;
    }

    const uint8_t *frame = s_frame[s_readIdx];

    // Strip the 32-byte sector headers: 2016 payload bytes per sector.
    for (int i = 0; i < STR_SECTORS_PER_FRAME; i++)
        memcpy(s_bs + i * STR_BS_CHUNK, frame + i * ISO_SECTOR + 32, STR_BS_CHUNK);

    // Huffman decode -> MDEC RLE buffer (s_rle[0] = MDEC(1) command word).
    if (DecDCTvlcStart2(&s_vlcCtx, s_rle, 0, (const uint32_t *)s_bs) < 0)
        return -1;

    // Hardware decode: RLE + IDCT + YUV->RGB.
    DecDCTin(s_rle, DECDCT_MODE_16BPP);
    DecDCTout((uint32_t *)s_video[s_readIdx], (size_t)(s_width * s_height / 2));
    DecDCToutSync(0);

    // Swap: the freshly decoded buffer becomes the display buffer, and the read
    // of the next frame goes into the other buffer.
    s_dispIdx = s_readIdx;
    s_frameIndex++;
    if (s_frameIndex >= s_frameCount) {
        s_eof = true;
        return 1; // last frame decoded; the next call reports EOF
    }
    s_readIdx = 1 - s_dispIdx;
    queueRead();
    return 1;
}

void PS1VideoClose() {
    if (s_str) {
        fclose(s_str);
        s_str = nullptr;
    }
}

uint16_t *PS1VideoPixels() { return s_video[s_dispIdx]; }
int       PS1VideoWidth()  { return s_width; }
int       PS1VideoHeight() { return s_height; }
