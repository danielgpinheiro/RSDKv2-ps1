#include "RetroEngine.hpp"

#if RETRO_PLATFORM == RETRO_PS1
#include <psyqo/application.hh>
#include <psyqo/gpu.hh>
#include "common/syscalls/syscalls.h"

psyqo::GPU *g_ps1GPU = nullptr;

void PS1PresentFramebuffer(ushort *fb) {
    if (!g_ps1GPU)
        return;
    unsigned parity = g_ps1GPU->getParity();
    int16_t  y      = (parity == 0) ? 0 : 256;
    g_ps1GPU->uploadToVRAM(fb, psyqo::Rect{{0, y}, {DEFAULT_SCREEN_XSIZE, SCREEN_YSIZE}});
}

class RSDKv2PS1App final : public psyqo::Application {
  public:
    void prepare() override {
        syscall_puts("[RSDKv2-PS1] boot ok; calling Engine.Init()\n");

        gpu().initialize(psyqo::GPU::Configuration{}
                             .set(psyqo::GPU::Resolution::W320)
                             .set(psyqo::GPU::VideoMode::AUTO)
                             .set(psyqo::GPU::ColorMode::C15BITS)
                             .set(psyqo::GPU::Interlace::PROGRESSIVE)
                             .set(psyqo::GPU::MiscSetting::CLEAR_VRAM));

        g_ps1GPU = &gpu();

        Engine.Init();
        syscall_puts("[RSDKv2-PS1] Engine.Init() returned (Phase 2 milestone)\n");

        // Phase 3 test: grayscale palette + horizontal-gradient framebuffer.
        for (int i = 0; i < 256; ++i)
            SetPaletteEntry((byte)i, (byte)i, (byte)i, (byte)i);
        for (int yy = 0; yy < SCREEN_YSIZE; ++yy)
            for (int xx = 0; xx < DEFAULT_SCREEN_XSIZE; ++xx)
                Engine.FrameBuffer[yy * DEFAULT_SCREEN_XSIZE + xx] = (byte)(xx & 0xFF);
    }

    void frame() override {
        // Task 1.3 verification: route the blit through FlipScreen().
        Engine.ColourMode = 1;               // 16-bit path (FlipScreen case 1)
        Engine.GameMode   = ENGINE_MAINGAME; // != EXITGAME so FlipScreen runs
        FlipScreen();
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
