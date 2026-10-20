// GPU type detection + the flip-rectangle readback test (ps1/gpu_detect.hh). Same routine as RSDKv3-ps1 / RSDKv4-ps1
// ps1/render.cpp PS1DetectGpu.
#include "gpu_detect.hh"

#include "psyqo/hardware/gpu.hh"

volatile uint32_t g_ps1GpuType = 0, g_ps1GpuQuery7 = 0, g_ps1FlipTest = 0, g_ps1FlipRectsOn = 1, g_ps1FlipRectsForce = 0;
volatile uint32_t g_ps1TileFlipRects = 0, g_ps1SpriteFlipRects = 0, g_ps1SpriteFlipQuads = 0;
bool g_ps1FlipRectsGpu = false, g_ps1FlipFrameUsed = false;

void PS1DetectGpu()
{
    using namespace psyqo::Hardware;
    auto ready = []() { while (!(GPU::Ctrl & (1 << 26))) {} };
    auto query = [](uint32_t index) { GPU::Ctrl = 0x10000000 | index; return (uint32_t)GPU::Data; };
    ready();
    const uint32_t area0 = query(3), area1 = query(4), offset = query(5), stat = GPU::Ctrl;
    // psx-spx "GPU Detection" (Perfect Assassin's routine): index 4 is latched first, so an old GPU, which leaves the
    // latch alone on index 7, can't answer 2 with a stale value (the draw offset, latched last above, could be 2).
    GPU::Ctrl = 0x10000004;
    const uint32_t q7 = query(7);
    uint32_t type     = 2; // new 208-pin GPU (LATE-PU-8 and up, every PSone; DuckStation >= a9c01a6)
    if ((q7 & 0xFFFFFF) != 2) {
        ready();
        GPU::Data = 0xE6000000; // mask bits off: GPUSTAT bit 12 below can only come from the texpage write
        GPU::Data = 0xE1001000 | (GPU::Ctrl & 0x3FFF);
        (void)(uint32_t)GPU::Data; // the routine's dummy read
        type = (GPU::Ctrl >> 12) & 1; // 1: an unknown (arcade / prototype) GPU, 0: the old 160-pin one
    }
    g_ps1GpuQuery7 = q7;
    g_ps1GpuType   = type;
    if (type) {
        // the corner of VRAM (x 1016-1023, y 511): texels u 60-63 of the 15-bit page at x 960, y 256; free at boot
        ready();
        GPU::Data = 0xA0000000;                  // CPU -> VRAM
        GPU::Data = (511u << 16) | 1020u;
        GPU::Data = (1u << 16) | 4u;
        GPU::Data = 0x56781234u;                 // u 60 = 1234h, u 61 = 5678h
        GPU::Data = 0x0F0F0F0Fu;                 // u 62, 63
        ready();
        GPU::Data = 0xE3000000;                  // drawing area: all of VRAM, no offset
        GPU::Data = 0xE4000000 | (511u << 10) | 1023u;
        GPU::Data = 0xE5000000;
        GPU::Data = 0xE1000000 | 0x1000 | 0x400 | 0x100 | 0x10 | 15; // X flip, draw to display, 15-bit, page (960, 256)
        GPU::Data = 0x01000000;                  // clear the texture cache
        GPU::Data = 0x65808080;                  // textured raw rect, variable size
        GPU::Data = (511u << 16) | 1016u;
        GPU::Data = (255u << 8) | 61u;           // u0 61, v 255
        GPU::Data = (1u << 16) | 2u;
        ready();
        GPU::Data = 0x01000000;
        GPU::Data = 0xC0000000;                  // VRAM -> CPU: the two drawn pixels
        GPU::Data = (511u << 16) | 1016u;
        GPU::Data = (1u << 16) | 2u;
        while (!(GPU::Ctrl & (1 << 27))) {}
        g_ps1FlipTest     = (uint32_t)GPU::Data;
        g_ps1FlipRectsGpu = g_ps1FlipTest == 0x12345678u; // pixel 1016 = u 61, 1017 = u 60: mirrored
    }
    ready();
    GPU::Data = 0xE1000000 | (stat & 0x7FF); // the texpage as it was (GPUSTAT bits 0-10), flip bits clear
    GPU::Data = 0xE6000000 | ((stat >> 11) & 3); // the mask bits as they were (GPUSTAT bits 11-12)
    GPU::Data = 0xE3000000 | (area0 & 0xFFFFF);
    GPU::Data = 0xE4000000 | (area1 & 0xFFFFF);
    GPU::Data = 0xE5000000 | (offset & 0x3FFFFF);
}
