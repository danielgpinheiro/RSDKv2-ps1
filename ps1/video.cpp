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
#include <psyqo/gpu.hh>
#include <psyqo/hardware/cpu.hh>
#include "common/hardware/counters.h"

extern psyqo::GPU *g_ps1GPU;

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
// Decode cost of the last frame, in hblanks (root counter 1, free-running under psyqo:
// 63.6 us each, ~262 per NTSC frame).
volatile uint32_t g_ps1VideoVlcHbl  = 0; // Huffman (VLC) decode on the CPU
volatile uint32_t g_ps1VideoMdecHbl = 0; // MDEC in/out DMA + wait

uint32_t PS1Hblanks() { return COUNTERS[1].value; }
uint32_t PS1HblanksSince(uint32_t t0) { return (PS1Hblanks() - t0) & 0xffff; }

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
static bool s_xa        = false; // STR with interleaved XA audio (sidecar .sti): never pauses, drops unrequested frames
static uint32_t s_dataSectors   = 0; // data (video) sectors the stream delivers in all
static uint32_t s_lastDelivered = 0; // PS1StreamDelivered() when it last moved ...
static uint32_t s_lastDataVbl   = 0; // ... and the frame count then
static int s_nextFrame  = 1; // STR frame numbers are 1-based
static int s_requested  = 0; // frames asked for by the game, not decoded yet
// Game-side decode pipeline state (see PS1VideoService).
enum { STAGE_EMPTY, STAGE_VLC, STAGE_MDEC };
static int      s_stage  = STAGE_EMPTY; // state of s_bs/s_rle (the frame being decoded)
static bool     s_pixReady = false;     // s_video holds a decoded frame not yet handed over
static uint32_t s_rlePos = 0; // RLE data words written so far (s_rle[1..s_rlePos])
static uint32_t s_vlcAcc = 0; // hblanks spent on this frame's VLC so far

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
    // The MDEC is set up for its mode (PIO by default) before the first picture (mdecRun); a failed
    // setup counts as a fault there. Nothing to do up front.
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

// Build-time sidecar next to an STR with XA audio (tools/fmv/build_str.py): X.str -> X.sti =
// 'STRI', u32 frames, u32 video (data) sectors, u8 xa file, u8 xa channel, u8 has audio, u8 0.
struct StrInfo {
    uint32_t frames = 0, videoSectors = 0;
    uint8_t xaFile = 0, xaChannel = 0;
    bool audio = false;
};
static bool readStrInfo(const char *path, StrInfo *info) {
    char sti[96];
    int n = (int)strlen(path);
    if (n < 4 || n >= (int)sizeof(sti))
        return false;
    memcpy(sti, path, n + 1);
    memcpy(sti + n - 3, "sti", 3);
    FILE *f = fopen(sti, "rb");
    if (!f)
        return false;
    uint8_t b[16];
    bool ok = fread(b, 1, 16, f) == 16 && !memcmp(b, "STRI", 4);
    fclose(f);
    if (!ok)
        return false;
    info->frames       = b[4] | (b[5] << 8) | (b[6] << 16) | ((uint32_t)b[7] << 24);
    info->videoSectors = b[8] | (b[9] << 8) | (b[10] << 16) | ((uint32_t)b[11] << 24);
    info->xaFile       = b[12];
    info->xaChannel    = b[13];
    info->audio        = b[14] != 0;
    return true;
}

volatile uint32_t g_ps1VideoXa = 0; // 1 while an STR with XA audio streams (GDB)

bool PS1VideoOpen(const char *path) {
    StrInfo info;
    bool hasInfo = readStrInfo(path, &info);
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
    s_xa         = hasInfo && info.audio;
    if (hasInfo)
        s_frameCount = (int)info.frames;
    g_ps1VideoXa = s_xa ? 1 : 0;
    s_dataSectors   = hasInfo ? info.videoSectors : sectors;
    s_lastDelivered = 0;
    s_lastDataVbl   = g_ps1GPU->getFrameCount();
    s_nextFrame  = 1;
    s_requested  = 0;
    s_stage      = STAGE_EMPTY;
    s_pixReady   = false;
    s_rlePos     = 0;
    // Geometry is fixed by the encoder (psxavenc -s 256x160).
    s_width  = 256;
    s_height = 160;
    if (s_xa) // the intro music: XA sectors interleaved in the STR, played by the drive
        return PS1StreamOpenXA(s_str->lba, info.videoSectors, s_ring, STR_RING_SECTORS, info.xaFile, info.xaChannel);
    return PS1StreamOpen(s_str->lba, sectors, s_ring, STR_RING_SECTORS);
}

// Takes the next complete frame (10 sectors, headers checked) out of the ring into s_bs.
// 1 = s_bs holds frame s_nextFrame, 0 = not yet (still reading / bad sector / skipped),
// -1 = end of stream or drive error.
static int fetchFrame() {
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
    // Sectors of this frame: 10 in a video-only STR, 8-9 when XA audio takes 1 sector in 8.
    int frameSectors = PS1StreamAvailable() ? hdr(0)->sectorCount : STR_SECTORS_PER_FRAME;
    if (frameSectors < 1 || frameSectors > STR_SECTORS_PER_FRAME) {
        PS1StreamConsume(1);
        g_ps1VideoResync = g_ps1VideoResync + 1;
        return 0;
    }
    if (PS1StreamAvailable() < (uint32_t)frameSectors)
        return PS1StreamEnded() ? -1 : 0; // still reading — keep showing the current frame

    // All sectors must belong to this frame, in order.
    for (int i = 0; i < frameSectors; i++) {
        const StrSectorHeader *h = hdr(i);
        if (h->magic != STR_MAGIC || h->sectorIndex != i || (int)h->frame != s_nextFrame) {
            PS1StreamConsume(1); // drop the bad head and resync next time
            g_ps1VideoResync = g_ps1VideoResync + 1;
            return 0;
        }
    }

    // Strip the 32-byte sector headers: 2016 payload bytes per sector.
    for (int i = 0; i < frameSectors; i++)
        memcpy(s_bs + i * STR_BS_CHUNK, PS1StreamSector(i) + 32, STR_BS_CHUNK);
    PS1StreamConsume(frameSectors);
    PS1StreamPump();

    // RLE size of this frame (BS header mdec0 low 16 bits) + the MDEC(1) command word.
    if ((((const uint32_t *)s_bs)[0] & 0xffff) + 1 > STR_MAX_RLE_WORDS) {
        g_ps1VideoRleOverflow = g_ps1VideoRleOverflow + 1;
        s_nextFrame++;
        return 0;
    }
    return 1;
}

// Hardware decode of s_rle: RLE + IDCT + YUV->RGB into s_video. Synchronous, so a single
// pixel buffer suffices (the caller uploads it before the next decode). s_video holds the frame as
// 16-pixel-wide columns (16 x height each, column-major macroblocks), as the MDEC-out DMA wrote it.
volatile uint32_t g_ps1VideoSyncFail    = 0; // MDEC waits that timed out
volatile uint32_t g_ps1VideoCdDeferred  = 0; // CD IRQs held back until an MDEC-out DMA finished

// ---- MDEC modes (the Sonic CD PSone tests, docs/28 appendix 12) ----------------------------------
// On a PSone the MDEC-out DMA never finished (black video, CPU starved behind it) and the hybrid mode
// stayed black too; the CPU driving the MDEC (PIO) played perfectly. So PIO is the default: the CPU
// writes the RLE words and reads the pixels (ps1-tests' hardware-tested primitives), with hybrid (DMA0
// in) and DMA as fallbacks. Every wait is bounded; a stuck picture resets the MDEC, two in a row switch
// the mode.
enum { MDEC_PIO = 0, MDEC_DMA = 1, MDEC_HYBRID = 2 }; // hybrid: DMA0 in, the CPU reads out
static const uint32_t s_mdecControl[3] = {0x00000000, 0x60000000, 0x40000000}; // MDEC1 DMA requests per mode
volatile uint32_t g_ps1MdecMode      = MDEC_PIO; // active mode
volatile int32_t  g_ps1MdecForceMode = -1;       // GDB test hook: 0 / 1 / 2 = use that mode from the next picture
volatile uint32_t g_ps1MdecFaults    = 0;        // pictures that timed out
volatile uint32_t g_ps1MdecSwitches  = 0;        // mode switches after faults
volatile uint32_t g_ps1MdecTestDead  = 0;        // GDB test hook: > 0 -> that many pictures without the MDEC(1) command
volatile uint32_t g_ps1MdecSnap[8];              // registers at the first fault (DecDCTSnapshot)
volatile uint32_t g_ps1MdecSnapTaken = 0;
volatile int32_t  g_ps1MdecOrder     = -1;       // PIO output order: 1 = 8x8 blocks (hardware), 0 = rows, -1 = unknown
// Where the first fault happened: kind (1 setup, 2 command, 3 pixels, 4 padding, 5 DMA wait, 6 DMA out),
// column x, macroblock, word, input words left, MDEC1 at that instant.
volatile uint32_t g_ps1MdecFail[6];
static uint32_t   s_mdecModeInit = 0xff; // mode the MDEC is set up for
static int        s_faultRun     = 0;    // consecutive faulty pictures

#define MDEC_WAIT_HBL  1000 // ~64 ms without progress = the MDEC is stuck (a macroblock takes ~0.1 ms)
#define MDEC_CMD_15BIT (0x38000000u | 0x02000000u) // MDEC(1), 15-bit, bit 15 set (the fade relies on it)
#define STR_COLUMN     16

static void mdecFail(uint32_t kind, int x, int mb, int w, uint32_t left) {
    if (g_ps1MdecFail[0])
        return;
    g_ps1MdecFail[1] = (uint32_t)x;
    g_ps1MdecFail[2] = (uint32_t)mb;
    g_ps1MdecFail[3] = (uint32_t)w;
    g_ps1MdecFail[4] = left;
    g_ps1MdecFail[5] = DecDCTStatus();
    g_ps1MdecFail[0] = kind;
}

static void mdecSnapshot() {
    if (g_ps1MdecSnapTaken)
        return;
    uint32_t r[8];
    DecDCTSnapshot(r);
    for (int i = 0; i < 8; i++)
        g_ps1MdecSnap[i] = r[i];
    g_ps1MdecSnapTaken = 1;
}

// PIO output order. Real hardware returns a 15-bit macroblock as four 8x8 blocks (Y1..Y4: top-left,
// top-right, bottom-left, bottom-right) and leaves the re-ordering to DMA1 (psx-spx
// macroblockdecodermdec.md; ps1-tests mdec_readDecoded); DuckStation returns it already in rows. One
// test macroblock tells which: its four quadrants have different brightness.
static int mdecCalibrate() {
    static const uint32_t rle[] = {
        // Cr, Cb: DC 0; Y1 -300, Y2 +300, Y3 -60, Y4 +60 (quant scale 1), each block ended by FE00
        0xfe000400, 0xfe000400, 0xfe000400 | (0x3ff & (uint32_t)-300), 0xfe000400 | 300u,
        0xfe000400 | (0x3ff & (uint32_t)-60), 0xfe000400 | 60u,
    };
    uint32_t words[32];
    for (int i = 0; i < 32; i++)
        words[i] = i < 6 ? rle[i] : 0xfe00fe00; // padded to 32 words with FE00 (psx-spx)
    uint32_t out[128];
    if (DecDCTCommandPIO(MDEC_CMD_15BIT | 32) < 0)
        return -1;
    int in = 0;
    for (int w = 0; w < 128; w++) {
        uint32_t t0 = PS1Hblanks();
        for (;;) {
            uint32_t st = DecDCTStatus();
            if (in < 32 && !(st & DECDCT_STAT_IN_FULL)) {
                DecDCTPutWord(words[in++]);
                continue;
            }
            if (!(st & DECDCT_STAT_OUT_EMPTY))
                break;
            if (PS1HblanksSince(t0) > MDEC_WAIT_HBL)
                return -1;
        }
        out[w] = DecDCTGetWord();
    }
    // The rest of the padding completes the command (else the MDEC stays busy waiting for it).
    if (DecDCTParamsPIO(words + in, 32 - in) < 0)
        return -1;
    // 8x8 blocks: words 0..31 are all Y1 (word 4 = row 1 of Y1); rows: word 4 = row 0, pixels 8-9 = Y2.
    if (out[0] == out[4] && out[0] != out[32])
        return 1;
    if (out[0] != out[4] && out[0] == out[8])
        return 0;
    return -1;
}

static bool mdecSetMode(uint32_t mode) {
    if (mode == MDEC_PIO) {
        if (DecDCTResetPIO(nullptr) < 0)
            return false;
    } else {
        DecDCTReset(0); // priorities, DMA requests on, tables by DMA
        DecDCTControl(s_mdecControl[mode]);
    }
    if (mode != MDEC_DMA) { // the CPU reads the pixels: learn their order
        g_ps1MdecOrder = mdecCalibrate();
        if (g_ps1MdecOrder < 0)
            return false;
    }
    s_mdecModeInit = mode;
    g_ps1MdecMode  = mode;
    return true;
}

// PIO: one interleaved loop keeps the input FIFO fed and takes the pixels as they come, placing each
// 32-word 8x8 block (or 16-pixel row) in its column of s_video. Hybrid: DMA0 feeds the input, the loop
// only reads. false = the MDEC stopped (timeout).
// The feeder stays at most 32 words (the real input FIFO) ahead of what the MDEC has taken, counted with
// MDEC1's "parameter words remaining": DuckStation's FIFO is 512 halfwords and drops a word written when
// only one is free, which its full flag doesn't show. Where that field counts received words the limit
// never binds, and "input FIFO full" does the job.
static bool mdecRunPIO(bool feed) {
    const uint32_t *in = s_rle + 1;
    uint32_t left      = feed ? s_rlePos : 0;
    bool dead          = g_ps1MdecTestDead != 0;
    if (dead) {
        g_ps1MdecTestDead = g_ps1MdecTestDead - 1;
        left              = 0;
    } else if (!feed) {
        uint32_t t0 = PS1Hblanks();
        while (DecDCTStatus() & DECDCT_STAT_BUSY) {
            if (PS1HblanksSince(t0) > MDEC_WAIT_HBL) {
                mdecFail(2, 0, 0, 0, 0);
                return false;
            }
        }
        DecDCTin(s_rle, DECDCT_MODE_16BPP_BIT15); // DMA0
    } else if (DecDCTCommandPIO(MDEC_CMD_15BIT | s_rlePos) < 0) {
        mdecFail(2, 0, 0, 0, left);
        return false;
    }
    const bool blocks = g_ps1MdecOrder == 1;
    const int  mbRows = s_height / 16;
    for (int x = 0; x < s_width; x += STR_COLUMN) {
        uint16_t *column = s_video + (x / STR_COLUMN) * STR_COLUMN * s_height;
        for (int mb = 0; mb < mbRows; mb++) {
            uint16_t *base = column + mb * 16 * STR_COLUMN;
            for (int w = 0; w < 128; w++) {
                uint32_t t0 = PS1Hblanks();
                for (;;) {
                    uint32_t st = DecDCTStatus();
                    if (left && !(st & DECDCT_STAT_IN_FULL)) {
                        uint32_t expected = ((st & 0xffff) + 1) & 0xffff; // parameter words still due
                        if ((int32_t)(expected - left) < 32) {            // written - taken (in flight)
                            DecDCTPutWord(*in++);
                            left--;
                            continue;
                        }
                    }
                    if (!(st & DECDCT_STAT_OUT_EMPTY))
                        break;
                    if (PS1HblanksSince(t0) > MDEC_WAIT_HBL) {
                        mdecFail(3, x, mb, w, left);
                        return false;
                    }
                }
                uint32_t v = DecDCTGetWord();
                int row, col;
                if (blocks) {
                    int b = w >> 5, i = w & 31;
                    row = (b >> 1) * 8 + (i >> 2);
                    col = (b & 1) * 8 + (i & 3) * 2;
                } else {
                    row = w >> 3;
                    col = (w & 7) * 2;
                }
                *(uint32_t *)(base + row * STR_COLUMN + col) = v; // 2 pixels, word-aligned (col even)
            }
        }
    }
    // The FE00 padding after the last macroblock completes the command (the next command waits for it).
    if (DecDCTParamsPIO(in, left) < 0) {
        mdecFail(4, s_width, 0, 0, left);
        return false;
    }
    return true;
}

// DMA mode, one column per MDEC-out DMA. Real hardware: a CD sector DMA must not run while the MDEC-out
// DMA (DMA1) does (PSn00bSDK examples/mdec/strvideo: "a hardware conflict ... which shall not run
// simultaneously"); our sector DMA starts in the CD IRQ, so the CD IRQ is masked while DMA1 runs. DMA1
// starts only once the MDEC has output (ps1-tests mdec_readDecodedDma), so a stuck MDEC is noticed on its
// status register, never behind a waiting DMA (which stalls the CPU's bus reads).
static bool mdecOut(uint32_t *dst, size_t words) {
    using namespace psyqo::Hardware::CPU;
    uint32_t t0 = PS1Hblanks();
    while (DecDCTStatus() & DECDCT_STAT_OUT_EMPTY) {
        if (PS1HblanksSince(t0) > MDEC_WAIT_HBL) {
            g_ps1VideoSyncFail = g_ps1VideoSyncFail + 1;
            mdecFail(5, 0, 0, 0, 0);
            return false;
        }
    }
    IMask.clear(IRQ::CDRom);
    flushWriteQueue();
    DecDCTout(dst, words);
    bool ok = true;
    t0      = PS1Hblanks();
    while (DecDCToutSync(1)) {
        if (PS1HblanksSince(t0) > MDEC_WAIT_HBL) {
            g_ps1VideoSyncFail = g_ps1VideoSyncFail + 1;
            mdecFail(6, 0, 0, 0, 0);
            ok = false;
            break;
        }
    }
    if (IReg.isSet(IRQ::CDRom))
        g_ps1VideoCdDeferred = g_ps1VideoCdDeferred + 1;
    IMask.set(IRQ::CDRom);
    return ok;
}

static bool mdecRunDMA() {
    if (g_ps1MdecTestDead) {
        g_ps1MdecTestDead = g_ps1MdecTestDead - 1;
        return mdecOut((uint32_t *)s_video, 32); // no command: the MDEC never has output
    }
    uint32_t t0 = PS1Hblanks();
    while (DecDCTStatus() & DECDCT_STAT_BUSY)
        if (PS1HblanksSince(t0) > MDEC_WAIT_HBL)
            return false;
    // Bit 15 (STP) set on every texel: MDEC's pure black would otherwise be 0x0000 =
    // transparent, and the additive fade overlay would then hit the background twice.
    DecDCTin(s_rle, DECDCT_MODE_16BPP_BIT15);
    for (int x = 0; x < s_width; x += STR_COLUMN)
        if (!mdecOut((uint32_t *)(s_video + (x / STR_COLUMN) * STR_COLUMN * s_height), (size_t)(STR_COLUMN * s_height / 2)))
            return false;
    return true;
}

static void mdecRun() {
    uint32_t t0 = PS1Hblanks();
    int32_t forced = g_ps1MdecForceMode;
    uint32_t want  = forced >= 0 ? (uint32_t)forced : g_ps1MdecMode;
    bool ok        = want == s_mdecModeInit || mdecSetMode(want);
    if (!ok)
        mdecFail(1, (int)want, 0, 0, 0);
    if (ok)
        ok = s_mdecModeInit == MDEC_DMA ? mdecRunDMA() : mdecRunPIO(s_mdecModeInit == MDEC_PIO);
    if (ok) {
        s_faultRun = 0;
    } else {
        // Stuck: keep the registers, reset the MDEC (the picture is lost), and after two in a row try the
        // other mode.
        g_ps1MdecFaults = g_ps1MdecFaults + 1;
        mdecSnapshot();
        DecDCTAbort(s_mdecModeInit < 3 ? s_mdecControl[s_mdecModeInit] : 0);
        if (++s_faultRun >= 2 && forced < 0) {
            s_faultRun = 0;
            // PIO -> hybrid (DMA0 in, CPU out) -> DMA -> PIO
            g_ps1MdecMode     = g_ps1MdecMode == MDEC_PIO ? MDEC_HYBRID : g_ps1MdecMode == MDEC_HYBRID ? MDEC_DMA : MDEC_PIO;
            s_mdecModeInit    = 0xff; // set up again (tables, calibration) before the next picture
            g_ps1MdecSwitches = g_ps1MdecSwitches + 1;
        }
    }
    g_ps1VideoMdecHbl = PS1HblanksSince(t0);
    s_nextFrame++;
    g_ps1VideoFrames = g_ps1VideoFrames + 1;
}

int PS1VideoNextFrame() {
    if (!s_str)
        return -1;
    int r = fetchFrame();
    if (r <= 0)
        return r;
    // Huffman decode -> MDEC RLE buffer (s_rle[0] = MDEC(1) command word), in one go.
    uint32_t t0 = PS1Hblanks();
    if (DecDCTvlcStart2(&s_vlcCtx, s_rle, 0, (const uint32_t *)s_bs) < 0)
        return -1;
    g_ps1VideoVlcHbl = PS1HblanksSince(t0);
    mdecRun();
    return 1;
}

void PS1VideoClose() {
    PS1StreamClose();
    s_xa         = false;
    g_ps1VideoXa = 0;
    if (s_str) {
        fclose(s_str);
        s_str = nullptr;
    }
    freeBuffers();
}

bool PS1VideoIsOpen() { return s_str != nullptr; }
int  PS1VideoFrameCount() { return s_frameCount; }

void PS1VideoRequestFrame() {
    // Capped: a stall can't make later frames flash by to "catch up"; the video just runs
    // a frame or two behind the script (the intro has 74 requests of slack after its end).
    if (s_str && s_requested < 2)
        s_requested++;
}

// ---- Game-side pipeline -------------------------------------------------------------
// One frame's VLC (~285 hblanks = 18 ms on the CPU, up to ~700 on busy frames) + MDEC (~77)
// is longer than a game frame, so decoding runs ahead in slices of at most WORK_BUDGET_HBL
// per game frame: fetch -> VLC in chunks -> MDEC into s_video -> handed over when the game
// asks (NextVideoFrame). The VLC of frame N+1 proceeds while frame N waits in s_video; only
// the MDEC (which overwrites s_video) waits for the handover.

// Measured in the intro: game logic + chain building ~55-75 hblanks per frame, so the
// video's share of a game frame (upload + decode, counted from PS1UpdateVideoTexture's
// start) stops starting new slices after 170 and never lets MDEC run past 190 (262 = one
// frame; heaviest measured intro frame at these values: see docs/20).
#define VLC_CHUNK_WORDS 512 // ~11-25 hblanks each
#define WORK_BUDGET_HBL 170
#define MDEC_LIMIT_HBL  190
#define MDEC_EST_HBL    80

volatile uint32_t g_ps1VideoWorkMaxHbl = 0; // longest decode slice spent in one game frame
volatile uint32_t g_ps1VideoLate       = 0; // game frames where a requested frame wasn't decoded yet
volatile uint32_t g_ps1VideoDropped    = 0; // frames skipped (not decoded) so the stream never pauses
volatile uint32_t g_ps1VideoFetchMaxHbl = 0; // slowest fetchFrame (resync + 20 KB copy + pump)

// One VLC chunk appended to s_rle. Every chunk starts with its own MDEC(1) header word; it
// is written over the previous chunk's last data word (saved + restored), so the data stays
// contiguous and a single header covering the whole frame is patched in at the end.
// 0 = frame complete, 1 = more to do, -1 = bad bitstream.
static int vlcChunk() {
    uint32_t t0 = PS1Hblanks();
    int r;
    if (s_rlePos == 0) {
        r = DecDCTvlcStart2(&s_vlcCtx, s_rle, VLC_CHUNK_WORDS + 1, (const uint32_t *)s_bs);
        if (r >= 0)
            s_rlePos = s_rle[0] & 0xffff;
    } else {
        uint32_t *at    = s_rle + s_rlePos;
        uint32_t  saved = *at;
        r               = DecDCTvlcContinue2(&s_vlcCtx, at, VLC_CHUNK_WORDS + 1);
        uint32_t n      = *at & 0xffff;
        *at             = saved;
        if (r >= 0)
            s_rlePos += n;
    }
    if (r == 0)
        s_rle[0] = 0x38000000 | s_rlePos;
    s_vlcAcc += PS1HblanksSince(t0);
    return r;
}

// While the game is waiting for frames, the stream must never fill the ring: a paused
// stream restarts with a seek (~200 ms without data) and at 2x the CD delivers exactly the
// 15 fps the game asks for, so that time is never won back. When the decoder falls behind,
// the oldest buffered frame is dropped instead (STR frames are all intra-coded). When the
// game isn't asking (the intro's fade-in shows frame 1 for ~4 s), pausing is intended.
#define RING_DROP_SECTORS (STR_RING_SECTORS - 4) // ~1.6 game frames of margin at 2.5 sectors/frame

// With XA audio the stream never pauses (the music would stop), so frames are dropped whether
// the game is asking or not: the video follows the stream, which runs in real time like the music.
static void relieveRing() {
    if (s_requested == 0 && !s_xa)
        return;
    while (PS1StreamAvailable() >= RING_DROP_SECTORS) {
        const StrSectorHeader *h = hdr(0);
        if (h->magic == STR_MAGIC && h->sectorIndex == 0 && h->sectorCount >= 1 && h->sectorCount <= STR_SECTORS_PER_FRAME) {
            PS1StreamConsume(h->sectorCount); // fetchFrame's resync accepts the next one
            g_ps1VideoDropped = g_ps1VideoDropped + 1;
        } else {
            PS1StreamConsume(1);
            g_ps1VideoResync = g_ps1VideoResync + 1;
        }
    }
}

int PS1VideoService() {
    if (!s_str)
        return -1;
    // An STR with audio plays at stream speed, like a real FMV player: every decoded frame is
    // shown as soon as it's ready, so the picture follows the stream (and its music) instead of
    // the script's NextVideoFrame cadence; ring pressure drops stale frames. Video-only STRs stay
    // paced by the script (PC semantics).
    if (s_xa && s_requested == 0)
        s_requested = 1;
    PS1StreamPump(); // keep reading ahead even between requests
    relieveRing();
    if (s_pixReady && s_requested > 0) {
        s_requested--;
        s_pixReady = false;
        return 1; // the caller uploads s_video now, then calls PS1VideoDecode()
    }
    if (s_requested > 0 && !s_xa)
        g_ps1VideoLate = g_ps1VideoLate + 1; // the game is waiting for a frame (script-paced STRs)
    return 0;
}

volatile uint32_t g_ps1VideoNoData = 0; // videos ended because the drive stopped delivering data

// An STR with XA audio never pauses the drive on purpose: when no data sector arrives for this long
// while some are still due, the drive has stopped delivering them (its XA audio may still play, as on
// the PSone with Sonic CD). The video ends instead of waiting forever. The first sector needs a seek.
#define NO_DATA_VBLANKS 180

int PS1VideoDecode(uint32_t t0) {
    if (!s_str)
        return -1;
    if (s_xa) {
        uint32_t delivered = PS1StreamDelivered(), vbl = g_ps1GPU->getFrameCount();
        if (delivered != s_lastDelivered || delivered >= s_dataSectors) {
            s_lastDelivered = delivered;
            s_lastDataVbl   = vbl;
        } else if (vbl - s_lastDataVbl > NO_DATA_VBLANKS) {
            g_ps1VideoNoData = g_ps1VideoNoData + 1;
            return -1;
        }
    }
    while (PS1HblanksSince(t0) < WORK_BUDGET_HBL) {
        if (s_stage == STAGE_EMPTY) {
            uint32_t tf = PS1Hblanks();
            int r       = fetchFrame();
            tf          = PS1HblanksSince(tf);
            if (tf > g_ps1VideoFetchMaxHbl)
                g_ps1VideoFetchMaxHbl = tf;
            if (r < 0) { // end of stream: report it once the game asks for a frame past it
                if (s_pixReady || s_requested == 0)
                    break;
                return -1;
            }
            if (r == 0)
                break; // sectors not here yet
            s_rlePos = 0;
            s_vlcAcc = 0;
            s_stage  = STAGE_VLC;
        } else if (s_stage == STAGE_VLC) {
            int r = vlcChunk();
            if (r < 0)
                return -1;
            if (r == 0) {
                g_ps1VideoVlcHbl = s_vlcAcc;
                s_stage          = STAGE_MDEC;
            }
        } else { // STAGE_MDEC
            if (s_pixReady)
                break; // s_video still holds a frame the game hasn't taken yet
            if (PS1HblanksSince(t0) + MDEC_EST_HBL > MDEC_LIMIT_HBL)
                break; // next game frame
            mdecRun();
            s_pixReady = true;
            s_stage    = STAGE_EMPTY;
        }
    }
    relieveRing();
    uint32_t spent = PS1HblanksSince(t0);
    if (spent > g_ps1VideoWorkMaxHbl)
        g_ps1VideoWorkMaxHbl = spent;
    return 0;
}

uint16_t *PS1VideoPixels() { return s_video; }
int       PS1VideoWidth()  { return s_width; }
int       PS1VideoHeight() { return s_height; }
