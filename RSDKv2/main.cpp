#include "RetroEngine.hpp"

#if RETRO_PLATFORM == RETRO_PS1
#include <psyqo/application.hh>
#include <psyqo/gpu.hh>
#include <psyqo/cdrom-device.hh>
#include "common/syscalls/syscalls.h"
#include "../ps1/video.hh"

#define PS1_TILE_TEST 0 // TEMP: synthetic tile-path test (Phase 2 verification)
#define PS1_SKIP_SPRITES 0 // TEMP: skip GIF sprite decode to speed tile-test boots
#define PS1_VIDEO_TEST 1 // TEMP: FMV Stage 3 — decode+display the first STR frame

psyqo::GPU         *g_ps1GPU   = nullptr;
psyqo::CDRomDevice *g_ps1CDRom = nullptr;
bool ps1SkipSprites = PS1_SKIP_SPRITES;

void PS1PresentFramebuffer(ushort *fb) {
    if (!g_ps1GPU)
        return;
    unsigned parity = g_ps1GPU->getParity();
    int16_t  y      = (parity == 0) ? 0 : 256;
    g_ps1GPU->uploadToVRAM(fb, psyqo::Rect{{0, y}, {DEFAULT_SCREEN_XSIZE, SCREEN_YSIZE}});
}

static void PS1VideoSelfTestInit() {
    syscall_puts("[RSDKv2-PS1] video self-test: init + open Intro.str\n");
    PS1VideoInit();
    if (!PS1VideoOpen("Data/Sprites/Videos/Intro.str"))
        syscall_puts("[RSDKv2-PS1] video self-test: FAILED to open Intro.str\n");
}

static void PS1VideoSelfTest() {
    // Advance the video as fast as the CD read allows — the read (~73ms/frame
    // in the emulator) is the bottleneck, so poll every tick instead of gating
    // at a fixed 15fps cadence.
    int r = PS1VideoNextFrame(); // 1 = decoded, 0 = still reading, -1 = EOF
    if (r < 0) {
        // EOF -> loop the video from the start.
        PS1VideoClose();
        if (!PS1VideoOpen("Data/Sprites/Videos/Intro.str"))
            return;
        PS1VideoNextFrame(); // re-queue the read of frame 0
    }

    // MDEC emits 16-pixel-wide vertical slices (column-major). Upload each
    // slice at its x offset instead of treating the buffer as row-major.
    uint16_t *pix   = PS1VideoPixels();
    unsigned parity = g_ps1GPU->getParity();
    int16_t  y      = (parity == 0) ? 0 : 256;
    for (int i = 0; i < 16; i++)
        g_ps1GPU->uploadToVRAM(pix + i * 160 * 16, psyqo::Rect{{(int16_t)(32 + 16 * i), (int16_t)(y + 40)}, {16, 160}});
}

class RSDKv2PS1App final : public psyqo::Application {
  public:
    psyqo::CDRomDevice m_cdrom;

    void prepare() override {
        syscall_puts("[RSDKv2-PS1] boot ok; calling Engine.Init()\n");

        gpu().initialize(psyqo::GPU::Configuration{}
                             .set(psyqo::GPU::Resolution::W320)
                             .set(psyqo::GPU::VideoMode::AUTO)
                             .set(psyqo::GPU::ColorMode::C15BITS)
                             .set(psyqo::GPU::Interlace::PROGRESSIVE)
                             .set(psyqo::GPU::MiscSetting::CLEAR_VRAM));

        g_ps1GPU = &gpu();

#if PS1_TILE_TEST
        // Synthetic tile test: skip CD-ROM + Engine.Init — the test fills
        // TileGfx/TilePalette itself. Temporary for Phase 2 verification.
        return;
#endif

#if PS1_VIDEO_TEST
        m_cdrom.prepare();
        m_cdrom.resetBlocking(gpu());
        g_ps1CDRom = &m_cdrom;
        PS1VideoSelfTestInit();
        return;
#endif

        m_cdrom.prepare();
        m_cdrom.resetBlocking(gpu());
        g_ps1CDRom = &m_cdrom;

        // Force-load a real stage (skip the title screen) to test tile rendering
        // without input. Temporary until PAD input (overall Phase 4) is wired.
        // NOTE: use a manual copy, not strcpy — the PS1 libc strcpy may be a stub.
        {
            const char *scnFolder = "SSZ";
            const char *scnID     = "1"; // stage-list id is the bare number; LoadActFile prepends "Act"
            int b = 0;
            while (scnFolder[b]) {
                Engine.startSceneFolder[b] = scnFolder[b];
                ++b;
            }
            Engine.startSceneFolder[b] = 0;
            b = 0;
            while (scnID[b]) {
                Engine.startSceneID[b] = scnID[b];
                ++b;
            }
            Engine.startSceneID[b] = 0;
        }

        Engine.Init();
        syscall_puts("[RSDKv2-PS1] Engine.Init() returned\n");
    }

    void frame() override {
#if PS1_TILE_TEST
        PS1TileSelfTest();
        return;
#endif
#if PS1_VIDEO_TEST
        PS1VideoSelfTest();
        return;
#endif
        Engine.RunFrame();
    }
};

static RSDKv2PS1App rsdkv2App;
#endif

#if !RETRO_USE_ORIGINAL_CODE

#if RETRO_PLATFORM == RETRO_WIN && _MSC_VER
#include "Windows.h"
#endif

void parseArguments(int argc, char *argv[]) {
    for (int a = 0; a < argc; ++a) {
        const char *find = "";

        find = strstr(argv[a], "stage=");
        if (find) {
            int b = 0;
            int c = 6;
            while (find[c] && find[c] != ';') Engine.startSceneFolder[b++] = find[c++];
            Engine.startSceneFolder[b] = 0;
        }

        find = strstr(argv[a], "scene=");
        if (find) {
            int b = 0;
            int c = 6;
            while (find[c] && find[c] != ';') Engine.startSceneID[b++] = find[c++];
            Engine.startSceneID[b] = 0;
        }

        find = strstr(argv[a], "console=true");
        if (find) {
            engineDebugMode       = true;
            Engine.devMenu        = true;
            Engine.consoleEnabled = true;
#if RETRO_PLATFORM == RETRO_WIN && _MSC_VER
            AllocConsole();
            freopen_s((FILE **)stdin, "CONIN$", "w", stdin);
            freopen_s((FILE **)stdout, "CONOUT$", "w", stdout);
            freopen_s((FILE **)stderr, "CONOUT$", "w", stderr);
#endif
        }

        find = strstr(argv[a], "usingCWD=true");
        if (find) {
            usingCWD = true;
        }
    }
}
#endif

int main(int argc, char *argv[])
{
#if RETRO_PLATFORM == RETRO_PS1
    (void)argc;
    (void)argv;
    return rsdkv2App.run();
#else
#if !RETRO_USE_ORIGINAL_CODE
    parseArguments(argc, argv);
#endif

    Engine.Init();
    Engine.Run();

#if !RETRO_USE_ORIGINAL_CODE
    if (Engine.consoleEnabled) {
#if RETRO_PLATFORM == RETRO_WIN && _MSC_VER
        FreeConsole();
#endif
    }
#endif

    return 0;
#endif
}
