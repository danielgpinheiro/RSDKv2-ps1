/*
 * PS1 DMA-chained renderer helpers (psyqo "fragment chaining").
 *
 * Every frame, the engine's draw calls append primitives to a per-parity arena and
 * link them into psyqo's DMA chain; psyqo sends the chain after frame() returns
 * (during the next frame), so CPU game logic and GPU drawing overlap.
 *
 * Rules (psyqo CONCEPTS.md "Fragment chaining"):
 *  - once chaining, ALL per-frame drawing must be chained (no sendPrimitive /
 *    sendFragment / clear / uploadToVRAM while a chain may be in flight);
 *  - a chained fragment is at most 255 words;
 *  - the chain draws into the buffer that becomes current after the flip, so the
 *    clear uses gpu().getNextClear();
 *  - any immediate GPU operation (tile-set / sprite-sheet uploads at load time) must
 *    call PS1GpuImmediate() first, which waits for the in-flight chain to finish.
 */
#pragma once

#include <stdint.h>

#include <psyqo/primitives/common.hh>
#include <psyqo/primitives/quads.hh>
#include <psyqo/primitives/rectangles.hh>
#include <psyqo/primitives/sprites.hh>

namespace psyqo {
class GPU;
}

void PS1ChainBegin(psyqo::GPU &gpu); // start of a frame: select the arena for this parity
void PS1ChainEnd();                  // end of a frame: close the open tile batch
void PS1GpuImmediate();              // wait for the in-flight chain before an immediate GPU op

void PS1ChainClear(uint8_t r = 0, uint8_t g = 0, uint8_t b = 0); // FastFill the next draw buffer
void PS1ChainUploadCLUT(const uint16_t *clut256, int16_t x, int16_t y); // 256-entry CLUT + cache flush
void PS1ChainTPage(psyqo::PrimPieces::TPageAttr attr);         // skipped if identical to the last one
void PS1ChainInvalidateTPage();                                // GPU texpage state unknown
void PS1ChainSprite16(const psyqo::Prim::Sprite16x16 &s);      // batched (tiles)
void PS1ChainSprite(const psyqo::Prim::Sprite &s);
void PS1ChainQuad(const psyqo::Prim::TexturedQuad &q);         // note: quads carry their own texpage
void PS1ChainRect(const psyqo::Prim::Rectangle &r);            // untextured; semi-trans uses the current texpage mode

// Debug/verification counters (read with GDB).
extern volatile uint32_t g_ps1ChainOverflow;  // primitives dropped because the arena was full
extern volatile uint32_t g_ps1ChainHighWater; // max arena words used in one frame
extern volatile uint32_t g_ps1ChainLastWords; // arena words used by the last frame
