/*
 * PS1 STR video playback (MDEC + VLC).
 *
 * Streams a psxavenc `-t strv` file (BS v2, 2048-byte sectors, 10 sectors/frame)
 * via the stdio FILE* shim, Huffman-decodes each frame with DecDCTvlcStart2, then
 * the MDEC hardware does RLE+IDCT+YUV->RGB into a 15bpp BGR555 buffer.
 */
#pragma once

#include <stdint.h>

bool      PS1VideoInit();                 // MDEC reset + VLC table build (once)
bool      PS1VideoOpen(const char *path); // open the STR, read w/h
bool      PS1VideoNextFrame();            // decode the next frame into the pixel buffer
void      PS1VideoClose();
uint16_t *PS1VideoPixels();               // w*h BGR555 buffer
int       PS1VideoWidth();
int       PS1VideoHeight();
