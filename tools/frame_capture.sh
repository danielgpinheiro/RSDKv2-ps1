#!/bin/bash
# Deterministic capture: halt the game right after RunFrame #N (PS1PerfTick) and dump VRAM.
# The engine is frame-driven, so frame N's image is comparable across builds regardless of fps.
# Usage: tools/frame_capture.sh N OUT_PREFIX [BASELINE_PNG]
N=$1; OUT=$2; BASE=$3
PCSX=/Applications/PCSX-Redux.app/Contents/MacOS/PCSX-Redux
GDB=/opt/homebrew/bin/gdb
pkill -f PCSX-Redux 2>/dev/null; sleep 1
cat > /tmp/frame_capture.gdb <<GDBEOF
set pagination off
set confirm off
target remote localhost:3333
symbol-file rsdkv2.elf
break PS1PerfTick if g_ps1PerfFrames == $N
continue
printf "CAPTURE frame=%u vsyncs=%u\n", g_ps1PerfFrames, g_ps1PerfVsyncs
shell python3 tools/vramshot.py $OUT $BASE
detach
GDBEOF
$PCSX -run -fastboot -interpreter -debugger -gdb -gdb-port 3333 -webserver \
      -iso "${DISC:-build/game.cue}" -exe rsdkv2.ps-exe >/dev/null 2>&1 &
sleep 3
timeout 600 $GDB --batch -x /tmp/frame_capture.gdb 2>&1 | grep -E "CAPTURE|fb[01]"
pkill -f PCSX-Redux
