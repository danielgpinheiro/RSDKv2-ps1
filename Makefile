# RSDKv2 (Sonic Nexus) — PlayStation 1 port
#
# Builds a .ps-exe via the psyqo toolchain (mipsel-none-elf, C++20).
# Run `make` from this directory; output lands in the repo-root (bin/ not used yet).
#
# Phase 2 of the port: get the RetroEngine core to compile and boot through
# RetroEngine::Init() with graphics/audio/input stubbed out.

# The name of the binary to be created.
TARGET = rsdkv2
# ps-exe is the standard bootable PS1 executable.
TYPE = ps-exe

# List of source files to compile within the binary.
# NOTE: RSDKv2/fcaseopen.c is intentionally omitted — it is only needed when
# FORCE_CASE_INSENSITIVE is defined, which the PS1 port does not use (CD-ROM
# ISO9660 file access is handled elsewhere in Phase 6).
SRCS = \
RSDKv2/Animation.cpp \
RSDKv2/Audio.cpp \
RSDKv2/Collision.cpp \
RSDKv2/Debug.cpp \
RSDKv2/Drawing.cpp \
RSDKv2/Ini.cpp \
RSDKv2/Input.cpp \
RSDKv2/main.cpp \
RSDKv2/Math.cpp \
RSDKv2/ModAPI.cpp \
RSDKv2/Object.cpp \
RSDKv2/Palette.cpp \
RSDKv2/Player.cpp \
RSDKv2/Reader.cpp \
RSDKv2/RetroEngine.cpp \
RSDKv2/Scene.cpp \
RSDKv2/Script.cpp \
RSDKv2/Sprite.cpp \
RSDKv2/String.cpp \
RSDKv2/Text.cpp \
RSDKv2/Userdata.cpp \
RSDKv2/Video.cpp \
ps1/libc/string.cpp \
ps1/libc/stdlib.cpp \
ps1/libc/math.cpp \
ps1/libc/stdio.cpp \
ps1/cdrom_fs.cpp \
ps1/mdec.cpp \
ps1/video.cpp \
ps1/cdrom_stream.cpp \

# Engine include dir (belt-and-suspenders; quote-includes resolve relative to the .cpp anyway).
# ps1/libc comes FIRST: its C/C++ shim headers (<string>, <cmath>, <stdio.h>, ...)
# shadow any system headers the freestanding toolchain might otherwise surface.
CPPFLAGS = -I ps1/libc -I RSDKv2
# C++20 is the minimum required by psyqo.
CXXFLAGS = -std=c++20

# This activates the PSYQo library and the rest of the toolchain.
include ../nugget/psyqo/psyqo.mk

# PS1 R3000A has NO FPU, but RSDKv2 uses float/double extensively. Force
# soft-float so the compiler emits libgcc helper calls instead of FPU
# instructions (which would hang/crash on hardware). Must come AFTER the
# include so it overrides common.mk's -mfp32 ARCHFLAGS.
CPPFLAGS += -msoft-float
LDFLAGS   += -msoft-float
# -nostdlib (from common.mk) drops libgcc, but -msoft-float emits calls to
# libgcc's software FP routines (__mulsf3, __divsf3, ...). Re-link it.
LDFLAGS   += -lgcc
# Real retail PS1 = 2MB RAM. The 8MB debug override is removed; the link must fit
# the 2MB `ram` region (EXE loads at 0x80010000, so ~1.94MB usable).
LDFLAGS   += -Wl,--defsym,RAM_SIZE=2M

# Cancel make's built-in `%: %.o` link rule. Without this, the extensionless libc shim
# headers (ps1/libc/string, vector, ...) listed in .dep files are treated as targets and
# `string` gets rebuilt from `string.o`, overwriting the header. (Previously worked around
# with a local -MP edit to nugget/common.mk.)
%: %.o
