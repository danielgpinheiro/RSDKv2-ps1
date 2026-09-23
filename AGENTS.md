# AGENTS.md — RSDKv2-ps1 (Sonic Nexus on PlayStation)

Operating manual for this repo. The workspace-level rules are in `../AGENTS.md`, the
knowledge base is `../docs/` (start at `../docs/README.md`).

## What this is
RSDKv2-Decompilation (Retro Engine v2, Sonic Nexus 2008) ported to the PS1 with
**psyqo** (C++20) on branch `ps1`. Remotes: `origin` = upstream (read-only for us),
`fork` = `github.com/danielgpinheiro/RSDKv2-ps1` (push here).

## First checks (every session)
- `git status` / `git log --oneline -10`: uncommitted work may belong to another agent.
- `RSDKv2/main.cpp` switches: `PS1_VIDEO_TEST` (FMV self-test),
  `PS1_TILE_TEST`, `PS1_SKIP_SPRITES`. Default build boots naturally (NIntro);
  `make BOOT=SSZ` (after `touch RSDKv2/main.cpp`) forces SSZ Act 1 for the harness.
- `../nugget` is plain upstream; the port must not depend on nugget changes (CD streaming
  lives in `ps1/cdrom_stream.*`, the make-rule fix is in our `Makefile`).
- Current state + maps: `../docs/20-rsdkv2-ps1-port-state.md`. Open problems:
  `../docs/26-port-review-findings.md`. Phases: `../docs/12-rsdkv2-port-plan.md`.

## Build / run / debug (details: `../docs/21-build-and-asset-runbook.md`)
```bash
make -j8                                   # rsdkv2.ps-exe, rsdkv2.elf, rsdkv2.map (natural boot)
mipsel-none-elf-size rsdkv2.elf            # must fit retail 2 MB
rm -f build/game.iso && hdiutil makehybrid -iso -joliet -o build/game.iso build/iso
timeout 60 /Applications/PCSX-Redux.app/Contents/MacOS/PCSX-Redux -run -fastboot \
  -iso build/game.iso -exe rsdkv2.ps-exe -stdout -logfile /tmp/rsdkv2.log &
# GDB: add -interpreter -debugger -gdb -gdb-port 3333, then
# /opt/homebrew/bin/gdb --batch -x verify.gdb   (target remote localhost:3333; symbol-file rsdkv2.elf)
```

## Layout
| Path | Role |
|---|---|
| `Makefile` | psyqo build (`TYPE=ps-exe`, `-msoft-float -lgcc`, `RAM_SIZE=2M`, includes `../nugget/psyqo/psyqo.mk`) |
| `RSDKv2/` | Engine; PS1 code behind `#if RETRO_PLATFORM == RETRO_PS1` (Drawing.cpp, Scene.*, RetroEngine.*, Sprite.cpp, main.cpp, …) |
| `RSDKv2/main.cpp` | `RSDKv2PS1App : psyqo::Application`, test switches, `Engine.RunFrame()` per frame |
| `RSDKv2/Drawing.cpp` (PS1 block at the top, `DrawStageGfx`, `DrawSprite`, `DrawRotatedSprite`) | GPU renderer: CLUT, tile/sprite pages, chained prims |
| `ps1/cdrom_fs.cpp` | ISO9660 walker + `FILE*` shim over `psyqo::CDRomDevice` |
| `ps1/mdec.cpp`, `ps1/video.cpp` | MDEC/VLC driver (psxpress port, `-O2`) + STR player: heap buffers only while open, budgeted decode-ahead for the game (`PS1VideoService/Decode`) |
| `RSDKv2/Video.cpp`, `Sprite.cpp` `LoadRSVFile` (PS1) | `LoadVideo`/`NextVideoFrame` → `Intro.str` as a 15-bit VRAM texture surface (`vramDirect15`) |
| `ps1/gpu_chain.*` | DMA-chained renderer helpers (arena per parity, ≤14-word fragments) |
| `ps1/cdrom_stream.*` | continuous CD streaming (`CDRomDevice::Action` subclass, ring buffer) |
| `tools/` | test harness (`run_test.sh`, `frame_capture.sh`, baselines), `rsdkscript` bytecode compiler, FMV tools |
| `ps1/libc/` | Freestanding libc/C++ shims (EASTL-backed `std::`), must stay first on `-I` |
| `*.py` (root) | Build-time asset pipeline (`extract_loose`, `convert_tiles`, `repack_gfx`, `convert_gfx --loose`, `convert_fmv`); `make bytecode` runs `tools/rsdkscript` |
| `build/iso/` | ISO root = the disc: `Data/**` loose assets + `PSX.EXE` only |
| `build/fmv-src/`, `build/Data.bin.*` | build inputs kept off the disc (`Intro.rsv`, reference `.mdec`, datapack backups) |
| `.hermes/plans/` | Previous agent's plans (gitignored, useful history) |

## Invariants — don't break these
- **2 MB RAM.** Check `mipsel-none-elf-size` after every change; ~239 KB heap never used in
  SSZ; during the intro FMV (235 KB borrowed) only ~14 KB is left. No per-frame `new`/`malloc`. Grep for PC literals after shrinking any
  `#define` (`TILE_COUNT 0x400`, `TILELAYER_CHUNK_W 0x100`, `TILELAYER_CHUNK_H 0x10`,
  `CHUNKTILE_COUNT 0x200*64`, `LAYER_COUNT 4`, `GFXDATA_MAX 0x60000`, `SCRIPTDATA_COUNT 0x8000`).
- **VRAM map**: FB at (0,0) and (0,256) 320×240; CLUT at (0,240); sprite page slots pageX 5/7
  × pageY 0/1 (reset every stage load; the FMV texture takes slots 2+3); tile pages pageX
  9/11 × pageY 0/1. Texture pages must stay at pageX ≥ 5 (pageX is ×64 halfwords).
- **CLUT index 0 = transparent (0x0000); visible black = 0x8000.** Sprites use modulation
  colour 0x80 (not raw mode).
- **Golden rule**: no runtime GIF/LZW/script parsing. Add a build step instead. (Known
  exception to remove in phase 3d: player sheets via `LoadGIFFile` in `Animation.cpp`.)
- **Sprite sheets**: `X.gif` loads `X.gfx`; sheets > 512×256 or overflowing `GraphicData`
  are refused (`g_ps1SpriteRefused`). Anything that sizes a copy by a PC literal
  (`0x200000`…) instead of `GFXDATA_MAX`/`GfxDataPosition` corrupts RAM.
- **Script VM guards** (`Div`/`Rand` by 0, opcode bounds) stay in place.
- Hardware access belongs in `Application::start()`/`frame()`, not `prepare()`.
- **Per-frame drawing is DMA-chained** (`ps1/gpu_chain.hh`): no `sendPrimitive`/`clear`/
  `uploadToVRAM` in the frame path; chained fragments ≤ 14 words; immediate uploads call
  `PS1GpuImmediate()` first. `uploadToVRAM` widths must be even (psyqo aborts).
- **One CD action at a time**: stop the FMV (`PS1StopVideo`) before any file read; stage
  loads already do.
- Verify with `tools/run_test.sh` (60 fps = `PERF last=1`, `CHAIN overflow=0`) and
  `tools/frame_capture.sh 41` against `tools/baseline/ssz_f41_vram.bin` (both on a `BOOT=SSZ` build).
  Intro/FMV checks: natural build, checkpoints listed in `../docs/20` (verification tools).

## Definition of done for a step
Builds cleanly · fits 2 MB · boots in PCSX-Redux · the feature is verified with evidence
(GDB/VRAM/numbers, and a visual check against the PC reference when relevant) · docs
`../docs/20` (state) and `../docs/12` (phase table) updated · commit only if the user asks.
