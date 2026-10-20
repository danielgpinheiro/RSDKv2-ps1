#pragma once
// GPU type detection and flip-bit rectangles (docs/43 G3; nocash psx-spx "GPU Versions" / "GPU Render Rectangle
// Commands"): GP0(E1h) bits 12 / 13 mirror textured rectangles on the new 208-pin GPU (LATE-PU-8 and up, every PSone)
// and not on the old 160-pin one, so flipped tiles and sprites were textured quads. PS1DetectGpu (main.cpp start(),
// before anything is drawn) runs psx-spx's GPU detection (v0 old: never flip bits) and, on v1 / v2, a readback test (an
// emulator can answer v2 and not draw the flips): flip rectangles are used only when both pass.
#include <stdint.h>

extern volatile uint32_t g_ps1GpuType;    // psx-spx's GPU version: 0 old, 1 unknown (arcade / prototype), 2 new
extern volatile uint32_t g_ps1GpuQuery7;  // the raw GP1(10h) index 7 answer
extern volatile uint32_t g_ps1FlipTest;   // the readback word (12345678h = mirrored; 0 = not tested: type 0)
extern volatile uint32_t g_ps1FlipRectsOn;    // GDB / test poke: 0 forces the quads
extern volatile uint32_t g_ps1FlipRectsForce; // 1 forces flip rects (harness: CPU cost on emulators that don't draw them)
extern bool g_ps1FlipRectsGpu;                // the GPU draws them (type >= 1 and the readback mirrored)
extern volatile uint32_t g_ps1TileFlipRects, g_ps1SpriteFlipRects, g_ps1SpriteFlipQuads; // totals (tests)
extern bool g_ps1FlipFrameUsed; // this frame chained a texpage with flip bits (PS1ChainEnd clears them)

void PS1DetectGpu();

static inline __attribute__((always_inline)) bool PS1FlipRectsLive()
{
    return (g_ps1FlipRectsGpu || g_ps1FlipRectsForce) && g_ps1FlipRectsOn;
}

// Texcoord of a 1:1 mirrored piece (u, v, w x h) drawn as a flip rectangle: the rectangle decrements its texcoords on a
// flipped axis, so it starts at the piece's last texel (u + w - 1 / v + h - 1). Returns false where the quad stays:
// - X flip ignores Texcoord.X's LSB and starts at an odd X: u + w - 1 must be odd (u + w even);
// - an unflipped X axis wants an even Texcoord.X (odd ones glitch every 4th / 8th pixel on hardware), so u even there;
// - the 1:1 quad it replaces (UV edges u + w - 1 .. u - 1 on a mirrored axis, u .. u + w on the other) must be exact:
//   every edge within a byte (0 .. 255).
static inline bool PS1FlipRectTex(int u, int v, int w, int h, bool flipX, bool flipY, int *tu, int *tv)
{
    int u0 = u - flipX, u1 = u + w - flipX, v0 = v - flipY, v1 = v + h - flipY; // the quad's UV edges
    *tu = flipX ? u1 : u;
    *tv = flipY ? v1 : v;
    return !((u0 | u1 | v0 | v1) & ~0xFF) && (*tu & 1) == flipX; // edges in a byte; X flip odd, else even
}
