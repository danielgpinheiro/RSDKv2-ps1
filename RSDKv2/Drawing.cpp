#include "RetroEngine.hpp"
#if RETRO_PLATFORM == RETRO_PS1
#include "common/syscalls/syscalls.h"
#include "../ps1/sprite_atlas.hh"
#include "../ps1/version_stamp.hh"
#endif


int SCREEN_XSIZE   = 320;
int SCREEN_CENTERX = (SCREEN_XSIZE / 2);

#if RETRO_PLATFORM != RETRO_PS1
byte BlendLookupTable[0x100 * 0x100]; // PS1: blending is done by GPU semi-transparency, not a LUT
#endif

byte TintLookupTable1[0x100];
byte TintLookupTable2[0x100];
byte TintLookupTable3[0x100];
byte TintLookupTable4[0x100];

DrawListEntry ObjectDrawOrderList[DRAWLAYER_COUNT];

int GfxDataPosition;
GFXSurface GfxSurface[SURFACE_MAX];
byte GraphicData[GFXDATA_MAX];

// DrawHLineScrollLayer's scroll-state update, shared by the PC rasteriser and the PS1 GPU path:
// advances the BG layer autoscroll and the HParallax line positions exactly like PC (this is
// game state as well as visuals) and returns the layer's Y scroll offset in pixels.
static int SetupHLineScrollLayer(int layerID) {
    TileLayer *layer = &StageLayouts[activeTileLayers[layerID]];
    int layerwidth   = layer->xsize;
    int yscrollOffset = 0;
    if (activeTileLayers[layerID]) { // BG Layer
        int yScroll    = YScrollOffset * layer->parallaxFactor >> 6;
        int fullheight = layer->ysize << 7;
        layer->scrollPos += layer->scrollSpeed;
        if (layer->scrollPos > fullheight << 16)
            layer->scrollPos -= fullheight << 16;
        yscrollOffset = (yScroll + (layer->scrollPos >> 16)) % fullheight;
    } else { // FG Layer
        LastXSize     = layer->xsize;
        yscrollOffset = YScrollOffset;
        for (int i = 0; i < PARALLAX_COUNT; ++i) HParallax.linePos[i] = XScrollOffset;
    }

    if (layer->type == LAYER_HSCROLL) {
        if (LastXSize != layerwidth) {
            int fullLayerwidth = layerwidth << 7;
            for (int i = 0; i < HParallax.entryCount; ++i) {
                HParallax.linePos[i] = XScrollOffset * HParallax.parallaxFactor[i] >> 7;
                HParallax.scrollPos[i] += HParallax.scrollSpeed[i];
                if (HParallax.scrollPos[i] > fullLayerwidth << 16)
                    HParallax.scrollPos[i] -= fullLayerwidth << 16;
                if (HParallax.scrollPos[i] < 0)
                    HParallax.scrollPos[i] += fullLayerwidth << 16;
                HParallax.linePos[i] += HParallax.scrollPos[i] >> 16;
                HParallax.linePos[i] %= fullLayerwidth;
            }
        }
        LastXSize = layerwidth;
    }
    return yscrollOffset;
}

#if RETRO_PLATFORM == RETRO_PS1
#include <psyqo/gpu.hh>
#include <psyqo/primitives/common.hh>
#include <psyqo/primitives/control.hh>
#include <psyqo/primitives/quads.hh>
#include <psyqo/primitives/sprites.hh>
#include "../ps1/gpu_chain.hh"
#include "../ps1/gpu_detect.hh"
#include "../ps1/loading.hh"
#include "../ps1/video.hh" // PS1Hblanks (layer profiling)

// VRAM layout (1MB, 1024x512 16-bit halfwords):
//  - framebuffer 320x240 at Y=0 (buf0) and Y=256 (buf1), X=0-319 halfwords (psyqo default)
//  - CLUT (256x1, 15-bit) at (X=0, Y=240) -- the 16-line gap below buf0
//  - sprite pages at pageX=5,7 (X base 320,448) x pageY=0,1 (Y 0,256)
//  - tile pages at pageX=9,11 (X base 576,704) x pageY=0,1 (Y 0,256) -- 4 pages = 1024 tiles
//    Page X base = pageX*64 halfwords (NOT pixels); a page is 128 halfwords = 256 texels.
//    Must start at >=320 halfwords to clear the framebuffer (X=0-319).
#define PS1_CLUT_X 0
#define PS1_CLUT_Y 240
#define PS1_TILE_PAGE_X 9
#define PS1_TILE_PAGE_Y 0
#define PS1_DRAW_BG_LAYERS 1
// PS1TileVisual word fields (see Drawing.hpp)
#define PS1_TV_PAGE_SHIFT 16
#define PS1_TV_DIR_SHIFT  18
#define PS1_TV_PLANE      0x100000u
#define PS1_TV_SOLID      0x200000u
static bool ps1TileSetUploaded = false;

// Palette and clear colour follow PC timing: PC applies the (faded) palette in FlipScreen,
// after the frame is drawn, so a SetFade called from an ObjectDraw sub (e.g. the title
// card's fade-in) shows in the same frame. Here the CLUT uploads and the clear are chained
// first (they must precede the primitives), filled with the current palette, and rewritten
// in PS1FinalizePalette() (FlipScreen) with the palette as it stands when drawing is done.
// 4-bit atlas CLUTs map 16 values to master entries and follow every fade / rotation.
// Ink effects (phase 3c) use more CLUT sources, filled at the same time:
//  - tint: a full-screen DrawTintRect recolours what was drawn before it (PC: FB index ->
//    TintLookupTable). The CLUT rows chained before that point are filled with pal[tint[i]], and
//    the call chains normal rows for what follows.
//  - INK_BLEND: semi-transparent sprites (B/2 + F/2) through blend CLUTs (rows 504-511, kept free
//    of atlas CLUTs): source colour = the blend ramp entry of the texel's luminance, STP set.
#define PS1_PAL_ENTRIES     (256 + PS1_ATLAS_MAX_CLUTS * 16)
#define PS1_BLEND_MASTER_Y  511 // 256-entry blend CLUT for 8-bit clusters at (0, 511)
#define PS1_BLEND_ROW_Y0    504 // 16-entry blend CLUTs for 4-bit CLUT k at ((k % 20) * 16, 504 + k / 20)
#define PS1_BLEND_SLOTS     (7 * 20)
#define PS1_MASK_CLUT_X     256 // tint-mask marking CLUT (16 entries) at (256, 511)
static uint16_t s_pal[PS1_PAL_ENTRIES];      // master CLUT, then the 4-bit CLUTs
static uint16_t s_palTint[PS1_PAL_ENTRIES];  // tinted (same layout)
static uint16_t s_palBlend[PS1_PAL_ENTRIES]; // blend sources (same layout)
static uint16_t s_palBlendTint[PS1_PAL_ENTRIES];
enum { PS1_PAL_NORMAL, PS1_PAL_BLEND };
struct PS1PalChunk {
    uint16_t *dst;
    uint16_t src, n;
    uint8_t kind, preTint; // preTint: chained before this frame's tint point
};
static PS1PalChunk s_palChunks[256];
static int s_palChunkCount                  = 0;
static psyqo::Prim::FastFill *s_clearPrim   = nullptr;
static bool s_palReserved                   = false;
static int s_tintID                         = -1;    // this frame's full-screen tint table (-1 = none)
static bool s_blendUsed                     = false; // blend CLUT rows chained this frame
static bool s_blendMasterReserved           = false;
static uint32_t s_blendRowReserved[(PS1_BLEND_SLOTS + 31) / 32];
static byte s_blendIdx[256];                         // SetBlendTable type 1: ramp entry per src index
static byte s_blendType                     = 0xFF;  // 0xFF = no blend table yet
volatile uint32_t g_ps1TintFrames   = 0; // frames with a full-screen tint (GDB)
volatile uint32_t g_ps1TintPartial  = 0; // frames whose tint left an untinted window (replayed)
volatile uint32_t g_ps1TintIgnored  = 0; // tint calls after the frame's tint sequence ended (tint of tint)
volatile uint32_t g_ps1TintMaskSkip = 0; // tint masks not drawn (not a 4-bit atlas cluster)
volatile uint32_t g_ps1TintWindowApprox = 0; // untinted area was not one rectangle (bounding box used)
// Tint sequence of a frame: DrawTintRect / DrawScaledTintMask calls in a row. The first call is
// the tint point; the sequence ends at the next chained primitive (chain hook) or FlipScreen.
static int s_tintSeq = 0; // 0 none, 1 open, 2 finished
static uint32_t *s_sceneMark = nullptr; // chain position after the frame-start CLUT rows
static uint32_t *s_tintMark  = nullptr; // chain position of the tint point
struct PS1TintRectRec {
    int16_t x0, y0, x1, y1;
};
static PS1TintRectRec s_tintRects[8];
static int s_tintRectCount = 0;
struct PS1TintMaskRec {
    int direction, XPos, YPos, pivotX, pivotY, scaleX, scaleY, width, height, sprX, sprY, sheetID;
};
static PS1TintMaskRec s_tintMask;
static bool s_tintMaskPending = false;
static uint8_t s_ps1QuadMaskMode = 0; // DrawScaledSprite: 1 = mark the mask bit, colours unchanged
volatile uint32_t g_ps1BlendSprites = 0; // INK_BLEND atlas sprites drawn

static uint16_t PS1Conv(const Colour &c) {
    uint16_t v = (uint16_t)((c.r >> 3) | ((c.g >> 3) << 5) | ((c.b >> 3) << 10));
    return v ? v : 0x8000; // visible black -> non-transparent black for opaque commands
}

static const byte *PS1TintTable(int id) {
    switch (id) {
        case 1: return TintLookupTable2;
        case 2: return TintLookupTable3;
        case 3: return TintLookupTable4;
        default: return TintLookupTable1;
    }
}

static void PS1Expand4Bit(uint16_t *base, int count) {
    for (int k = 0; k < count; ++k) {
        const uint8_t *map = g_ps1AtlasCluts[k].map;
        uint16_t *dst      = &base[256 + k * 16];
        dst[0]             = 0x0000; // transparent
        for (int i = 1; i < 16; ++i) dst[i] = base[map[i]];
    }
}

static void PS1BuildPalette() {
    // PaletteMode set: the faded palette (TilePaletteF, updated by SetFade).
    const Colour *pal = PaletteMode ? TilePaletteF : TilePalette;
    s_pal[0] = 0x0000; // index 0 = transparent (matches RSDKv2 sprite transparency)
    for (int i = 1; i < 256; ++i) s_pal[i] = PS1Conv(pal[i]);
    PS1Expand4Bit(s_pal, g_ps1AtlasClutCount);
    const byte *tint = s_tintID >= 0 ? PS1TintTable(s_tintID) : nullptr;
    if (tint) {
        s_palTint[0] = 0x0000;
        for (int i = 1; i < 256; ++i) s_palTint[i] = PS1Conv(pal[tint[i]]);
        PS1Expand4Bit(s_palTint, g_ps1AtlasClutCount);
    }
    if (s_blendUsed) {
        int n4 = g_ps1AtlasClutCount < PS1_BLEND_SLOTS ? g_ps1AtlasClutCount : PS1_BLEND_SLOTS;
        s_palBlend[0] = 0x0000;
        for (int i = 1; i < 256; ++i) s_palBlend[i] = (s_blendType == 1 ? PS1Conv(pal[s_blendIdx[i]]) : s_pal[i]) | 0x8000;
        PS1Expand4Bit(s_palBlend, n4);
        if (tint) {
            s_palBlendTint[0] = 0x0000;
            for (int i = 1; i < 256; ++i)
                s_palBlendTint[i] = (s_blendType == 1 ? PS1Conv(pal[tint[s_blendIdx[i]]]) : s_palTint[i]) | 0x8000;
            PS1Expand4Bit(s_palBlendTint, n4);
        }
    }
}

static psyqo::Color PS1ClearColour() {
    if (s_tintID >= 0) { // PC: the cleared index 0 goes through the tint table too
        const Colour &c = (PaletteMode ? TilePaletteF : TilePalette)[PS1TintTable(s_tintID)[0]];
        return psyqo::Color{{.r = c.r, .g = c.g, .b = c.b}};
    }
    // Under a fade the empty screen is the fade colour's share (PC: ClearScreen(0) + faded
    // palette entry 0 = black).
    if (!PaletteMode)
        return psyqo::Color{{.r = 0, .g = 0, .b = 0}};
    return psyqo::Color{{.r = (uint8_t)((ps1FadeR * ps1FadeA) >> 8), .g = (uint8_t)((ps1FadeG * ps1FadeA) >> 8),
                         .b = (uint8_t)((ps1FadeB * ps1FadeA) >> 8)}};
}

static void PS1FillPaletteChunks() {
    for (int i = 0; i < s_palChunkCount; ++i) {
        const PS1PalChunk &c = s_palChunks[i];
        bool tinted          = c.preTint && s_tintID >= 0;
        const uint16_t *src  = c.kind == PS1_PAL_BLEND ? (tinted ? s_palBlendTint : s_palBlend) : (tinted ? s_palTint : s_pal);
        memcpy(c.dst, &src[c.src], c.n * sizeof(uint16_t));
    }
}

static void PS1ReserveRow(int src, int count, int16_t x, int16_t y, uint8_t kind = PS1_PAL_NORMAL) {
    uint16_t *data[32];
    uint16_t len[32];
    int n = PS1ChainReserveRow(count, x, y, data, len, 32);
    for (int i = 0; i < n && s_palChunkCount < 256; ++i) {
        s_palChunks[s_palChunkCount++] = PS1PalChunk{data[i], (uint16_t)src, len[i], kind, (uint8_t)(s_tintID < 0)};
        src += len[i];
    }
}

// Master CLUT + the 4-bit atlas CLUT rows, chained here and filled at FlipScreen.
static void PS1ReservePaletteRows() {
    PS1ReserveRow(0, 256, PS1_CLUT_X, PS1_CLUT_Y);
    for (int k = 0; k < g_ps1AtlasClutCount;) { // runs of adjacent slots in one VRAM row
        int run = 1;
        while (k + run < g_ps1AtlasClutCount && g_ps1AtlasCluts[k + run].y == g_ps1AtlasCluts[k].y
               && g_ps1AtlasCluts[k + run].x == g_ps1AtlasCluts[k].x + 16 * run)
            ++run;
        PS1ReserveRow(256 + k * 16, run * 16, (int16_t)g_ps1AtlasCluts[k].x, (int16_t)g_ps1AtlasCluts[k].y);
        k += run;
    }
}

void PS1UploadPalette() {
    s_tintID              = -1;
    s_blendUsed           = false;
    s_blendMasterReserved = false;
    memset(s_blendRowReserved, 0, sizeof(s_blendRowReserved));
    PS1BuildPalette();
    s_palChunkCount = 0;
    PS1ReservePaletteRows();
    PS1FillPaletteChunks();
    PS1ChainFlushCache(); // the GPU caches CLUTs
    s_palReserved = true;
    s_tintSeq     = 0;
    s_sceneMark   = PS1ChainMark();
}

void PS1FinalizePalette() {
    if (!s_palReserved)
        return;
    PS1ChainRunHook(); // a tint sequence ending the frame
    s_palReserved = false;
    PS1BuildPalette();
    PS1FillPaletteChunks();
    if (s_clearPrim)
        s_clearPrim->setColor(PS1ClearColour());
    s_clearPrim = nullptr;
}

void DrawScaledSprite(int direction, int XPos, int YPos, int pivotX, int pivotY, int scaleX, int scaleY, int width, int height, int sprX, int sprY,
                      int sheetID);

// End of a tint sequence (PC: the rects and the tint mask recolour the framebuffer's indices).
// The whole frame so far was drawn tinted (frame-start CLUT rows); the untinted window is
// drawn again on top: mark the tint mask's texels with the mask bit (additive black: colours
// unchanged), clip to the window, and replay the scene chained before the tint point with the
// normal CLUT rows and the mask test on (psx-spx GP0 E6h).
static void PS1FinishTint() {
    s_tintSeq = 2;
    // Untinted area = screen minus the rects: cells between rect edges not covered by any rect.
    int xs[18], ys[18], nx = 0, ny = 0;
    xs[nx++] = 0, xs[nx++] = SCREEN_XSIZE, ys[ny++] = 0, ys[ny++] = SCREEN_YSIZE;
    for (int i = 0; i < s_tintRectCount; ++i) {
        xs[nx++] = s_tintRects[i].x0, xs[nx++] = s_tintRects[i].x1;
        ys[ny++] = s_tintRects[i].y0, ys[ny++] = s_tintRects[i].y1;
    }
    int bx0 = SCREEN_XSIZE, by0 = SCREEN_YSIZE, bx1 = 0, by1 = 0, uncovered = 0;
    for (int a = 0; a < nx; ++a)
        for (int b = 0; b < ny; ++b) {
            // cell [xs[a], next x) x [ys[b], next y)
            int cx1 = SCREEN_XSIZE, cy1 = SCREEN_YSIZE;
            for (int k = 0; k < nx; ++k)
                if (xs[k] > xs[a] && xs[k] < cx1)
                    cx1 = xs[k];
            for (int k = 0; k < ny; ++k)
                if (ys[k] > ys[b] && ys[k] < cy1)
                    cy1 = ys[k];
            if (xs[a] >= SCREEN_XSIZE || ys[b] >= SCREEN_YSIZE)
                continue;
            bool covered = false;
            for (int i = 0; i < s_tintRectCount && !covered; ++i)
                covered = s_tintRects[i].x0 <= xs[a] && s_tintRects[i].x1 >= cx1 && s_tintRects[i].y0 <= ys[b] && s_tintRects[i].y1 >= cy1;
            if (covered)
                continue;
            uncovered += (cx1 - xs[a]) * (cy1 - ys[b]);
            if (xs[a] < bx0) bx0 = xs[a];
            if (ys[b] < by0) by0 = ys[b];
            if (cx1 > bx1) bx1 = cx1;
            if (cy1 > by1) by1 = cy1;
        }
    if (!uncovered) // the rects cover the screen: a plain full tint (the tint mask adds nothing)
        return;
    // cells are counted once per distinct (x, y) edge pair; duplicates only inflate `uncovered`
    if (uncovered < (bx1 - bx0) * (by1 - by0))
        g_ps1TintWindowApprox = g_ps1TintWindowApprox + 1;
    g_ps1TintPartial = g_ps1TintPartial + 1;
    bool skipPrims       = g_ps1ChainSkipPrims; // layers-only debug captures: the marks belong to the layers
    g_ps1ChainSkipPrims  = false;
    bool masked = false;
    if (s_tintMaskPending) {
        const PS1TintMaskRec &m = s_tintMask;
        uint32_t before         = g_ps1TintMaskSkip;
        PS1ChainMaskControl(true, false);
        s_ps1QuadMaskMode = 1;
        DrawScaledSprite(m.direction, m.XPos, m.YPos, m.pivotX, m.pivotY, m.scaleX, m.scaleY, m.width, m.height, m.sprX, m.sprY, m.sheetID);
        s_ps1QuadMaskMode = 0;
        masked = g_ps1TintMaskSkip == before;
    }
    PS1ChainMaskControl(false, masked);
    PS1ChainDrawArea(bx0, by0, bx1, by1);
    PS1ChainReplay(s_sceneMark, s_tintMark);
    PS1ChainDrawAreaReset();
    if (masked)
        PS1ChainMaskControl(false, false);
    PS1ChainInvalidateTPage();
    g_ps1ChainSkipPrims = skipPrims;
}

// The tint point: the first DrawTintRect / DrawScaledTintMask of a frame.
static void PS1TintBegin(int tintID) {
    s_tintID = tintID;
    g_ps1TintFrames = g_ps1TintFrames + 1;
    s_tintMark = PS1ChainMark();
    PS1ReservePaletteRows();
    PS1ChainFlushCache();
    s_blendMasterReserved = false; // blend rows chained before the tint are tinted: re-chain
    memset(s_blendRowReserved, 0, sizeof(s_blendRowReserved));
    s_tintSeq         = 1;
    s_tintRectCount   = 0;
    s_tintMaskPending = false;
    PS1ChainSetHook(PS1FinishTint);
}

// DrawTintRect: the first call of a frame is the tint point. Everything chained before it is shown
// through the tint table (the frame-start CLUT rows get pal[tint[i]] at FlipScreen); normal CLUT
// rows are chained here for what follows. Partial rects leave an untinted window (PS1FinishTint).
static void PS1TintRect(int x, int y, int w, int h, int tintID) {
    if (!s_palReserved)
        return;
    if (s_tintSeq == 2) { // a second tint sequence in one frame (tint of tint): not modelled
        g_ps1TintIgnored = g_ps1TintIgnored + 1;
        return;
    }
    if (s_tintSeq == 0)
        PS1TintBegin(tintID);
    if (s_tintRectCount < 8)
        s_tintRects[s_tintRectCount++] = PS1TintRectRec{(int16_t)x, (int16_t)y, (int16_t)(x + w), (int16_t)(y + h)};
}

// Blend CLUT for an atlas cluster, chained (once per frame and tint side) before its first use.
static bool PS1BlendClut(const PS1AtlasCluster *c, psyqo::PrimPieces::ClutIndex &out) {
    if (c->clut == 0xFFFF) {
        if (!s_blendMasterReserved) {
            PS1ReserveRow(0, 256, 0, PS1_BLEND_MASTER_Y, PS1_PAL_BLEND);
            PS1ChainFlushCache();
            s_blendMasterReserved = true;
        }
        out = psyqo::PrimPieces::ClutIndex(psyqo::Vertex{{.x = 0, .y = PS1_BLEND_MASTER_Y}});
    } else {
        int k = c->clut;
        if (k >= PS1_BLEND_SLOTS)
            return false;
        int16_t x = (int16_t)((k % 20) * 16), y = (int16_t)(PS1_BLEND_ROW_Y0 + k / 20);
        if (!(s_blendRowReserved[k >> 5] & (1u << (k & 31)))) {
            PS1ReserveRow(256 + k * 16, 16, x, y, PS1_PAL_BLEND);
            PS1ChainFlushCache();
            s_blendRowReserved[k >> 5] |= 1u << (k & 31);
        }
        out = psyqo::PrimPieces::ClutIndex(psyqo::Vertex{{.x = x, .y = y}});
    }
    s_blendUsed = true;
    return true;
}

static void PS1ClearClusterCache();

void PS1ResetSpriteSurfaces() {
    for (int i = 0; i < SURFACE_MAX; ++i) {
        GfxSurface[i].vramPageX    = -1;
        GfxSurface[i].vramDirect15 = false;
        GfxSurface[i].atlasSheet   = -1;
    }
    PS1ClearClusterCache(); // cluster indices change with the atlas
    PS1ChainInvalidateTPage();
}

void PS1BindSheet(int sheetID) {
    GFXSurface *s = &GfxSurface[sheetID];
    s->atlasSheet = -1;
    if (s->vramDirect15 || !s->fileName[0] || !g_ps1AtlasSheetCount)
        return; // no atlas yet (player sheets at boot): bound at the next atlas load
    int a = PS1AtlasFindSheet(s->fileName);
    if (a < 0) {
        ramsyscall_printf("[sprite] '%s' is not in this stage's atlas: not drawn\n", s->fileName);
        s->width = s->height = 0;
        return;
    }
    s->atlasSheet = (short)a;
    s->width      = g_ps1AtlasSheets[a].width;
    s->height     = g_ps1AtlasSheets[a].height;
}

void PS1BindAllSheets() {
    for (int i = 0; i < SURFACE_MAX; ++i) PS1BindSheet(i);
}

// Atlas lookup: the cluster of atlas sheet `sheet` that holds texel (x, y). The same frames
// are drawn every frame, so a small direct-mapped cache answers almost every call.
struct PS1ClusterCacheEntry {
    int16_t sheet, x, y, index;
};
static PS1ClusterCacheEntry s_clusterCache[64];
volatile uint32_t g_ps1AtlasDrawMiss = 0; // draws whose texels are in no cluster (not in the atlas)

static void PS1ClearClusterCache() { memset(s_clusterCache, 0xFF, sizeof(s_clusterCache)); }

static const PS1AtlasCluster *PS1FindCluster(int sheet, int x, int y) {
    PS1ClusterCacheEntry &e = s_clusterCache[((unsigned)x * 7 + (unsigned)y * 13 + (unsigned)sheet * 31) & 63];
    if (e.sheet == sheet && e.x == x && e.y == y)
        return &g_ps1AtlasClusters[e.index];
    const PS1AtlasSheet &sh = g_ps1AtlasSheets[sheet];
    for (int i = sh.firstCluster; i < sh.firstCluster + sh.clusterCount; ++i) {
        const PS1AtlasCluster &c = g_ps1AtlasClusters[i];
        if (x >= c.srcX && x < c.srcX + c.w && y >= c.srcY && y < c.srcY + c.h) {
            e = PS1ClusterCacheEntry{(int16_t)sheet, (int16_t)x, (int16_t)y, (int16_t)i};
            return &c;
        }
    }
    g_ps1AtlasDrawMiss = g_ps1AtlasDrawMiss + 1;
    return nullptr;
}

static psyqo::PrimPieces::TPageAttr PS1ClusterTPage(const PS1AtlasCluster *c) {
    psyqo::PrimPieces::TPageAttr attr;
    attr.set(c->depth == 4 ? psyqo::Prim::TPageAttr::ColorMode::Tex4Bits : psyqo::Prim::TPageAttr::ColorMode::Tex8Bits);
    attr.setPageX(c->tpageX);
    attr.setPageY(c->tpageY);
    attr.set(psyqo::Prim::TPageAttr::SemiTrans::HalfBackAndHalfFront);
    return attr;
}

static psyqo::PrimPieces::ClutIndex PS1ClusterClut(const PS1AtlasCluster *c) {
    if (c->clut == 0xFFFF)
        return psyqo::PrimPieces::ClutIndex(psyqo::Vertex{{.x = PS1_CLUT_X, .y = PS1_CLUT_Y}});
    const PS1AtlasClut &k = g_ps1AtlasCluts[c->clut];
    return psyqo::PrimPieces::ClutIndex(psyqo::Vertex{{.x = (int16_t)k.x, .y = (int16_t)k.y}});
}

// Draws sheet texels (sprX, sprY, w, h) at (XPos, YPos). A frame can span several clusters
// (frames wider/taller than a texture page are cut by the atlas builder): draw the part in
// the cluster under the top-left texel, then the strip to its right and the band below.
static void PS1DrawAtlasRect(int sheet, int XPos, int YPos, int width, int height, int sprX, int sprY, bool blend = false) {
    struct Rect16 {
        int16_t x, y, w, h;
    } stack[16];
    int sp      = 0;
    stack[sp++] = Rect16{(int16_t)sprX, (int16_t)sprY, (int16_t)width, (int16_t)height};
    while (sp) {
        Rect16 r                 = stack[--sp];
        const PS1AtlasCluster *c = PS1FindCluster(sheet, r.x, r.y);
        if (!c)
            continue;
        int iw = c->srcX + c->w - r.x, ih = c->srcY + c->h - r.y;
        if (iw > r.w) iw = r.w;
        if (ih > r.h) ih = r.h;
        PS1ChainTPage(PS1ClusterTPage(c));
        psyqo::Prim::Sprite s(psyqo::Color{{.r = 0x80, .g = 0x80, .b = 0x80}});
        s.position = psyqo::Vertex{{.x = (int16_t)(XPos + r.x - sprX), .y = (int16_t)(YPos + r.y - sprY)}};
        s.size     = psyqo::Vertex{{.w = (int16_t)iw, .h = (int16_t)ih}};
        psyqo::PrimPieces::ClutIndex clut = PS1ClusterClut(c);
        if (blend && PS1BlendClut(c, clut)) { // INK_BLEND: B/2 + F/2 (the cluster texpage's mode 0)
            s.setSemiTrans();
            g_ps1BlendSprites = g_ps1BlendSprites + 1;
        }
        s.texInfo = psyqo::PrimPieces::TexInfo{(uint8_t)(c->u + r.x - c->srcX), (uint8_t)(c->v + r.y - c->srcY), clut};
        PS1ChainSprite(s);
        if (r.w > iw && sp < 16)
            stack[sp++] = Rect16{(int16_t)(r.x + iw), r.y, (int16_t)(r.w - iw), (int16_t)ih};
        if (r.h > ih && sp < 16)
            stack[sp++] = Rect16{r.x, (int16_t)(r.y + ih), r.w, (int16_t)(r.h - ih)};
    }
}

// 15-bit (FMV) surfaces don't go through the CLUT, so a palette fade is re-created on the
// GPU: modulate the texels by (255-a) and add fadeColour*a with an additive rectangle,
// i.e. PC's (c*a + (255-a)*p) >> 8. Call after drawing the surface at (x, y, w, h).
static uint8_t PS1DirectModulation() { return PaletteMode ? (uint8_t)((0x80 * (255 - ps1FadeA)) >> 8) : 0x80; }

static void PS1DirectFadeOverlay(int x, int y, int w, int h) {
    if (!PaletteMode || !ps1FadeA || !(ps1FadeR | ps1FadeG | ps1FadeB))
        return;
    psyqo::PrimPieces::TPageAttr attr; // untextured semi-trans takes its blend mode from the texpage
    attr.set(psyqo::Prim::TPageAttr::SemiTrans::FullBackAndFullFront);
    PS1ChainTPage(attr);
    psyqo::Prim::Rectangle r(psyqo::Color{{.r = (uint8_t)((ps1FadeR * ps1FadeA) >> 8),
                                           .g = (uint8_t)((ps1FadeG * ps1FadeA) >> 8),
                                           .b = (uint8_t)((ps1FadeB * ps1FadeA) >> 8)}});
    r.setSemiTrans();
    r.position = psyqo::Vertex{{.x = (int16_t)x, .y = (int16_t)y}};
    r.size     = psyqo::Vertex{{.w = (int16_t)w, .h = (int16_t)h}};
    PS1ChainRect(r);
}

bool PS1AllocDirectSurface(int sheetID) {
    // The stage atlas reserves one 15-bit texture page (256 halfwords wide) for the FMV.
    if (g_ps1AtlasFmvX == 0xFFFF)
        return false;
    GFXSurface *s   = &GfxSurface[sheetID];
    s->vramPageX    = g_ps1AtlasFmvX / 64;
    s->vramPageY    = g_ps1AtlasFmvY / 256;
    s->vramDirect15 = true;
    s->atlasSheet   = -1;
    return true;
}

// Upload the stage tile set: 16x16Tiles.vram holds 4 pages already packed in texture-page
// layout at build time (convert_tiles.py), so each page is one 64 KB read + one DMA.
void PS1InvalidateTileSet() { ps1TileSetUploaded = false; } // stage change: next frame uploads the new set

// BG line strips (tools/bgstrips/build_bgstrips.py -> Data/Stages/<Stage>/BGStrips.bin): BG layer
// lines pre-rendered at build time as 8-bit rows of one horizontal period, in VRAM the sprite
// atlas leaves free. A band of lines in a strip region costs 1-3 sprites instead of ~21 tiles.
#define PS1_STRIP_MAX 64
struct PS1StripRegion {
    uint8_t layer, plane, pieces, pad;
    uint16_t line0, lines, period;
    uint16_t x[3], y[3], w[3]; // piece VRAM origin (halfwords, column start) and width (texels)
};
static PS1StripRegion s_ps1Strips[PS1_STRIP_MAX];
static int s_ps1StripCount = 0;
static uint8_t s_ps1StripRow[LAYER_COUNT][2][TILELAYER_SCROLL_MAX / TILE_SIZE]; // region + 1 per tile row
volatile uint32_t g_ps1StripRegions = 0; // regions loaded for this stage (GDB)

// Per chunk: bit (tileRow * 2 + plane) set when that 8-tile row has a visible tile on that
// plane, so the tile renderer skips empty chunk rows (the high plane is almost empty).
static uint16_t s_ps1ChunkRowPlanes[CHUNKTILE_COUNT / 64];
static void PS1BuildChunkRowPlanes() {
    const uint32_t *vis = (const uint32_t *)StageTiles.gfxDataPos;
    for (int c = 0; c < CHUNKTILE_COUNT / 64; ++c) {
        uint16_t m = 0;
        for (int t = 0; t < 64; ++t) {
            uint32_t tv = vis[(c << 6) + t];
            if (tv & PS1_TV_SOLID)
                m |= 1 << ((t >> 3) * 2 + ((tv & PS1_TV_PLANE) ? 1 : 0));
        }
        s_ps1ChunkRowPlanes[c] = m;
    }
}

static void PS1LoadBGStrips() {
    s_ps1StripCount = 0;
    g_ps1StripRegions = 0;
    memset(s_ps1StripRow, 0, sizeof(s_ps1StripRow));
    FileInfo info;
    if (!LoadStageFile("BGStrips.bin", StageListPosition, &info))
        return;
    byte head[8];
    FileRead(head, 8);
    int count = head[4] | (head[5] << 8);
    if (memcmp(head, "BGS1", 4) || count > PS1_STRIP_MAX) {
        CloseFile();
        return;
    }
    for (int i = 0; i < count; ++i) {
        byte e[28];
        FileRead(e, 28);
        PS1StripRegion &r = s_ps1Strips[i];
        r.layer  = e[0];
        r.plane  = e[1] & 1;
        r.line0  = e[2] | (e[3] << 8);
        r.lines  = e[4] | (e[5] << 8);
        r.period = e[6] | (e[7] << 8);
        r.pieces = e[8];
        for (int k = 0; k < 3; ++k) {
            r.x[k] = e[10 + 6 * k] | (e[11 + 6 * k] << 8);
            r.y[k] = e[12 + 6 * k] | (e[13 + 6 * k] << 8);
            r.w[k] = e[14 + 6 * k] | (e[15 + 6 * k] << 8);
        }
    }
    PS1GpuImmediate();
    for (int i = 0; i < count; ++i) {
        PS1StripRegion &r = s_ps1Strips[i];
        for (int k = 0; k < r.pieces; ++k) {
            int rowsPerChunk = (int)sizeof(TileGfx) / r.w[k];
            for (int row = 0; row < r.lines; row += rowsPerChunk) {
                int n = r.lines - row < rowsPerChunk ? r.lines - row : rowsPerChunk;
                FileRead(TileGfx, n * r.w[k]);
                g_ps1GPU->uploadToVRAM((const uint16_t *)TileGfx,
                                       psyqo::Rect{{(int16_t)r.x[k], (int16_t)(r.y[k] + row)}, {(int16_t)(r.w[k] / 2), (int16_t)n}});
            }
        }
        if (r.layer < LAYER_COUNT)
            for (int row = r.line0 >> 4; row < (r.line0 + r.lines) >> 4; ++row) s_ps1StripRow[r.layer][r.plane][row] = (uint8_t)(i + 1);
    }
    CloseFile();
    s_ps1StripCount   = count;
    g_ps1StripRegions = count;
}

void PS1UploadTileSet() {
    if (ps1TileSetUploaded)
        return;
    FileInfo info;
    if (!LoadStageFile("16x16Tiles.vram", StageListPosition, &info))
        return;
    SetFilePosition(4 + 768); // skip width/height + palette
    PS1GpuImmediate();        // immediate VRAM uploads: wait for the in-flight DMA chain
    PS1LoadingBegin();        // tile set + BG strips: part of the stage load (first frame)
    const int pageCount = TILE_COUNT / 256; // 4 (1024 tiles)
    for (int page = 0; page < pageCount; ++page) {
        FileRead(TileGfx, 0x10000);
        int px = PS1_TILE_PAGE_X + 2 * (page & 1); // 9, 11, 9, 11
        int py = PS1_TILE_PAGE_Y + (page >> 1);    // 0, 0, 1, 1
        g_ps1GPU->uploadToVRAM((const uint16_t *)TileGfx, psyqo::Rect{{(int16_t)(px * 64), (int16_t)(py * 256)}, {128, 256}});
    }
    CloseFile();
    PS1LoadBGStrips();
    PS1BuildChunkRowPlanes();
    PS1LoadingEnd();
    ps1TileSetUploaded = true;
}

// Tile texture origin in the pre-packed tile pages (16x16Tiles.vram, 256 tiles per page).
static void PS1TileTexOf(int tileIndex, int &pageX, int &pageY, int &u, int &v) {
    int page = tileIndex >> 8; // 0..3 (1024 tiles / 256 per page)
    int pt   = tileIndex & 0xFF;
    u        = (pt & 0xF) << 4;
    v        = (pt >> 4) << 4;
    pageX    = PS1_TILE_PAGE_X + 2 * (page & 1); // 9, 11, 9, 11
    pageY    = PS1_TILE_PAGE_Y + (page >> 1);    // 0, 0, 1, 1
}

// Textured quad (x, y, w, h) sampling u uL..uR (exclusive, may run backwards) and v vT..vB.
static void PS1ChainTileQuad(int x, int y, int w, int h, int pageX, int pageY, int uL, int uR, int vT, int vB) {
    psyqo::Prim::TexturedQuad quad(psyqo::Color{{.r = 0x80, .g = 0x80, .b = 0x80}});
    quad.pointA    = psyqo::Vertex{{.x = (int16_t)x, .y = (int16_t)y}};
    quad.uvA       = psyqo::PrimPieces::UVCoords{(uint8_t)uL, (uint8_t)vT};
    quad.clutIndex = psyqo::PrimPieces::ClutIndex(psyqo::Vertex{{.x = PS1_CLUT_X, .y = PS1_CLUT_Y}});
    quad.pointB    = psyqo::Vertex{{.x = (int16_t)(x + w), .y = (int16_t)y}};
    quad.uvB       = psyqo::PrimPieces::UVCoords{(uint8_t)uR, (uint8_t)vT};
    quad.tpage.setPageX((uint8_t)pageX);
    quad.tpage.setPageY((uint8_t)pageY);
    quad.tpage.set(psyqo::Prim::TPageAttr::ColorMode::Tex8Bits);
    quad.pointC = psyqo::Vertex{{.x = (int16_t)x, .y = (int16_t)(y + h)}};
    quad.uvC    = psyqo::PrimPieces::UVCoordsPadded{(uint8_t)uL, (uint8_t)vB, 0};
    quad.pointD = psyqo::Vertex{{.x = (int16_t)(x + w), .y = (int16_t)(y + h)}};
    quad.uvD    = psyqo::PrimPieces::UVCoordsPadded{(uint8_t)uR, (uint8_t)vB, 0};
    PS1ChainQuad(quad);
}

// Flipped tiles as flip rectangles (docs/43 G3, NX-041; PS1FlipRectsLive): PS1DrawTilePiece records a band's flipped
// tiles (screen x, image word | key << 16, key = page * 3 + dir - 1; the band's hot loop is untouched) and
// PS1DrawTileBand chains them at its end, grouped by page and direction: one texpage with GP0(E1h) bit 12 (X) / 13 (Y)
// each, then raw 16x16 sprites (or a partial band's sized sprites) from the tile's last texel column u + 15 / row
// v + 15 - ty0. Tiles of a band never overlap, so the order is free (RSDKv4-ps1 PS1ChainFlipTiles).
static uint32_t s_flipX[24], s_flipTv[24];
static int s_flipN = 0, s_flipKeys = 0;
static __attribute__((noinline)) void PS1FlipTileRecord(int dx, uint32_t tv, int key) {
    if (s_flipN >= 24)
        return;
    s_flipX[s_flipN]    = (uint16_t)dx;
    s_flipTv[s_flipN++] = (tv & 0xFFFF) | (uint32_t)key << 16;
    s_flipKeys |= 1 << key;
}
static __attribute__((noinline)) void PS1ChainFlipTiles(const psyqo::PrimPieces::TPageAttr *tpages, uint32_t clutWord, int sy,
                                                        int ty0, int h) {
    const uint32_t *x = s_flipX, *tv = s_flipTv;
    const int n = s_flipN, keys = s_flipKeys;
    s_flipN = s_flipKeys = 0;
    const uint32_t yWord = (uint32_t)(uint16_t)sy << 16;
    for (int k = 0; k < 12; ++k) {
        if (!(keys & (1 << k)))
            continue;
        int dir = k % 3 + 1;
        uint16_t raw;
        memcpy(&raw, &tpages[k / 3], 2);
        raw = (uint16_t)(raw | (dir << 12)); // RSDK FLIP_X 1 / FLIP_Y 2 = GP0(E1h) bit 12 / 13
        psyqo::PrimPieces::TPageAttr attr;
        memcpy(&attr, &raw, 2);
        PS1ChainTPage(attr);
        const uint32_t du = (dir & FLIP_X) ? 15 : 0, dv = (uint32_t)((dir & FLIP_Y) ? 15 - ty0 : ty0) << 8;
        for (int i = 0; i < n; ++i) {
            if ((int)(tv[i] >> 16) != k)
                continue;
            uint32_t tex = ((tv[i] & 0xFFFF) + du + dv) | clutWord;
            if (h == TILE_SIZE)
                PS1ChainSprite16Raw(yWord | x[i], tex);
            else {
                psyqo::Prim::Sprite s(psyqo::Color{{.r = 0x80, .g = 0x80, .b = 0x80}});
                s.position = psyqo::Vertex{{.x = (int16_t)x[i], .y = (int16_t)sy}};
                s.texInfo  = psyqo::PrimPieces::TexInfo{(uint8_t)tex, (uint8_t)(tex >> 8), psyqo::PrimPieces::ClutIndex(
                                                            psyqo::Vertex{{.x = PS1_CLUT_X, .y = PS1_CLUT_Y}})};
                s.size     = psyqo::Vertex{{.x = TILE_SIZE, .y = (int16_t)h}};
                PS1ChainSprite(s);
            }
        }
    }
    g_ps1TileFlipRects = g_ps1TileFlipRects + n;
    g_ps1FlipFrameUsed = true;
}

// Rows [ty0, ty0 + h) of a 16x16 tile (tile rows before flipping) at screen (sx, sy).
// Unflipped: Sprite16x16 / Sprite. Flipped (FLIP_X/Y/XY): a textured quad whose UVs run
// backwards -- column k samples u+15-k from uL = u+15 to uR = u-1 (exclusive), 1:1 like PC.
// On a GPU that draws GP0(E1h) rectangle flips, flipped tiles are recorded for PS1DrawTileBand's PS1ChainFlipTiles.
volatile uint32_t g_ps1LayerPrims = 0; // tile-layer primitives chained this frame (GDB)
static void PS1DrawTilePiece(int sx, int sy, int tileIndex, int ty0, int h, int dir) {
    int pageX, pageY, u, v;
    PS1TileTexOf(tileIndex, pageX, pageY, u, v);
    if (dir != FLIP_NONE && PS1FlipRectsLive()) {
        PS1FlipTileRecord(sx, (uint32_t)(u | v << 8), (tileIndex >> 8) * 3 + dir - 1);
        return;
    }
    if (dir == FLIP_NONE) {
        psyqo::PrimPieces::TPageAttr attr;
        attr.set(psyqo::Prim::TPageAttr::ColorMode::Tex8Bits);
        attr.setPageX((uint8_t)pageX);
        attr.setPageY((uint8_t)pageY);
        PS1ChainTPage(attr);
        psyqo::PrimPieces::ClutIndex clut(psyqo::Vertex{{.x = PS1_CLUT_X, .y = PS1_CLUT_Y}});
        if (h == TILE_SIZE) {
            psyqo::Prim::Sprite16x16 s(psyqo::Color{{.r = 0x80, .g = 0x80, .b = 0x80}});
            s.position = psyqo::Vertex{{.x = (int16_t)sx, .y = (int16_t)sy}};
            s.texInfo  = psyqo::PrimPieces::TexInfo{(uint8_t)u, (uint8_t)v, clut};
            PS1ChainSprite16(s);
        } else {
            psyqo::Prim::Sprite s(psyqo::Color{{.r = 0x80, .g = 0x80, .b = 0x80}});
            s.position = psyqo::Vertex{{.x = (int16_t)sx, .y = (int16_t)sy}};
            s.texInfo  = psyqo::PrimPieces::TexInfo{(uint8_t)u, (uint8_t)(v + ty0), clut};
            s.size     = psyqo::Vertex{{.x = TILE_SIZE, .y = (int16_t)h}};
            PS1ChainSprite(s);
        }
        return;
    }
    int uL = (dir & FLIP_X) ? u + 15 : u;
    int uR = (dir & FLIP_X) ? u - 1 : u + 16;
    int dv = (dir & FLIP_Y) ? -1 : 1;
    int vT = (dir & FLIP_Y) ? v + 15 - ty0 : v + ty0;
    // u = -1 / 256 doesn't fit a UV byte: address the tile from the neighbouring 8-bit page
    // (pageX +-1 = 128 texels); the texels actually sampled stay inside the tile.
    if (uR < 0) {
        pageX -= 1, uL += 128, uR += 128;
    } else if (uR > 255) {
        pageX += 1, uL -= 128, uR -= 128;
    }
    int vB = vT + h * dv;
    if (vB < 0 || vB > 255) { // pageY can't shift: the last row gets its own constant-v strip
        int vLast = vT + (h - 1) * dv;
        if (h > 1)
            PS1ChainTileQuad(sx, sy, TILE_SIZE, h - 1, pageX, pageY, uL, uR, vT, vLast);
        PS1ChainTileQuad(sx, sy + h - 1, TILE_SIZE, 1, pageX, pageY, uL, uR, vLast, vLast);
        return;
    }
    PS1ChainTileQuad(sx, sy, TILE_SIZE, h, pageX, pageY, uL, uR, vT, vB);
}

uint32_t PS1TileVisual(int tileIndex, int direction, int visualPlane) {
    if (tileIndex <= 0) // tile 0 = empty (its texels are all 0)
        return 0;
    int pageX, pageY, u, v;
    PS1TileTexOf(tileIndex, pageX, pageY, u, v);
    return (uint32_t)(u | (v << 8)) | ((uint32_t)(tileIndex >> 8) << PS1_TV_PAGE_SHIFT) | ((uint32_t)(direction & 3) << PS1_TV_DIR_SHIFT)
           | (visualPlane ? PS1_TV_PLANE : 0) | PS1_TV_SOLID;
}

// One band: screen lines [sy, sy + h) showing layer lines [ly, ly + h) (inside one tile row)
// scrolled to chunkX. Lines off the top/bottom of the screen are free to draw (the GPU clips
// them), so a band touching a screen edge is extended to whole tile rows (Sprite16x16).
// Hot path (whole unflipped tiles): one visual-word load + compare, 3 raw words chained.
static void PS1DrawTileBand(const TileLayer *layer, byte above, int sy, int h, int ly, int chunkX, int fullW) {
    int ty0 = ly & 0xF;
    if (sy == 0) {
        sy -= ty0, h += ty0, ty0 = 0;
    }
    if (sy + h == SCREEN_YSIZE)
        h = TILE_SIZE - ty0;
    static uint32_t s_tileClutWord = 0;
    static psyqo::PrimPieces::TPageAttr s_tileTPage[4];
    if (!s_tileClutWord) {
        psyqo::PrimPieces::ClutIndex clut(psyqo::Vertex{{.x = PS1_CLUT_X, .y = PS1_CLUT_Y}});
        uint16_t c;
        memcpy(&c, &clut, 2);
        s_tileClutWord = (uint32_t)c << 16;
        for (int p = 0; p < 4; ++p) {
            s_tileTPage[p].set(psyqo::Prim::TPageAttr::ColorMode::Tex8Bits);
            s_tileTPage[p].setPageX((uint8_t)(PS1_TILE_PAGE_X + 2 * (p & 1)));
            s_tileTPage[p].setPageY((uint8_t)(PS1_TILE_PAGE_Y + (p >> 1)));
        }
    }
    const ushort *chunkRow = &layer->tiles[(ly >> 7) * TILELAYER_CHUNK_W];
    const uint32_t *vis    = (const uint32_t *)StageTiles.gfxDataPos;
    int tileRow            = ((ly & 0x7F) >> 4) << 3;
    uint32_t want          = PS1_TV_SOLID | (above ? PS1_TV_PLANE : 0);
    uint32_t yWord         = (uint32_t)(uint16_t)sy << 16;
    bool whole             = h == TILE_SIZE;
    int curPage            = -1;
    int prims              = 0;
    int px                 = chunkX & ~0xF;
    uint16_t rowBit        = 1 << ((tileRow >> 3) * 2 + (above ? 1 : 0));
    for (int sx = -(chunkX & 0xF); sx < SCREEN_XSIZE;) {
        int chunk = chunkRow[px >> 7];
        if (!(s_ps1ChunkRowPlanes[chunk] & rowBit)) { // nothing on this plane in this chunk row
            int skip = CHUNK_SIZE - (px & 0x7F);
            sx += skip;
            px += skip;
            if (px >= fullW)
                px -= fullW;
            continue;
        }
        uint32_t tv = vis[(chunk << 6) + ((px & 0x7F) >> 4) + tileRow];
        sx += TILE_SIZE;
        px += TILE_SIZE;
        if (px >= fullW)
            px = 0;
        if ((tv & (PS1_TV_SOLID | PS1_TV_PLANE)) != want)
            continue;
        ++prims;
        int dir  = (tv >> PS1_TV_DIR_SHIFT) & 3;
        int page = (tv >> PS1_TV_PAGE_SHIFT) & 3;
        int dx = sx - TILE_SIZE;
        if (whole && dir == FLIP_NONE) {
            if (page != curPage) {
                PS1ChainTPage(s_tileTPage[page]);
                curPage = page;
            }
            PS1ChainSprite16Raw(yWord | (uint16_t)dx, (tv & 0xFFFF) | s_tileClutWord);
        } else {
            int tileIndex = (page << 8) | ((tv >> 8) & 0xF0) | ((tv & 0xFF) >> 4);
            PS1DrawTilePiece(dx, sy, tileIndex, ty0, h, dir);
            curPage = -1; // quads carry their own texpage; sprites set it themselves
        }
    }
    if (s_flipN) // flipped tiles PS1DrawTilePiece recorded (flip rectangles, NX-041)
        PS1ChainFlipTiles(s_tileTPage, s_tileClutWord, sy, ty0, h);
    g_ps1LayerPrims = g_ps1LayerPrims + prims;
}

// A band of lines [ly, ly + h) inside a strip region, scrolled to chunkX: walk the period from
// chunkX % period across the screen, one sprite per piece run (a piece is <= 256 texels, one
// page). Only the runs of piece `piece` are drawn: strip bands never overlap, so the caller
// draws all bands piece by piece and the texpage changes once per piece, not per sprite.
static int PS1DrawStripBand(const PS1StripRegion &r, int sy, int h, int ly, int chunkX, int piece) {
    static uint32_t s_clut = 0;
    if (!s_clut) {
        psyqo::PrimPieces::ClutIndex clut(psyqo::Vertex{{.x = PS1_CLUT_X, .y = PS1_CLUT_Y}});
        uint16_t c;
        memcpy(&c, &clut, 2);
        s_clut = c;
    }
    int row   = ly - r.line0;
    int t     = chunkX % r.period;
    int prims = 0;
    for (int x = 0; x < SCREEN_XSIZE;) {
        int k = 0, s0 = 0;
        while (t >= s0 + r.w[k]) s0 += r.w[k++];
        int u   = t - s0;
        int run = r.w[k] - u;
        if (run > SCREEN_XSIZE - x)
            run = SCREEN_XSIZE - x;
        if (k != piece) {
            x += run;
            t += run;
            if (t >= r.period)
                t -= r.period;
            continue;
        }
        psyqo::PrimPieces::TPageAttr attr;
        attr.set(psyqo::Prim::TPageAttr::ColorMode::Tex8Bits);
        attr.setPageX((uint8_t)(r.x[k] >> 6));
        attr.setPageY((uint8_t)(r.y[k] >> 8));
        PS1ChainTPage(attr);
        psyqo::Prim::Sprite s(psyqo::Color{{.r = 0x80, .g = 0x80, .b = 0x80}});
        s.position = psyqo::Vertex{{.x = (int16_t)x, .y = (int16_t)sy}};
        psyqo::PrimPieces::ClutIndex clut;
        memcpy(&clut, &s_clut, 2);
        s.texInfo = psyqo::PrimPieces::TexInfo{(uint8_t)u, (uint8_t)((r.y[k] & 0xFF) + row), clut};
        s.size    = psyqo::Vertex{{.x = (int16_t)run, .y = (int16_t)h}};
        PS1ChainSprite(s);
        ++prims;
        x += run;
        t += run;
        if (t >= r.period)
            t -= r.period;
    }
    return prims;
}

// GPU version of DrawHLineScrollLayer: the same per-line scroll (HParallax line positions,
// deformation, BG Y parallax + autoscroll, wrap, visual plane, flips), drawn as bands of lines
// that share one X scroll (a band never crosses a 16-line tile row).
static short s_ps1LineX[SCREEN_YSIZE];
struct PS1StripBand {
    short sy, h, ly, chunkX;
    uint8_t region;
};
static PS1StripBand s_ps1StripBands[SCREEN_YSIZE];
volatile uint32_t g_ps1LayerHbl = 0; // CPU time of this frame's tile layers (hblanks, GDB)
static void PS1DrawTileLayerImpl(int layerID);
static void PS1DrawTileLayer(int layerID) {
    uint32_t t0 = PS1Hblanks();
    PS1DrawTileLayerImpl(layerID);
    g_ps1LayerHbl = g_ps1LayerHbl + PS1HblanksSince(t0);
}
static void PS1DrawTileLayerImpl(int layerID) {
    TileLayer *layer = &StageLayouts[activeTileLayers[layerID]];
    if (!layer->xsize || !layer->ysize)
        return;
    if (layer->type != LAYER_HSCROLL) // VScroll / 3D cloud: not used by Sonic Nexus (phase 3c scope)
        return;
    int yscrollOffset = SetupHLineScrollLayer(layerID);
    bool bgLayer      = activeTileLayers[layerID] != 0;
    int *deformationData, *deformationDataW;
    if (bgLayer) {
        deformationData  = &BGDeformationData3[(byte)(yscrollOffset + DeformationPos3)];
        deformationDataW = &BGDeformationData4[(byte)(yscrollOffset + waterDrawPos + DeformationPos4)];
    } else {
        deformationData  = &BGDeformationData1[(byte)(yscrollOffset + DeformationPos1)];
        deformationDataW = &BGDeformationData2[(byte)(yscrollOffset + waterDrawPos + DeformationPos2)];
    }
#if !PS1_DRAW_BG_LAYERS
    if (bgLayer) // phase 3c step 1: BG layers keep their scroll state but aren't drawn yet
        return;
#endif
    byte above = (byte)(layerID >= tLayerMidPoint);
    int fullW  = layer->xsize << 7;
    int fullH  = layer->ysize << 7;
    int tileYPos = yscrollOffset % fullH;
    if (tileYPos < 0)
        tileYPos += fullH;
    bool ss5Fix = StrComp(stageList[ActiveStageList][StageListPosition].name, "5") && ActiveStageList == STAGELIST_SPECIAL;
    int waterLines = waterDrawPos < 0 ? 0 : waterDrawPos > SCREEN_YSIZE ? SCREEN_YSIZE : waterDrawPos;

    // Per-line X scroll, exactly the PC loop (above / below the water line). The FG without
    // deformation scrolls every line by XScrollOffset: one band per tile row, no per-line pass.
    bool uniform = false;
    if (!bgLayer) {
        uniform = true;
        for (int i = 0; i < HParallax.entryCount && uniform; ++i) uniform = !HParallax.deform[i];
        if (!HParallax.entryCount)
            uniform = !HParallax.deform[0];
    }
    int ly = tileYPos;
    if (uniform) {
        int chunkX = XScrollOffset % fullW;
        if (chunkX < 0)
            chunkX += fullW;
        for (int y = 0; y < SCREEN_YSIZE;) {
            int h = TILE_SIZE - (ly & 0xF);
            if (h > SCREEN_YSIZE - y)
                h = SCREEN_YSIZE - y;
            PS1DrawTileBand(layer, above, y, h, ly, chunkX, fullW);
            y += h;
            ly += h;
            if (ly >= fullH)
                ly -= fullH;
        }
        return;
    }
    for (int y = 0; y < SCREEN_YSIZE; ++y) {
        byte idx   = layer->lineScroll[ly];
        int chunkX = HParallax.linePos[idx];
        if (y < waterLines) {
            if (HParallax.deform[idx]) {
                int deform = *deformationData;
                if (ss5Fix)
                    deform >>= 4;
                chunkX += deform;
            }
            ++deformationData;
        } else {
            if (HParallax.deform[idx])
                chunkX += *deformationDataW;
            ++deformationDataW;
        }
        if (chunkX < 0)
            chunkX += fullW;
        if (chunkX >= fullW)
            chunkX -= fullW;
        s_ps1LineX[y] = (short)chunkX;
        if (++ly >= fullH)
            ly = 0;
    }

    int stripBands         = 0;
    int layerIdx           = activeTileLayers[layerID];
    const uint8_t *stripRow = s_ps1StripCount && layerIdx < LAYER_COUNT ? s_ps1StripRow[layerIdx][above] : nullptr;
    ly = tileYPos;
    for (int y = 0; y < SCREEN_YSIZE;) {
        int chunkX = s_ps1LineX[y];
        int ri     = stripRow ? stripRow[ly >> 4] : 0;
        int h      = 1;
        if (ri) { // strip region: a band runs until the X scroll changes or the region ends
            const PS1StripRegion &r = s_ps1Strips[ri - 1];
            int end                 = r.line0 + r.lines;
            while (y + h < SCREEN_YSIZE && ly + h < end && s_ps1LineX[y + h] == chunkX) ++h;
            PS1StripBand &b = s_ps1StripBands[stripBands++]; // drawn after the loop, piece by piece
            b.sy = (short)y, b.h = (short)h, b.ly = (short)ly, b.chunkX = (short)chunkX, b.region = (uint8_t)(ri - 1);
            y += h;
            ly += h;
            if (ly >= fullH)
                ly -= fullH;
            continue;
        }
        int rowLeft = TILE_SIZE - (ly & 0xF);
        while (h < rowLeft && y + h < SCREEN_YSIZE && s_ps1LineX[y + h] == chunkX) ++h;
        PS1DrawTileBand(layer, above, y, h, ly, chunkX, fullW);
        y += h;
        ly += h;
        if (ly >= fullH)
            ly -= fullH;
    }
    // Strip bands, grouped by region and piece (one texpage each).
    for (int i = 0; i < stripBands;) {
        int region = s_ps1StripBands[i].region, j = i;
        while (j < stripBands && s_ps1StripBands[j].region == region) ++j;
        const PS1StripRegion &r = s_ps1Strips[region];
        int prims               = 0;
        for (int k = 0; k < r.pieces; ++k)
            for (int b = i; b < j; ++b) {
                const PS1StripBand &sb = s_ps1StripBands[b];
                prims += PS1DrawStripBand(r, sb.sy, sb.h, sb.ly, sb.chunkX, k);
            }
        g_ps1LayerPrims = g_ps1LayerPrims + prims;
        i = j;
    }
}


#endif

int InitRenderDevice() {
    char gameTitle[0x40];

    sprintf(gameTitle, "%s%s", Engine.GameWindowText, Engine.UseBinFile ? "" : " (Using Data Folder)");

#if RETRO_PLATFORM != RETRO_PS1
    // PS1 draws with GPU primitives; the 8-bit CPU framebuffer is never used (saves 76.8 KB heap).
    Engine.FrameBuffer = new byte[SCREEN_XSIZE * SCREEN_YSIZE];
    memset(Engine.FrameBuffer, 0, (SCREEN_XSIZE * SCREEN_YSIZE) * sizeof(byte));
#endif

#if RETRO_USING_SDL2
    SDL_Init(SDL_INIT_EVERYTHING);

    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");
    SDL_SetHint(SDL_HINT_RENDER_VSYNC, Engine.vsync ? "1" : "0");
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
    SDL_SetHint(SDL_HINT_WINRT_HANDLE_BACK_BUTTON, "1");

    byte flags      = 0;
    Engine.window   = SDL_CreateWindow(gameTitle, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, SCREEN_XSIZE * Engine.windowScale,
                                       SCREEN_YSIZE * Engine.windowScale, SDL_WINDOW_ALLOW_HIGHDPI | flags);
    Engine.renderer = SDL_CreateRenderer(Engine.window, -1, SDL_RENDERER_ACCELERATED);

    if (!Engine.window) {
        PrintLog("ERROR: failed to create window!");
        Engine.GameMode = ENGINE_EXITGAME;
        return 0;
    }

    if (!Engine.renderer) {
        PrintLog("ERROR: failed to create renderer!");
        Engine.GameMode = ENGINE_EXITGAME;
        return 0;
    }

    SDL_RenderSetLogicalSize(Engine.renderer, SCREEN_XSIZE, SCREEN_YSIZE);
    SDL_SetRenderDrawBlendMode(Engine.renderer, SDL_BLENDMODE_BLEND);

    int colourMode = 0;
    if (Engine.ColourMode == 1)
        colourMode = SDL_PIXELFORMAT_RGB565;
    else
        colourMode = SDL_PIXELFORMAT_RGB888;

    Engine.screenBuffer = SDL_CreateTexture(Engine.renderer, colourMode, SDL_TEXTUREACCESS_STREAMING, SCREEN_XSIZE, SCREEN_YSIZE);

    if (!Engine.screenBuffer) {
        PrintLog("ERROR: failed to create screen buffer!\nerror msg: %s", SDL_GetError());
        return 0;
    }

    if (Engine.startFullScreen) {
        SDL_RestoreWindow(Engine.window);
        SDL_SetWindowFullscreen(Engine.window, SDL_WINDOW_FULLSCREEN_DESKTOP);
        Engine.isFullScreen = true;
    }

    if (Engine.borderless) {
        SDL_RestoreWindow(Engine.window);
        SDL_SetWindowBordered(Engine.window, SDL_FALSE);
    }

    SDL_SetWindowPosition(Engine.window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);

    SDL_DisplayMode disp;
    int winID = SDL_GetWindowDisplayIndex(Engine.window);
    if (SDL_GetCurrentDisplayMode(winID, &disp) == 0) {
        Engine.screenRefreshRate = disp.refresh_rate;
    } else {
        printf("error: %s", SDL_GetError());
    }

#if RETRO_PLATFORM == RETRO_iOS
    SDL_RestoreWindow(Engine.window);
    SDL_SetWindowFullscreen(Engine.window, SDL_WINDOW_FULLSCREEN_DESKTOP);
    Engine.isFullScreen = true;
#endif

#endif

#if RETRO_USING_SDL1
    SDL_Init(SDL_INIT_EVERYTHING);

    // SDL1.2 doesn't support hints it seems
    // SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");
    // SDL_SetHint(SDL_HINT_RENDER_VSYNC, Engine.vsync ? "1" : "0");
    // SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
    // SDL_SetHint(SDL_HINT_WINRT_HANDLE_BACK_BUTTON, "1");

    Engine.windowSurface = SDL_SetVideoMode(SCREEN_XSIZE * Engine.windowScale, SCREEN_YSIZE * Engine.windowScale, 32, SDL_SWSURFACE);
    if (!Engine.windowSurface) {
        PrintLog("ERROR: failed to create window!\nerror msg: %s", SDL_GetError());
        return 0;
    }
    // Set the window caption
    SDL_WM_SetCaption(gameTitle, NULL);

    Engine.screenBuffer =
        SDL_CreateRGBSurface(0, SCREEN_XSIZE * Engine.windowScale, SCREEN_YSIZE * Engine.windowScale, 16, 0xF800, 0x7E0, 0x1F, 0x00);

    if (!Engine.screenBuffer) {
        PrintLog("ERROR: failed to create screen buffer!\nerror msg: %s", SDL_GetError());
        return 0;
    }

    /*Engine.screenBuffer2x = SDL_SetVideoMode(SCREEN_XSIZE * 2, SCREEN_YSIZE * 2, 16, SDL_SWSURFACE);

    if (!Engine.screenBuffer2x) {
        PrintLog("ERROR: failed to create screen buffer HQ!\nerror msg: %s", SDL_GetError());
        return 0;
    }*/

    if (Engine.startFullScreen) {
        Engine.windowSurface =
            SDL_SetVideoMode(SCREEN_XSIZE * Engine.windowScale, SCREEN_YSIZE * Engine.windowScale, 16, SDL_SWSURFACE | SDL_FULLSCREEN);
        SDL_ShowCursor(SDL_FALSE);
        Engine.isFullScreen = true;
    }

    // TODO: not supported in 1.2?
    if (Engine.borderless) {
        // SDL_RestoreWindow(Engine.window);
        // SDL_SetWindowBordered(Engine.window, SDL_FALSE);
    }

    // SDL_SetWindowPosition(Engine.window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);

    Engine.useHQModes = false; // disabled
    Engine.borderless = false; // disabled
#endif

    OBJECT_BORDER_X2 = SCREEN_XSIZE + 0x80;
    // OBJECT_BORDER_Y2 = SCREEN_YSIZE + 0x100;

    return 1;
}
void FlipScreen() {
    if (Engine.GameMode == ENGINE_EXITGAME)
        return;
#if RETRO_PLATFORM == RETRO_PS1
    // The GPU renderer already chained the frame (DrawStageGfx); psyqo flips on vsync. Like
    // PC's palette conversion here, the palette is taken as it stands after drawing.
    PS1FinalizePalette();
    {
        // Build stamp (ps1/version_stamp.hh): bottom-left of the title (NTitle) only, 8 px from the edges, after
        // everything else, faded with the palette (SetFade); its texture again on the title's first frame.
        static int s_list = -1, s_pos = -1;
        static bool s_title = false;
        if (ActiveStageList != s_list || StageListPosition != s_pos) {
            s_list     = ActiveStageList;
            s_pos      = StageListPosition;
            bool title = s_list == STAGELIST_PRESENTATION && StrComp(stageList[s_list][s_pos].folder, "NTitle");
            if (title && !s_title)
                PS1VersionUpload();
            s_title = title;
        }
        if (s_title)
            PS1VersionDraw(8, SCREEN_YSIZE - 8 - PS1_VERSION_TEXT_H, PS1DirectModulation());
    }
    return;
#endif

    // Clear the screen. This is needed to keep the
    // pillarboxes in fullscreen from displaying garbage data.
#if RETRO_USING_SDL2
    SDL_RenderClear(Engine.renderer);
#endif

    int pitch    = 0;
    void *pixels = NULL;

    switch (Engine.ColourMode) {
        case 0: // 8-bit
        {
            uint *frameBuffer = new uint[SCREEN_XSIZE * SCREEN_YSIZE];
            memset(frameBuffer, 0, (SCREEN_XSIZE * SCREEN_YSIZE) * sizeof(uint));
            if (PaletteMode) {
                for (int y = 0; y < waterDrawPos; ++y) {
                    for (int x = 0; x < SCREEN_XSIZE; ++x) {
                        Colour clr                          = TilePaletteF[Engine.FrameBuffer[(y * SCREEN_XSIZE) + x]];
                        frameBuffer[(y * SCREEN_XSIZE) + x] = PACK_RGB888(clr.r, clr.g, clr.b);
                    }
                }

                for (int y = waterDrawPos; y < SCREEN_YSIZE; ++y) {
                    for (int x = 0; x < SCREEN_XSIZE; ++x) {
                        Colour clr                          = TilePaletteWF[Engine.FrameBuffer[(y * SCREEN_XSIZE) + x]];
                        frameBuffer[(y * SCREEN_XSIZE) + x] = PACK_RGB888(clr.r, clr.g, clr.b);
                    }
                }
            } else {
                for (int y = 0; y < waterDrawPos; ++y) {
                    for (int x = 0; x < SCREEN_XSIZE; ++x) {
                        Colour clr                          = TilePalette[Engine.FrameBuffer[(y * SCREEN_XSIZE) + x]];
                        frameBuffer[(y * SCREEN_XSIZE) + x] = PACK_RGB888(clr.r, clr.g, clr.b);
                    }
                }

                for (int y = waterDrawPos; y < SCREEN_YSIZE; ++y) {
                    for (int x = 0; x < SCREEN_XSIZE; ++x) {
                        Colour clr                          = TilePaletteW[Engine.FrameBuffer[(y * SCREEN_XSIZE) + x]];
                        frameBuffer[(y * SCREEN_XSIZE) + x] = PACK_RGB888(clr.r, clr.g, clr.b);
                    }
                }
            }

#if RETRO_USING_SDL2
            SDL_LockTexture(Engine.screenBuffer, NULL, &pixels, &pitch);
            memcpy(pixels, frameBuffer, pitch * SCREEN_YSIZE);
            SDL_UnlockTexture(Engine.screenBuffer);
#endif
            if (frameBuffer)
                delete[] frameBuffer;
            break;
        }
        case 1: // 16-bit
        {
            ushort *frameBuffer = new ushort[SCREEN_XSIZE * SCREEN_YSIZE];
            memset(frameBuffer, 0, (SCREEN_XSIZE * SCREEN_YSIZE) * sizeof(ushort));
            if (PaletteMode) {
                for (int y = 0; y < waterDrawPos; ++y) {
                    for (int x = 0; x < SCREEN_XSIZE; ++x) {
                        frameBuffer[(y * SCREEN_XSIZE) + x] = TilePalette16F[Engine.FrameBuffer[(y * SCREEN_XSIZE) + x]];
                    }
                }

                for (int y = waterDrawPos; y < SCREEN_YSIZE; ++y) {
                    for (int x = 0; x < SCREEN_XSIZE; ++x) {
                        frameBuffer[(y * SCREEN_XSIZE) + x] = TilePaletteW16F[Engine.FrameBuffer[(y * SCREEN_XSIZE) + x]];
                    }
                }
            } else {
                for (int y = 0; y < waterDrawPos; ++y) {
                    for (int x = 0; x < SCREEN_XSIZE; ++x) {
                        frameBuffer[(y * SCREEN_XSIZE) + x] = TilePalette16[Engine.FrameBuffer[(y * SCREEN_XSIZE) + x]];
                    }
                }

                for (int y = waterDrawPos; y < SCREEN_YSIZE; ++y) {
                    for (int x = 0; x < SCREEN_XSIZE; ++x) {
                        frameBuffer[(y * SCREEN_XSIZE) + x] = TilePaletteW16[Engine.FrameBuffer[(y * SCREEN_XSIZE) + x]];
                    }
                }
            }

#if RETRO_USING_SDL2
            SDL_LockTexture(Engine.screenBuffer, NULL, &pixels, &pitch);
            memcpy(pixels, frameBuffer, pitch * SCREEN_YSIZE);
            SDL_UnlockTexture(Engine.screenBuffer);
#endif
            if (frameBuffer)
                delete[] frameBuffer;
            break;
        }
        case 2: // 32-bit
        {
            uint *frameBuffer = new uint[SCREEN_XSIZE * SCREEN_YSIZE];
            memset(frameBuffer, 0, (SCREEN_XSIZE * SCREEN_YSIZE) * sizeof(uint));
            if (PaletteMode) {
                for (int y = 0; y < waterDrawPos; ++y) {
                    for (int x = 0; x < SCREEN_XSIZE; ++x) {
                        uint clr                            = TilePalette32F[Engine.FrameBuffer[(y * SCREEN_XSIZE) + x]];
                        byte r                              = (clr >> 16) & 0xFF;
                        byte g                              = (clr >> 8) & 0xFF;
                        byte b                              = (clr >> 0) & 0xFF;
                        frameBuffer[(y * SCREEN_XSIZE) + x] = PACK_RGB888(r, g, b);
                    }
                }

                for (int y = waterDrawPos; y < SCREEN_YSIZE; ++y) {
                    for (int x = 0; x < SCREEN_XSIZE; ++x) {
                        uint clr                            = TilePaletteW32F[Engine.FrameBuffer[(y * SCREEN_XSIZE) + x]];
                        byte r                              = (clr >> 16) & 0xFF;
                        byte g                              = (clr >> 8) & 0xFF;
                        byte b                              = (clr >> 0) & 0xFF;
                        frameBuffer[(y * SCREEN_XSIZE) + x] = PACK_RGB888(r, g, b);
                    }
                }
            } else {
                for (int y = 0; y < waterDrawPos; ++y) {
                    for (int x = 0; x < SCREEN_XSIZE; ++x) {
                        uint clr                            = TilePalette32[Engine.FrameBuffer[(y * SCREEN_XSIZE) + x]];
                        byte r                              = (clr >> 16) & 0xFF;
                        byte g                              = (clr >> 8) & 0xFF;
                        byte b                              = (clr >> 0) & 0xFF;
                        frameBuffer[(y * SCREEN_XSIZE) + x] = PACK_RGB888(r, g, b);
                    }
                }

                for (int y = waterDrawPos; y < SCREEN_YSIZE; ++y) {
                    for (int x = 0; x < SCREEN_XSIZE; ++x) {
                        uint clr                            = TilePaletteW32[Engine.FrameBuffer[(y * SCREEN_XSIZE) + x]];
                        byte r                              = (clr >> 16) & 0xFF;
                        byte g                              = (clr >> 8) & 0xFF;
                        byte b                              = (clr >> 0) & 0xFF;
                        frameBuffer[(y * SCREEN_XSIZE) + x] = PACK_RGB888(r, g, b);
                    }
                }
            }

#if RETRO_USING_SDL2
            SDL_LockTexture(Engine.screenBuffer, NULL, &pixels, &pitch);
            memcpy(pixels, frameBuffer, pitch * SCREEN_YSIZE);
            SDL_UnlockTexture(Engine.screenBuffer);
#endif
            if (frameBuffer)
                delete[] frameBuffer;
            break;
        }
    }

#if RETRO_USING_SDL2
    SDL_SetRenderTarget(Engine.renderer, NULL);
    SDL_RenderCopy(Engine.renderer, Engine.screenBuffer, NULL, NULL);

    SDL_RenderPresent(Engine.renderer);
#endif
}

void ReleaseRenderDevice() {
    if (Engine.FrameBuffer)
        delete[] Engine.FrameBuffer;
#if RETRO_USING_SDL2
    SDL_DestroyTexture(Engine.screenBuffer);
    Engine.screenBuffer = NULL;
#endif

#if RETRO_USING_SDL1
    SDL_FreeSurface(Engine.screenBuffer);
#endif

#if RETRO_USING_SDL2
    SDL_DestroyRenderer(Engine.renderer);
    SDL_DestroyWindow(Engine.window);
#endif
}

void ClearScreen(byte index)
{
#if RETRO_PLATFORM == RETRO_PS1
    return; // PS1: software rasteriser output is never displayed; GPU path TODO (phase 3c)
#endif
    byte *pixelBuffer = Engine.FrameBuffer;
    int cnt           = SCREEN_XSIZE * SCREEN_YSIZE;
    while (cnt--) {
        *pixelBuffer = index;
        ++pixelBuffer;
    }
}

void SetScreenSize(int width, int lineSize) {
    SCREEN_XSIZE        = width;
    SCREEN_CENTERX      = (width / 2);
    SCREEN_SCROLL_LEFT  = SCREEN_CENTERX - 8;
    SCREEN_SCROLL_RIGHT = SCREEN_CENTERX + 8;
    OBJECT_BORDER_X2    = width + 0x80;
}

#if RETRO_PLATFORM == RETRO_PS1
// Verification aid (GDB): bit 0 = drop the primitives of object lists (their draw scripts still
// run, so game state is unchanged) -> layers-only frames for tools/layer_ref.py.
volatile uint8_t g_ps1DebugDrawMask = 0;
#endif

void DrawObjectList(int Layer) {
#if RETRO_PLATFORM == RETRO_PS1
    g_ps1ChainSkipPrims = (g_ps1DebugDrawMask & 1) != 0;
#endif
    int size = ObjectDrawOrderList[Layer].listSize;
    for (int i = 0; i < size; ++i) {
        ObjectLoop = ObjectDrawOrderList[Layer].entityRefs[i];
        int type   = ObjectEntityList[ObjectLoop].type;

        if (type == OBJ_TYPE_PLAYER) {
            Player *player = &PlayerList[ObjectLoop];
            ProcessPlayerAnimationChange(player);
            if (player->visible) {
                DrawPlayer(player, &PlayerScriptList[player->type].animations[player->animation].frames[player->frame]);
            }
        } else if (type) {
            PlayerNo = 0;
            if (ScriptData[ObjectScriptList[type].subDraw.scriptCodePtr] > 0)
                ProcessScript(ObjectScriptList[type].subDraw.scriptCodePtr, ObjectScriptList[type].subDraw.jumpTablePtr, SUB_DRAW);
        }
    }
#if RETRO_PLATFORM == RETRO_PS1
    g_ps1ChainSkipPrims = false;
#endif
}
void DrawStageGfx() {
#if RETRO_PLATFORM == RETRO_PS1
    if (g_ps1GPU) {
        g_ps1LayerPrims = 0;
        g_ps1LayerHbl   = 0;
        PS1UploadTileSet();        // once per stage (immediate, waits for the in-flight chain)
        // Chained FastFill of the buffer this frame draws into + chained CLUT uploads; both are
        // rewritten by PS1FinalizePalette() (FlipScreen) with the end-of-frame palette.
        psyqo::Color bg = PS1ClearColour();
        s_clearPrim     = PS1ChainClear(bg.r, bg.g, bg.b);
        PS1UploadPalette();
        PS1ChainInvalidateTPage();
    }
#endif
    DrawObjectList(0);
    if (activeTileLayers[0] < LAYER_COUNT) {
#if RETRO_PLATFORM == RETRO_PS1
        PS1DrawTileLayer(0);
#else
        switch (StageLayouts[activeTileLayers[0]].type) {
            case LAYER_HSCROLL: DrawHLineScrollLayer(0); break;
            case LAYER_VSCROLL: DrawVLineScrollLayer(0); break;
            case LAYER_3DCLOUD: Draw3DCloudLayer(0); break;
            default: break;
        }
#endif
    }

    DrawObjectList(1);
    if (activeTileLayers[1] < LAYER_COUNT) {
#if RETRO_PLATFORM == RETRO_PS1
        PS1DrawTileLayer(1);
#else
        switch (StageLayouts[activeTileLayers[1]].type) {
            case LAYER_HSCROLL: DrawHLineScrollLayer(1); break;
            case LAYER_VSCROLL: DrawVLineScrollLayer(1); break;
            case LAYER_3DCLOUD: Draw3DCloudLayer(1); break;
            default: break;
        }
#endif
    }

    DrawObjectList(2);
    if (activeTileLayers[2] < LAYER_COUNT) {
#if RETRO_PLATFORM == RETRO_PS1
        PS1DrawTileLayer(2);
#else
        switch (StageLayouts[activeTileLayers[2]].type) {
            case LAYER_HSCROLL: DrawHLineScrollLayer(2); break;
            case LAYER_VSCROLL: DrawVLineScrollLayer(2); break;
            case LAYER_3DCLOUD: Draw3DCloudLayer(2); break;
            default: break;
        }
#endif
    }

    DrawObjectList(3);
    DrawObjectList(4);
    if (activeTileLayers[3] < LAYER_COUNT) {
#if RETRO_PLATFORM == RETRO_PS1
        PS1DrawTileLayer(3);
#else
        switch (StageLayouts[activeTileLayers[3]].type) {
            case LAYER_HSCROLL: DrawHLineScrollLayer(3); break;
            case LAYER_VSCROLL: DrawVLineScrollLayer(3); break;
            case LAYER_3DCLOUD: Draw3DCloudLayer(3); break;
            default: break;
        }
#endif
    }

    DrawObjectList(5);
    DrawObjectList(6);
}

void GenerateBlendTable(ushort alpha, byte type, byte a3, byte a4) {
#if RETRO_PLATFORM == RETRO_PS1
    // No 64 KB LUT: GPU semi-transparency does the 50/50 mix. Type 1 (luminance ramp) keeps, per
    // source index, the ramp entry of its luminance (palette at call time, like PC).
    s_blendType = type;
    for (int i = 0; i < 256; ++i) {
        int v         = (TilePalette[i].b + TilePalette[i].g + TilePalette[i].r) / 3;
        s_blendIdx[i] = type == 1 ? (byte)(a4 + a3 * v / 0x100) : (byte)i;
    }
    (void)alpha;
#else
    switch (type) {
        case 0:
            for (int y = 0; y < 256; ++y) {
                for (int x = 0; x < 256; ++x) {
                    ushort mixR = (ushort)((0xFF - alpha) * TilePalette[y].r + alpha * TilePalette[x].r) >> 8;
                    ushort mixG = (ushort)((0xFF - alpha) * TilePalette[y].g + alpha * TilePalette[x].g) >> 8;
                    ushort mixB = (ushort)((0xFF - alpha) * TilePalette[y].b + alpha * TilePalette[x].b) >> 8;
                    int index   = 0;
                    int r       = -1;
                    int g       = -1;
                    int b       = -1;
                    for (int i = 0; i < 256; ++i) {
                        int mixR2 = abs(TilePalette[i].r - mixR);
                        int mixG2 = abs(TilePalette[i].g - mixG);
                        int mixB2 = abs(TilePalette[i].b - mixB);
                        if (mixR2 < r && mixG2 < g && mixB2 < b) {
                            r     = mixR2;
                            g     = mixG2;
                            b     = mixB2;
                            index = i;
                        }
                    }
                    BlendLookupTable[(0x100 * y) + x] = index;
                }
            }
            break;
        case 1:
            for (int y = 0; y < 0x100; ++y) {
                for (int x = 0; x < 0x100; ++x) {
                    int v1                          = (byte)((TilePalette[y].b + TilePalette[y].g + TilePalette[y].r) / 3);
                    int v2                          = (byte)((TilePalette[x].b + TilePalette[x].g + TilePalette[x].r) / 3);
                    BlendLookupTable[0x100 * y + x] = a4 + a3 * ((ushort)((0xFF - alpha) * v1 + alpha * v2) >> 8) / 0x100;
                }
            }
            break;
    }
#endif
}

void GenerateTintTable(short alpha, short a2, byte type, byte a4, byte a5, byte tableID) {
    byte *tintTable = NULL;
    switch (tableID) {
        case 0: tintTable = TintLookupTable1; break;
        case 1: tintTable = TintLookupTable2; break;
        case 2: tintTable = TintLookupTable3; break;
        case 3: tintTable = TintLookupTable4; break;
        default: break;
    }

    switch (type) {
        case 0:
            for (int i = 0; i < 256; ++i) {
                byte val     = (byte)((TilePalette[i].b + TilePalette[i].g + TilePalette[i].r) / 3);
                tintTable[i] = a5 + a4 * ((ushort)((0xFF - alpha) * val + alpha * a2) >> 8) / 256;
            }
            break;
        case 1:
            for (int i = 0; i < 256; ++i) {
                tintTable[i] = a5 + a4 * ((ushort)((0xFF - alpha) * TilePalette[i].r + alpha * a2) >> 8) / 256;
            }
            break;
        case 2:
            for (int i = 0; i < 256; ++i) {
                tintTable[i] = a5 + a4 * ((ushort)((0xFF - alpha) * TilePalette[i].g + alpha * a2) >> 8) / 256;
            }
            break;
        case 3:
            for (int i = 0; i < 256; ++i) {
                tintTable[i] = a5 + a4 * ((ushort)((0xFF - alpha) * TilePalette[i].b + alpha * a2) >> 8) / 256;
            }
            break;
        default: break;
    }
}

void DrawHLineScrollLayer(int layerID) {
#if RETRO_PLATFORM == RETRO_PS1
    return; // PS1: software rasteriser output is never displayed; GPU path TODO (phase 3c)
#endif
    TileLayer *layer = &StageLayouts[activeTileLayers[layerID]];
    if (!layer->xsize || !layer->ysize)
        return;
    int screenwidth16  = (SCREEN_XSIZE >> 4) - 1;
    int layerwidth     = layer->xsize;
    int layerheight    = layer->ysize;
    bool aboveMidPoint = layerID >= tLayerMidPoint;

    int yscrollOffset      = SetupHLineScrollLayer(layerID);
    byte *lineScroll       = layer->lineScroll;
    int *deformationData   = NULL;
    int *deformationDataW  = NULL;
    if (activeTileLayers[layerID]) { // BG Layer
        deformationData  = &BGDeformationData3[(byte)(yscrollOffset + DeformationPos3)];
        deformationDataW = &BGDeformationData4[(byte)(yscrollOffset + waterDrawPos + DeformationPos4)];
    } else { // FG Layer
        deformationData  = &BGDeformationData1[(byte)(yscrollOffset + DeformationPos1)];
        deformationDataW = &BGDeformationData2[(byte)(yscrollOffset + waterDrawPos + DeformationPos2)];
    }

    byte *pixelBufferPtr = Engine.FrameBuffer;
    int tileYPos         = yscrollOffset % (layerheight << 7);
    if (tileYPos < 0)
        tileYPos += layerheight << 7;
    byte *scrollIndex = &lineScroll[tileYPos];
    int tileY16       = tileYPos & 0xF;
    int chunkY        = tileYPos >> 7;
    int tileY         = (tileYPos & 0x7F) >> 4;

    // Draw Above Water (if applicable)
    int drawableLines[2] = { waterDrawPos, SCREEN_YSIZE - waterDrawPos };
    for (int i = 0; i < 2; ++i) {
        while (drawableLines[i]--) {
            int chunkX = HParallax.linePos[*scrollIndex];
            if (i == 0) {
                int deform = 0;
                if (HParallax.deform[*scrollIndex])
                    deform = *deformationData;

                // Fix for SS5 mobile bug
                if (StrComp(stageList[ActiveStageList][StageListPosition].name, "5") && ActiveStageList == STAGELIST_SPECIAL)
                    deform >>= 4;

                chunkX += deform;
                ++deformationData;
            } else {
                if (HParallax.deform[*scrollIndex])
                    chunkX += *deformationDataW;
                ++deformationDataW;
            }
            ++scrollIndex;
            int fullLayerwidth = layerwidth << 7;
            if (chunkX < 0)
                chunkX += fullLayerwidth;
            if (chunkX >= fullLayerwidth)
                chunkX -= fullLayerwidth;
            int chunkXPos         = chunkX >> 7;
            int tilePxXPos        = chunkX & 0xF;
            int tileXPxRemain     = TILE_SIZE - tilePxXPos;
            int chunk             = (layer->tiles[(chunkX >> 7) + (chunkY * TILELAYER_CHUNK_W)] << 6) + ((chunkX & 0x7F) >> 4) + 8 * tileY;
            int tileOffsetY       = TILE_SIZE * tileY16;
            int tileOffsetYFlipX  = TILE_SIZE * tileY16 + 0xF;
            int tileOffsetYFlipY  = TILE_SIZE * (0xF - tileY16);
            int tileOffsetYFlipXY = TILE_SIZE * (0xF - tileY16) + 0xF;
            int lineRemain        = SCREEN_XSIZE;

            byte *gfxDataPtr  = NULL;
            int tilePxLineCnt = 0;

            // Draw the first tile to the left
            if (StageTiles.visualPlane[chunk] == (byte)aboveMidPoint) {
                tilePxLineCnt = TILE_SIZE - tilePxXPos;
                lineRemain -= tilePxLineCnt;
                switch (StageTiles.direction[chunk]) {
                    case FLIP_NONE:
                        gfxDataPtr = &TileGfx[tileOffsetY + StageTiles.gfxDataPos[chunk] + tilePxXPos];
                        while (tilePxLineCnt--) {
                            if (*gfxDataPtr > 0)
                                *pixelBufferPtr = *gfxDataPtr;
                            ++pixelBufferPtr;
                            ++gfxDataPtr;
                        }
                        break;

                    case FLIP_X:
                        gfxDataPtr = &TileGfx[tileOffsetYFlipX + StageTiles.gfxDataPos[chunk] - tilePxXPos];
                        while (tilePxLineCnt--) {
                            if (*gfxDataPtr > 0)
                                *pixelBufferPtr = *gfxDataPtr;
                            ++pixelBufferPtr;
                            --gfxDataPtr;
                        }
                        break;

                    case FLIP_Y:
                        gfxDataPtr = &TileGfx[tileOffsetYFlipY + StageTiles.gfxDataPos[chunk] + tilePxXPos];
                        while (tilePxLineCnt--) {
                            if (*gfxDataPtr > 0)
                                *pixelBufferPtr = *gfxDataPtr;
                            ++pixelBufferPtr;
                            ++gfxDataPtr;
                        }
                        break;

                    case FLIP_XY:
                        gfxDataPtr = &TileGfx[tileOffsetYFlipXY + StageTiles.gfxDataPos[chunk] - tilePxXPos];
                        while (tilePxLineCnt--) {
                            if (*gfxDataPtr > 0)
                                *pixelBufferPtr = *gfxDataPtr;
                            ++pixelBufferPtr;
                            --gfxDataPtr;
                        }
                        break;
                    default: break;
                }
            } else {
                pixelBufferPtr += tileXPxRemain;
                lineRemain -= tileXPxRemain;
            }

            // Draw the bulk of the tiles
            int chunkTileX   = ((chunkX & 0x7F) >> 4) + 1;
            int tilesPerLine = screenwidth16;
            while (tilesPerLine--) {
                if (chunkTileX < 8) {
                    ++chunk;
                } else {
                    if (++chunkXPos == layerwidth)
                        chunkXPos = 0;
                    chunkTileX = 0;
                    chunk      = (layer->tiles[chunkXPos + (chunkY * TILELAYER_CHUNK_W)] << 6) + 8 * tileY;
                }
                lineRemain -= TILE_SIZE;

                // Loop Unrolling (faster but messier code)
                if (StageTiles.visualPlane[chunk] == (byte)aboveMidPoint) {
                    switch (StageTiles.direction[chunk]) {
                        case FLIP_NONE:
                            gfxDataPtr = &TileGfx[StageTiles.gfxDataPos[chunk] + tileOffsetY];
                            if (*gfxDataPtr > 0)
                                *pixelBufferPtr = *gfxDataPtr;
                            ++pixelBufferPtr;
                            ++gfxDataPtr;
                            if (*gfxDataPtr > 0)
                                *pixelBufferPtr = *gfxDataPtr;
                            ++pixelBufferPtr;
                            ++gfxDataPtr;
                            if (*gfxDataPtr > 0)
                                *pixelBufferPtr = *gfxDataPtr;
                            ++pixelBufferPtr;
                            ++gfxDataPtr;
                            if (*gfxDataPtr > 0)
                                *pixelBufferPtr = *gfxDataPtr;
                            ++pixelBufferPtr;
                            ++gfxDataPtr;
                            if (*gfxDataPtr > 0)
                                *pixelBufferPtr = *gfxDataPtr;
                            ++pixelBufferPtr;
                            ++gfxDataPtr;
                            if (*gfxDataPtr > 0)
                                *pixelBufferPtr = *gfxDataPtr;
                            ++pixelBufferPtr;
                            ++gfxDataPtr;
                            if (*gfxDataPtr > 0)
                                *pixelBufferPtr = *gfxDataPtr;
                            ++pixelBufferPtr;
                            ++gfxDataPtr;
                            if (*gfxDataPtr > 0)
                                *pixelBufferPtr = *gfxDataPtr;
                            ++pixelBufferPtr;
                            ++gfxDataPtr;
                            if (*gfxDataPtr > 0)
                                *pixelBufferPtr = *gfxDataPtr;
                            ++pixelBufferPtr;
                            ++gfxDataPtr;
                            if (*gfxDataPtr > 0)
                                *pixelBufferPtr = *gfxDataPtr;
                            ++pixelBufferPtr;
                            ++gfxDataPtr;
                            if (*gfxDataPtr > 0)
                                *pixelBufferPtr = *gfxDataPtr;
                            ++pixelBufferPtr;
                            ++gfxDataPtr;
                            if (*gfxDataPtr > 0)
                                *pixelBufferPtr = *gfxDataPtr;
                            ++pixelBufferPtr;
                            ++gfxDataPtr;
                            if (*gfxDataPtr > 0)
                                *pixelBufferPtr = *gfxDataPtr;
                            ++pixelBufferPtr;
                            ++gfxDataPtr;
                            if (*gfxDataPtr > 0)
                                *pixelBufferPtr = *gfxDataPtr;
                            ++pixelBufferPtr;
                            ++gfxDataPtr;
                            if (*gfxDataPtr > 0)
                                *pixelBufferPtr = *gfxDataPtr;
                            ++pixelBufferPtr;
                            ++gfxDataPtr;
                            if (*gfxDataPtr > 0)
                                *pixelBufferPtr = *gfxDataPtr;
                            ++pixelBufferPtr;
                            ++gfxDataPtr;
                            break;

                        case FLIP_X:
                            gfxDataPtr = &TileGfx[StageTiles.gfxDataPos[chunk] + tileOffsetYFlipX];
                            if (*gfxDataPtr > 0)
                                *pixelBufferPtr = *gfxDataPtr;
                            ++pixelBufferPtr;
                            --gfxDataPtr;
                            if (*gfxDataPtr > 0)
                                *pixelBufferPtr = *gfxDataPtr;
                            ++pixelBufferPtr;
                            --gfxDataPtr;
                            if (*gfxDataPtr > 0)
                                *pixelBufferPtr = *gfxDataPtr;
                            ++pixelBufferPtr;
                            --gfxDataPtr;
                            if (*gfxDataPtr > 0)
                                *pixelBufferPtr = *gfxDataPtr;
                            ++pixelBufferPtr;
                            --gfxDataPtr;
                            if (*gfxDataPtr > 0)
                                *pixelBufferPtr = *gfxDataPtr;
                            ++pixelBufferPtr;
                            --gfxDataPtr;
                            if (*gfxDataPtr > 0)
                                *pixelBufferPtr = *gfxDataPtr;
                            ++pixelBufferPtr;
                            --gfxDataPtr;
                            if (*gfxDataPtr > 0)
                                *pixelBufferPtr = *gfxDataPtr;
                            ++pixelBufferPtr;
                            --gfxDataPtr;
                            if (*gfxDataPtr > 0)
                                *pixelBufferPtr = *gfxDataPtr;
                            ++pixelBufferPtr;
                            --gfxDataPtr;
                            if (*gfxDataPtr > 0)
                                *pixelBufferPtr = *gfxDataPtr;
                            ++pixelBufferPtr;
                            --gfxDataPtr;
                            if (*gfxDataPtr > 0)
                                *pixelBufferPtr = *gfxDataPtr;
                            ++pixelBufferPtr;
                            --gfxDataPtr;
                            if (*gfxDataPtr > 0)
                                *pixelBufferPtr = *gfxDataPtr;
                            ++pixelBufferPtr;
                            --gfxDataPtr;
                            if (*gfxDataPtr > 0)
                                *pixelBufferPtr = *gfxDataPtr;
                            ++pixelBufferPtr;
                            --gfxDataPtr;
                            if (*gfxDataPtr > 0)
                                *pixelBufferPtr = *gfxDataPtr;
                            ++pixelBufferPtr;
                            --gfxDataPtr;
                            if (*gfxDataPtr > 0)
                                *pixelBufferPtr = *gfxDataPtr;
                            ++pixelBufferPtr;
                            --gfxDataPtr;
                            if (*gfxDataPtr > 0)
                                *pixelBufferPtr = *gfxDataPtr;
                            ++pixelBufferPtr;
                            --gfxDataPtr;
                            if (*gfxDataPtr > 0)
                                *pixelBufferPtr = *gfxDataPtr;
                            ++pixelBufferPtr;
                            --gfxDataPtr;
                            break;

                        case FLIP_Y:
                            gfxDataPtr = &TileGfx[StageTiles.gfxDataPos[chunk] + tileOffsetYFlipY];
                            if (*gfxDataPtr > 0)
                                *pixelBufferPtr = *gfxDataPtr;
                            ++pixelBufferPtr;
                            ++gfxDataPtr;
                            if (*gfxDataPtr > 0)
                                *pixelBufferPtr = *gfxDataPtr;
                            ++pixelBufferPtr;
                            ++gfxDataPtr;
                            if (*gfxDataPtr > 0)
                                *pixelBufferPtr = *gfxDataPtr;
                            ++pixelBufferPtr;
                            ++gfxDataPtr;
                            if (*gfxDataPtr > 0)
                                *pixelBufferPtr = *gfxDataPtr;
                            ++pixelBufferPtr;
                            ++gfxDataPtr;
                            if (*gfxDataPtr > 0)
                                *pixelBufferPtr = *gfxDataPtr;
                            ++pixelBufferPtr;
                            ++gfxDataPtr;
                            if (*gfxDataPtr > 0)
                                *pixelBufferPtr = *gfxDataPtr;
                            ++pixelBufferPtr;
                            ++gfxDataPtr;
                            if (*gfxDataPtr > 0)
                                *pixelBufferPtr = *gfxDataPtr;
                            ++pixelBufferPtr;
                            ++gfxDataPtr;
                            if (*gfxDataPtr > 0)
                                *pixelBufferPtr = *gfxDataPtr;
                            ++pixelBufferPtr;
                            ++gfxDataPtr;
                            if (*gfxDataPtr > 0)
                                *pixelBufferPtr = *gfxDataPtr;
                            ++pixelBufferPtr;
                            ++gfxDataPtr;
                            if (*gfxDataPtr > 0)
                                *pixelBufferPtr = *gfxDataPtr;
                            ++pixelBufferPtr;
                            ++gfxDataPtr;
                            if (*gfxDataPtr > 0)
                                *pixelBufferPtr = *gfxDataPtr;
                            ++pixelBufferPtr;
                            ++gfxDataPtr;
                            if (*gfxDataPtr > 0)
                                *pixelBufferPtr = *gfxDataPtr;
                            ++pixelBufferPtr;
                            ++gfxDataPtr;
                            if (*gfxDataPtr > 0)
                                *pixelBufferPtr = *gfxDataPtr;
                            ++pixelBufferPtr;
                            ++gfxDataPtr;
                            if (*gfxDataPtr > 0)
                                *pixelBufferPtr = *gfxDataPtr;
                            ++pixelBufferPtr;
                            ++gfxDataPtr;
                            if (*gfxDataPtr > 0)
                                *pixelBufferPtr = *gfxDataPtr;
                            ++pixelBufferPtr;
                            ++gfxDataPtr;
                            if (*gfxDataPtr > 0)
                                *pixelBufferPtr = *gfxDataPtr;
                            ++pixelBufferPtr;
                            ++gfxDataPtr;
                            break;

                        case FLIP_XY:
                            gfxDataPtr = &TileGfx[StageTiles.gfxDataPos[chunk] + tileOffsetYFlipXY];
                            if (*gfxDataPtr > 0)
                                *pixelBufferPtr = *gfxDataPtr;
                            ++pixelBufferPtr;
                            --gfxDataPtr;
                            if (*gfxDataPtr > 0)
                                *pixelBufferPtr = *gfxDataPtr;
                            ++pixelBufferPtr;
                            --gfxDataPtr;
                            if (*gfxDataPtr > 0)
                                *pixelBufferPtr = *gfxDataPtr;
                            ++pixelBufferPtr;
                            --gfxDataPtr;
                            if (*gfxDataPtr > 0)
                                *pixelBufferPtr = *gfxDataPtr;
                            ++pixelBufferPtr;
                            --gfxDataPtr;
                            if (*gfxDataPtr > 0)
                                *pixelBufferPtr = *gfxDataPtr;
                            ++pixelBufferPtr;
                            --gfxDataPtr;
                            if (*gfxDataPtr > 0)
                                *pixelBufferPtr = *gfxDataPtr;
                            ++pixelBufferPtr;
                            --gfxDataPtr;
                            if (*gfxDataPtr > 0)
                                *pixelBufferPtr = *gfxDataPtr;
                            ++pixelBufferPtr;
                            --gfxDataPtr;
                            if (*gfxDataPtr > 0)
                                *pixelBufferPtr = *gfxDataPtr;
                            ++pixelBufferPtr;
                            --gfxDataPtr;
                            if (*gfxDataPtr > 0)
                                *pixelBufferPtr = *gfxDataPtr;
                            ++pixelBufferPtr;
                            --gfxDataPtr;
                            if (*gfxDataPtr > 0)
                                *pixelBufferPtr = *gfxDataPtr;
                            ++pixelBufferPtr;
                            --gfxDataPtr;
                            if (*gfxDataPtr > 0)
                                *pixelBufferPtr = *gfxDataPtr;
                            ++pixelBufferPtr;
                            --gfxDataPtr;
                            if (*gfxDataPtr > 0)
                                *pixelBufferPtr = *gfxDataPtr;
                            ++pixelBufferPtr;
                            --gfxDataPtr;
                            if (*gfxDataPtr > 0)
                                *pixelBufferPtr = *gfxDataPtr;
                            ++pixelBufferPtr;
                            --gfxDataPtr;
                            if (*gfxDataPtr > 0)
                                *pixelBufferPtr = *gfxDataPtr;
                            ++pixelBufferPtr;
                            --gfxDataPtr;
                            if (*gfxDataPtr > 0)
                                *pixelBufferPtr = *gfxDataPtr;
                            ++pixelBufferPtr;
                            --gfxDataPtr;
                            if (*gfxDataPtr > 0)
                                *pixelBufferPtr = *gfxDataPtr;
                            ++pixelBufferPtr;
                            --gfxDataPtr;
                            break;
                    }
                } else {
                    pixelBufferPtr += TILE_SIZE;
                }
                ++chunkTileX;
            }

            // Draw any remaining tiles
            while (lineRemain > 0) {
                if (chunkTileX++ < 8) {
                    ++chunk;
                } else {
                    chunkTileX = 0;
                    if (++chunkXPos == layerwidth)
                        chunkXPos = 0;

                    chunk = (layer->tiles[chunkXPos + (chunkY * TILELAYER_CHUNK_W)] << 6) + 8 * tileY;
                }

                tilePxLineCnt = lineRemain >= TILE_SIZE ? TILE_SIZE : lineRemain;
                lineRemain -= tilePxLineCnt;
                if (StageTiles.visualPlane[chunk] == (byte)aboveMidPoint) {
                    switch (StageTiles.direction[chunk]) {
                        case FLIP_NONE:
                            gfxDataPtr = &TileGfx[StageTiles.gfxDataPos[chunk] + tileOffsetY];
                            while (tilePxLineCnt--) {
                                if (*gfxDataPtr > 0)
                                    *pixelBufferPtr = *gfxDataPtr;
                                ++pixelBufferPtr;
                                ++gfxDataPtr;
                            }
                            break;

                        case FLIP_X:
                            gfxDataPtr = &TileGfx[StageTiles.gfxDataPos[chunk] + tileOffsetYFlipX];
                            while (tilePxLineCnt--) {
                                if (*gfxDataPtr > 0)
                                    *pixelBufferPtr = *gfxDataPtr;
                                ++pixelBufferPtr;
                                --gfxDataPtr;
                            }
                            break;

                        case FLIP_Y:
                            gfxDataPtr = &TileGfx[StageTiles.gfxDataPos[chunk] + tileOffsetYFlipY];
                            while (tilePxLineCnt--) {
                                if (*gfxDataPtr > 0)
                                    *pixelBufferPtr = *gfxDataPtr;
                                ++pixelBufferPtr;
                                ++gfxDataPtr;
                            }
                            break;

                        case FLIP_XY:
                            gfxDataPtr = &TileGfx[StageTiles.gfxDataPos[chunk] + tileOffsetYFlipXY];
                            while (tilePxLineCnt--) {
                                if (*gfxDataPtr > 0)
                                    *pixelBufferPtr = *gfxDataPtr;
                                ++pixelBufferPtr;
                                --gfxDataPtr;
                            }
                            break;

                        default: break;
                    }
                } else {
                    pixelBufferPtr += tilePxLineCnt;
                }
            }

            if (++tileY16 >= TILE_SIZE) {
                tileY16 = 0;
                ++tileY;
            }

            if (tileY >= 8) {
                if (++chunkY == layerheight) {
                    chunkY = 0;
                    scrollIndex -= 0x80 * layerheight;
                }
                tileY = 0;
            }
        }
    }
}
void DrawVLineScrollLayer(int layerID) {
#if RETRO_PLATFORM == RETRO_PS1
    return; // PS1: software rasteriser output is never displayed; GPU path TODO (phase 3c)
#endif

    TileLayer *layer = &StageLayouts[activeTileLayers[layerID]];
    if (!layer->xsize || !layer->ysize)
        return;

    int layerwidth     = layer->xsize;
    int layerheight    = layer->ysize;
    bool aboveMidPoint = layerID >= tLayerMidPoint;

    byte *lineScroll;
    int *deformationData;

    int xscrollOffset = 0;
    if (activeTileLayers[layerID]) { // BG Layer
        int xScroll        = XScrollOffset * layer->parallaxFactor >> 6;
        int fullLayerwidth = layerwidth << 7;
        layer->scrollPos += layer->scrollSpeed;
        if (layer->scrollPos > fullLayerwidth << 16)
            layer->scrollPos -= fullLayerwidth << 16;
        xscrollOffset   = (xScroll + (layer->scrollPos >> 16)) % fullLayerwidth;
        layerwidth      = fullLayerwidth >> 7;
        lineScroll      = layer->lineScroll;
        deformationData = &BGDeformationData3[(byte)(xscrollOffset + DeformationPos3)];
    } else { // FG Layer
        LastYSize            = layer->ysize;
        xscrollOffset        = XScrollOffset;
        lineScroll           = layer->lineScroll;
        VParallax.linePos[0] = YScrollOffset;
        VParallax.deform[0]  = true;
        deformationData      = &BGDeformationData1[(byte)(XScrollOffset + DeformationPos1)];
    }

    if (layer->type == LAYER_VSCROLL) {
        if (LastYSize != layerheight) {
            int fullLayerheight = layerheight << 7;
            for (int i = 0; i < VParallax.entryCount; ++i) {
                VParallax.linePos[i] = YScrollOffset * VParallax.parallaxFactor[i] >> 7;

                VParallax.scrollPos[i] += VParallax.scrollPos[i] << 16;
                if (VParallax.scrollPos[i] > fullLayerheight << 16)
                    VParallax.scrollPos[i] -= fullLayerheight << 16;

                VParallax.linePos[i] += VParallax.scrollPos[i] >> 16;
                VParallax.linePos[i] %= fullLayerheight;
            }
            layerheight = fullLayerheight >> 7;
        }
        LastYSize = layerheight;
    }

    byte *pixelBufferPtr = Engine.FrameBuffer;
    int tileXPos         = xscrollOffset % (layerheight << 7);
    if (tileXPos < 0)
        tileXPos += layerheight << 7;
    byte *scrollIndex = &lineScroll[tileXPos];
    int chunkX        = tileXPos >> 7;
    int tileX16       = tileXPos & 0xF;
    int tileX         = (tileXPos & 0x7F) >> 4;

    // Draw Above Water (if applicable)
    int drawableLines = SCREEN_XSIZE;
    while (drawableLines--) {
        int chunkY = VParallax.linePos[*scrollIndex];
        if (VParallax.deform[*scrollIndex])
            chunkY += *deformationData;
        ++deformationData;
        ++scrollIndex;

        int fullLayerHeight = layerheight << 7;
        if (chunkY < 0)
            chunkY += fullLayerHeight;
        if (chunkY >= fullLayerHeight)
            chunkY -= fullLayerHeight;

        int chunkYPos         = chunkY >> 7;
        int tileY             = chunkY & 0xF;
        int tileYPxRemain     = TILE_SIZE - tileY;
        int chunk             = (layer->tiles[chunkX + ((chunkY >> 7) * TILELAYER_CHUNK_W)] << 6) + tileX + 8 * ((chunkY & 0x7F) >> 4);
        int tileOffsetXFlipX  = 0xF - tileX16;
        int tileOffsetXFlipY  = tileX16 + SCREEN_YSIZE;
        int tileOffsetXFlipXY = 0xFF - tileX16;
        int lineRemain        = SCREEN_YSIZE;

        byte *gfxDataPtr  = NULL;
        int tilePxLineCnt = tileYPxRemain;

        // Draw the first tile to the left
        if (StageTiles.visualPlane[chunk] == (byte)aboveMidPoint) {
            lineRemain -= tilePxLineCnt;
            switch (StageTiles.direction[chunk]) {
                case FLIP_NONE:
                    gfxDataPtr = &TileGfx[TILE_SIZE * tileY + tileX16 + StageTiles.gfxDataPos[chunk]];
                    while (tilePxLineCnt--) {
                        if (*gfxDataPtr > 0)
                            *pixelBufferPtr = *gfxDataPtr;
                        pixelBufferPtr += SCREEN_XSIZE;
                        gfxDataPtr += TILE_SIZE;
                    }
                    break;

                case FLIP_X:
                    gfxDataPtr = &TileGfx[TILE_SIZE * tileY + tileOffsetXFlipX + StageTiles.gfxDataPos[chunk]];
                    while (tilePxLineCnt--) {
                        if (*gfxDataPtr > 0)
                            *pixelBufferPtr = *gfxDataPtr;
                        pixelBufferPtr += SCREEN_XSIZE;
                        gfxDataPtr += TILE_SIZE;
                    }
                    break;

                case FLIP_Y:
                    gfxDataPtr = &TileGfx[tileOffsetXFlipY + StageTiles.gfxDataPos[chunk] - TILE_SIZE * tileY];
                    while (tilePxLineCnt--) {
                        if (*gfxDataPtr > 0)
                            *pixelBufferPtr = *gfxDataPtr;
                        pixelBufferPtr += SCREEN_XSIZE;
                        gfxDataPtr -= TILE_SIZE;
                    }
                    break;

                case FLIP_XY:
                    gfxDataPtr = &TileGfx[tileOffsetXFlipXY + StageTiles.gfxDataPos[chunk] - TILE_SIZE * tileY];
                    while (tilePxLineCnt--) {
                        if (*gfxDataPtr > 0)
                            *pixelBufferPtr = *gfxDataPtr;
                        pixelBufferPtr += SCREEN_XSIZE;
                        gfxDataPtr -= TILE_SIZE;
                    }
                    break;

                default: break;
            }
        } else {
            pixelBufferPtr += SCREEN_XSIZE * tileYPxRemain;
            lineRemain -= tilePxLineCnt;
        }

        // Draw the bulk of the tiles
        int chunkTileY   = ((chunkY & 0x7F) >> 4) + 1;
        int tilesPerLine = (SCREEN_YSIZE >> 4) - 1;

        while (tilesPerLine--) {
            if (chunkTileY < 8) {
                chunk += 8;
            } else {
                if (++chunkYPos == layerheight)
                    chunkYPos = 0;

                chunkTileY = 0;
                chunk      = (layer->tiles[chunkX + (chunkYPos * TILELAYER_CHUNK_W)] << 6) + tileX;
            }
            lineRemain -= TILE_SIZE;

            // Loop Unrolling (faster but messier code)
            if (StageTiles.visualPlane[chunk] == (byte)aboveMidPoint) {
                switch (StageTiles.direction[chunk]) {
                    case FLIP_NONE:
                        gfxDataPtr = &TileGfx[StageTiles.gfxDataPos[chunk] + tileX16];
                        if (*gfxDataPtr > 0)
                            *pixelBufferPtr = *gfxDataPtr;
                        pixelBufferPtr += SCREEN_XSIZE;
                        gfxDataPtr += TILE_SIZE;

                        if (*gfxDataPtr > 0)
                            *pixelBufferPtr = *gfxDataPtr;
                        pixelBufferPtr += SCREEN_XSIZE;
                        gfxDataPtr += TILE_SIZE;

                        if (*gfxDataPtr > 0)
                            *pixelBufferPtr = *gfxDataPtr;
                        pixelBufferPtr += SCREEN_XSIZE;
                        gfxDataPtr += TILE_SIZE;

                        if (*gfxDataPtr > 0)
                            *pixelBufferPtr = *gfxDataPtr;
                        pixelBufferPtr += SCREEN_XSIZE;
                        gfxDataPtr += TILE_SIZE;

                        if (*gfxDataPtr > 0)
                            *pixelBufferPtr = *gfxDataPtr;
                        pixelBufferPtr += SCREEN_XSIZE;
                        gfxDataPtr += TILE_SIZE;

                        if (*gfxDataPtr > 0)
                            *pixelBufferPtr = *gfxDataPtr;
                        pixelBufferPtr += SCREEN_XSIZE;
                        gfxDataPtr += TILE_SIZE;

                        if (*gfxDataPtr > 0)
                            *pixelBufferPtr = *gfxDataPtr;
                        pixelBufferPtr += SCREEN_XSIZE;
                        gfxDataPtr += TILE_SIZE;

                        if (*gfxDataPtr > 0)
                            *pixelBufferPtr = *gfxDataPtr;
                        pixelBufferPtr += SCREEN_XSIZE;
                        gfxDataPtr += TILE_SIZE;

                        if (*gfxDataPtr > 0)
                            *pixelBufferPtr = *gfxDataPtr;
                        pixelBufferPtr += SCREEN_XSIZE;
                        gfxDataPtr += TILE_SIZE;

                        if (*gfxDataPtr > 0)
                            *pixelBufferPtr = *gfxDataPtr;
                        pixelBufferPtr += SCREEN_XSIZE;
                        gfxDataPtr += TILE_SIZE;

                        if (*gfxDataPtr > 0)
                            *pixelBufferPtr = *gfxDataPtr;
                        pixelBufferPtr += SCREEN_XSIZE;
                        gfxDataPtr += TILE_SIZE;

                        if (*gfxDataPtr > 0)
                            *pixelBufferPtr = *gfxDataPtr;
                        pixelBufferPtr += SCREEN_XSIZE;
                        gfxDataPtr += TILE_SIZE;

                        if (*gfxDataPtr > 0)
                            *pixelBufferPtr = *gfxDataPtr;
                        pixelBufferPtr += SCREEN_XSIZE;
                        gfxDataPtr += TILE_SIZE;

                        if (*gfxDataPtr > 0)
                            *pixelBufferPtr = *gfxDataPtr;
                        pixelBufferPtr += SCREEN_XSIZE;
                        gfxDataPtr += TILE_SIZE;

                        if (*gfxDataPtr > 0)
                            *pixelBufferPtr = *gfxDataPtr;
                        pixelBufferPtr += SCREEN_XSIZE;
                        gfxDataPtr += TILE_SIZE;

                        if (*gfxDataPtr > 0)
                            *pixelBufferPtr = *gfxDataPtr;
                        pixelBufferPtr += SCREEN_XSIZE;
                        break;

                    case FLIP_X:
                        gfxDataPtr = &TileGfx[StageTiles.gfxDataPos[chunk] + tileOffsetXFlipX];
                        if (*gfxDataPtr > 0)
                            *pixelBufferPtr = *gfxDataPtr;
                        pixelBufferPtr += SCREEN_XSIZE;
                        gfxDataPtr += TILE_SIZE;

                        if (*gfxDataPtr > 0)
                            *pixelBufferPtr = *gfxDataPtr;
                        pixelBufferPtr += SCREEN_XSIZE;
                        gfxDataPtr += TILE_SIZE;

                        if (*gfxDataPtr > 0)
                            *pixelBufferPtr = *gfxDataPtr;
                        pixelBufferPtr += SCREEN_XSIZE;
                        gfxDataPtr += TILE_SIZE;

                        if (*gfxDataPtr > 0)
                            *pixelBufferPtr = *gfxDataPtr;
                        pixelBufferPtr += SCREEN_XSIZE;
                        gfxDataPtr += TILE_SIZE;

                        if (*gfxDataPtr > 0)
                            *pixelBufferPtr = *gfxDataPtr;
                        pixelBufferPtr += SCREEN_XSIZE;
                        gfxDataPtr += TILE_SIZE;

                        if (*gfxDataPtr > 0)
                            *pixelBufferPtr = *gfxDataPtr;
                        pixelBufferPtr += SCREEN_XSIZE;
                        gfxDataPtr += TILE_SIZE;

                        if (*gfxDataPtr > 0)
                            *pixelBufferPtr = *gfxDataPtr;
                        pixelBufferPtr += SCREEN_XSIZE;
                        gfxDataPtr += TILE_SIZE;

                        if (*gfxDataPtr > 0)
                            *pixelBufferPtr = *gfxDataPtr;
                        pixelBufferPtr += SCREEN_XSIZE;
                        gfxDataPtr += TILE_SIZE;

                        if (*gfxDataPtr > 0)
                            *pixelBufferPtr = *gfxDataPtr;
                        pixelBufferPtr += SCREEN_XSIZE;
                        gfxDataPtr += TILE_SIZE;

                        if (*gfxDataPtr > 0)
                            *pixelBufferPtr = *gfxDataPtr;
                        pixelBufferPtr += SCREEN_XSIZE;
                        gfxDataPtr += TILE_SIZE;

                        if (*gfxDataPtr > 0)
                            *pixelBufferPtr = *gfxDataPtr;
                        pixelBufferPtr += SCREEN_XSIZE;
                        gfxDataPtr += TILE_SIZE;

                        if (*gfxDataPtr > 0)
                            *pixelBufferPtr = *gfxDataPtr;
                        pixelBufferPtr += SCREEN_XSIZE;
                        gfxDataPtr += TILE_SIZE;

                        if (*gfxDataPtr > 0)
                            *pixelBufferPtr = *gfxDataPtr;
                        pixelBufferPtr += SCREEN_XSIZE;
                        gfxDataPtr += TILE_SIZE;

                        if (*gfxDataPtr > 0)
                            *pixelBufferPtr = *gfxDataPtr;
                        pixelBufferPtr += SCREEN_XSIZE;
                        gfxDataPtr += TILE_SIZE;

                        if (*gfxDataPtr > 0)
                            *pixelBufferPtr = *gfxDataPtr;
                        pixelBufferPtr += SCREEN_XSIZE;
                        gfxDataPtr += TILE_SIZE;

                        if (*gfxDataPtr > 0)
                            *pixelBufferPtr = *gfxDataPtr;
                        pixelBufferPtr += SCREEN_XSIZE;
                        break;

                    case FLIP_Y:
                        gfxDataPtr = &TileGfx[StageTiles.gfxDataPos[chunk] + tileOffsetXFlipY];
                        if (*gfxDataPtr > 0)
                            *pixelBufferPtr = *gfxDataPtr;
                        pixelBufferPtr += SCREEN_XSIZE;
                        gfxDataPtr -= TILE_SIZE;

                        if (*gfxDataPtr > 0)
                            *pixelBufferPtr = *gfxDataPtr;
                        pixelBufferPtr += SCREEN_XSIZE;
                        gfxDataPtr -= TILE_SIZE;

                        if (*gfxDataPtr > 0)
                            *pixelBufferPtr = *gfxDataPtr;
                        pixelBufferPtr += SCREEN_XSIZE;
                        gfxDataPtr -= TILE_SIZE;

                        if (*gfxDataPtr > 0)
                            *pixelBufferPtr = *gfxDataPtr;
                        pixelBufferPtr += SCREEN_XSIZE;
                        gfxDataPtr -= TILE_SIZE;

                        if (*gfxDataPtr > 0)
                            *pixelBufferPtr = *gfxDataPtr;
                        pixelBufferPtr += SCREEN_XSIZE;
                        gfxDataPtr -= TILE_SIZE;

                        if (*gfxDataPtr > 0)
                            *pixelBufferPtr = *gfxDataPtr;
                        pixelBufferPtr += SCREEN_XSIZE;
                        gfxDataPtr -= TILE_SIZE;

                        if (*gfxDataPtr > 0)
                            *pixelBufferPtr = *gfxDataPtr;
                        pixelBufferPtr += SCREEN_XSIZE;
                        gfxDataPtr -= TILE_SIZE;

                        if (*gfxDataPtr > 0)
                            *pixelBufferPtr = *gfxDataPtr;
                        pixelBufferPtr += SCREEN_XSIZE;
                        gfxDataPtr -= TILE_SIZE;

                        if (*gfxDataPtr > 0)
                            *pixelBufferPtr = *gfxDataPtr;
                        pixelBufferPtr += SCREEN_XSIZE;
                        gfxDataPtr -= TILE_SIZE;

                        if (*gfxDataPtr > 0)
                            *pixelBufferPtr = *gfxDataPtr;
                        pixelBufferPtr += SCREEN_XSIZE;
                        gfxDataPtr -= TILE_SIZE;

                        if (*gfxDataPtr > 0)
                            *pixelBufferPtr = *gfxDataPtr;
                        pixelBufferPtr += SCREEN_XSIZE;
                        gfxDataPtr -= TILE_SIZE;

                        if (*gfxDataPtr > 0)
                            *pixelBufferPtr = *gfxDataPtr;
                        pixelBufferPtr += SCREEN_XSIZE;
                        gfxDataPtr -= TILE_SIZE;

                        if (*gfxDataPtr > 0)
                            *pixelBufferPtr = *gfxDataPtr;
                        pixelBufferPtr += SCREEN_XSIZE;
                        gfxDataPtr -= TILE_SIZE;

                        if (*gfxDataPtr > 0)
                            *pixelBufferPtr = *gfxDataPtr;
                        pixelBufferPtr += SCREEN_XSIZE;
                        gfxDataPtr -= TILE_SIZE;

                        if (*gfxDataPtr > 0)
                            *pixelBufferPtr = *gfxDataPtr;
                        pixelBufferPtr += SCREEN_XSIZE;
                        gfxDataPtr -= TILE_SIZE;

                        if (*gfxDataPtr > 0)
                            *pixelBufferPtr = *gfxDataPtr;
                        pixelBufferPtr += SCREEN_XSIZE;
                        break;

                    case FLIP_XY:
                        gfxDataPtr = &TileGfx[StageTiles.gfxDataPos[chunk] + tileOffsetXFlipXY];
                        if (*gfxDataPtr > 0)
                            *pixelBufferPtr = *gfxDataPtr;
                        pixelBufferPtr += SCREEN_XSIZE;
                        gfxDataPtr -= TILE_SIZE;

                        if (*gfxDataPtr > 0)
                            *pixelBufferPtr = *gfxDataPtr;
                        pixelBufferPtr += SCREEN_XSIZE;
                        gfxDataPtr -= TILE_SIZE;

                        if (*gfxDataPtr > 0)
                            *pixelBufferPtr = *gfxDataPtr;
                        pixelBufferPtr += SCREEN_XSIZE;
                        gfxDataPtr -= TILE_SIZE;

                        if (*gfxDataPtr > 0)
                            *pixelBufferPtr = *gfxDataPtr;
                        pixelBufferPtr += SCREEN_XSIZE;
                        gfxDataPtr -= TILE_SIZE;

                        if (*gfxDataPtr > 0)
                            *pixelBufferPtr = *gfxDataPtr;
                        pixelBufferPtr += SCREEN_XSIZE;
                        gfxDataPtr -= TILE_SIZE;

                        if (*gfxDataPtr > 0)
                            *pixelBufferPtr = *gfxDataPtr;
                        pixelBufferPtr += SCREEN_XSIZE;
                        gfxDataPtr -= TILE_SIZE;

                        if (*gfxDataPtr > 0)
                            *pixelBufferPtr = *gfxDataPtr;
                        pixelBufferPtr += SCREEN_XSIZE;
                        gfxDataPtr -= TILE_SIZE;

                        if (*gfxDataPtr > 0)
                            *pixelBufferPtr = *gfxDataPtr;
                        pixelBufferPtr += SCREEN_XSIZE;
                        gfxDataPtr -= TILE_SIZE;

                        if (*gfxDataPtr > 0)
                            *pixelBufferPtr = *gfxDataPtr;
                        pixelBufferPtr += SCREEN_XSIZE;
                        gfxDataPtr -= TILE_SIZE;

                        if (*gfxDataPtr > 0)
                            *pixelBufferPtr = *gfxDataPtr;
                        pixelBufferPtr += SCREEN_XSIZE;
                        gfxDataPtr -= TILE_SIZE;

                        if (*gfxDataPtr > 0)
                            *pixelBufferPtr = *gfxDataPtr;
                        pixelBufferPtr += SCREEN_XSIZE;
                        gfxDataPtr -= TILE_SIZE;

                        if (*gfxDataPtr > 0)
                            *pixelBufferPtr = *gfxDataPtr;
                        pixelBufferPtr += SCREEN_XSIZE;
                        gfxDataPtr -= TILE_SIZE;

                        if (*gfxDataPtr > 0)
                            *pixelBufferPtr = *gfxDataPtr;
                        pixelBufferPtr += SCREEN_XSIZE;
                        gfxDataPtr -= TILE_SIZE;

                        if (*gfxDataPtr > 0)
                            *pixelBufferPtr = *gfxDataPtr;
                        pixelBufferPtr += SCREEN_XSIZE;
                        gfxDataPtr -= TILE_SIZE;

                        if (*gfxDataPtr > 0)
                            *pixelBufferPtr = *gfxDataPtr;
                        pixelBufferPtr += SCREEN_XSIZE;
                        gfxDataPtr -= TILE_SIZE;

                        if (*gfxDataPtr > 0)
                            *pixelBufferPtr = *gfxDataPtr;
                        pixelBufferPtr += SCREEN_XSIZE;
                        break;
                }
            } else {
                pixelBufferPtr += SCREEN_XSIZE * TILE_SIZE;
            }
            ++chunkTileY;
        }

        // Draw any remaining tiles
        while (lineRemain > 0) {
            if (chunkTileY < 8) {
                chunk += 8;
            } else {
                if (++chunkYPos == layerheight)
                    chunkYPos = 0;

                chunkTileY = 0;
                chunk      = (layer->tiles[chunkX + (chunkYPos * TILELAYER_CHUNK_W)] << 6) + tileX;
            }

            tilePxLineCnt = lineRemain >= TILE_SIZE ? TILE_SIZE : lineRemain;
            lineRemain -= tilePxLineCnt;

            if (StageTiles.visualPlane[chunk] == (byte)aboveMidPoint) {
                switch (StageTiles.direction[chunk]) {
                    case FLIP_NONE:
                        gfxDataPtr = &TileGfx[StageTiles.gfxDataPos[chunk] + tileX16];
                        while (tilePxLineCnt--) {
                            if (*gfxDataPtr > 0)
                                *pixelBufferPtr = *gfxDataPtr;
                            pixelBufferPtr += SCREEN_XSIZE;
                            gfxDataPtr += TILE_SIZE;
                        }
                        break;

                    case FLIP_X:
                        gfxDataPtr = &TileGfx[StageTiles.gfxDataPos[chunk] + tileOffsetXFlipX];
                        while (tilePxLineCnt--) {
                            if (*gfxDataPtr > 0)
                                *pixelBufferPtr = *gfxDataPtr;
                            pixelBufferPtr += SCREEN_XSIZE;
                            gfxDataPtr += TILE_SIZE;
                        }
                        break;

                    case FLIP_Y:
                        gfxDataPtr = &TileGfx[StageTiles.gfxDataPos[chunk] + tileOffsetXFlipY];
                        while (tilePxLineCnt--) {
                            if (*gfxDataPtr > 0)
                                *pixelBufferPtr = *gfxDataPtr;
                            pixelBufferPtr += SCREEN_XSIZE;
                            gfxDataPtr -= TILE_SIZE;
                        }
                        break;

                    case FLIP_XY:
                        gfxDataPtr = &TileGfx[StageTiles.gfxDataPos[chunk] + tileOffsetXFlipXY];
                        while (tilePxLineCnt--) {
                            if (*gfxDataPtr > 0)
                                *pixelBufferPtr = *gfxDataPtr;
                            pixelBufferPtr += SCREEN_XSIZE;
                            gfxDataPtr -= TILE_SIZE;
                        }
                        break;

                    default: break;
                }
            } else {
                pixelBufferPtr += SCREEN_XSIZE * tilePxLineCnt;
            }
            chunkTileY++;
        }

        if (++tileX16 >= TILE_SIZE) {
            tileX16 = 0;
            ++tileX;
        }

        if (tileX >= 8) {
            if (++chunkX == layerwidth) {
                chunkX = 0;
                scrollIndex -= 0x80 * layerwidth;
            }
            tileX = 0;
        }

        pixelBufferPtr -= (SCREEN_XSIZE * SCREEN_YSIZE) - 1;
    }
}
void Draw3DCloudLayer(int layerID) { TileLayer *layer = &StageLayouts[activeTileLayers[layerID]]; }

void DrawTintRect(int XPos, int YPos, int width, int height, byte tintID) {
    if (width + XPos > SCREEN_XSIZE)
        width = SCREEN_XSIZE - XPos;
    if (XPos < 0) {
        width += XPos;
        XPos = 0;
    }

    if (height + YPos > SCREEN_YSIZE)
        height = SCREEN_YSIZE - YPos;
    if (YPos < 0) {
        height += YPos;
        YPos = 0;
    }
    if (width < 0 || height < 0)
        return;
#if RETRO_PLATFORM == RETRO_PS1
    if (g_ps1GPU)
        PS1TintRect(XPos, YPos, width, height, tintID);
    return;
#endif

    byte *tintTable = NULL;
    switch (tintID) {
        case 0: tintTable = TintLookupTable1; break;
        case 1: tintTable = TintLookupTable2; break;
        case 2: tintTable = TintLookupTable3; break;
        case 3: tintTable = TintLookupTable4; break;
        default: break;
    }

    int yOffset = SCREEN_XSIZE - width;
    for (byte *pixelBufferPtr = &Engine.FrameBuffer[XPos + SCREEN_XSIZE * YPos];; pixelBufferPtr += yOffset) {
        height--;
        if (height < 0)
            break;
        int w = width;
        while (w--) {
            *pixelBufferPtr = tintTable[*pixelBufferPtr];
            ++pixelBufferPtr;
        }
    }
}
void DrawScaledTintMask(int direction, int XPos, int YPos, int pivotX, int pivotY, int scaleX, int scaleY, int width, int height, int sprX, int sprY,
                        int tintID, int sheetID) {
#if RETRO_PLATFORM == RETRO_PS1
    // Part of a tint sequence (title card colour circle): its texels stay tinted inside the
    // untinted window (PS1FinishTint). On its own it is not modelled.
    if (s_tintSeq == 0 && g_ps1GPU && s_palReserved) // a mask on its own (window larger than the screen)
        PS1TintBegin(tintID);
    if (s_tintSeq == 1 && !s_tintMaskPending) {
        s_tintMask        = PS1TintMaskRec{direction, XPos, YPos, pivotX, pivotY, scaleX, scaleY, width, height, sprX, sprY, sheetID};
        s_tintMaskPending = true;
    } else {
        g_ps1TintIgnored = g_ps1TintIgnored + 1;
    }
    return;
#endif
    int roundedYPos = 0;
    int roundedXPos = 0;
    int truescaleX  = 4 * scaleX;
    int truescaleY  = 4 * scaleY;
    int widthM1     = width - 1;
    int trueXPos    = XPos - (truescaleX * pivotX >> 11);
    width           = truescaleX * width >> 11;
    int trueYPos    = YPos - (truescaleY * pivotY >> 11);
    height          = truescaleY * height >> 11;
    int finalscaleX = (signed int)(float)((float)(2048.0 / (float)truescaleX) * 2048.0);
    int finalscaleY = (signed int)(float)((float)(2048.0 / (float)truescaleY) * 2048.0);
    if (width + trueXPos > SCREEN_XSIZE) {
        width = SCREEN_XSIZE - trueXPos;
    }

    if (direction) {
        if (trueXPos < 0) {
            widthM1 -= trueXPos * -finalscaleX >> 11;
            roundedXPos = (ushort)trueXPos * -(short)finalscaleX & 0x7FF;
            width += trueXPos;
            trueXPos = 0;
        }
    } else if (trueXPos < 0) {
        sprX += trueXPos * -finalscaleX >> 11;
        roundedXPos = (ushort)trueXPos * -(short)finalscaleX & 0x7FF;
        width += trueXPos;
        trueXPos = 0;
    }

    if (height + trueYPos > SCREEN_YSIZE) {
        height = SCREEN_YSIZE - trueYPos;
    }
    if (trueYPos < 0) {
        sprY += trueYPos * -finalscaleY >> 11;
        roundedYPos = (ushort)trueYPos * -(short)finalscaleY & 0x7FF;
        height += trueYPos;
        trueYPos = 0;
    }

    if (width <= 0 || height <= 0)
        return;

    byte *tintTable = NULL;
    switch (tintID) {
        case 0: tintTable = TintLookupTable1; break;
        case 1: tintTable = TintLookupTable2; break;
        case 2: tintTable = TintLookupTable3; break;
        case 3: tintTable = TintLookupTable4; break;
        default: break;
    }

    GFXSurface *surface = &GfxSurface[sheetID];
    int pitch           = SCREEN_XSIZE - width;
    int gfxwidth        = surface->width;
    // byte *lineBuffer       = &gfxLineBuffer[trueYPos];
    byte *gfxData        = &GraphicData[sprX + surface->width * sprY + surface->dataPosition];
    byte *pixelBufferPtr = &Engine.FrameBuffer[trueXPos + SCREEN_XSIZE * trueYPos];
    if (direction == FLIP_X) {
        byte *gfxDataPtr = &gfxData[widthM1];
        int gfxPitch     = 0;
        while (height--) {
            int roundXPos = roundedXPos;
            int w         = width;
            while (w--) {
                if (*gfxDataPtr > 0)
                    *pixelBufferPtr = tintTable[*pixelBufferPtr];
                int offsetX = finalscaleX + roundXPos;
                gfxDataPtr -= offsetX >> 11;
                gfxPitch += offsetX >> 11;
                roundXPos = offsetX & 0x7FF;
                ++pixelBufferPtr;
            }
            pixelBufferPtr += pitch;
            int offsetY = finalscaleY + roundedYPos;
            gfxDataPtr += gfxPitch + (offsetY >> 11) * gfxwidth;
            roundedYPos = offsetY & 0x7FF;
            gfxPitch    = 0;
        }
    } else {
        int gfxPitch = 0;
        int h        = height;
        while (h--) {
            int roundXPos = roundedXPos;
            int w         = width;
            while (w--) {
                if (*gfxData > 0)
                    *pixelBufferPtr = tintTable[*pixelBufferPtr];
                int offsetX = finalscaleX + roundXPos;
                gfxData += offsetX >> 11;
                gfxPitch += offsetX >> 11;
                roundXPos = offsetX & 0x7FF;
                ++pixelBufferPtr;
            }
            pixelBufferPtr += pitch;
            int offsetY = finalscaleY + roundedYPos;
            gfxData += (offsetY >> 11) * gfxwidth - gfxPitch;
            roundedYPos = offsetY & 0x7FF;
            gfxPitch    = 0;
        }
    }
}

void DrawSprite(int XPos, int YPos, int width, int height, int sprX, int sprY, int sheetID) {
    if (width + XPos > SCREEN_XSIZE)
        width = SCREEN_XSIZE - XPos;
    if (XPos < 0) {
        sprX -= XPos;
        width += XPos;
        XPos = 0;
    }
    if (height + YPos > SCREEN_YSIZE)
        height = SCREEN_YSIZE - YPos;
    if (YPos < 0) {
        sprY -= YPos;
        height += YPos;
        YPos = 0;
    }
    if (width <= 0 || height <= 0)
        return;

#if RETRO_PLATFORM == RETRO_PS1
    if (g_ps1GPU) {
        GFXSurface *surface = &GfxSurface[sheetID];
        if (surface->vramDirect15) {
            // 15-bit surface (FMV): one texture page, texels are colours (no CLUT).
            psyqo::PrimPieces::TPageAttr attr;
            attr.set(psyqo::Prim::TPageAttr::ColorMode::Tex16Bits);
            attr.setPageX((uint8_t)surface->vramPageX);
            attr.setPageY((uint8_t)surface->vramPageY);
            attr.set(psyqo::Prim::TPageAttr::SemiTrans::HalfBackAndHalfFront);
            PS1ChainTPage(attr);
            uint8_t m = PS1DirectModulation();
            psyqo::Prim::Sprite s(psyqo::Color{{.r = m, .g = m, .b = m}});
            s.position = psyqo::Vertex{{.x = (int16_t)XPos, .y = (int16_t)YPos}};
            s.size     = psyqo::Vertex{{.w = (int16_t)width, .h = (int16_t)height}};
            s.texInfo  = psyqo::PrimPieces::TexInfo{(uint8_t)sprX, (uint8_t)sprY, psyqo::PrimPieces::ClutIndex()};
            PS1ChainSprite(s);
            PS1DirectFadeOverlay(XPos, YPos, width, height);
            return;
        }
        if (surface->atlasSheet >= 0) {
            PS1DrawAtlasRect(surface->atlasSheet, XPos, YPos, width, height, sprX, sprY);
            return;
        }
    }
#endif

#if RETRO_PLATFORM == RETRO_PS1
    return; // PS1: software rasteriser output is never displayed; GPU path TODO (phase 3c)
#endif
    GFXSurface *surface  = &GfxSurface[sheetID];
    int pitch            = SCREEN_XSIZE - width;
    int gfxPitch         = surface->width - width;
    byte *gfxDataPtr     = &GraphicData[sprX + surface->width * sprY + surface->dataPosition];
    byte *pixelBufferPtr = &Engine.FrameBuffer[XPos + SCREEN_XSIZE * YPos];
    while (height--) {
        int w = width;
        while (w--) {
            if (*gfxDataPtr > 0)
                *pixelBufferPtr = *gfxDataPtr;
            ++gfxDataPtr;
            ++pixelBufferPtr;
        }
        pixelBufferPtr += pitch;
        gfxDataPtr += gfxPitch;
    }
}

void DrawSpriteNoKey(int XPos, int YPos, int width, int height, int sprX, int sprY, int sheetID) {
#if RETRO_PLATFORM == RETRO_PS1
    return; // PS1: software rasteriser output is never displayed; GPU path TODO (phase 3c)
#endif
    if (width + XPos > SCREEN_XSIZE)
        width = SCREEN_XSIZE - XPos;
    if (XPos < 0) {
        sprX -= XPos;
        width += XPos;
        XPos = 0;
    }
    if (height + YPos > SCREEN_YSIZE)
        height = SCREEN_YSIZE - YPos;
    if (YPos < 0) {
        sprY -= YPos;
        height += YPos;
        YPos = 0;
    }
    if (width <= 0 || height <= 0)
        return;

    GFXSurface *surface  = &GfxSurface[sheetID];
    int pitch            = SCREEN_XSIZE - width;
    int gfxPitch         = surface->width - width;
    byte *gfxDataPtr     = &GraphicData[sprX + surface->width * sprY + surface->dataPosition];
    byte *pixelBufferPtr = &Engine.FrameBuffer[XPos + SCREEN_XSIZE * YPos];
    while (height--) {
        int w = width;
        while (w--) {
            *pixelBufferPtr = *gfxDataPtr;
            ++gfxDataPtr;
            ++pixelBufferPtr;
        }
        pixelBufferPtr += pitch;
        gfxDataPtr += gfxPitch;
    }
}

void DrawSpriteClipped(int XPos, int YPos, int width, int height, int sprX, int sprY, int sheetID, int clipY) {
#if RETRO_PLATFORM == RETRO_PS1
    return; // PS1: software rasteriser output is never displayed; GPU path TODO (phase 3c)
#endif
    if (width + XPos > SCREEN_XSIZE)
        width = SCREEN_XSIZE - XPos;
    if (XPos < 0) {
        sprX -= XPos;
        width += XPos;
        XPos = 0;
    }
    if (height + YPos > clipY)
        height = clipY - YPos;
    if (YPos < 0) {
        sprY -= YPos;
        height += YPos;
        YPos = 0;
    }
    if (width <= 0 || height <= 0)
        return;

    GFXSurface *surface  = &GfxSurface[sheetID];
    int pitch            = SCREEN_XSIZE - width;
    int gfxPitch         = surface->width - width;
    byte *gfxDataPtr     = &GraphicData[sprX + surface->width * sprY + surface->dataPosition];
    byte *pixelBufferPtr = &Engine.FrameBuffer[XPos + SCREEN_XSIZE * YPos];
    while (height--) {
        int w = width;
        while (w--) {
            if (*gfxDataPtr > 0)
                *pixelBufferPtr = *gfxDataPtr;
            ++gfxDataPtr;
            ++pixelBufferPtr;
        }
        pixelBufferPtr += pitch;
        gfxDataPtr += gfxPitch;
    }
}

// One piece of a scaled frame: frame texels [fx0, fx1) x [fy0, fy1), frame texel f at u = uBase + f,
// v = vBase + f in its texture page. The frame covers screen (x0, y0, w, h); [vx0, vx1) x [vy0, vy1)
// is its visible texel range in screen order. The quad is clipped at texel boundaries (so its
// vertices stay near the screen: the GPU refuses polygons wider than 1023 or taller than 511) and
// its UVs stay exact integers.
static void PS1ScaledPiece(GFXSurface *surf, const PS1AtlasCluster *cl, int texPage, int texPageY, int uBase, int vBase, int direction,
                           int x0, int y0, int w, int h, int width, int height, int vx0, int vx1, int vy0, int vy1, int fx0, int fx1, int fy0,
                           int fy1) {
    int sa = direction == FLIP_X ? width - fx1 : fx0, sb = direction == FLIP_X ? width - fx0 : fx1; // screen order
    int a = sa > vx0 ? sa : vx0, b = sb < vx1 ? sb : vx1;
    int ta = fy0 > vy0 ? fy0 : vy0, tb = fy1 < vy1 ? fy1 : vy1;
    if (a >= b || ta >= tb)
        return;
    if (s_ps1QuadMaskMode && (!cl || cl->depth != 4)) { // mask CLUT exists for 4-bit clusters only
        g_ps1TintMaskSkip = g_ps1TintMaskSkip + 1;
        return;
    }
    int qx0 = x0 + a * w / width, qx1 = x0 + b * w / width;
    int qy0 = y0 + ta * h / height, qy1 = y0 + tb * h / height;
    if (qx1 <= qx0 || qy1 <= qy0)
        return;
    int uL, uR; // FLIP_X mirrors the texels: screen-left shows the sprite's right edge
    const bool unit = w == width && h == height;
    if (direction == FLIP_X) {
        // 1:1: screen column k shows texel u + w - 1 - k as PC does (DrawScaledSprite from widthM1 down; NX-011): the GPU
        // samples a quad's texels at their top-left point, so the UV edges are pulled back one texel (u + w - 1 .. u - 1);
        // the far edge u + w would be the atlas's next column. A frame at u 0 can't (UV -1): the old edges stay.
        int lo = uBase + width - b; // the lowest texel drawn
        if (unit && !surf->vramDirect15 && !s_ps1QuadMaskMode && PS1FlipRectsLive()) {
            int tu, tv;
            if (PS1FlipRectTex(lo, vBase + ta, b - a, tb - ta, true, false, &tu, &tv)) { // flip rectangle (docs/43 G3)
                psyqo::PrimPieces::TPageAttr attr = PS1ClusterTPage(cl);
                uint16_t raw;
                memcpy(&raw, &attr, 2);
                raw = (uint16_t)(raw | 0x1000); // GP0(E1h) bit 12: X flip
                memcpy(&attr, &raw, 2);
                PS1ChainTPage(attr);
                psyqo::Prim::Sprite s(psyqo::Color{{.r = 0x80, .g = 0x80, .b = 0x80}});
                s.position = psyqo::Vertex{{.x = (int16_t)qx0, .y = (int16_t)qy0}};
                s.size     = psyqo::Vertex{{.x = (int16_t)(b - a), .y = (int16_t)(tb - ta)}};
                s.texInfo  = psyqo::PrimPieces::TexInfo{(uint8_t)tu, (uint8_t)tv, PS1ClusterClut(cl)};
                PS1ChainSprite(s);
                g_ps1SpriteFlipRects = g_ps1SpriteFlipRects + 1;
                g_ps1FlipFrameUsed   = true;
                return;
            }
            g_ps1SpriteFlipQuads = g_ps1SpriteFlipQuads + 1;
        }
        int pull = unit && lo >= 1 ? 1 : 0;
        uL = uBase + width - a - pull;
        uR = uBase + width - b - pull;
    } else {
        uL = uBase + a;
        uR = uBase + b;
    }
    auto clampU = [](int u) { return (uint8_t)(u > 255 ? 255 : u); }; // 256 doesn't fit a UV byte
    int vT = vBase + ta, vB = vBase + tb;
    uint8_t m = surf->vramDirect15 ? PS1DirectModulation() : 0x80;
    psyqo::Prim::TexturedQuad quad(psyqo::Color{{.r = m, .g = m, .b = m}});
    quad.pointA    = psyqo::Vertex{{.x = (int16_t)qx0, .y = (int16_t)qy0}};
    quad.uvA       = psyqo::PrimPieces::UVCoords{clampU(uL), clampU(vT)};
    quad.clutIndex = cl ? PS1ClusterClut(cl) : psyqo::PrimPieces::ClutIndex();
    quad.pointB    = psyqo::Vertex{{.x = (int16_t)qx1, .y = (int16_t)qy0}};
    quad.uvB       = psyqo::PrimPieces::UVCoords{clampU(uR), clampU(vT)};
    quad.tpage.setPageX((uint8_t)texPage);
    quad.tpage.setPageY((uint8_t)texPageY);
    quad.tpage.set(surf->vramDirect15 ? psyqo::Prim::TPageAttr::ColorMode::Tex16Bits
                   : cl->depth == 4   ? psyqo::Prim::TPageAttr::ColorMode::Tex4Bits
                                      : psyqo::Prim::TPageAttr::ColorMode::Tex8Bits);
    quad.pointC = psyqo::Vertex{{.x = (int16_t)qx0, .y = (int16_t)qy1}};
    quad.uvC    = psyqo::PrimPieces::UVCoordsPadded{clampU(uL), clampU(vB), 0};
    quad.pointD = psyqo::Vertex{{.x = (int16_t)qx1, .y = (int16_t)qy1}};
    quad.uvD    = psyqo::PrimPieces::UVCoordsPadded{clampU(uR), clampU(vB), 0};
    if (s_ps1QuadMaskMode) {
        // Mask-bit marking (PS1FinishTint): every opaque texel -> 0x8000 (STP set) drawn B + F =
        // unchanged colour, with GP0 E6h "force mask bit" on (set by the caller).
        quad.clutIndex = psyqo::PrimPieces::ClutIndex(psyqo::Vertex{{.x = PS1_MASK_CLUT_X, .y = PS1_BLEND_MASTER_Y}});
        quad.tpage.set(psyqo::Prim::TPageAttr::SemiTrans::FullBackAndFullFront);
        quad.setSemiTrans();
    }
    PS1ChainQuad(quad);
    if (surf->vramDirect15) {
        int ox0 = qx0 < 0 ? 0 : qx0, oy0 = qy0 < 0 ? 0 : qy0;
        int ox1 = qx1 > SCREEN_XSIZE ? SCREEN_XSIZE : qx1, oy1 = qy1 > SCREEN_YSIZE ? SCREEN_YSIZE : qy1;
        PS1DirectFadeOverlay(ox0, oy0, ox1 - ox0, oy1 - oy0);
    }
}

void DrawScaledSprite(int direction, int XPos, int YPos, int pivotX, int pivotY, int scaleX, int scaleY, int width, int height, int sprX, int sprY,
                      int sheetID) {
#if RETRO_PLATFORM == RETRO_PS1
    // GPU path: textured quads, one per atlas cluster the frame spans (one for a 15-bit
    // surface). Same placement maths as the software version below (512 = 1.0).
    if (!g_ps1GPU)
        return;
    GFXSurface *surf = &GfxSurface[sheetID];
    if (width <= 0 || height <= 0)
        return;
    if (!surf->vramDirect15 && surf->atlasSheet < 0)
        return;
    int sx = 4 * scaleX, sy = 4 * scaleY;
    if (sx <= 0 || sy <= 0)
        return;
    int x0 = XPos - (sx * pivotX >> 11), y0 = YPos - (sy * pivotY >> 11);
    int w = sx * width >> 11, h = sy * height >> 11;
    if (w <= 0 || h <= 0 || x0 >= SCREEN_XSIZE || y0 >= SCREEN_YSIZE || x0 + w <= 0 || y0 + h <= 0)
        return;
    // Visible texel range [t0, t1) per axis (screen order); screen edge of texel t: x0 + t * w / width.
    int tx0 = x0 < 0 ? (-x0 * width) / w : 0;
    int tx1 = x0 + w > SCREEN_XSIZE ? ((SCREEN_XSIZE - x0) * width + w - 1) / w : width;
    int ty0 = y0 < 0 ? (-y0 * height) / h : 0;
    int ty1 = y0 + h > SCREEN_YSIZE ? ((SCREEN_YSIZE - y0) * height + h - 1) / h : height;
    if (tx1 > width) tx1 = width;
    if (ty1 > height) ty1 = height;
    if (s_ps1QuadMaskMode) {
        static const uint16_t s_maskClut[16] = { 0x0000, 0x8000, 0x8000, 0x8000, 0x8000, 0x8000, 0x8000, 0x8000,
                                                 0x8000, 0x8000, 0x8000, 0x8000, 0x8000, 0x8000, 0x8000, 0x8000 };
        PS1ChainUploadRow(s_maskClut, 16, PS1_MASK_CLUT_X, PS1_BLEND_MASTER_Y);
        PS1ChainFlushCache();
    }
    if (surf->vramDirect15) {
        PS1ScaledPiece(surf, nullptr, surf->vramPageX, surf->vramPageY, sprX, sprY, direction, x0, y0, w, h, width, height, tx0, tx1, ty0, ty1, 0,
                       width, 0, height);
        return;
    }
    // Frames split across atlas clusters: one quad per cluster piece (PS1DrawAtlasRect's walk).
    struct Rect16 {
        int16_t x, y, w, h;
    } stack[16];
    int sp      = 0;
    stack[sp++] = Rect16{(int16_t)sprX, (int16_t)sprY, (int16_t)width, (int16_t)height};
    while (sp) {
        Rect16 r                 = stack[--sp];
        const PS1AtlasCluster *c = PS1FindCluster(surf->atlasSheet, r.x, r.y);
        if (!c)
            continue;
        int iw = c->srcX + c->w - r.x, ih = c->srcY + c->h - r.y;
        if (iw > r.w) iw = r.w;
        if (ih > r.h) ih = r.h;
        int fx0 = r.x - sprX, fy0 = r.y - sprY;
        PS1ScaledPiece(surf, c, c->tpageX, c->tpageY, c->u + (r.x - c->srcX) - fx0, c->v + (r.y - c->srcY) - fy0, direction, x0, y0, w, h, width,
                       height, tx0, tx1, ty0, ty1, fx0, fx0 + iw, fy0, fy0 + ih);
        if (r.w > iw && sp < 16)
            stack[sp++] = Rect16{(int16_t)(r.x + iw), r.y, (int16_t)(r.w - iw), (int16_t)ih};
        if (r.h > ih && sp < 16)
            stack[sp++] = Rect16{r.x, (int16_t)(r.y + ih), r.w, (int16_t)(r.h - ih)};
    }
    return;
#endif
    int roundedYPos = 0;
    int roundedXPos = 0;
    int truescaleX  = 4 * scaleX;
    int truescaleY  = 4 * scaleY;
    int widthM1     = width - 1;
    int trueXPos    = XPos - (truescaleX * pivotX >> 11);
    width           = truescaleX * width >> 11;
    int trueYPos    = YPos - (truescaleY * pivotY >> 11);
    height          = truescaleY * height >> 11;
    int finalscaleX = (signed int)(float)((float)(2048.0 / (float)truescaleX) * 2048.0);
    int finalscaleY = (signed int)(float)((float)(2048.0 / (float)truescaleY) * 2048.0);
    if (width + trueXPos > SCREEN_XSIZE) {
        width = SCREEN_XSIZE - trueXPos;
    }

    if (direction) {
        if (trueXPos < 0) {
            widthM1 -= trueXPos * -finalscaleX >> 11;
            roundedXPos = (ushort)trueXPos * -(short)finalscaleX & 0x7FF;
            width += trueXPos;
            trueXPos = 0;
        }
    } else if (trueXPos < 0) {
        sprX += trueXPos * -finalscaleX >> 11;
        roundedXPos = (ushort)trueXPos * -(short)finalscaleX & 0x7FF;
        width += trueXPos;
        trueXPos = 0;
    }

    if (height + trueYPos > SCREEN_YSIZE) {
        height = SCREEN_YSIZE - trueYPos;
    }
    if (trueYPos < 0) {
        sprY += trueYPos * -finalscaleY >> 11;
        roundedYPos = (ushort)trueYPos * -(short)finalscaleY & 0x7FF;
        height += trueYPos;
        trueYPos = 0;
    }

    if (width <= 0 || height <= 0)
        return;

    GFXSurface *surface  = &GfxSurface[sheetID];
    int pitch            = SCREEN_XSIZE - width;
    int gfxwidth         = surface->width;
    byte *gfxData        = &GraphicData[sprX + surface->width * sprY + surface->dataPosition];
    byte *pixelBufferPtr = &Engine.FrameBuffer[trueXPos + SCREEN_XSIZE * trueYPos];
    if (direction == FLIP_X) {
        byte *gfxDataPtr = &gfxData[widthM1];
        int gfxPitch     = 0;
        while (height--) {
            int roundXPos = roundedXPos;
            int w         = width;
            while (w--) {
                if (*gfxDataPtr > 0)
                    *pixelBufferPtr = *gfxDataPtr;
                int offsetX = finalscaleX + roundXPos;
                gfxDataPtr -= offsetX >> 11;
                gfxPitch += offsetX >> 11;
                roundXPos = offsetX & 0x7FF;
                ++pixelBufferPtr;
            }
            pixelBufferPtr += pitch;
            int offsetY = finalscaleY + roundedYPos;
            gfxDataPtr += gfxPitch + (offsetY >> 11) * gfxwidth;
            roundedYPos = offsetY & 0x7FF;
            gfxPitch    = 0;
        }
    } else {
        int gfxPitch = 0;
        int h        = height;
        while (h--) {
            int roundXPos = roundedXPos;
            int w         = width;
            while (w--) {
                if (*gfxData > 0)
                    *pixelBufferPtr = *gfxData;
                int offsetX = finalscaleX + roundXPos;
                gfxData += offsetX >> 11;
                gfxPitch += offsetX >> 11;
                roundXPos = offsetX & 0x7FF;
                ++pixelBufferPtr;
            }
            pixelBufferPtr += pitch;
            int offsetY = finalscaleY + roundedYPos;
            gfxData += (offsetY >> 11) * gfxwidth - gfxPitch;
            roundedYPos = offsetY & 0x7FF;
            gfxPitch    = 0;
        }
    }
}

void DrawRotatedSprite(int direction, int XPos, int YPos, int pivotX, int pivotY, int sprX, int sprY, int width, int height, int rotation,
                       int sheetID) {
#if RETRO_PLATFORM == RETRO_PS1
    if (g_ps1GPU && rotation == 0 && direction == FLIP_NONE) {
        // rotation 0 + no flip == a plain sprite; the PS1 path draws textured quads
        // directly (DrawSprite), while the software blit below targets FrameBuffer
        // which the PS1 renderer never displays.
        DrawSprite(XPos - pivotX, YPos - pivotY, width, height, sprX, sprY, sheetID);
        return;
    }
    if (g_ps1GPU && !(rotation & 0x1FF) && direction == FLIP_X) {
        // rotation 0 facing left (Sonic standing / running on flat ground): PC draws an exact 1:1 mirror, screen column k
        // (from XPos + pivotX - width) showing texel sprX + width - 1 - k. The rotated quad below spanned width + 4 pixels
        // (the software path's +-2 bounding margins) and started at the far edge u + width (NX-071): the 1:1 scaled path
        // draws it instead (PS1ScaledPiece: a flip rectangle where the GPU draws them, else an exact quad; its pivot is
        // the mirrored one, width - pivotX)
        DrawScaledSprite(FLIP_X, XPos, YPos, width - pivotX, pivotY, 0x200, 0x200, width, height, sprX, sprY, sheetID);
        return;
    }
#endif
    int sprXPos    = (pivotX + sprX) << 9;
    int sprYPos    = (pivotY + sprY) << 9;
    int fullwidth  = width + sprX;
    int fullheight = height + sprY;
    int angle      = rotation & 0x1FF;
    if (angle < 0)
        angle += 0x200;
    if (angle)
        angle = 0x200 - angle;
    int sine   = SinValue512[angle];
    int cosine = CosValue512[angle];
    int XPositions[4];
    int YPositions[4];

    if (direction == FLIP_X) {
        XPositions[0] = XPos + ((sine * (-pivotY - 2) + cosine * (pivotX + 2)) >> 9);
        YPositions[0] = YPos + ((cosine * (-pivotY - 2) - sine * (pivotX + 2)) >> 9);
        XPositions[1] = XPos + ((sine * (-pivotY - 2) + cosine * (pivotX - width - 2)) >> 9);
        YPositions[1] = YPos + ((cosine * (-pivotY - 2) - sine * (pivotX - width - 2)) >> 9);
        XPositions[2] = XPos + ((sine * (height - pivotY + 2) + cosine * (pivotX + 2)) >> 9);
        YPositions[2] = YPos + ((cosine * (height - pivotY + 2) - sine * (pivotX + 2)) >> 9);
        int a         = pivotX - width - 2;
        int b         = height - pivotY + 2;
        XPositions[3] = XPos + ((sine * b + cosine * a) >> 9);
        YPositions[3] = YPos + ((cosine * b - sine * a) >> 9);
    } else {
        XPositions[0] = XPos + ((sine * (-pivotY - 2) + cosine * (-pivotX - 2)) >> 9);
        YPositions[0] = YPos + ((cosine * (-pivotY - 2) - sine * (-pivotX - 2)) >> 9);
        XPositions[1] = XPos + ((sine * (-pivotY - 2) + cosine * (width - pivotX + 2)) >> 9);
        YPositions[1] = YPos + ((cosine * (-pivotY - 2) - sine * (width - pivotX + 2)) >> 9);
        XPositions[2] = XPos + ((sine * (height - pivotY + 2) + cosine * (-pivotX - 2)) >> 9);
        YPositions[2] = YPos + ((cosine * (height - pivotY + 2) - sine * (-pivotX - 2)) >> 9);
        int a         = width - pivotX + 2;
        int b         = height - pivotY + 2;
        XPositions[3] = XPos + ((sine * b + cosine * a) >> 9);
        YPositions[3] = YPos + ((cosine * b - sine * a) >> 9);
    }

    int left = SCREEN_XSIZE;
    for (int i = 0; i < 4; ++i) {
        if (XPositions[i] < left)
            left = XPositions[i];
    }
    if (left < 0)
        left = 0;

    int right = 0;
    for (int i = 0; i < 4; ++i) {
        if (XPositions[i] > right)
            right = XPositions[i];
    }
    if (right > SCREEN_XSIZE)
        right = SCREEN_XSIZE;
    int maxX = right - left;

    int top = SCREEN_YSIZE;
    for (int i = 0; i < 4; ++i) {
        if (YPositions[i] < top)
            top = YPositions[i];
    }
    if (top < 0)
        top = 0;

    int bottom = 0;
    for (int i = 0; i < 4; ++i) {
        if (YPositions[i] > bottom)
            bottom = YPositions[i];
    }
    if (bottom > SCREEN_YSIZE)
        bottom = SCREEN_YSIZE;
    int maxY = bottom - top;

    if (maxX <= 0 || maxY <= 0)
        return;

#if RETRO_PLATFORM == RETRO_PS1
    if (g_ps1GPU) {
        GFXSurface *surf = &GfxSurface[sheetID];
        // The frame must sit in one atlas cluster (one texture page); frames the atlas
        // builder split across clusters aren't drawn rotated by any v2 script.
        const PS1AtlasCluster *cl = surf->atlasSheet >= 0 ? PS1FindCluster(surf->atlasSheet, sprX, sprY) : nullptr;
        if (cl && sprX + width <= cl->srcX + cl->w && sprY + height <= cl->srcY + cl->h) {
            int u0 = cl->u + sprX - cl->srcX, v0 = cl->v + sprY - cl->srcY;
            auto clampU = [](int u) { return (uint8_t)(u > 255 ? 255 : u); }; // 256 doesn't fit a UV byte
            // RSDKv2 corner order: FLIP_NONE -> [0]=TL [1]=TR [2]=BL [3]=BR.
            // FLIP_X mirrors X, so [1]=TL [0]=TR [3]=BL [2]=BR.
            int a = 0, b = 1, c = 2, d = 3;
            int uLeft, uRight;
            if (direction == FLIP_X) {
                a = 1; b = 0; c = 3; d = 2;
                uLeft = u0 + width; uRight = u0;
            } else {
                uLeft = u0; uRight = u0 + width;
            }
            psyqo::Prim::TexturedQuad quad(psyqo::Color{{.r = 0x80, .g = 0x80, .b = 0x80}});
            quad.pointA    = psyqo::Vertex{{.x = (int16_t)XPositions[a], .y = (int16_t)YPositions[a]}};
            quad.uvA       = psyqo::PrimPieces::UVCoords{clampU(uLeft), (uint8_t)v0};
            quad.clutIndex = PS1ClusterClut(cl);
            quad.pointB    = psyqo::Vertex{{.x = (int16_t)XPositions[b], .y = (int16_t)YPositions[b]}};
            quad.uvB       = psyqo::PrimPieces::UVCoords{clampU(uRight), (uint8_t)v0};
            quad.tpage     = PS1ClusterTPage(cl);
            quad.pointC = psyqo::Vertex{{.x = (int16_t)XPositions[c], .y = (int16_t)YPositions[c]}};
            quad.uvC    = psyqo::PrimPieces::UVCoordsPadded{clampU(uLeft), clampU(v0 + height), 0};
            quad.pointD = psyqo::Vertex{{.x = (int16_t)XPositions[d], .y = (int16_t)YPositions[d]}};
            quad.uvD    = psyqo::PrimPieces::UVCoordsPadded{clampU(uRight), clampU(v0 + height), 0};
            PS1ChainQuad(quad);
            return;
        }
    }
#endif

#if RETRO_PLATFORM == RETRO_PS1
    return; // PS1: software rasteriser output is never displayed; GPU path TODO (phase 3c)
#endif
    GFXSurface *surface  = &GfxSurface[sheetID];
    int pitch            = SCREEN_XSIZE - maxX;
    byte *pixelBufferPtr = &Engine.FrameBuffer[left + SCREEN_XSIZE * top];
    int startX           = left - XPos;
    int startY           = top - YPos;
    int shiftPivot       = (sprX << 9) - 1;
    fullwidth <<= 9;
    int shiftheight = (sprY << 9) - 1;
    fullheight <<= 9;
    byte *gfxData = &GraphicData[surface->dataPosition];
    if (cosine < 0 || sine < 0)
        sprYPos += sine + cosine;

    if (direction == FLIP_X) {
        int drawX = sprXPos - (cosine * startX - sine * startY) - 0x100;
        int drawY = cosine * startY + sprYPos + sine * startX;
        while (maxY--) {
            int finalX = drawX;
            int finalY = drawY;
            int w      = maxX;
            while (w--) {
                if (finalX > shiftPivot && finalX < fullwidth && finalY > shiftheight && finalY < fullheight) {
                    byte index = gfxData[((finalY >> 9) * surface->width) + (finalX >> 9)];
                    if (index > 0)
                        *pixelBufferPtr = index;
                }
                ++pixelBufferPtr;
                finalX -= cosine;
                finalY += sine;
            }
            drawX += sine;
            drawY += cosine;
            pixelBufferPtr += pitch;
        }
    } else {
        int drawX = sprXPos + cosine * startX - sine * startY;
        int drawY = cosine * startY + sprYPos + sine * startX;
        while (maxY--) {
            int finalX = drawX;
            int finalY = drawY;
            int w      = maxX;
            while (w--) {
                if (finalX > shiftPivot && finalX < fullwidth && finalY > shiftheight && finalY < fullheight) {
                    byte index = gfxData[((finalY >> 9) * surface->width) + (finalX >> 9)];
                    if (index > 0)
                        *pixelBufferPtr = index;
                }
                ++pixelBufferPtr;
                finalX += cosine;
                finalY += sine;
            }
            drawX -= sine;
            drawY += cosine;
            pixelBufferPtr += pitch;
        }
    }
}

void DrawBlendedSprite(int XPos, int YPos, int width, int height, int sprX, int sprY, int sheetID) {
#if RETRO_PLATFORM == RETRO_PS1
    if (width + XPos > SCREEN_XSIZE)
        width = SCREEN_XSIZE - XPos;
    if (XPos < 0) {
        sprX -= XPos;
        width += XPos;
        XPos = 0;
    }
    if (height + YPos > SCREEN_YSIZE)
        height = SCREEN_YSIZE - YPos;
    if (YPos < 0) {
        sprY -= YPos;
        height += YPos;
        YPos = 0;
    }
    if (width <= 0 || height <= 0 || !g_ps1GPU)
        return;
    // PC: BlendLookupTable[dst][src] (SetBlendTable). GPU: B/2 + F/2 with a blend CLUT; exact
    // for type 0 at alpha 128, and for type 1 when dst already is a ramp colour (see PS1BlendClut).
    if (GfxSurface[sheetID].atlasSheet >= 0)
        PS1DrawAtlasRect(GfxSurface[sheetID].atlasSheet, XPos, YPos, width, height, sprX, sprY, s_blendType != 0xFF);
#else
    if (width + XPos > SCREEN_XSIZE)
        width = SCREEN_XSIZE - XPos;
    if (XPos < 0) {
        sprX -= XPos;
        width += XPos;
        XPos = 0;
    }
    if (height + YPos > SCREEN_YSIZE)
        height = SCREEN_YSIZE - YPos;
    if (YPos < 0) {
        sprY -= YPos;
        height += YPos;
        YPos = 0;
    }
    if (width <= 0 || height <= 0)
        return;

    GFXSurface *surface  = &GfxSurface[sheetID];
    int pitch            = SCREEN_XSIZE - width;
    int gfxPitch         = surface->width - width;
    byte *gfxData        = &GraphicData[sprX + surface->width * sprY + surface->dataPosition];
    byte *pixelBufferPtr = &Engine.FrameBuffer[XPos + SCREEN_XSIZE * YPos];
    while (height--) {
        int w = width;
        while (w--) {
            if (*gfxData > 0)
                *pixelBufferPtr = BlendLookupTable[(0x100 * *pixelBufferPtr) + *gfxData];
            ++gfxData;
            ++pixelBufferPtr;
        }
        pixelBufferPtr += pitch;
        gfxData += gfxPitch;
    }
#endif
}

void DrawTextMenuEntry(void *menu, int rowID, int XPos, int YPos, int textHighlight) {
    TextMenu *tMenu = (TextMenu *)menu;
    int id          = tMenu->entryStart[rowID];
    for (int i = 0; i < tMenu->entrySize[rowID]; ++i) {
        if (tMenu->textData[id] > 0)
            DrawSprite(XPos + 8 * i, YPos, 8, 8, textHighlight, 8 * tMenu->textData[id] - 8, TextMenuSurfaceNo);
        ++id;
    }
}
void DrawBlendedTextMenuEntry(void *menu, int rowID, int XPos, int YPos, int textHighlight) {
    TextMenu *tMenu = (TextMenu *)menu;
    int id          = tMenu->entryStart[rowID];
    for (int i = 0; i < tMenu->entrySize[rowID]; ++i) {
        if (tMenu->textData[id] > 0)
            DrawBlendedSprite(XPos + 8 * i, YPos, 8, 8, textHighlight, 8 * tMenu->textData[id] - 8, TextMenuSurfaceNo);
        ++id;
    }
}
void DrawStageTextEntry(void *menu, int rowID, int XPos, int YPos, int textHighlight) {
    TextMenu *tMenu = (TextMenu *)menu;
    int id          = tMenu->entryStart[rowID];
    for (int i = 0; i < tMenu->entrySize[rowID]; ++i) {
        if (tMenu->textData[id] > 0) {
            if (i == tMenu->entrySize[rowID] - 1)
                DrawSprite(XPos + 8 * i, YPos, 8, 8, 0, 8 * tMenu->textData[id] - 8, TextMenuSurfaceNo);
            else
                DrawSprite(XPos + 8 * i, YPos, 8, 8, textHighlight, 8 * tMenu->textData[id] - 8, TextMenuSurfaceNo);
        }
        id++;
    }
}
void DrawTextMenu(void *menu, int XPos, int YPos) {
    TextMenu *tMenu = (TextMenu *)menu;
    if (tMenu->selectionCount == 3) {
        tMenu->selection2 = -1;
        for (int i = 0; i < tMenu->selection1 + 1; ++i) {
            if (tMenu->entryHighlight[i] == 1) {
                tMenu->selection2 = i;
            }
        }
    }
    switch (tMenu->alignment) {
        case MENU_ALIGN_LEFT:
            for (int i = 0; i < tMenu->rowCount; ++i) {
                switch (tMenu->selectionCount) {
                    case 1:
                        if (i == tMenu->selection1)
                            DrawTextMenuEntry(tMenu, i, XPos, YPos, 8);
                        else
                            DrawTextMenuEntry(tMenu, i, XPos, YPos, 0);
                        break;
                    case 2:
                        if (i == tMenu->selection1 || i == tMenu->selection2)
                            DrawTextMenuEntry(tMenu, i, XPos, YPos, 8);
                        else
                            DrawTextMenuEntry(tMenu, i, XPos, YPos, 0);
                        break;
                    case 3:
                        if (i == tMenu->selection1)
                            DrawTextMenuEntry(tMenu, i, XPos, YPos, 8);
                        else
                            DrawTextMenuEntry(tMenu, i, XPos, YPos, 0);
                        if (i == tMenu->selection2 && i != tMenu->selection1)
                            DrawStageTextEntry(tMenu, i, XPos, YPos, 8);
                        break;
                }
                YPos += 8;
            }
            return;
        case MENU_ALIGN_RIGHT:
            for (int i = 0; i < tMenu->rowCount; ++i) {
                int textX = XPos - (tMenu->entrySize[i] << 3);
                switch (tMenu->selectionCount) {
                    case 1:
                        if (i == tMenu->selection1)
                            DrawTextMenuEntry(tMenu, i, textX, YPos, 8);
                        else
                            DrawTextMenuEntry(tMenu, i, textX, YPos, 0);
                        break;
                    case 2:
                        if (i == tMenu->selection1 || i == tMenu->selection2)
                            DrawTextMenuEntry(tMenu, i, textX, YPos, 8);
                        else
                            DrawTextMenuEntry(tMenu, i, textX, YPos, 0);
                        break;
                    case 3:
                        if (i == tMenu->selection1)
                            DrawTextMenuEntry(tMenu, i, textX, YPos, 8);
                        else
                            DrawTextMenuEntry(tMenu, i, textX, YPos, 0);
                        if (i == tMenu->selection2 && i != tMenu->selection1)
                            DrawStageTextEntry(tMenu, i, textX, YPos, 8);
                        break;
                }
                YPos += 8;
            }
            return;
        case MENU_ALIGN_CENTER:
            for (int i = 0; i < tMenu->rowCount; ++i) {
                int textX = XPos - (tMenu->entrySize[i] >> 1 << 3);
                switch (tMenu->selectionCount) {
                    case 1:
                        if (i == tMenu->selection1)
                            DrawTextMenuEntry(tMenu, i, textX, YPos, 8);
                        else
                            DrawTextMenuEntry(tMenu, i, textX, YPos, 0);
                        break;
                    case 2:
                        if (i == tMenu->selection1 || i == tMenu->selection2)
                            DrawTextMenuEntry(tMenu, i, textX, YPos, 8);
                        else
                            DrawTextMenuEntry(tMenu, i, textX, YPos, 0);
                        break;
                    case 3:
                        if (i == tMenu->selection1)
                            DrawTextMenuEntry(tMenu, i, textX, YPos, 8);
                        else
                            DrawTextMenuEntry(tMenu, i, textX, YPos, 0);

                        if (i == tMenu->selection2 && i != tMenu->selection1)
                            DrawStageTextEntry(tMenu, i, textX, YPos, 8);
                        break;
                }
                YPos += 8;
            }
            return;
        default: return;
    }
}
