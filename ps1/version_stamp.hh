/*
 * Build stamp: the git commit of the build (VERSION.TXT at the disc root, written by tools/disc/build_disc.py with
 * tools/gen_version.py) drawn in the bottom-left corner of the title screen (NTitle), e.g. "v a1b2c3d4" / "v a1b2c3d4-dirty",
 * "v unknown" when the disc has no VERSION.TXT.
 *
 * PS1VersionLoad (boot, before the first load) reads the file with one sector read into a stack buffer (no heap,
 * no read-ahead window) and uploads the text once as a 4-bit texture: rows 248-255 of VRAM, x 0-31, its CLUT at
 * (32, 248). Those rows are 4-bit atlas CLUT slots (filled from row 240 on; the title's atlas takes 9 slots, rows
 * 240-241) and rows 504-511 hold the INK_BLEND CLUTs, so the texture is valid on the title only: the renderer
 * (FlipScreen, after PS1FinalizePalette) re-uploads it on the title's first frame (PS1VersionUpload) and then chains
 * it each title frame (PS1VersionDraw: texpage + a dark shadow sprite + the light text sprite). Kept compact: the
 * SSZ heap floor (tests nexus-natural-flow, NX-045) left ~870 B for it.
 */
#pragma once

#include <stdint.h>

void PS1VersionLoad();   // boot: VERSION.TXT -> the stamp text (32-byte static buffer) + texture upload
void PS1VersionUpload(); // the texture + CLUT again (immediate upload: waits for the in-flight chain)
const char *PS1VersionText(); // "v a1b2c3d4" (GDB / tests)

// Chains the stamp with its text's top-left at x, y: texpage + two sprites of the same texels, a dark grey shadow one
// pixel down-right, then the light grey text, both modulated by `level` (0x80 = unfaded).
void PS1VersionDraw(int x, int y, uint8_t level);

#define PS1_VERSION_TEXT_H 7

extern volatile uint32_t g_ps1VersionFound;   // 1 = VERSION.TXT was read
extern volatile uint32_t g_ps1VersionUploads; // texture uploads (boot + each return to the title)
extern volatile uint32_t g_ps1VersionDraws;   // frames the stamp was drawn
