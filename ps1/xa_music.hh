/*
 * CD-XA music (phase 5): plays one channel of Data/Music/Music.xa (tools/audio/build_music.py)
 * through the CD drive's XA-ADPCM decoder into the SPU's CD input. No CPU decoding, no SPU RAM.
 *
 * A long-lived psyqo::CDRomDevice::Action (raw commands, like ps1/cdrom_stream.cpp):
 * Setloc -> Setfilter(1, channel) -> Setmode(2x | XA-ADPCM | 2340-byte sectors | filter) ->
 * ReadS. Audio sectors never reach the CPU; data sectors do (INT1): each is DMA'd into a small
 * buffer, and the channel's end-marker sector either loops the track (Setloc + ReadS) or pauses.
 * One CD action at a time: stop the music before any file read (cdrom_fs does it as a safety
 * net and counts it).
 */
#pragma once

#include <stdint.h>

bool PS1XaInit();                                   // reads Data/Music/Music.bin + finds Music.xa
int  PS1XaFindTrack(const char *path);              // "Data/Music/Nexus/X.ogg" -> channel, -1 if absent,
                                                    // PS1_XA_IN_VIDEO: plays inside the FMV stream (the intro)
#define PS1_XA_IN_VIDEO 0xFF
void PS1XaPlay(int channel, bool loop);             // (re)start a track from its beginning
void PS1XaStop();                                   // pause the drive and wait until the action ended
void PS1XaPause();                                  // PauseSound: stop reading, keep the position
void PS1XaResume();                                 // ResumeSound: ReadS again from the current position
bool PS1XaPlaying();                                // reading (not ended, not paused/stopped)
bool PS1XaEnded();                                  // a non-looping track reached its end marker

extern volatile uint32_t g_ps1XaStarts;      // tracks started
extern volatile uint32_t g_ps1XaLoops;       // loops (end marker -> Setloc + ReadS)
extern volatile uint32_t g_ps1XaEnds;        // non-looping ends
extern volatile uint32_t g_ps1XaDataSectors; // data sectors seen (end markers of any channel)
extern volatile uint32_t g_ps1XaForcedStops; // stopped by a file read (cdrom_fs safety net)
extern volatile uint32_t g_ps1XaLoopFrame;   // g_ps1PerfFrames at the last loop (gap check)
extern volatile uint32_t g_ps1XaStartFrame;  // g_ps1PerfFrames when the current track started
extern volatile int32_t g_ps1XaChannel;      // channel of the current track
