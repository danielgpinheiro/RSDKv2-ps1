#!/bin/bash
# Memory card self-test (phase 8) on temporary card images — never the player's cards.
# Needs a `make MEMCARD_TEST=1` build (+ make disc). Port 1 = formatted card, port 2 = unformatted.
# PCSX-Redux stores -memcard1/-memcard2 in ~/.config/pcsx-redux/pcsx.json, so this script backs the
# file up and restores it on exit. Evidence:
#   - GDB: g_ps1McTest* counters + per-step error codes (ps1/memcard_test.cpp);
#   - card 1 snapshots after the save steps (3, 9) checked with tools/memcard/mcd.py;
#   - the file is gone at the end, card 2 is still all zero;
#   - the pad still works after the card transfers (Cross via tools/pad.lua -> g_ps1PadButtons).
# Usage: tools/memcard/selftest.sh [WORKDIR]
set -u
cd "$(dirname "$0")/../.."
W=${1:-/tmp/rsdk_memcard_test}; mkdir -p "$W"
PCSX=/Applications/PCSX-Redux.app/Contents/MacOS/PCSX-Redux
GDB=${GDB:-tools/gdbw}
CFG="$HOME/.config/pcsx-redux"
MCD="python3 tools/memcard/mcd.py"
mipsel-none-elf-nm rsdkv2.elf | grep -q PS1McSelfTest || { echo "not a MEMCARD_TEST=1 build"; exit 1; }

cp -p "$CFG/pcsx.json" "$W/pcsx.json.bak"
shasum "$CFG"/memcard*.mcd > "$W/cards.sha" 2>/dev/null
restore() { cp -p "$W/pcsx.json.bak" "$CFG/pcsx.json"; }
trap restore EXIT

$MCD format "$W/mc1.mcd" >/dev/null && $MCD raw "$W/mc2.mcd" >/dev/null
cat > "$W/selftest.gdb" <<GDBEOF
set pagination off
set confirm off
target remote localhost:3333
symbol-file rsdkv2.elf
break PS1McTestHook
commands
silent
printf "STEP %u err=%u passed=%u\n", step, g_ps1McTestErr[step], g_ps1McTestPassed
if step == 3 || step == 9
  eval "shell cp $W/mc1.mcd $W/mc1_step%u.mcd", step
end
continue
end
tbreak PS1PerfTick if g_ps1PerfFrames == 120
continue
printf "RESULT steps=%u passed=%u firstFail=%u lastError=%u done=%u\n", g_ps1McTestSteps, g_ps1McTestPassed, g_ps1McTestFirstFail, g_ps1McTestLastError, g_ps1McTestDone
printf "PAD before connected=%u buttons=%04x\n", g_ps1PadConnected, g_ps1PadButtons
shell curl -s 'http://localhost:8080/api/v1/lua/pad?press=Cross' >/dev/null
tbreak PS1PerfTick if g_ps1PerfFrames == 130
continue
printf "PAD pressed connected=%u buttons=%04x\n", g_ps1PadConnected, g_ps1PadButtons
shell curl -s 'http://localhost:8080/api/v1/lua/pad?release=Cross' >/dev/null
tbreak PS1PerfTick if g_ps1PerfFrames == 140
continue
printf "PAD released connected=%u buttons=%04x\n", g_ps1PadConnected, g_ps1PadButtons
detach
GDBEOF
pkill -f PCSX-Redux 2>/dev/null; sleep 1
$PCSX -fastboot -interpreter -debugger -gdb -gdb-port 3333 -webserver -exec "dofile('$PWD/tools/pad.lua')" \
      -memcard1 "$W/mc1.mcd" -memcard2 "$W/mc2.mcd" \
      -iso "${DISC:-build/game.cue}" -exe rsdkv2.ps-exe >/dev/null 2>&1 &
sleep 4
timeout 1200 $GDB --batch -x "$W/selftest.gdb" 2>&1 | grep -E "^(STEP|RESULT|PAD)"
pkill -f PCSX-Redux; sleep 1
restore

echo "--- card 1 after the first save (step 3)"
$MCD check "$W/mc1_step3.mcd" BAHBRW-00000TEST --title "RSDK SAVE TEST" --icon s_ps1McIconSonicCD --pattern 3000
echo "--- card 1 after the 2-block replacement (step 9)"
$MCD check "$W/mc1_step9.mcd" BAHBRW-00000TEST --title "RSDK SAVE TEST" --icon s_ps1McIconSonic2 --pattern 9000
echo "--- card 1 at the end"
$MCD absent "$W/mc1.mcd" BAHBRW-00000TEST && $MCD list "$W/mc1.mcd"
echo "--- card 2 (unformatted) untouched"
cmp -s "$W/mc2.mcd" <(head -c 131072 /dev/zero) && echo "card 2: all zero" || echo "card 2: WRITTEN"
echo "--- player's PCSX-Redux config and cards"
cmp -s "$W/pcsx.json.bak" "$CFG/pcsx.json" && echo "pcsx.json restored" || echo "pcsx.json DIFFERS"
shasum "$CFG"/memcard*.mcd 2>/dev/null | diff -q - "$W/cards.sha" >/dev/null && echo "memcard1/2.mcd unchanged" || echo "memcard1/2.mcd CHANGED"
