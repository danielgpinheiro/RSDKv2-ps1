#include "RetroEngine.hpp"

int CurrentVideoFrame = 0;
int VideoFrameCount   = 0;
int VideoWidth        = 0;
int VideoHeight       = 0;
int VideoSurface      = 0;
int VideoFilePos      = 0;
bool VideoPlaying     = false;

#if RETRO_PLATFORM == RETRO_PS1
#include "../ps1/video.hh"
#include "../ps1/gpu_chain.hh"
#include <psyqo/gpu.hh>
extern psyqo::GPU *g_ps1GPU;

// PS1: the .rsv (LZW GIF per frame) is replaced at build time by an MDEC STR stream
// (LoadRSVFile). NextVideoFrame only asks for a frame; PS1UpdateVideoTexture() decodes it
// when its sectors have arrived and uploads it into the surface's 15-bit texture page.
void UpdateVideoFrame() {
    if (VideoPlaying)
        PS1VideoRequestFrame();
}

volatile uint32_t g_ps1VideoWaitHbl   = 0; // last frame: wait for the in-flight chain (hblanks)
volatile uint32_t g_ps1VideoUploadHbl = 0; // last frame: 16 slice uploads to VRAM (hblanks)

void PS1StopVideo() {
    if (PS1VideoIsOpen())
        PS1VideoClose(); // the last frame stays in VRAM (PC keeps its last frame too)
    VideoPlaying = false;
}

void PS1UpdateVideoTexture() {
    if (!PS1VideoIsOpen()) {
        VideoPlaying = false; // closed elsewhere (e.g. cdrom_fs safety net)
        return;
    }
    uint32_t start = PS1Hblanks(); // the video's share of this game frame starts here
    int r          = PS1VideoService();
    if (r < 0) {
        PS1StopVideo();
        return;
    }
    GFXSurface *surface = &GfxSurface[VideoSurface];
    uint16_t *pix       = PS1VideoPixels();
    if (r > 0 && surface->vramDirect15 && pix) {
        // MDEC emits 16-pixel-wide column slices (column-major macroblocks): one upload each.
        int16_t x   = (int16_t)(surface->vramPageX * 64);
        int16_t y   = (int16_t)(surface->vramPageY * 256);
        int h       = PS1VideoHeight();
        uint32_t t0 = PS1Hblanks();
        PS1GpuImmediate(); // the previous frame's chain may still be texturing from this page
        g_ps1VideoWaitHbl = PS1HblanksSince(t0);
        t0                = PS1Hblanks();
        for (int i = 0; i < PS1VideoWidth() / 16; i++)
            g_ps1GPU->uploadToVRAM(pix + i * h * 16, psyqo::Rect{{(int16_t)(x + 16 * i), y}, {16, (int16_t)h}});
        g_ps1VideoUploadHbl = PS1HblanksSince(t0);
        ++CurrentVideoFrame;
    }
    if (PS1VideoDecode(start) < 0)
        PS1StopVideo(); // EOF (the game asked past the last frame) or drive error
}
#else
void UpdateVideoFrame() {
    if (VideoPlaying) {
        if (VideoFrameCount <= CurrentVideoFrame) {
            VideoPlaying = false;
            CloseFile();
        } else {
            GFXSurface *surface = &GfxSurface[VideoSurface];
            byte fileBuffer     = 0;
            ushort fileBuffer2  = 0;
            FileRead(&fileBuffer, 1);
            VideoFilePos += fileBuffer;
            FileRead(&fileBuffer, 1);
            VideoFilePos += fileBuffer << 8;
            FileRead(&fileBuffer, 1);
            VideoFilePos += fileBuffer << 16;
            FileRead(&fileBuffer, 1);
            VideoFilePos += fileBuffer << 24;

            byte clr[3];
            for (int i = 0; i < 0x80; ++i) {
                FileRead(&clr, 3);
                SetPaletteEntry(i, clr[0], clr[1], clr[2]);
            }
            SetPaletteEntry(0, 0x00, 0x00, 0x00);

            FileRead(&fileBuffer, 1);
            while (fileBuffer != ',') FileRead(&fileBuffer, 1); // gif image start identifier

            FileRead(&fileBuffer2, 2); // IMAGE LEFT
            FileRead(&fileBuffer2, 2); // IMAGE TOP
            FileRead(&fileBuffer2, 2); // IMAGE WIDTH
            FileRead(&fileBuffer2, 2); // IMAGE HEIGHT
            FileRead(&fileBuffer, 1);  // PaletteType
            bool interlaced = (fileBuffer & 0x40) >> 6;
            if (fileBuffer >> 7 == 1) {
                int c = 0x80;
                do {
                    ++c;
                    FileRead(&fileBuffer, 1);
                    FileRead(&fileBuffer, 1);
                    FileRead(&fileBuffer, 1);
                } while (c != 0x100);
            }
            ReadGifPictureData(surface->width, surface->height, interlaced, GraphicData, surface->dataPosition);

            SetFilePosition(VideoFilePos);
            ++CurrentVideoFrame;
        }
    }
}
#endif
