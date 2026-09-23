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
#define PS1_CLUT_CHUNK        22 // halfwords per CLUT upload chunk: 3 + 11 words = 14 // 84 * 3 words = 252 <= 255-word chained fragment limit

volatile uint32_t g_ps1ChainOverflow  = 0;
volatile uint32_t g_ps1ChainHighWater = 0;
volatile uint32_t g_ps1ChainLastWords = 0;

namespace {

typedef psyqo::Fragments::FixedFragment<psyqo::Prim::Sprite16x16, PS1_TILE_BATCH> TileBatch;

struct ClutChunkFragment {
    uintptr_t head;                 // psyqo chain link (must be uintptr_t)
    psyqo::Prim::VRAMUpload upload; // 3 words
    uint16_t data[PS1_CLUT_CHUNK];  // 11 words
    unsigned words;                 // payload size in words (not sent)
    size_t getActualFragmentSize() const { return words; }
};
static_assert(sizeof(psyqo::Prim::VRAMUpload) / 4 + PS1_CLUT_CHUNK / 2 <= PS1_CHAIN_MAX_WORDS, "CLUT chunk too big");
static_assert(PS1_TILE_BATCH * sizeof(psyqo::Prim::Sprite16x16) / 4 <= PS1_CHAIN_MAX_WORDS, "tile batch too big");

alignas(4) uint32_t s_arena[2][PS1_CHAIN_ARENA_WORDS];
uint32_t *s_base    = nullptr;
uint32_t *s_cur     = nullptr;
uint32_t *s_end     = nullptr;
psyqo::GPU *s_gpu   = nullptr;
TileBatch *s_batch  = nullptr; // open tile batch (not yet chained)
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
}

template <typename Prim>
void chainSimple(const Prim &prim) {
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
    s_tpageValid = false;
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

void PS1ChainClear(uint8_t r, uint8_t g, uint8_t b) {
    flushBatch();
    auto *frag = allocRaw<psyqo::Fragments::SimpleFragment<psyqo::Prim::FastFill>>();
    if (!frag)
        return;
    new (frag) psyqo::Fragments::SimpleFragment<psyqo::Prim::FastFill>();
    s_gpu->getNextClear(frag->primitive, psyqo::Color{{.r = r, .g = g, .b = b}});
    s_gpu->chain(*frag);
}

void PS1ChainUploadRow(const uint16_t *data, int count, int16_t x, int16_t y) {
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

void PS1ChainSprite16(const psyqo::Prim::Sprite16x16 &s) {
    if (!s_batch || s_batch->count >= PS1_TILE_BATCH) {
        flushBatch();
        s_batch = allocRaw<TileBatch>();
        if (!s_batch)
            return;
        new (s_batch) TileBatch();
        s_batch->count = 0;
    }
    s_batch->primitives[s_batch->count++] = s;
}

void PS1ChainSprite(const psyqo::Prim::Sprite &s) { chainSimple(s); }

void PS1ChainRect(const psyqo::Prim::Rectangle &r) { chainSimple(r); }

void PS1ChainQuad(const psyqo::Prim::TexturedQuad &q) {
    chainSimple(q);
    s_tpageValid = false; // a textured polygon's texpage attribute changes the GPU draw mode
}
