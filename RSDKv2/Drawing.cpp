#include "RetroEngine.hpp"
#if RETRO_PLATFORM == RETRO_PS1
#include "common/syscalls/syscalls.h"
#include "../ps1/sprite_atlas.hh"
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

#if RETRO_PLATFORM == RETRO_PS1
#include <psyqo/gpu.hh>
#include <psyqo/primitives/common.hh>
#include <psyqo/primitives/control.hh>
#include <psyqo/primitives/quads.hh>
#include <psyqo/primitives/sprites.hh>
#include "../ps1/gpu_chain.hh"

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
static bool ps1TileSetUploaded = false;

void PS1UploadPalette() {
    static uint16_t clut[256];
    // Honour the RSDKv2 palette fade: when PaletteMode is set, use the faded
    // palette (TilePaletteF, updated by SetFade) instead of the normal one.
    const Colour *pal = PaletteMode ? TilePaletteF : TilePalette;
    for (int i = 0; i < 256; ++i) {
        byte r = pal[i].r, g = pal[i].g, b = pal[i].b;
        uint16_t c = (uint16_t)(((r >> 3)) | ((g >> 3) << 5) | ((b >> 3) << 10));
        if (i == 0)
            c = 0x0000; // index 0 = transparent (matches RSDKv2 sprite transparency)
        else if (c == 0x0000)
            c = 0x8000; // visible black -> non-transparent black for opaque commands
        clut[i] = c;
    }
    PS1ChainUploadRow(clut, 256, PS1_CLUT_X, PS1_CLUT_Y); // chained VRAM upload

    // 4-bit atlas CLUTs map 16 values to master entries: rebuilt from the (faded / rotated)
    // master CLUT every frame, uploaded as runs of adjacent slots in the same VRAM row.
    static uint16_t clut4[PS1_ATLAS_MAX_CLUTS * 16];
    for (int k = 0; k < g_ps1AtlasClutCount; ++k) {
        const uint8_t *map = g_ps1AtlasCluts[k].map;
        clut4[k * 16] = 0x0000; // transparent
        for (int i = 1; i < 16; ++i) clut4[k * 16 + i] = clut[map[i]];
    }
    for (int k = 0; k < g_ps1AtlasClutCount;) {
        int run = 1;
        while (k + run < g_ps1AtlasClutCount && g_ps1AtlasCluts[k + run].y == g_ps1AtlasCluts[k].y
               && g_ps1AtlasCluts[k + run].x == g_ps1AtlasCluts[k].x + 16 * run)
            ++run;
        PS1ChainUploadRow(&clut4[k * 16], run * 16, (int16_t)g_ps1AtlasCluts[k].x, (int16_t)g_ps1AtlasCluts[k].y);
        k += run;
    }
    PS1ChainFlushCache(); // the GPU caches CLUTs
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
static void PS1DrawAtlasRect(int sheet, int XPos, int YPos, int width, int height, int sprX, int sprY) {
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
        s.texInfo  = psyqo::PrimPieces::TexInfo{(uint8_t)(c->u + r.x - c->srcX), (uint8_t)(c->v + r.y - c->srcY),
                                               PS1ClusterClut(c)};
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
void PS1UploadTileSet() {
    if (ps1TileSetUploaded)
        return;
    FileInfo info;
    if (!LoadStageFile("16x16Tiles.vram", StageListPosition, &info))
        return;
    SetFilePosition(4 + 768); // skip width/height + palette
    PS1GpuImmediate();        // immediate VRAM uploads: wait for the in-flight DMA chain
    const int pageCount = TILE_COUNT / 256; // 4 (1024 tiles)
    for (int page = 0; page < pageCount; ++page) {
        FileRead(TileGfx, 0x10000);
        int px = PS1_TILE_PAGE_X + 2 * (page & 1); // 9, 11, 9, 11
        int py = PS1_TILE_PAGE_Y + (page >> 1);    // 0, 0, 1, 1
        g_ps1GPU->uploadToVRAM((const uint16_t *)TileGfx, psyqo::Rect{{(int16_t)(px * 64), (int16_t)(py * 256)}, {128, 256}});
    }
    CloseFile();
    ps1TileSetUploaded = true;
}

// Draw one 16x16 tile as a Sprite16x16. tileIndex is StageTiles.tileIndex[].
static void PS1DrawTileQuad(int screenX, int screenY, int tileIndex) {
    if (tileIndex <= 0)           // tile 0 = transparent/empty
        return;
    int page  = tileIndex >> 8;   // 0..3 (1024 tiles / 256 per page)
    int pt    = tileIndex & 0xFF; // 0..255 within the page
    int u     = (pt & 0xF) << 4;  // 16 * (pt % 16)
    int v     = (pt >> 4) << 4;   // 16 * (pt / 16)
    int pageX = PS1_TILE_PAGE_X + 2 * (page & 1); // 9, 11, 9, 11
    int pageY = PS1_TILE_PAGE_Y + (page >> 1);    // 0, 0, 1, 1

    psyqo::PrimPieces::TPageAttr attr;
    attr.set(psyqo::Prim::TPageAttr::ColorMode::Tex8Bits);
    attr.setPageX((uint8_t)pageX);
    attr.setPageY((uint8_t)pageY);
    PS1ChainTPage(attr);

    psyqo::Prim::Sprite16x16 s(psyqo::Color{{.r = 0x80, .g = 0x80, .b = 0x80}});
    s.position = psyqo::Vertex{{.x = (int16_t)screenX, .y = (int16_t)screenY}};
    s.texInfo  = psyqo::PrimPieces::TexInfo{(uint8_t)u, (uint8_t)v,
                   psyqo::PrimPieces::ClutIndex(psyqo::Vertex{{.x = PS1_CLUT_X, .y = PS1_CLUT_Y}})};
    PS1ChainSprite16(s);
}

// Minimal flat tile renderer: no flip/deform/water yet. Only the chunks that
// intersect the 320x240 view are visited (a view touches at most 4x3 chunks).
static void PS1DrawTileLayer(int layerID) {
    TileLayer *layer = &StageLayouts[activeTileLayers[layerID]];
    if (!layer->xsize || !layer->ysize)
        return;
    // Visible chunk range (128x128-pixel chunks), clamped to the layer.
    int cx0 = XScrollOffset >> 7;
    int cy0 = YScrollOffset >> 7;
    int cx1 = (XScrollOffset + SCREEN_XSIZE - 1) >> 7;
    int cy1 = (YScrollOffset + SCREEN_YSIZE - 1) >> 7;
    if (cx0 < 0)
        cx0 = 0;
    if (cy0 < 0)
        cy0 = 0;
    if (cx1 >= layer->xsize)
        cx1 = layer->xsize - 1;
    if (cy1 >= layer->ysize)
        cy1 = layer->ysize - 1;
    for (int cy = cy0; cy <= cy1; ++cy) {
        for (int cx = cx0; cx <= cx1; ++cx) {
            int chunkIndex = layer->tiles[cy * TILELAYER_CHUNK_W + cx];
            for (int sy = 0; sy < 8; ++sy) {
                int screenY = (cy << 7) + (sy << 4) - YScrollOffset;
                if (screenY < -16 || screenY >= SCREEN_YSIZE)
                    continue;
                for (int sx = 0; sx < 8; ++sx) {
                    int tile = StageTiles.tileIndex[(chunkIndex << 6) + sx + (sy << 3)];
                    if (tile == 0)
                        continue;
                    int screenX = (cx << 7) + (sx << 4) - XScrollOffset;
                    if (screenX < -16 || screenX >= SCREEN_XSIZE)
                        continue;
                    PS1DrawTileQuad(screenX, screenY, tile);
                }
            }
        }
    }
}

static bool ps1TileTestInit = false;

// Self-contained tile-path test: fill a synthetic tile, upload it, draw it every
// frame. Bypasses Engine.Init() and the script VM entirely. Temporary for Phase 2.
void PS1TileSelfTest() {
    PS1ChainClear();

    if (!ps1TileTestInit) {
        TilePalette[181].r = 255; TilePalette[181].g = 0; TilePalette[181].b = 0;   // red
        TilePalette[153].r = 0;   TilePalette[153].g = 0; TilePalette[153].b = 255; // blue
        for (int y = 0; y < 16; ++y)
            for (int x = 0; x < 16; ++x)
                TileGfx[188 * 256 + y * 16 + x] = (byte)((((x >> 2) + (y >> 2)) & 1) ? 153 : 181);
        PS1UploadTileSet();
        ps1TileTestInit = true;
    }

    PS1UploadPalette();
    PS1DrawTileQuad(100, 100, 188);
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
    // The GPU renderer already drew the frame (DrawStageGfx); psyqo flips on vsync.
    // The palette conversion below would be ~76,800 wasted RAM lookups per frame.
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

void DrawObjectList(int Layer) {
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
}
void DrawStageGfx() {
#if RETRO_PLATFORM == RETRO_PS1
    if (g_ps1GPU) {
        PS1UploadTileSet();        // once per stage (immediate, waits for the in-flight chain)
        // Chained FastFill of the buffer this frame draws into. Under a fade the empty screen
        // is the fade colour's share (PC: ClearScreen(0) + faded palette entry 0 = black).
        if (PaletteMode)
            PS1ChainClear((ps1FadeR * ps1FadeA) >> 8, (ps1FadeG * ps1FadeA) >> 8, (ps1FadeB * ps1FadeA) >> 8);
        else
            PS1ChainClear();
        PS1UploadPalette();        // chained CLUT upload (fade / palette cycling)
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
    return; // PS1: no blend LUT; GPU semi-transparency TODO (phase 3c)
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

    byte *lineScroll;
    int *deformationData;
    int *deformationDataW;

    int yscrollOffset = 0;
    if (activeTileLayers[layerID]) { // BG Layer
        int yScroll    = YScrollOffset * layer->parallaxFactor >> 6;
        int fullheight = layerheight << 7;
        layer->scrollPos += layer->scrollSpeed;
        if (layer->scrollPos > fullheight << 16)
            layer->scrollPos -= fullheight << 16;
        yscrollOffset    = (yScroll + (layer->scrollPos >> 16)) % fullheight;
        layerheight      = fullheight >> 7;
        lineScroll       = layer->lineScroll;
        deformationData  = &BGDeformationData3[(byte)(yscrollOffset + DeformationPos3)];
        deformationDataW = &BGDeformationData4[(byte)(yscrollOffset + waterDrawPos + DeformationPos4)];
    } else { // FG Layer
        LastXSize     = layer->xsize;
        yscrollOffset = YScrollOffset;
        lineScroll    = layer->lineScroll;
        for (int i = 0; i < PARALLAX_COUNT; ++i) HParallax.linePos[i] = XScrollOffset;
        deformationData  = &BGDeformationData1[(byte)(yscrollOffset + DeformationPos1)];
        deformationDataW = &BGDeformationData2[(byte)(yscrollOffset + waterDrawPos + DeformationPos2)];
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
#if RETRO_PLATFORM == RETRO_PS1
    return; // PS1: software rasteriser output is never displayed; GPU path TODO (phase 3c)
#endif
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
    return; // PS1: software rasteriser output is never displayed; GPU path TODO (phase 3c)
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

void DrawScaledSprite(int direction, int XPos, int YPos, int pivotX, int pivotY, int scaleX, int scaleY, int width, int height, int sprX, int sprY,
                      int sheetID) {
#if RETRO_PLATFORM == RETRO_PS1
    // GPU path: one textured quad. Same placement maths as the software version below
    // (512 = 1.0). The quad is clipped to the screen at texel boundaries so its vertices stay
    // within one scaled texel of the screen (the GPU refuses polygons wider than 1023 or
    // taller than 511; the intro zooms 256x160 to 3.5x) and the UVs stay exact integers.
    if (!g_ps1GPU)
        return;
    GFXSurface *surf = &GfxSurface[sheetID];
    if (width <= 0 || height <= 0)
        return;
    int texPage, texPageY, u0, v0;
    const PS1AtlasCluster *cl = nullptr;
    if (surf->vramDirect15) {
        texPage = surf->vramPageX, texPageY = surf->vramPageY, u0 = sprX, v0 = sprY;
    } else {
        if (surf->atlasSheet < 0 || !(cl = PS1FindCluster(surf->atlasSheet, sprX, sprY)))
            return;
        texPage = cl->tpageX, texPageY = cl->tpageY;
        u0 = cl->u + sprX - cl->srcX, v0 = cl->v + sprY - cl->srcY;
        if (sprX + width > cl->srcX + cl->w || sprY + height > cl->srcY + cl->h)
            return; // frame split across clusters: not needed scaled by any v2 script
    }
    if (u0 + width > 256 || v0 + height > 256)
        return;
    int sx = 4 * scaleX, sy = 4 * scaleY;
    if (sx <= 0 || sy <= 0)
        return;
    int x0 = XPos - (sx * pivotX >> 11), y0 = YPos - (sy * pivotY >> 11);
    int w = sx * width >> 11, h = sy * height >> 11;
    if (w <= 0 || h <= 0 || x0 >= SCREEN_XSIZE || y0 >= SCREEN_YSIZE || x0 + w <= 0 || y0 + h <= 0)
        return;
    // Visible texel range [t0, t1) per axis; screen edge of texel t: x0 + t * w / width.
    int tx0 = x0 < 0 ? (-x0 * width) / w : 0;
    int tx1 = x0 + w > SCREEN_XSIZE ? ((SCREEN_XSIZE - x0) * width + w - 1) / w : width;
    int ty0 = y0 < 0 ? (-y0 * height) / h : 0;
    int ty1 = y0 + h > SCREEN_YSIZE ? ((SCREEN_YSIZE - y0) * height + h - 1) / h : height;
    if (tx1 > width) tx1 = width;
    if (ty1 > height) ty1 = height;
    int qx0 = x0 + tx0 * w / width, qx1 = x0 + tx1 * w / width;
    int qy0 = y0 + ty0 * h / height, qy1 = y0 + ty1 * h / height;
    if (qx1 <= qx0 || qy1 <= qy0)
        return;
    int uL, uR; // FLIP_X mirrors the texels: screen-left shows the sprite's right edge
    if (direction == FLIP_X) {
        uL = u0 + width - tx0;
        uR = u0 + width - tx1;
    } else {
        uL = u0 + tx0;
        uR = u0 + tx1;
    }
    auto clampU = [](int u) { return (uint8_t)(u > 255 ? 255 : u); }; // 256 doesn't fit a UV byte
    int vT = v0 + ty0, vB = v0 + ty1;
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
    PS1ChainQuad(quad);
    if (surf->vramDirect15) {
        int ox0 = qx0 < 0 ? 0 : qx0, oy0 = qy0 < 0 ? 0 : qy0;
        int ox1 = qx1 > SCREEN_XSIZE ? SCREEN_XSIZE : qx1, oy1 = qy1 > SCREEN_YSIZE ? SCREEN_YSIZE : qy1;
        PS1DirectFadeOverlay(ox0, oy0, ox1 - ox0, oy1 - oy0);
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
    return; // PS1: no blend LUT; GPU semi-transparency TODO (phase 3c)
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
