# RSDKv2 on PlayStation — Sonic Nexus for the original PS1

A port of the [RSDKv2 decompilation](https://github.com/RSDKModding/RSDKv2-Decompilation) (Retro
Engine v2, Sonic Nexus 2008) to the original Sony PlayStation, built with
[psyqo](https://github.com/pcsx-redux/nugget/tree/main/psyqo) and developed against
[PCSX-Redux](https://github.com/grumpycoders/pcsx-redux). Target: a retail console with **2 MB RAM**.

The upstream decompilation README (PC build) is kept as [README-upstream.md](README-upstream.md).
Players who only want a disc image should use the public
[Sonic Nexus PS1 ISO Builder](https://github.com/danielgpinheiro/Sonic-Nexus-PS1-ISO-Builder)
(prebuilt executable + asset tools; you bring the game data).

## Status

| Area | State |
|---|---|
| Boot, CD file system, game loop | ✅ natural boot: Retro Engine logo → intro video → title → Sunset Shore Zone |
| Rendering | ✅ fully DMA-chained GPU renderer at 60 fps: tile layers with PC-exact per-line parallax, per-stage sprite atlas in VRAM, rotation/scaling, fades, tint and blend effects |
| Input | ✅ `psyqo::AdvancedPad` |
| Audio | ✅ own SPU driver (sound effects as SPU-ADPCM), music as CD-XA streamed by the drive, intro music interleaved in the video |
| Video | ✅ intro as MDEC/STR with XA audio |
| Loading | ✅ spinning-Sonic loading icon; read-ahead + load-order disc layout (boot → SSZ ≈ 23 s emulated) |
| Memory card | ✅ generic save layer (`ps1/memcard.*`) for later ports — Sonic Nexus itself saves nothing |
| Distribution disc | ✅ trimmed, byte-verified BIN/CUE (`make dist`) |
| Real hardware | ⏸ not tested yet |

Rule of the port: **no asset decoding at runtime.** GIFs, scripts, audio and video are converted
at build time into formats the PS1 hardware uses directly (4/8-bit VRAM pages, bytecode,
SPU-ADPCM, CD-XA, MDEC bitstreams).

## Building

Requirements:
- `mipsel-none-elf-gcc` (C++20) and [nugget](https://github.com/pcsx-redux/nugget) checked out
  next to this repo (`../nugget`, plain upstream);
- Python 3 with Pillow and numpy, a C++17 host compiler, ffmpeg/ffprobe,
  [psxavenc](https://codeberg.org/WonderfulToolchain/psxavenc),
  [mkpsxiso](https://github.com/Lameguy64/mkpsxiso) (on PATH, or via the `PSXAVENC`/`MKPSXISO`/
  `FFMPEG`/`FFPROBE`/`CXX` environment variables);
- the Sonic Nexus 2008 `Data.bin` ([Sonic Retro](https://info.sonicretro.org/Sonic_Nexus));
- optionally a Sony license file (`LICENSE.DAT`, never committed) for retail consoles.

```bash
make -j8                                  # rsdkv2.ps-exe (+ .elf, .map); must fit 2 MB
make assets DATA=/path/to/Data.bin        # whole asset pipeline -> build/iso/Data (reproducible)
make disc                                 # mkpsxiso BIN/CUE + tools/disc/verify_disc.py
make dist                                 # build/dist/SonicNexus-PS1.bin/.cue + SHA256SUMS
```

`make BOOT=SSZ` boots straight into Sunset Shore Zone (regression harness);
`make MEMCARD_TEST=1` builds the memory card self-test. Details of every stage are in the tools'
docstrings and in [AGENTS.md](AGENTS.md).

## Testing (PCSX-Redux)

```bash
tools/run_test.sh               # BOOT=SSZ build: 60 fps / chain counters over GDB
tools/frame_capture.sh 41 OUT   # frame 41 VRAM vs tools/baseline/ssz_f41_vram.bin
tools/load_profile.sh natural LOG   # per-load / per-file CD request profile
tools/memcard/selftest.sh       # memory card self-test on temporary card images
```

## Credits

- **Retro Engine / RSDK**: Christian "Taxman" Whitehead.
- **RSDKv2 decompilation**: Rubberduckycooly and RMGRich (RSDKModding).
- **Sonic Nexus** (2008): the Sonic Nexus team — see [Sonic Retro](https://info.sonicretro.org/Sonic_Nexus).
- **psyqo / nugget / PCSX-Redux**: the PCSX-Redux authors (MIT). **EASTL**: Electronic Arts (BSD-3).
- **[ps1-bare-metal](https://github.com/spicyjpeg/ps1-bare-metal)** (SPU and CD-ROM driver model): spicyjpeg (MIT).
- **psxpress** MDEC/VLC decoder (ported in `ps1/mdec.cpp`): [PSn00bSDK](https://github.com/Lameguy64/PSn00bSDK) contributors (MPL-2.0).
- **[mkpsxiso](https://github.com/Lameguy64/mkpsxiso)**: Lameguy64 and contributors (GPL-2.0).
  **[psxavenc](https://codeberg.org/WonderfulToolchain/psxavenc)**: Ben "GreaseMonkey" Russell and Adrian "asie" Siekierka.
- **[psx-spx](https://psx-spx.consoledev.net/)** hardware documentation: Martin "nocash" Korth and contributors.
- Loading icon: "Sonic-Spinning" GIF by [SoniczheFan87686](https://tenor.com/en-GB/users/soniczhefan87686).

## License

The decompilation license applies ([LICENSE.md](LICENSE.md)): **non-commercial use only**, credit the
original and decompilation authors, and **no game assets may be distributed** — this repo contains
none; build them from your own copy of the game. Sonic the Hedgehog is a trademark of SEGA.
