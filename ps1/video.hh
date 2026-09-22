/*
 * PS1 STR video playback (MDEC + VLC).
 *
 * Streams a psxavenc `-t strv` file (BS v2, 2048-byte sectors, 10 sectors/frame)
 * via the CD-ROM asynchronously (double-buffered), Huffman-decodes each frame
 * with DecDCTvlcStart2, then the MDEC hardware does RLE+IDCT+YUV->RGB into a
 * 15bpp BGR555 buffer.
 */
#pragma once

#include <stdint.h>

bool      PS1VideoInit();                 // MDEC reset + VLC table build (once)
bool      PS1VideoOpen(const char *path); // open the STR, start the async read
int       PS1VideoNextFrame();            // 1 = decoded a frame, 0 = still reading, -1 = EOF
void      PS1VideoClose();
uint16_t *PS1VideoPixels();               // w*h BGR555 buffer
int       PS1VideoWidth();
int       PS1VideoHeight();
