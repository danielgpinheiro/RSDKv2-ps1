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
  `PS1_TILE_TEST`, `PS1_SKIP_SPRITES`, plus the forced start in SSZ Act 1 in `start()`.
- `../nugget` is plain upstream; the port must not depend on nugget changes (CD streaming
  lives in `ps1/cdrom_stream.*`, the make-rule fix is in our `Makefile`).
- Current state + maps: `../docs/20-rsdkv2-ps1-port-state.md`. Open problems:
  `../docs/26-port-review-findings.md`. Phases: `../docs/12-rsdkv2-port-plan.md`.

## Build / run / debug (details: `../docs/21-build-and-asset-runbook.md`)
```bash
make -j8                                   # rsdkv2.ps-exe, rsdkv2.elf, rsdkv2.map
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
| `ps1/mdec.cpp`, `ps1/video.cpp` | MDEC/VLC driver (psxpress port) + STR player (heap buffers only while open) |
| `ps1/gpu_chain.*` | DMA-chained renderer helpers (arena per parity, ≤14-word fragments) |
| `ps1/cdrom_stream.*` | continuous CD streaming (`CDRomDevice::Action` subclass, ring buffer) |
| `tools/` | test harness (`run_test.sh`, `frame_capture.sh`, baselines), `rsdkscript` bytecode compiler, FMV tools |
| `ps1/libc/` | Freestanding libc/C++ shims (EASTL-backed `std::`), must stay first on `-I` |
| `*.py` (root) | Build-time asset pipeline (`extract_loose`, `convert_tiles`, `repack_gfx`, `convert_fmv`); `make bytecode` runs `tools/rsdkscript` |
| `build/iso/` | ISO root: `Data/**` loose assets (+ `Data.bin.bak`) |
| `docs/20-*`, `docs/21-*` | Repo-local FMV verification + slowness analysis |
| `.hermes/plans/` | Previous agent's plans (gitignored, useful history) |

## Invariants — don't break these
- **2 MB RAM.** Check `mipsel-none-elf-size` after every change; ~249 KB heap never used in
  the game build (FMV borrows 235 KB while playing). No per-frame `new`/`malloc`. Grep for PC literals after shrinking any
  `#define` (`TILE_COUNT 0x400`, `TILELAYER_CHUNK_W 0x100`, `TILELAYER_CHUNK_H 0x10`,
  `CHUNKTILE_COUNT 0x200*64`, `LAYER_COUNT 4`, `GFXDATA_MAX 0x60000`, `SCRIPTDATA_COUNT 0x8000`).
- **VRAM map**: FB at (0,0) and (0,256) 320×240; CLUT at (0,240); sprite pages pageX 5/7;
  tile pages pageX 9/11 × pageY 0/1. Texture pages must stay at pageX ≥ 5 (pageX is ×64 halfwords).
- **CLUT index 0 = transparent (0x0000); visible black = 0x8000.** Sprites use modulation
  colour 0x80 (not raw mode).
- **Golden rule**: no runtime GIF/LZW/script parsing. Add a build step instead.
- **Script VM guards** (`Div`/`Rand` by 0, opcode bounds) stay in place.
- Hardware access belongs in `Application::start()`/`frame()`, not `prepare()`.
- **Per-frame drawing is DMA-chained** (`ps1/gpu_chain.hh`): no `sendPrimitive`/`clear`/
  `uploadToVRAM` in the frame path; chained fragments ≤ 14 words; immediate uploads call
  `PS1GpuImmediate()` first.
- Verify with `tools/run_test.sh` (60 fps = `PERF last=1`, `CHAIN overflow=0`) and
  `tools/frame_capture.sh 41` against `tools/baseline/ssz_f41_vram.bin`.

## Definition of done for a step
Builds cleanly · fits 2 MB · boots in PCSX-Redux · the feature is verified with evidence
(GDB/VRAM/numbers, and a visual check against the PC reference when relevant) · docs
`../docs/20` (state) and `../docs/12` (phase table) updated · commit only if the user asks.
