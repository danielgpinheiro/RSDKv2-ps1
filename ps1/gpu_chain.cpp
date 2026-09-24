/*
 * PS1 DMA-chained renderer helpers — see gpu_chain.hh.
 */
#include "gpu_chain.hh"

#include <string.h>

#include <new>

#include <psyqo/fragments.hh>
#include <psyqo/gpu.hh>
#include <psyqo/primitives/control.hh>
#include <psyqo/primitives/misc.hh>

// Arena per frame parity: 24 KB each (sized from the measured high-water mark, see
// docs/20). A frame's arena is being DMA'd while the next frame builds in the other one.
#define PS1_CHAIN_ARENA_WORDS (24 * 1024 / 4)
// Every chained fragment is kept <= 14 words (psyqo GPU::c_chainThreshold / 4). psyqo links
// bigger fragments with bit 23 set so the linked-list DMA stops, then its DMA IRQ reads DMA2
// MADR to find and send the big node by hand. Real hardware leaves MADR at the failing link,
// but PCSX-Redux just treats bit 23 as end-of-list and leaves MADR at the chain start, so psyqo
// re-sends earlier nodes: primitives get drawn twice and out of order (verified: Sonic drawn
// over the front grass). Small fragments avoid that path on both.
#define PS1_CHAIN_MAX_WORDS   14
#define PS1_TILE_BATCH        4  // 4 * 3 words = 12 <= 14
#define PS1_SPRITE_BATCH      3  // 3 * 4 words = 12 <= 14
#define PS1_CLUT_CHUNK        22 // halfwords per CLUT upload chunk: 3 + 11 words = 14 // 84 * 3 words = 252 <= 255-word chained fragment limit

volatile uint32_t g_ps1ChainOverflow  = 0;
volatile uint32_t g_ps1ChainHighWater = 0;
volatile uint32_t g_ps1ChainLastWords = 0;
volatile uint32_t g_ps1ChainReplayWords = 0;

namespace {

typedef psyqo::Fragments::FixedFragment<psyqo::Prim::Sprite16x16, PS1_TILE_BATCH> TileBatch;
typedef psyqo::Fragments::FixedFragment<psyqo::Prim::Sprite, PS1_SPRITE_BATCH> SpriteBatch;

struct ClutChunkFragment {
    uintptr_t head;                 // psyqo chain link (must be uintptr_t)
    psyqo::Prim::VRAMUpload upload; // 3 words
    uint16_t data[PS1_CLUT_CHUNK];  // 11 words
    unsigned words;                 // payload size in words (not sent)
    size_t getActualFragmentSize() const { return words; }
};
static_assert(sizeof(psyqo::Prim::VRAMUpload) / 4 + PS1_CLUT_CHUNK / 2 <= PS1_CHAIN_MAX_WORDS, "CLUT chunk too big");
static_assert(PS1_TILE_BATCH * sizeof(psyqo::Prim::Sprite16x16) / 4 <= PS1_CHAIN_MAX_WORDS, "tile batch too big");
static_assert(PS1_SPRITE_BATCH * sizeof(psyqo::Prim::Sprite) / 4 <= PS1_CHAIN_MAX_WORDS, "sprite batch too big");

// A copied fragment (PS1ChainReplay): payload <= PS1_CHAIN_MAX_WORDS like every fragment here.
struct RawFragment {
    uintptr_t head;
    uint32_t data[PS1_CHAIN_MAX_WORDS];
    unsigned words;
    size_t getActualFragmentSize() const { return words; }
};

alignas(4) uint32_t s_arena[2][PS1_CHAIN_ARENA_WORDS];
void (*s_hook)() = nullptr;

inline void runHook() {
    if (s_hook) {
        auto h = s_hook;
        s_hook = nullptr;
        h();
    }
}
uint32_t *s_base    = nullptr;
uint32_t *s_cur     = nullptr;
uint32_t *s_end     = nullptr;
psyqo::GPU *s_gpu   = nullptr;
TileBatch *s_batch  = nullptr; // open tile batch (not yet chained)
SpriteBatch *s_sbatch = nullptr; // open sprite batch (not yet chained); at most one batch is open
uint16_t s_lastTPage = 0xFFFF;  // raw TPageAttr of the last chained TPage (0xFFFF = unknown)
bool s_tpageValid    = false;

template <typename T>
T *allocRaw() {
    size_t words = (sizeof(T) + 3) / 4;
    if (!s_cur || s_cur + words > s_end) {
        g_ps1ChainOverflow = g_ps1ChainOverflow + 1;
        return nullptr;
    }
    T *p = reinterpret_cast<T *>(s_cur);
    s_cur += words;
    return p;
}

void flushBatch() {
    if (s_batch) {
        if (s_batch->count)
            s_gpu->chain(*s_batch);
        s_batch = nullptr;
    }
    if (s_sbatch) {
        if (s_sbatch->count)
            s_gpu->chain(*s_sbatch);
        s_sbatch = nullptr;
    }
}

template <typename Prim>
void chainSimple(const Prim &prim) {
    runHook();
    flushBatch();
    auto *frag = allocRaw<psyqo::Fragments::SimpleFragment<Prim>>();
    if (!frag)
        return;
    new (frag) psyqo::Fragments::SimpleFragment<Prim>(prim);
    s_gpu->chain(*frag);
}

} // namespace

void PS1ChainBegin(psyqo::GPU &gpu) {
    s_gpu        = &gpu;
    s_base       = s_arena[gpu.getParity()];
    s_cur        = s_base;
    s_end        = s_base + PS1_CHAIN_ARENA_WORDS;
    s_batch      = nullptr;
    s_sbatch     = nullptr;
    s_tpageValid = false;
    s_hook       = nullptr;
    g_ps1ChainReplayWords = 0;
}

void PS1ChainEnd() {
    flushBatch();
    uint32_t used = (uint32_t)(s_cur - s_base);
    g_ps1ChainLastWords = used;
    if (used > g_ps1ChainHighWater)
        g_ps1ChainHighWater = used;
}

void PS1GpuImmediate() {
    if (s_gpu)
        s_gpu->waitChainIdle();
}

psyqo::Prim::FastFill *PS1ChainClear(uint8_t r, uint8_t g, uint8_t b) {
    flushBatch();
    auto *frag = allocRaw<psyqo::Fragments::SimpleFragment<psyqo::Prim::FastFill>>();
    if (!frag)
        return nullptr;
    new (frag) psyqo::Fragments::SimpleFragment<psyqo::Prim::FastFill>();
    s_gpu->getNextClear(frag->primitive, psyqo::Color{{.r = r, .g = g, .b = b}});
    s_gpu->chain(*frag);
    return &frag->primitive;
}

void PS1ChainUploadRow(const uint16_t *data, int count, int16_t x, int16_t y) {
    runHook();
    flushBatch();
    for (int off = 0; off < count; off += PS1_CLUT_CHUNK) {
        int n    = (count - off < PS1_CLUT_CHUNK) ? (count - off) : PS1_CLUT_CHUNK; // 256 = 22 x 11 + 14
        auto *fr = allocRaw<ClutChunkFragment>();
        if (!fr)
            return;
        new (&fr->upload) psyqo::Prim::VRAMUpload();
        fr->upload.region = psyqo::Rect{{(int16_t)(x + off), y}, {(int16_t)n, 1}};
        memcpy(fr->data, data + off, n * sizeof(uint16_t));
        fr->words = (unsigned)(sizeof(psyqo::Prim::VRAMUpload) / 4 + n / 2); // n is even
        s_gpu->chain(*fr);
    }
}

int PS1ChainReserveRow(int count, int16_t x, int16_t y, uint16_t **chunkData, uint16_t *chunkLen, int maxChunks) {
    runHook();
    flushBatch();
    int chunks = 0;
    for (int off = 0; off < count && chunks < maxChunks; off += PS1_CLUT_CHUNK) {
        int n    = (count - off < PS1_CLUT_CHUNK) ? (count - off) : PS1_CLUT_CHUNK;
        auto *fr = allocRaw<ClutChunkFragment>();
        if (!fr)
            return 0;
        new (&fr->upload) psyqo::Prim::VRAMUpload();
        fr->upload.region = psyqo::Rect{{(int16_t)(x + off), y}, {(int16_t)n, 1}};
        fr->words         = (unsigned)(sizeof(psyqo::Prim::VRAMUpload) / 4 + n / 2); // n is even
        s_gpu->chain(*fr);
        chunkData[chunks] = fr->data;
        chunkLen[chunks]  = (uint16_t)n;
        ++chunks;
    }
    return chunks;
}

namespace {
// GP0(80h) VRAM-to-VRAM rectangle copy (no psyqo primitive for it).
struct VRAMCopy {
    uint32_t command = 0x80000000;
    psyqo::Vertex src, dst, size;
};
static_assert(sizeof(VRAMCopy) == 16, "VRAMCopy is 4 words");
} // namespace

void PS1ChainRepeatLastFrame() {
    psyqo::Prim::FastFill next; // getNextClear() tells which buffer this frame's chain draws into
    s_gpu->getNextClear(next, psyqo::Color{{.r = 0, .g = 0, .b = 0}});
    VRAMCopy c;
    c.dst  = next.rect.pos;
    c.src  = psyqo::Vertex{{.x = next.rect.pos.x, .y = (int16_t)(next.rect.pos.y ? 0 : 256)}};
    c.size = next.rect.size;
    chainSimple(c);
}

void PS1ChainFlushCache() {
    flushBatch();
    chainSimple(psyqo::Prim::FlushCache());
}

void PS1ChainUploadCLUT(const uint16_t *clut256, int16_t x, int16_t y) {
    PS1ChainUploadRow(clut256, 256, x, y);
    // The GPU caches the CLUT: flush after changing it at the same VRAM location.
    PS1ChainFlushCache();
}

void PS1ChainTPage(psyqo::PrimPieces::TPageAttr attr) {
    runHook();
    uint16_t raw;
    static_assert(sizeof(attr) == sizeof(raw), "TPageAttr is 16 bits");
    memcpy(&raw, &attr, sizeof(raw));
    if (s_tpageValid && raw == s_lastTPage)
        return;
    psyqo::Prim::TPage tp;
    tp.attr = attr;
    chainSimple(tp);
    s_lastTPage  = raw;
    s_tpageValid = true;
}

void PS1ChainInvalidateTPage() { s_tpageValid = false; }

bool g_ps1ChainSkipPrims = false;

void PS1ChainSprite16(const psyqo::Prim::Sprite16x16 &s) {
    if (g_ps1ChainSkipPrims)
        return;
    runHook();
    if (!s_batch || s_batch->count >= PS1_TILE_BATCH) {
        flushBatch(); // also closes an open sprite batch (keeps the draw order)
        s_batch = allocRaw<TileBatch>();
        if (!s_batch)
            return;
        new (s_batch) TileBatch();
        s_batch->count = 0;
    }
    s_batch->primitives[s_batch->count++] = s;
}

void PS1ChainSprite16Raw(uint32_t xy, uint32_t tex) {
    static const uint32_t cmd = [] {
        psyqo::Prim::Sprite16x16 s(psyqo::Color{{.r = 0x80, .g = 0x80, .b = 0x80}});
        uint32_t w;
        memcpy(&w, &s, 4);
        return w;
    }();
    if (g_ps1ChainSkipPrims)
        return;
    runHook();
    if (!s_batch || s_batch->count >= PS1_TILE_BATCH) {
        flushBatch();
        s_batch = allocRaw<TileBatch>();
        if (!s_batch)
            return;
        new (s_batch) TileBatch();
        s_batch->count = 0;
    }
    uint32_t *w = reinterpret_cast<uint32_t *>(&s_batch->primitives[s_batch->count++]);
    w[0]        = cmd;
    w[1]        = xy;
    w[2]        = tex;
}

void PS1ChainSprite(const psyqo::Prim::Sprite &s) {
    if (g_ps1ChainSkipPrims)
        return;
    runHook();
    if (!s_sbatch || s_sbatch->count >= PS1_SPRITE_BATCH) {
        flushBatch();
        s_sbatch = allocRaw<SpriteBatch>();
        if (!s_sbatch)
            return;
        new (s_sbatch) SpriteBatch();
        s_sbatch->count = 0;
    }
    s_sbatch->primitives[s_sbatch->count++] = s;
}

void PS1ChainRect(const psyqo::Prim::Rectangle &r) {
    if (!g_ps1ChainSkipPrims)
        chainSimple(r);
}

void PS1ChainQuad(const psyqo::Prim::TexturedQuad &q) {
    if (g_ps1ChainSkipPrims)
        return;
    chainSimple(q);
    s_tpageValid = false; // a textured polygon's texpage attribute changes the GPU draw mode
}

uint32_t *PS1ChainMark() {
    flushBatch();
    return s_cur;
}

int PS1ChainReplay(uint32_t *from, uint32_t *to) {
    flushBatch();
    int words = 0;
    uint32_t *p = from;
    for (int guard = 0; p && p != to && p >= s_base && p < s_end && guard < 4096; ++guard) {
        uint32_t head = *p;
        unsigned n    = head >> 24;
        uint32_t *next = reinterpret_cast<uint32_t *>(0x80000000u | (head & 0xffffff));
        if (n && n <= PS1_CHAIN_MAX_WORDS && (p[1] >> 24) != 0xA0) { // skip CPU->VRAM uploads (CLUT rows)
            auto *f = allocRaw<RawFragment>();
            if (!f)
                break;
            memcpy(f->data, p + 1, n * 4);
            f->words = n;
            s_gpu->chain(*f);
            words += n + 1;
        }
        p = next;
    }
    g_ps1ChainReplayWords = g_ps1ChainReplayWords + words;
    s_tpageValid = false;
    return words;
}

static int16_t chainBufferY() { // the buffer this frame's chain draws into (psyqo getNextScissor)
    return s_gpu->getParity() ? 0 : 256;
}

void PS1ChainDrawArea(int x0, int y0, int x1, int y1) {
    int16_t by = chainBufferY();
    chainSimple(psyqo::Prim::DrawingAreaStart(psyqo::Vertex{{.x = (int16_t)x0, .y = (int16_t)(by + y0)}}));
    chainSimple(psyqo::Prim::DrawingAreaEnd(psyqo::Vertex{{.x = (int16_t)x1, .y = (int16_t)(by + y1)}}));
}

void PS1ChainDrawAreaReset() { PS1ChainDrawArea(0, 0, 320, 240); }

void PS1ChainMaskControl(bool forceSet, bool test) {
    chainSimple(psyqo::Prim::MaskControl(forceSet ? psyqo::Prim::MaskControl::ForceSet : psyqo::Prim::MaskControl::FromSource,
                                         test ? psyqo::Prim::MaskControl::Test::Yes : psyqo::Prim::MaskControl::Test::No));
}

void PS1ChainSetHook(void (*fn)()) { s_hook = fn; }
void PS1ChainRunHook() { runHook(); }
