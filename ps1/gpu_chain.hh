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
#include <psyqo/primitives/misc.hh>
#include <psyqo/primitives/sprites.hh>

namespace psyqo {
class GPU;
}

void PS1ChainBegin(psyqo::GPU &gpu); // start of a frame: select the arena for this parity
void PS1ChainEnd();                  // end of a frame: close the open tile batch
void PS1GpuImmediate();              // wait for the in-flight chain before an immediate GPU op

// FastFill the next draw buffer. Returns the chained primitive (its colour may still be
// changed until the frame ends: the chain is sent after frame() returns), or nullptr.
psyqo::Prim::FastFill *PS1ChainClear(uint8_t r = 0, uint8_t g = 0, uint8_t b = 0);
void PS1ChainUploadCLUT(const uint16_t *clut256, int16_t x, int16_t y); // 256-entry CLUT + cache flush
void PS1ChainUploadRow(const uint16_t *data, int n, int16_t x, int16_t y); // n (even) halfwords, no flush
void PS1ChainFlushCache();                                     // after CLUT/texture changes
// Copy the last finished frame (the displayed buffer) into the buffer this frame draws into,
// so a frame that draws nothing (paused game) shows the same image in both buffers.
void PS1ChainRepeatLastFrame();
// Chains a row upload of `count` (even) halfwords whose data is written later, before the
// frame ends: keeps its early place in the chain. Returns the number of chunks (data pointers
// in chunkData[], halfword counts in chunkLen[]), 0 if the arena is full.
int PS1ChainReserveRow(int count, int16_t x, int16_t y, uint16_t **chunkData, uint16_t *chunkLen, int maxChunks);
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
