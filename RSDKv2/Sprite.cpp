#include "RetroEngine.hpp"

#if RETRO_PLATFORM == RETRO_PS1
#include "common/syscalls/syscalls.h"
extern bool ps1SkipSprites; // set in main.cpp — skip GIF decode to speed tile-test boots
#endif

struct GifDecoder {
    int depth;
    int clearCode;
    int eofCode;
    int runningCode;
    int runningBits;
    int prevCode;
    int currentCode;
    int maxCodePlusOne;
    int stackPtr;
    int shiftState;
    int fileState;
    int position;
    int bufferSize;
    uint shiftData;
    uint pixelCount;
    byte buffer[256];
    byte stack[4096];
    byte suffix[4096];
    uint prefix[4096];
};

const int LOADING_IMAGE = 0;
const int LOAD_COMPLETE = 1;
const int LZ_MAX_CODE   = 4095;
const int LZ_BITS       = 12;
const int FIRST_CODE    = 4097;
const int NO_SUCH_CODE  = 4098;

struct GifDecoder GDecoder;
int codeMasks[] = { 0, 1, 3, 7, 15, 31, 63, 127, 255, 511, 1023, 2047, 4095 };

int ReadGifCode(void);
byte ReadGifByte(void);
byte TraceGifPrefix(uint *prefix, int code, int clearCode);

void InitGifDecoder() {
    byte val = 0;
    FileRead(&val, 1);
    GDecoder.fileState      = LOADING_IMAGE;
    GDecoder.position       = 0;
    GDecoder.bufferSize     = 0;
    GDecoder.buffer[0]      = 0;
    GDecoder.depth          = val;
    GDecoder.clearCode      = 1 << val;
    GDecoder.eofCode        = GDecoder.clearCode + 1;
    GDecoder.runningCode    = GDecoder.eofCode + 1;
    GDecoder.runningBits    = val + 1;
    GDecoder.maxCodePlusOne = 1 << GDecoder.runningBits;
    GDecoder.stackPtr       = 0;
    GDecoder.prevCode       = NO_SUCH_CODE;
    GDecoder.shiftState     = 0;
    GDecoder.shiftData      = 0u;
    for (int i = 0; i <= LZ_MAX_CODE; ++i) GDecoder.prefix[i] = (byte)NO_SUCH_CODE;
}
void ReadGifLine(byte *line, int length, int offset) {
    int i         = 0;
    int stackPtr  = GDecoder.stackPtr;
    int eofCode   = GDecoder.eofCode;
    int clearCode = GDecoder.clearCode;
    int prevCode  = GDecoder.prevCode;
    if (stackPtr != 0) {
        while (stackPtr != 0) {
            if (i >= length) {
                break;
            }
            line[offset++] = GDecoder.stack[--stackPtr];
            i++;
        }
    }
    while (i < length) {
        int gifCode = ReadGifCode();
        if (gifCode == eofCode) {
            if (i != length - 1 | GDecoder.pixelCount != 0u) {
                return;
            }
            i++;
        } else {
            if (gifCode == clearCode) {
                for (int j = 0; j <= LZ_MAX_CODE; j++) {
                    GDecoder.prefix[j] = NO_SUCH_CODE;
                }
                GDecoder.runningCode    = GDecoder.eofCode + 1;
                GDecoder.runningBits    = GDecoder.depth + 1;
                GDecoder.maxCodePlusOne = 1 << GDecoder.runningBits;
                prevCode                  = (GDecoder.prevCode = NO_SUCH_CODE);
            } else {
                if (gifCode < clearCode) {
                    line[offset] = (byte)gifCode;
                    offset++;
                    i++;
                } else {
                    if (gifCode < 0 | gifCode > LZ_MAX_CODE) {
                        return;
                    }
                    int code;
                    if (GDecoder.prefix[gifCode] == NO_SUCH_CODE) {
                        if (gifCode != GDecoder.runningCode - 2) {
                            return;
                        }
                        code = prevCode;
                        GDecoder.suffix[GDecoder.runningCode - 2] =
                            (GDecoder.stack[stackPtr++] = TraceGifPrefix(GDecoder.prefix, prevCode, clearCode));
                    } else {
                        code = gifCode;
                    }
                    int c = 0;
                    while (c++ <= LZ_MAX_CODE && code > clearCode && code <= LZ_MAX_CODE) {
                        GDecoder.stack[stackPtr++] = GDecoder.suffix[code];
                        code                         = GDecoder.prefix[code];
                    }
                    if (c >= LZ_MAX_CODE | code > LZ_MAX_CODE) {
                        return;
                    }
                    GDecoder.stack[stackPtr++] = (byte)code;
                    while (stackPtr != 0 && i++ < length) {
                        line[offset++] = GDecoder.stack[--stackPtr];
                    }
                }
                if (prevCode != NO_SUCH_CODE) {
                    if (GDecoder.runningCode < 2 | GDecoder.runningCode > FIRST_CODE) {
                        return;
                    }
                    GDecoder.prefix[GDecoder.runningCode - 2] = prevCode;
                    if (gifCode == GDecoder.runningCode - 2) {
                        GDecoder.suffix[GDecoder.runningCode - 2] = TraceGifPrefix(GDecoder.prefix, prevCode, clearCode);
                    } else {
                        GDecoder.suffix[GDecoder.runningCode - 2] = TraceGifPrefix(GDecoder.prefix, gifCode, clearCode);
                    }
                }
                prevCode = gifCode;
            }
        }
    }
    GDecoder.prevCode = prevCode;
    GDecoder.stackPtr = stackPtr;
}

int ReadGifCode() {
    while (GDecoder.shiftState < GDecoder.runningBits) {
        byte b = ReadGifByte();
        GDecoder.shiftData |= (uint)((uint)b << GDecoder.shiftState);
        GDecoder.shiftState += 8;
    }
    int result = (int)((unsigned long)GDecoder.shiftData & (unsigned long)(codeMasks[GDecoder.runningBits]));
    GDecoder.shiftData >>= GDecoder.runningBits;
    GDecoder.shiftState -= GDecoder.runningBits;
    if (++GDecoder.runningCode > GDecoder.maxCodePlusOne && GDecoder.runningBits < LZ_BITS) {
        GDecoder.maxCodePlusOne <<= 1;
        GDecoder.runningBits++;
    }
    return result;
}

byte ReadGifByte() {
    char c = '\0';
    if (GDecoder.fileState == LOAD_COMPLETE)
        return c;

    byte b;
    if (GDecoder.position == GDecoder.bufferSize) {
        FileRead(&b, 1);
        GDecoder.bufferSize = (int)b;
        if (GDecoder.bufferSize == 0) {
            GDecoder.fileState = LOAD_COMPLETE;
            return c;
        }
        FileRead(GDecoder.buffer, GDecoder.bufferSize);
        b                   = GDecoder.buffer[0];
        GDecoder.position = 1;
    } else {
        b = GDecoder.buffer[GDecoder.position++];
    }
    return b;
}

byte TraceGifPrefix(uint *prefix, int code, int clearCode) {
    int i = 0;
    while (code > clearCode && i++ <= LZ_MAX_CODE) code = prefix[code];

    return code;
}
void ReadGifPictureData(int width, int height, bool interlaced, byte *gfxData, int offset) {
    int array[]  = { 0, 4, 2, 1 };
    int array2[] = { 8, 8, 4, 2 };
    InitGifDecoder();
    if (interlaced) {
        for (int i = 0; i < 4; ++i) {
            for (int j = array[i]; j < height; j += array2[i]) {
                ReadGifLine(gfxData, width, j * width + offset);
            }
        }
        return;
    }
    for (int h = 0; h < height; ++h) ReadGifLine(gfxData, width, h * width + offset);
}

int AddGraphicsFile(const char *filePath) {
    char sheetPath[!RETRO_USE_ORIGINAL_CODE ? 0x100 : 0x40];

    StrCopy(sheetPath, "Data/Sprites/");
    StrAdd(sheetPath, filePath);
#if RETRO_PLATFORM == RETRO_PS1
    // Sprites are pre-converted to .gfx at build time (no LZW decode on PS1): X.gif -> X.gfx.
    // Done before the lookup below so an already-loaded sheet is found under its .gfx name.
    {
        int sl = StrLength(sheetPath);
        if (sl >= 4 && sheetPath[sl - 4] == '.' && sheetPath[sl - 3] == 'g' && sheetPath[sl - 2] == 'i' && sheetPath[sl - 1] == 'f') {
            sheetPath[sl - 2] = 'f';
            sheetPath[sl - 1] = 'x';
        }
    }
#endif
    int sheetID = 0;
    while (StrLength(GfxSurface[sheetID].fileName) > 0) {
        if (StrComp(GfxSurface[sheetID].fileName, sheetPath))
            return sheetID;
        // NOTE
        // This uses SPRITESHEETS_MAX (16), while RemoveGraphicsFile, uses SURFACE_MAX (24)...
        if (++sheetID >= SPRITESHEETS_MAX)
            return 0;
    }
    byte fileExtension = (byte)sheetPath[(StrLength(sheetPath) - 1) & 0xFF];
#if RETRO_PLATFORM == RETRO_PS1
    ramsyscall_printf("[sprite] '%s' id=%d gfxpos=%d\n", sheetPath, sheetID, GfxDataPosition);
#endif
#if RETRO_PLATFORM == RETRO_PS1
    if (ps1SkipSprites) {
        StrCopy(GfxSurface[sheetID].fileName, sheetPath);
        GfxSurface[sheetID].width  = 0;
        GfxSurface[sheetID].height = 0;
        return sheetID;
    }
#endif
    switch (fileExtension) {
        case 'f': LoadGIFFile(sheetPath, sheetID); break;
        case 'p': LoadBMPFile(sheetPath, sheetID); break;
        case 'v': LoadRSVFile(sheetPath, sheetID); break;
        case 'x': LoadGFXFile(sheetPath, sheetID); break;
    }
#if RETRO_PLATFORM == RETRO_PS1
    ramsyscall_printf("[sprite] done id=%d w=%d h=%d gfxpos=%d\n", sheetID, GfxSurface[sheetID].width,
                      GfxSurface[sheetID].height, GfxDataPosition);
#endif

    return sheetID;
}
void RemoveGraphicsFile(const char *filePath, int sheetID) {
    if (sheetID < 0) {
        for (int i = 0; i < SURFACE_MAX; ++i) {
            if (StrLength(GfxSurface[i].fileName) > 0 && StrComp(GfxSurface[i].fileName, filePath))
                sheetID = i;
        }
    }

    if (sheetID >= 0 && StrLength(GfxSurface[sheetID].fileName)) {
        StrCopy(GfxSurface[sheetID].fileName, "");
#if RETRO_PLATFORM == RETRO_PS1
        if (GfxSurface[sheetID].vramDirect15) { // FMV surface: pixels live only in VRAM
            GfxSurface[sheetID].vramDirect15 = false;
            GfxSurface[sheetID].width = GfxSurface[sheetID].height = 0;
            return;
        }
#endif
        int dataPosStart = GfxSurface[sheetID].dataPosition;
        int dataPosEnd   = GfxSurface[sheetID].dataPosition + GfxSurface[sheetID].height * GfxSurface[sheetID].width;
#if RETRO_PLATFORM == RETRO_PS1
        // PC compacts a 2 MB buffer (0x200000); PS1's GraphicData is GFXDATA_MAX, so only move
        // the bytes in use (the PC literal wrote far past the array and trashed RAM).
        for (int i = GfxDataPosition - dataPosEnd; i > 0; --i) GraphicData[dataPosStart++] = GraphicData[dataPosEnd++];
#else
        for (int i = 0x200000 - dataPosEnd; i > 0; --i) GraphicData[dataPosStart++] = GraphicData[dataPosEnd++];
#endif
        GfxDataPosition -= GfxSurface[sheetID].height * GfxSurface[sheetID].width;
        for (int i = 0; i < SURFACE_MAX; ++i) {
            if (GfxSurface[i].dataPosition > GfxSurface[sheetID].dataPosition)
                GfxSurface[i].dataPosition -= GfxSurface[sheetID].height * GfxSurface[sheetID].width;
        }
    }
}

int LoadBMPFile(const char *filePath, byte sheetID) {
    FileInfo info;
    if (LoadFile(filePath, &info)) {
        GFXSurface *surface = &GfxSurface[sheetID];
        StrCopy(surface->fileName, filePath);

        byte fileBuffer = 0;

        SetFilePosition(18);
        FileRead(&fileBuffer, 1);
        surface->width = fileBuffer;
        FileRead(&fileBuffer, 1);
        surface->width += fileBuffer << 8;
        FileRead(&fileBuffer, 1);
        surface->width += fileBuffer << 16;
        FileRead(&fileBuffer, 1);
        surface->width += fileBuffer << 24;

        FileRead(&fileBuffer, 1);
        surface->height = fileBuffer;
        FileRead(&fileBuffer, 1);
        surface->height += fileBuffer << 8;
        FileRead(&fileBuffer, 1);
        surface->height += fileBuffer << 16;
        FileRead(&fileBuffer, 1);
        surface->height += fileBuffer << 24;

        SetFilePosition(info.fileSize - surface->height * surface->width);
        surface->dataPosition = GfxDataPosition;
        byte *gfxData         = &GraphicData[surface->dataPosition + surface->width * (surface->height - 1)];
        for (int y = 0; y < surface->height; ++y) {
            for (int x = 0; x < surface->width; ++x) {
                FileRead(&fileBuffer, 1);
                *gfxData++ = fileBuffer;
            }
            gfxData -= 2 * surface->width;
        }
        GfxDataPosition += surface->height * surface->width;

        if (GfxDataPosition >= GFXDATA_MAX) {
            GfxDataPosition = 0;
            PrintLog("WARNING: Exceeded max gfx size!");
        }

        CloseFile();
        return true;
    }
    return false;
}
int LoadGIFFile(const char *filePath, byte sheetID) {
    FileInfo info;
    if (LoadFile(filePath, &info)) {
        GFXSurface *surface = &GfxSurface[sheetID];
        StrCopy(surface->fileName, filePath);

        byte fileBuffer = 0;
        byte fileBuffer2[2];

        SetFilePosition(6); // GIF89a
        FileRead(&fileBuffer, 1);
        surface->width = fileBuffer;
        FileRead(&fileBuffer, 1);
        surface->width += (fileBuffer << 8);
        FileRead(&fileBuffer, 1);
        surface->height = fileBuffer;
        FileRead(&fileBuffer, 1);
        surface->height += (fileBuffer << 8);

        FileRead(&fileBuffer, 1); // Palette Size
        int has_pallete  = (fileBuffer & 0x80) >> 7;
        int colors       = ((fileBuffer & 0x70) >> 4) + 1;
        int palette_size = (fileBuffer & 0x7) + 1;
        if (palette_size > 0)
            palette_size = 1 << palette_size;

        FileRead(&fileBuffer, 1); // BG Colour index (thrown away)
        FileRead(&fileBuffer, 1); // idk actually (still thrown away)

        int c = 0;
        byte clr[3];
        do {
            ++c;
            FileRead(clr, 3);
        } while (c != palette_size);

        FileRead(&fileBuffer, 1);
        while (fileBuffer != ',') FileRead(&fileBuffer, 1); // gif image start identifier

        FileRead(fileBuffer2, 2);
        FileRead(fileBuffer2, 2);
        FileRead(fileBuffer2, 2);
        FileRead(fileBuffer2, 2);
        FileRead(&fileBuffer, 1);
        bool interlaced = (fileBuffer & 0x40) >> 6;
        if (fileBuffer >> 7 == 1) {
            int c = 128;
            do {
                ++c;
                FileRead(clr, 3);
            } while (c != 256);
        }

        surface->dataPosition = GfxDataPosition;

        GfxDataPosition += surface->width * surface->height;
        if (GfxDataPosition < GFXDATA_MAX) {
            ReadGifPictureData(surface->width, surface->height, interlaced, GraphicData, surface->dataPosition);
        } else {
            GfxDataPosition = 0;
            PrintLog("WARNING: Exceeded max gfx surface size!");
        }

        CloseFile();
        return true;
    }
    return false;
}
#if RETRO_PLATFORM == RETRO_PS1
volatile uint32_t g_ps1SpriteRefused = 0; // GDB-visible: sheets refused by LoadGFXFile
#endif
int LoadGFXFile(const char *filePath, byte sheetID) {
    FileInfo info;
    if (LoadFile(filePath, &info)) {
        GFXSurface *surface = &GfxSurface[sheetID];
        StrCopy(surface->fileName, filePath);
#if RETRO_PLATFORM == RETRO_PS1
        surface->vramPageX    = -1; // a reused sheet slot must not inherit the old VRAM page
        surface->vramDirect15 = false;
#endif

        byte fileBuffer = 0;
        FileRead(&fileBuffer, 1);
        surface->width = fileBuffer << 8;
        FileRead(&fileBuffer, 1);
        surface->width += fileBuffer;
        FileRead(&fileBuffer, 1);
        surface->height = fileBuffer << 8;
        FileRead(&fileBuffer, 1);
        surface->height += fileBuffer;

#if RETRO_PLATFORM == RETRO_PS1
        // Refuse sheets the PS1 can't hold instead of overflowing GraphicData (which wraps to
        // 0 and overwrites loaded sheets) or keeping pixels the GPU path can never draw
        // (PS1UploadSpriteSheet: <= 512x256). The name stays set so later loads of the same
        // sheet hit the lookup; a 0x0 surface is never drawn. Real fix: sprite pipeline phase.
        if (surface->width > 512 || surface->height > 256
            || GfxDataPosition + surface->width * surface->height > GFXDATA_MAX) {
            ramsyscall_printf("[sprite] REFUSED '%s' %dx%d gfxpos=%d\n", filePath, surface->width, surface->height,
                              GfxDataPosition);
            ++g_ps1SpriteRefused;
            surface->width  = 0;
            surface->height = 0;
            CloseFile();
            return false;
        }
#endif
        byte clr[3];
        for (int i = 0; i < 0xFF; ++i) FileRead(&clr, 3); // Palette

        surface->dataPosition = GfxDataPosition;
        byte *gfxData         = &GraphicData[surface->dataPosition];
        byte buf[3];
        while (true) {
            FileRead(&buf[0], 1);
            if (buf[0] == 0xFF) {
                FileRead(&buf[1], 1);
                if (buf[1] == 0xFF) {
                    break;
                } else {
                    FileRead(&buf[2], 1);
                    for (int i = 0; i < buf[2]; ++i) *gfxData++ = buf[1];
                }
            } else {
                *gfxData++ = buf[0];
            }
        }

        GfxDataPosition += surface->height * surface->width;

        if (GfxDataPosition >= GFXDATA_MAX) {
            GfxDataPosition = 0;
            PrintLog("WARNING: Exceeded max gfx size!");
        }

        CloseFile();
        return true;
    }
    return false;
}
#if RETRO_PLATFORM == RETRO_PS1
#include "../ps1/video.hh"
// PS1: Videos/X.rsv is shipped as Videos/X.str (MDEC, built by tools/dump_y4m.py + psxavenc).
// The surface is a 15-bit texture page written straight to VRAM; no GraphicData is used.
int LoadRSVFile(const char *filePath, byte sheetID) {
    static bool mdecReady = false;
    char strPath[0x100];
    StrCopy(strPath, filePath);
    int sl = StrLength(strPath);
    if (sl >= 4 && strPath[sl - 4] == '.') {
        strPath[sl - 3] = 's';
        strPath[sl - 2] = 't';
        strPath[sl - 1] = 'r';
    }
    PS1StopVideo();
    if (!mdecReady) {
        PS1VideoInit();
        mdecReady = true;
    }
    GFXSurface *surface = &GfxSurface[sheetID];
    if (!PS1VideoOpen(strPath)) {
        ramsyscall_printf("[video] can't open '%s' (missing file or heap)\n", strPath);
        return false;
    }
    if (!PS1AllocDirectSurface(sheetID)) {
        ramsyscall_printf("[video] no VRAM page for '%s'\n", strPath);
        PS1VideoClose();
        return false;
    }
    StrCopy(surface->fileName, filePath);
    VideoSurface          = sheetID;
    CurrentVideoFrame     = 0;
    VideoFrameCount       = PS1VideoFrameCount();
    VideoWidth            = PS1VideoWidth();
    VideoHeight           = PS1VideoHeight();
    VideoFilePos          = 0;
    VideoPlaying          = true;
    surface->width        = VideoWidth;
    surface->height       = VideoHeight;
    surface->dataPosition = GfxDataPosition; // nothing reserved
    ramsyscall_printf("[video] '%s' %d frames %dx%d page=%d,%d\n", strPath, VideoFrameCount, VideoWidth, VideoHeight,
                      surface->vramPageX, surface->vramPageY);
    return true;
}
#else
int LoadRSVFile(const char *filePath, byte sheetID) {
    FileInfo info;
    if (LoadFile(filePath, &info)) {
        GFXSurface *surface = &GfxSurface[sheetID];
        StrCopy(surface->fileName, filePath);

        VideoSurface      = sheetID;
        CurrentVideoFrame = 0;

        byte fileBuffer = 0;

        FileRead(&fileBuffer, 1);
        VideoFrameCount = fileBuffer;
        FileRead(&fileBuffer, 1);
        VideoFrameCount += fileBuffer << 8;

        FileRead(&fileBuffer, 1);
        VideoWidth = fileBuffer;
        FileRead(&fileBuffer, 1);
        VideoWidth += fileBuffer << 8;

        FileRead(&fileBuffer, 1);
        VideoHeight = fileBuffer;
        FileRead(&fileBuffer, 1);
        VideoHeight += fileBuffer << 8;

        VideoFilePos          = (int)GetFilePosition();
        VideoPlaying          = true;
        surface->width        = VideoWidth;
        surface->height       = VideoHeight;
        surface->dataPosition = GfxDataPosition;
        GfxDataPosition += surface->width * surface->height;

        if (GfxDataPosition >= GFXDATA_MAX) {
            GfxDataPosition = 0;
            PrintLog("WARNING: Exceeded max gfx size!");
        }

        return true;
    }
    return false;
}
#endif
