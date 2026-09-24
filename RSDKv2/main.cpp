#include "../ps1/loading.hh"
#include "RetroEngine.hpp"

#if RETRO_PLATFORM == RETRO_PS1
#include <psyqo/application.hh>
#include <psyqo/gpu.hh>
#include <psyqo/cdrom-device.hh>
#include <psyqo/advancedpad.hh>
#include "common/syscalls/syscalls.h"
#include "../ps1/video.hh"
#include "../ps1/gpu_chain.hh"

#define PS1_TILE_TEST 0 // TEMP: synthetic tile-path test (Phase 2 verification)
#define PS1_SKIP_SPRITES 0 // TEMP: skip GIF sprite decode to speed tile-test boots
#define PS1_VIDEO_TEST 0 // TEMP: FMV Stage 3 — decode+display the first STR frame

#include <psyqo/alloc.h>

// Perf/memory counters for GDB-based verification (tools/perf.gdb).
extern "C" char __heap_start, __stack_start;
volatile uint32_t g_ps1PerfFrames          = 0; // frames run
volatile uint32_t g_ps1PerfVsyncs          = 0; // vsyncs elapsed across those frames
volatile uint32_t g_ps1PerfLastFrameVsyncs = 0; // vsyncs taken by the last frame (1 = 60 fps)
volatile uint32_t g_ps1HeapTotal           = 0; // bytes between bss end and the stack
volatile uint32_t g_ps1HeapHeadroom        = 0; // bytes never reached by the heap high-water mark
static uint32_t   s_ps1PerfLastCount       = 0;
volatile uint32_t g_ps1PerfVideoFrames     = 0; // game frames run while a video was open
volatile uint32_t g_ps1PerfVideoMaxVsyncs  = 0; // slowest of those (1 = 60 fps)
volatile uint32_t g_ps1PerfVideoSlow       = 0; // those that took more than 1 vsync
volatile uint32_t g_ps1HeapVideoHeadroom   = 0; // heap headroom right after the last video frame
volatile uint32_t g_ps1PerfVideoMaxHbl     = 0; // longest frame() CPU time while a video plays (hblanks, 262 = 1 frame)
volatile uint32_t g_ps1PerfLastHbl         = 0; // frame() CPU time of the last frame (hblanks)
volatile uint32_t g_ps1PerfMaxHbl          = 0; // longest frame() CPU time (hblanks; GDB may reset it)
volatile uint32_t g_ps1PerfOverHbl         = 0; // frames whose CPU time exceeded one frame (262 hblanks)
extern volatile uint32_t g_ps1LayerHbl;
volatile uint32_t g_ps1PerfMaxLayerHbl     = 0; // tile-layer CPU time in the longest frame
static bool       s_ps1PerfVideoTick       = false;

__attribute__((noinline)) static void PS1PerfTick(psyqo::GPU &gpu) {
    uint32_t now = gpu.getFrameCount();
    if (g_ps1PerfFrames) {
        uint32_t d = now - s_ps1PerfLastCount;
        g_ps1PerfVsyncs += d;
        g_ps1PerfLastFrameVsyncs = d;
        if (s_ps1PerfVideoTick && PS1VideoIsOpen()) {
            g_ps1PerfVideoFrames++;
            if (d > g_ps1PerfVideoMaxVsyncs)
                g_ps1PerfVideoMaxVsyncs = d;
            if (d > 1)
                g_ps1PerfVideoSlow++;
        }
    }
    s_ps1PerfLastCount = now;
    g_ps1PerfFrames++;
    uintptr_t hs  = (uintptr_t)&__heap_start;
    uintptr_t ss  = (uintptr_t)&__stack_start;
    uintptr_t top = (uintptr_t)psyqo_heap_end();
    if (top < hs)
        top = hs;
    g_ps1HeapTotal    = ss - hs;
    g_ps1HeapHeadroom = ss - top;
    if (s_ps1PerfVideoTick)
        g_ps1HeapVideoHeadroom = g_ps1HeapHeadroom;
}

psyqo::GPU         *g_ps1GPU   = nullptr;
psyqo::AdvancedPad *g_ps1Pad   = nullptr; // Input.cpp ReadInputDevice (PS1 branch)
psyqo::CDRomDevice *g_ps1CDRom = nullptr;
bool ps1SkipSprites = PS1_SKIP_SPRITES;


static void PS1VideoSelfTestInit() {
    syscall_puts("[RSDKv2-PS1] video self-test: init + open Intro.str\n");
    PS1VideoInit();
    if (!PS1VideoOpen("Data/Sprites/Videos/Intro.str"))
        syscall_puts("[RSDKv2-PS1] video self-test: FAILED to open Intro.str\n");
}

static void PS1VideoSelfTest() {
    // Advance the video as fast as the CD stream delivers frames (poll every tick).
    int r = PS1VideoNextFrame(); // 1 = decoded, 0 = still reading, -1 = EOF
    if (r < 0) {
        // EOF -> loop the video from the start (buffers are freed and re-allocated).
        PS1VideoClose();
        if (!PS1VideoOpen("Data/Sprites/Videos/Intro.str"))
            return;
    }

    // MDEC emits 16-pixel-wide vertical slices (column-major). Upload each
    // slice at its x offset instead of treating the buffer as row-major.
    uint16_t *pix   = PS1VideoPixels();
    if (!pix)
        return;
    unsigned parity = g_ps1GPU->getParity();
    int16_t  y      = (parity == 0) ? 0 : 256;
    for (int i = 0; i < 16; i++)
        g_ps1GPU->uploadToVRAM(pix + i * 160 * 16, psyqo::Rect{{(int16_t)(32 + 16 * i), (int16_t)(y + 40)}, {16, 160}});
}

class RSDKv2PS1App final : public psyqo::Application {
  public:
    psyqo::CDRomDevice m_cdrom;
    psyqo::AdvancedPad m_pad;

    // psyqo lifecycle: prepare() runs with IRQs disabled — only configure objects
    // here (GPU init, CDRomDevice::prepare). All hardware I/O (CD reset, file loads,
    // Engine.Init) happens in start(), which runs with IRQs enabled.
    void prepare() override {
        gpu().initialize(psyqo::GPU::Configuration{}
                             .set(psyqo::GPU::Resolution::W320)
                             .set(psyqo::GPU::VideoMode::AUTO)
                             .set(psyqo::GPU::ColorMode::C15BITS)
                             .set(psyqo::GPU::Interlace::PROGRESSIVE)
                             .set(psyqo::GPU::MiscSetting::CLEAR_VRAM));

        g_ps1GPU = &gpu();
        m_cdrom.prepare();
    }

    void start() override {
        syscall_puts("[RSDKv2-PS1] boot ok; calling Engine.Init()\n");

#if PS1_TILE_TEST
        // Synthetic tile test: skip CD-ROM + Engine.Init — the test fills
        // TileGfx/TilePalette itself. Temporary for Phase 2 verification.
        return;
#endif

        m_cdrom.resetBlocking(gpu());
        g_ps1CDRom = &m_cdrom;
        // Pads: psyqo::AdvancedPad polls SIO0 itself from psyqo's frame loop. Not SimplePad:
        // the BIOS pad driver (InitPAD/StartPAD) reads both pads inside the vblank IRQ with long
        // busy-waits, and with that in the BIOS IRQ chain psyqo's blocking CD reads aborted
        // ("CDRomDevice::irq() called with no action" / "invoked with unknown cause": the CD
        // IRQ was dispatched while psyqo polled it with the CD IRQ masked). Fast = both ports
        // every vsync (pad 1 at 60 Hz).
        m_pad.initialize(psyqo::AdvancedPad::PollingMode::Fast);
        g_ps1Pad = &m_pad;

#if PS1_VIDEO_TEST
        PS1VideoSelfTestInit();
        return;
#endif

#if PS1_BOOT_SSZ
        // `make BOOT=SSZ`: force-load SSZ Act 1 (skips the intro) for the regression
        // harness and the tools/baseline captures.
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
#endif

        PS1LoadingBegin(); // boot load (scripts, config, global SFX, music table)
        Engine.Init();
        PS1LoadingEnd();
        syscall_puts("[RSDKv2-PS1] Engine.Init() returned\n");
    }

    void frame() override {
#if PS1_TILE_TEST
        PS1ChainBegin(gpu());
        PS1TileSelfTest();
        PS1ChainEnd();
        return;
#endif
#if PS1_VIDEO_TEST
        PS1VideoSelfTest();
        PS1PerfTick(gpu());
        return;
#endif
        // FMV: decode + upload the next video frame (immediate VRAM upload, so before the
        // chain starts). Counted only when the video was already open (not its load frame).
        s_ps1PerfVideoTick = PS1VideoIsOpen();
        uint32_t t0        = PS1Hblanks();
        if (s_ps1PerfVideoTick)
            PS1UpdateVideoTexture();
        PS1ChainBegin(gpu()); // all per-frame drawing is DMA-chained (ps1/gpu_chain.hh)
        PS1AudioUpdate();     // frees SFX channels whose sample ended (before the scripts run)
        PS1SpuDebugService();
        Engine.RunFrame();
        PS1ChainEnd();        // psyqo sends the chain after frame() returns
        uint32_t cpu     = PS1HblanksSince(t0);
        g_ps1PerfLastHbl = cpu;
        if (cpu > g_ps1PerfMaxHbl) {
            g_ps1PerfMaxHbl      = cpu;
            g_ps1PerfMaxLayerHbl = g_ps1LayerHbl;
        }
        if (cpu > 262)
            g_ps1PerfOverHbl = g_ps1PerfOverHbl + 1;
        if (s_ps1PerfVideoTick && PS1VideoIsOpen()) {
            if (cpu > g_ps1PerfVideoMaxHbl)
                g_ps1PerfVideoMaxHbl = cpu;
        }
        PS1PerfTick(gpu());
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
