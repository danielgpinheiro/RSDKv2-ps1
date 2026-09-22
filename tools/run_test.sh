#!/bin/bash
# Boot rsdkv2.ps-exe in PCSX-Redux, sample the perf counters twice (WINDOW s apart),
# dump VRAM, kill the emulator. Run from the RSDKv2-ps1 root.
# Usage: tools/run_test.sh OUT_PREFIX [BOOT_WAIT=40] [WINDOW=5] [BASELINE_PREFIX]
OUT=${1:-/tmp/rsdk_test}; BOOT=${2:-40}; WIN=${3:-5}; BASE=$4
PCSX=/Applications/PCSX-Redux.app/Contents/MacOS/PCSX-Redux
GDB=/opt/homebrew/bin/gdb
pkill -f PCSX-Redux 2>/dev/null; sleep 1
$PCSX -run -fastboot -interpreter -debugger -gdb -gdb-port 3333 -webserver \
      -iso build/game.iso -exe rsdkv2.ps-exe -stdout -logfile "$OUT.log" >/dev/null 2>&1 &
sleep "$BOOT"
timeout 20 $GDB --batch -x tools/perf.gdb 2>/dev/null | grep -E "PERF|CHAIN"
sleep "$WIN"
timeout 20 $GDB --batch -x tools/perf.gdb 2>/dev/null | grep -E "PERF|CHAIN"
python3 tools/vramshot.py "$OUT" $BASE
pkill -f PCSX-Redux
grep -a "RSDKv2-PS1" "$OUT.log" | tail -5
