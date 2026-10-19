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
ps1/gpu_chain.cpp \
ps1/cdrom_stream.cpp \
ps1/irq_guard.cpp \
ps1/sprite_atlas.cpp \
ps1/spu.cpp \
ps1/xa_music.cpp \
ps1/loading.cpp \
ps1/stack.cpp \
ps1/version_stamp.cpp \

# Engine include dir (belt-and-suspenders; quote-includes resolve relative to the .cpp anyway).
# ps1/libc comes FIRST: its C/C++ shim headers (<string>, <cmath>, <stdio.h>, ...)
# shadow any system headers the freestanding toolchain might otherwise surface.
CPPFLAGS = -I ps1/libc -I RSDKv2
# The main stack is a .bss array (ps1/stack.hh, NX-070): the test mode's stack checkpoint measures it there.
CPPFLAGS += -DPS1_SEPARATE_STACK=1
# `make BOOT=SSZ`: skip the intro and force-load SSZ Act 1 (regression harness /
# baselines in tools/). Default: natural boot (Retro logo -> intro FMV -> title).
# `make MEMCARD_TEST=1`: memory card self-test at boot (phase 8, RSDKv2/main.cpp; run it with
# tools/memcard/selftest.sh, which uses temporary card images).
# Nexus never saves, so the card layer (ps1/memcard*.cpp) is only compiled for this test; a port
# that saves adds ps1/memcard.cpp to SRCS.
ifeq ($(MEMCARD_TEST),1)
CPPFLAGS += -DPS1_MEMCARD_TEST=1
SRCS += ps1/memcard.cpp ps1/memcard_test.cpp
endif

ifeq ($(BOOT),SSZ)
CPPFLAGS += -DPS1_BOOT_SSZ=1
endif

# `make TEST=1`: the regression test mode (ps1/test_mode.cpp, ../docs/45; the scenario comes from the test disc's
# Data/TEST.SEL, written by ../tests/run.py): harness only, never for make dist. Rebuilds main.o and relinks when
# switched (build/gen/test.txt).
ifeq ($(TEST),1)
CPPFLAGS += -DPS1_TEST=1
SRCS += ps1/test_mode.cpp
endif
$(shell mkdir -p build/gen; [ -f build/gen/test.txt ] && [ "$$(cat build/gen/test.txt)" = "$(TEST)" ] || { echo "$(TEST)" > build/gen/test.txt; rm -f RSDKv2/main.o rsdkv2.elf; })
# `make TEST=1 PROFILE=1`: work counters for the regression suite's work scenarios (../tests, docs/45 section 9):
# g_ps1VmOps (script VM instructions, RSDKv2/Script.cpp) and g_ps1ObjCalls (objects updated, RSDKv2/Object.cpp).
# Harness only (make dist refuses a TEST build). Rebuilds those two objects when switched (build/gen/profile.txt).
ifeq ($(PROFILE),1)
CPPFLAGS += -DPS1_PROFILE=1
endif
$(shell mkdir -p build/gen; [ -f build/gen/profile.txt ] && [ "$$(cat build/gen/profile.txt)" = "$(PROFILE)" ] || { echo "$(PROFILE)" > build/gen/profile.txt; rm -f RSDKv2/Script.o RSDKv2/Object.o rsdkv2.elf; })
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
# nugget's hardware-measured memcpy/memset (8-word / 4-word blocks) instead of the small
# default ones. The FMV copies 20 KB per video frame; loads and MEM_ZERO also benefit.
LDFLAGS   += -Wl,-wrap,memcpy -Wl,-wrap,memset

# The FMV Huffman (VLC) decoder is the hot loop of video playback: optimise it for speed.
# Target-specific CPPFLAGS land after the global -Os (make's C++ rule: CXXFLAGS CPPFLAGS).
ps1/mdec.o: CPPFLAGS += -O2
RSDKv2/Drawing.o: CPPFLAGS += -O2

# Cancel make's built-in `%: %.o` link rule. Without this, the extensionless libc shim
# headers (ps1/libc/string, vector, ...) listed in .dep files are treated as targets and
# `string` gets rebuilt from `string.o`, overwriting the header. (Previously worked around
# with a local -MP edit to nugget/common.mk.)
%: %.o

# Build-time RetroScript compiler (host tool) -> per-stage bytecode on the ISO tree.
# Re-run whenever a .txt script, GameConfig/StageConfig or SCRIPTDATA/JUMPTABLE sizes change.
# The whole asset pipeline from the original game (tools/build_assets.py): Sonic Nexus Data.bin ->
# build/iso/Data (+ build/fmv-src, build/manifest). Needs an empty build/iso. Reproducible.
DATA ?= ../Nexus/Data.bin
assets:
	/usr/bin/python3 tools/build_assets.py --data $(DATA) --out .

bytecode:
	$(MAKE) -C tools/rsdkscript
	mkdir -p build/iso/Data/Scripts/Bytecode
	tools/rsdkscript/rsdkscript build/iso build/iso/Data/Scripts/Bytecode --manifest build/manifest

# Build-time sprite atlas per stage (tools/atlas/build_atlas.py, needs PIL + numpy from the
# system python): every frame a stage can draw, packed 4/8-bit into VRAM pages + CLUT maps.
atlas: bytecode
	/usr/bin/python3 tools/atlas/build_atlas.py build/iso/Data build/manifest
	/usr/bin/python3 tools/bgstrips/build_bgstrips.py build/iso/Data

# Build-time audio (phase 5): Data/SoundFX/**/*.wav -> .vag (SPU-ADPCM, psxavenc).
audio:
	/usr/bin/python3 tools/audio/build_sfx.py build/iso/Data
	/usr/bin/python3 tools/audio/build_music.py build/iso/Data
	/usr/bin/python3 tools/fmv/build_str.py build/iso/Data

# Disc image (mkpsxiso BIN/CUE, MODE2 for CD-XA): build/game.bin + build/game.cue.
disc:
	/usr/bin/python3 tools/disc/build_disc.py
	/usr/bin/python3 tools/disc/verify_disc.py

# Distribution image (phase 9): the verified disc as build/dist/SonicNexus-PS1.bin/.cue (NTSC-U
# license). Build the natural game first (`make -j8`, not BOOT=SSZ / MEMCARD_TEST).
dist: disc
	@test -f rsdkv2.elf || { echo "dist: no rsdkv2.elf (a TEST= switch removed it): run make first"; exit 1; }
	@if mipsel-none-elf-nm rsdkv2.elf | grep -q g_ps1TestBuild; then echo "dist: rsdkv2.ps-exe is a TEST=1 build"; exit 1; fi
	mkdir -p build/dist
	cp build/game.bin build/dist/SonicNexus-PS1.bin
	printf 'FILE "SonicNexus-PS1.bin" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n' > build/dist/SonicNexus-PS1.cue
	cd build/dist && shasum -a 256 SonicNexus-PS1.bin SonicNexus-PS1.cue > SHA256SUMS && cat SHA256SUMS

# Loading icon (phase 5b): ../sonic-spinning.gif -> ps1/loading_icon.h (committed; rerun after
# changing the GIF).
icon:
	/usr/bin/python3 tools/loading/build_icon.py

# Memory card icons (phase 8): ../Memcard icons/*-mc-icon.png -> ps1/memcard_icons.h (committed).
mcicons:
	/usr/bin/python3 tools/memcard/build_icons.py

.PHONY: assets bytecode atlas audio disc dist icon mcicons

# Regression suite (../docs/44 ledger, ../docs/45 design, ../tests/README.md): `make test` = this port's quick tier (run it
# before every commit), `make test-full` = every scenario on both emulators, perf and memory gates (after engine,
# interpreter, renderer or perf changes). The suite builds in its own shadow copy (../.rt-<port>), never here.
.PHONY: test test-full
test:
	/usr/bin/python3 ../tests/run.py --port nexus
test-full:
	/usr/bin/python3 ../tests/run.py --tier full --port nexus
