#include "RetroEngine.hpp"
#if RETRO_PLATFORM == RETRO_PS1
#include "common/syscalls/syscalls.h"
#endif


int SCREEN_XSIZE   = 320;
int SCREEN_CENTERX = (SCREEN_XSIZE / 2);

byte BlendLookupTable[0x100 * 0x100];

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
#include <psyqo/primitives/sprites.hh>

// VRAM layout (1MB, 1024x512 16-bit halfwords):
//  - framebuffer 320x240 at Y=0 (buf0) and Y=256 (buf1), X=0-319 halfwords (psyqo default)
//  - CLUT (256x1, 15-bit) at (X=0, Y=240) -- the 16-line gap below buf0
//  - sprite pages at pageX=5,7 (X base 320,448) x pageY=0,1 (Y 0,256)
//  - tile pages at pageX=9,11 (X base 576,704) x pageY=0 (Y 0) -- 2 pages = 512 tiles
//    Page X base = pageX*64 halfwords (NOT pixels); a page is 128 halfwords = 256 texels.
//    Must start at >=320 halfwords to clear the framebuffer (X=0-319).
#define PS1_CLUT_X 0
#define PS1_CLUT_Y 240
static int ps1PageSlotIdx = 0;
static const int ps1PageSlotX[4] = { 5, 7, 5, 7 };
static const int ps1PageSlotY[4] = { 0, 0, 1, 1 };
#define PS1_TILE_PAGE_X 9
#define PS1_TILE_PAGE_Y 0
static bool ps1TileSetUploaded = false;
static int ps1TileCurPage = -1;
static int ps1CurPageX = -1;
static int ps1CurPageY = -1;

static bool PS1AllocPages(int pagesX, int *outX, int *outY) {
    if (pagesX < 1 || pagesX > 2 || ps1PageSlotIdx + pagesX > 4)
        return false;
    *outX = ps1PageSlotX[ps1PageSlotIdx];
    *outY = ps1PageSlotY[ps1PageSlotIdx];
    ps1PageSlotIdx += pagesX;
    return true;
}

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
    g_ps1GPU->uploadToVRAM(clut, psyqo::Rect{{PS1_CLUT_X, PS1_CLUT_Y}, {256, 1}});
}

void PS1ResetSpriteVRAM() {
    ps1PageSlotIdx = 0;
    ps1CurPageX    = -1;
    ps1CurPageY    = -1;
    ps1TileCurPage = -1;
}

// Re-pack the 16-wide tile strip into 16x16 tile grids and upload as 8-bit pages.
// TileGfx layout: tile N is a CONTIGUOUS 16x16 block at byte offset N*256
// (gfxDataPos = tileIndex << 8), 16 bytes/row stride.
void PS1UploadTileSet() {
    if (ps1TileSetUploaded)
        return;
    const int tilesPerPage = 256;
    int pageCount          = (TILE_COUNT + tilesPerPage - 1) / tilesPerPage; // 2
    static uint16_t row[128];                                                // 256 texels = 128 halfwords
    for (int page = 0; page < pageCount; ++page) {
        int px        = PS1_TILE_PAGE_X + 2 * page; // pageX 9, 11
        int vramHalfX = px * 64;
        int vramY     = PS1_TILE_PAGE_Y * 256;
        for (int ty = 0; ty < 256; ++ty) {
            int gridRow = ty >> 4;
            int y       = ty & 0xF;
            for (int halfX = 0; halfX < 128; ++halfX) {
                int tx0  = halfX << 1;
                int col0 = tx0 >> 4, x0 = tx0 & 0xF;
                int n0   = page * tilesPerPage + (gridRow << 4) + col0;
                int col1 = (tx0 + 1) >> 4, x1 = (tx0 + 1) & 0xF;
                int n1   = page * tilesPerPage + (gridRow << 4) + col1;
                byte lo  = (n0 < TILE_COUNT) ? TileGfx[n0 * 256 + y * 16 + x0] : 0;
                byte hi  = (n1 < TILE_COUNT) ? TileGfx[n1 * 256 + y * 16 + x1] : 0;
                row[halfX] = (uint16_t)(lo | (hi << 8));
            }
            g_ps1GPU->uploadToVRAM(row, psyqo::Rect{{vramHalfX, vramY + ty}, {128, 1}});
        }
    }
    ps1TileSetUploaded = true;
}

// Draw one 16x16 tile as a Sprite16x16. tileIndex is StageTiles.tileIndex[].
static void PS1DrawTileQuad(int screenX, int screenY, int tileIndex) {
    if (tileIndex <= 0)           // tile 0 = transparent/empty
        return;
    int page  = tileIndex >> 8;   // 0 or 1 (512 tiles / 256 per page)
    int pt    = tileIndex & 0xFF; // 0..255 within the page
    int u     = (pt & 0xF) << 4;  // 16 * (pt % 16)
    int v     = (pt >> 4) << 4;   // 16 * (pt / 16)
    int pageX = PS1_TILE_PAGE_X + (page << 1); // 9 or 11

    if (pageX != ps1TileCurPage) {
        psyqo::Prim::TPage tp;
        tp.attr.set(psyqo::Prim::TPageAttr::ColorMode::Tex8Bits);
        tp.attr.setPageX((uint8_t)pageX);
        tp.attr.setPageY((uint8_t)PS1_TILE_PAGE_Y);
        g_ps1GPU->sendPrimitive(tp);
        ps1TileCurPage = pageX;
    }

    psyqo::Prim::Sprite16x16 s(psyqo::Color{{.r = 0x80, .g = 0x80, .b = 0x80}});
    s.position = psyqo::Vertex{{.x = (int16_t)screenX, .y = (int16_t)screenY}};
    s.texInfo  = psyqo::PrimPieces::TexInfo{(uint8_t)u, (uint8_t)v,
                   psyqo::PrimPieces::ClutIndex(psyqo::Vertex{{.x = PS1_CLUT_X, .y = PS1_CLUT_Y}})};
    g_ps1GPU->sendPrimitive(s);
}

// Minimal flat tile renderer: draws the layer grid at scroll 0, no flip/deform/water.
static void PS1DrawTileLayer(int layerID) {
    TileLayer *layer = &StageLayouts[activeTileLayers[layerID]];
    static bool layerEnterLogged = false;
    if (!layerEnterLogged) {
        layerEnterLogged = true;
        ramsyscall_printf("[layer] enter layerID=%d activeLayer=%d xsize=%d ysize=%d\n",
            layerID, activeTileLayers[layerID], layer->xsize, layer->ysize);
    }
    if (!layer->xsize || !layer->ysize)
        return;
    static bool layerLogged = false;
    if (!layerLogged) {
        layerLogged = true;
        int total  = layer->xsize * layer->ysize * 64;
        int nonzero = 0;
        for (int i = 0; i < total; ++i)
            if (StageTiles.tileIndex[i] != 0)
                ++nonzero;
        ramsyscall_printf("[layer] layerID=%d xsize=%d ysize=%d tiles=%d nonzero=%d\n",
            layerID, layer->xsize, layer->ysize, total, nonzero);
        int firstNZ = 0;
        for (int i = 0; i < total; ++i) {
            if (StageTiles.tileIndex[i] != 0) { firstNZ = StageTiles.tileIndex[i]; break; }
        }
        if (firstNZ > 0)
            ramsyscall_printf("[layer] firstNZ tile=%d gfx=%d,%d,%d,%d,%d,%d,%d,%d\n",
                firstNZ, TileGfx[firstNZ * 256], TileGfx[firstNZ * 256 + 1],
                TileGfx[firstNZ * 256 + 2], TileGfx[firstNZ * 256 + 3],
                TileGfx[firstNZ * 256 + 4], TileGfx[firstNZ * 256 + 5],
                TileGfx[firstNZ * 256 + 6], TileGfx[firstNZ * 256 + 7]);
        else
            ramsyscall_printf("[layer] no nonzero tiles\n");
        ramsyscall_printf("[layer] tileIndex[0..15]:");
        for (int i = 0; i < 16; ++i)
            ramsyscall_printf(" %d", StageTiles.tileIndex[i]);
        ramsyscall_printf("\n");
    }
    int xsize = layer->xsize; // in 128x128 chunks
    int ysize = layer->ysize;
    for (int cy = 0; cy < ysize; ++cy) {
        for (int cx = 0; cx < xsize; ++cx) {
            int chunkIndex = layer->tiles[cy * TILELAYER_CHUNK_W + cx];
            for (int sy = 0; sy < 8; ++sy) {
                for (int sx = 0; sx < 8; ++sx) {
                    int chunk = (chunkIndex << 6) + sx + (sy << 3);
                    int tile  = StageTiles.tileIndex[chunk];
                    if (tile == 0)
                        continue;
                    int screenX = (cx << 7) + (sx << 4);
                    int screenY = (cy << 7) + (sy << 4);
                    if (screenX < -16 || screenX >= SCREEN_XSIZE || screenY < -16 || screenY >= SCREEN_YSIZE)
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
    g_ps1GPU->clear();

    if (!ps1TileTestInit) {
        TilePalette[1].r = 255; TilePalette[1].g = 0; TilePalette[1].b = 0;   // red
        TilePalette[2].r = 0;   TilePalette[2].g = 0; TilePalette[2].b = 255; // blue
        for (int y = 0; y < 16; ++y)
            for (int x = 0; x < 16; ++x)
                TileGfx[256 + y * 16 + x] = (byte)((((x >> 2) + (y >> 2)) & 1) ? 2 : 1);
        PS1UploadTileSet();
        ps1TileTestInit = true;
    }

    PS1UploadPalette();
    PS1DrawTileQuad(100, 100, 1);
}

void PS1UploadSpriteSheet(int sheetID) {
    GFXSurface *s = &GfxSurface[sheetID];
    if (s->width <= 0 || s->height <= 0 || s->vramPageX >= 0)
        return;
    int pagesX = (s->width + 255) / 256;
    if (pagesX > 2 || s->height > 256)
        return; // too big for the GPU path; CPU fallback handles it
    int px, py;
    if (!PS1AllocPages(pagesX, &px, &py))
        return; // out of texture pages; CPU fallback handles it
    s->vramPageX = px;
    s->vramPageY = py;

    // Pack 8-bit indices into 16-bit halfwords (2 texels per halfword) and upload row by row.
    const byte *src = &GraphicData[s->dataPosition];
    int halfW      = (s->width + 1) / 2;
    int vramHalfX  = px * 64; // pageX * 64 halfwords
    int vramY      = py * 256;
    static uint16_t row[256];
    for (int y = 0; y < s->height; ++y) {
        for (int x = 0; x < halfW; ++x) {
            int i   = y * s->width + x * 2;
            byte lo = src[i];
            byte hi = (x * 2 + 1 < s->width) ? src[i + 1] : 0;
            row[x]  = (uint16_t)(lo | (hi << 8));
        }
        g_ps1GPU->uploadToVRAM(row, psyqo::Rect{{vramHalfX, vramY + y}, {halfW, 1}});
    }
}
#endif

int InitRenderDevice() {
    char gameTitle[0x40];

    sprintf(gameTitle, "%s%s", Engine.GameWindowText, Engine.UseBinFile ? "" : " (Using Data Folder)");

    Engine.FrameBuffer = new byte[SCREEN_XSIZE * SCREEN_YSIZE];
    memset(Engine.FrameBuffer, 0, (SCREEN_XSIZE * SCREEN_YSIZE) * sizeof(byte));

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
#if RETRO_PLATFORM == RETRO_PS1
            static ushort ps1FrameBuffer[320 * 240]; // SCREEN_XSIZE/YSIZE are runtime vars; screen is fixed 320x240
            ushort *frameBuffer = ps1FrameBuffer;
#else
            ushort *frameBuffer = new ushort[SCREEN_XSIZE * SCREEN_YSIZE];
#endif
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

#if RETRO_PLATFORM == RETRO_PS1
            // GPU path: DrawStageGfx renders directly into VRAM via sendPrimitive;
            // the 8-bit CPU framebuffer is not uploaded (would overwrite the GPU output).
#elif RETRO_USING_SDL2
            SDL_LockTexture(Engine.screenBuffer, NULL, &pixels, &pitch);
            memcpy(pixels, frameBuffer, pitch * SCREEN_YSIZE);
            SDL_UnlockTexture(Engine.screenBuffer);
#endif
#if !RETRO_PLATFORM
            if (frameBuffer)
                delete[] frameBuffer;
#endif
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
    static bool drawLogged = false;
    if (!drawLogged) {
        drawLogged = true;
        ramsyscall_printf("[draw] DrawStageGfx first call, activeTileLayers[0]=%d\n", activeTileLayers[0]);
        ramsyscall_printf("[draw] tilegfx0=%d,%d,%d,%d pal0=%d,%d,%d pal1=%d,%d,%d\n",
            TileGfx[0], TileGfx[1], TileGfx[2], TileGfx[3],
            TilePalette[0].r, TilePalette[0].g, TilePalette[0].b,
            TilePalette[1].r, TilePalette[1].g, TilePalette[1].b);
        ramsyscall_printf("[draw] pal181=%d,%d,%d pal153=%d,%d,%d pal165=%d,%d,%d\n",
            TilePalette[181].r, TilePalette[181].g, TilePalette[181].b,
            TilePalette[153].r, TilePalette[153].g, TilePalette[153].b,
            TilePalette[165].r, TilePalette[165].g, TilePalette[165].b);
    }
    if (g_ps1GPU) {
        g_ps1GPU->clear();
        PS1UploadPalette();
        PS1UploadTileSet();
        ps1CurPageX = -1;
        ps1CurPageY = -1;
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
        if (surface->vramPageX < 0 && surface->width > 0 && surface->height > 0)
            PS1UploadSpriteSheet(sheetID);
        if (surface->vramPageX >= 0) {
            int basePageX = surface->vramPageX;
            int basePageY = surface->vramPageY;
            int x         = sprX;
            int remaining = width;
            int drawX     = XPos;
            while (remaining > 0) {
                int chunkW = remaining;
                int maxW   = 256 - (x & 0xFF);
                if (chunkW > maxW)
                    chunkW = maxW;
                int pageX = basePageX + 2 * (x / 256);
                int u     = x & 0xFF;
                if (pageX != ps1CurPageX || basePageY != ps1CurPageY) {
                    psyqo::Prim::TPage tp;
                    tp.attr.set(psyqo::Prim::TPageAttr::ColorMode::Tex8Bits);
                    tp.attr.setPageX((uint8_t)pageX);
                    tp.attr.setPageY((uint8_t)basePageY);
                    tp.attr.set(psyqo::Prim::TPageAttr::SemiTrans::HalfBackAndHalfFront);
                    g_ps1GPU->sendPrimitive(tp);
                    ps1CurPageX = pageX;
                    ps1CurPageY = basePageY;
                }
                psyqo::Prim::Sprite s(psyqo::Color{{.r = 0x80, .g = 0x80, .b = 0x80}});
                s.position = psyqo::Vertex{{.x = (int16_t)drawX, .y = (int16_t)YPos}};
                s.size     = psyqo::Vertex{{.w = (int16_t)chunkW, .h = (int16_t)height}};
                s.texInfo  = psyqo::PrimPieces::TexInfo{(uint8_t)u, (uint8_t)sprY,
                                                        psyqo::PrimPieces::ClutIndex(psyqo::Vertex{{.x = PS1_CLUT_X, .y = PS1_CLUT_Y}})};
                g_ps1GPU->sendPrimitive(s);
                x += chunkW;
                drawX += chunkW;
                remaining -= chunkW;
            }
            return;
        }
    }
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
