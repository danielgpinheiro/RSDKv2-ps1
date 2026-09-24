#!/bin/bash
# Layers-only capture for tools/layer_ref.py: object primitives are dropped from frame 1
# (g_ps1DebugDrawMask = 1; scripts still run), the layer state is dumped after RunFrame #N
# and VRAM after #N+1 (frame N's chain has been drawn by then).
# Usage: tools/layer_capture.sh N[,N2,...] OUT_PREFIX [DRAWMASK] [EXTRA_GDB_FILE]
#   several frames: outputs OUT_PREFIX_f<N>.*
#   EXTRA_GDB_FILE: gdb commands run at frame 1 (e.g. pad test-mask breakpoints)
N=$1; OUT=$2; MASK=${3:-1}; EXTRA=$4
PCSX=/Applications/PCSX-Redux.app/Contents/MacOS/PCSX-Redux
GDB=/opt/homebrew/bin/gdb
pkill -f PCSX-Redux 2>/dev/null; sleep 1
{
cat <<GDBEOF
set pagination off
set confirm off
target remote localhost:3333
symbol-file rsdkv2.elf
break PS1PerfTick if g_ps1PerfFrames == 1
continue
set var g_ps1DebugDrawMask = $MASK
delete
GDBEOF
[ -n "$EXTRA" ] && cat "$EXTRA"
for F in ${N//,/ }; do
O=$OUT
[ "$N" != "$F" ] && O=${OUT}_f$F
NEXT=$((F + 1))
cat <<GDBEOF
tbreak PS1PerfTick if g_ps1PerfFrames == $F
continue
printf "CAPTURE frame=%u\n", g_ps1PerfFrames
shell rm -f $O.state
set logging file $O.state
set logging overwrite on
set logging redirect on
set logging enabled on
printf "S frame %u\n", g_ps1PerfFrames
printf "S XScrollOffset %d\n", XScrollOffset
printf "S YScrollOffset %d\n", YScrollOffset
printf "S LastXSize %d\n", LastXSize
printf "S waterDrawPos %d\n", waterDrawPos
printf "S layerScrollPos0 %d\n", StageLayouts[0].scrollPos
printf "S layerScrollPos1 %d\n", StageLayouts[1].scrollPos
printf "S layerScrollPos2 %d\n", StageLayouts[2].scrollPos
printf "S layerScrollPos3 %d\n", StageLayouts[3].scrollPos
printf "S chainWords %u\n", g_ps1ChainLastWords
printf "S chainHigh %u\n", g_ps1ChainHighWater
printf "S chainOverflow %u\n", g_ps1ChainOverflow
printf "S perfLastVsyncs %u\n", g_ps1PerfLastFrameVsyncs
printf "S perfLastHbl %u\\n", g_ps1PerfLastHbl
printf "S perfMaxHbl %u\\n", g_ps1PerfMaxHbl
printf "S layerPrims %u\\n", g_ps1LayerPrims
printf "S layerHbl %u\\n", g_ps1LayerHbl
printf "S tintFrames %u\\n", g_ps1TintFrames
printf "S tintPartial %u\\n", g_ps1TintPartial
printf "S blendSprites %u\\n", g_ps1BlendSprites
printf "S tcScale %d\\n", ObjectEntityList[24].scale
printf "S tcState %d\\n", ObjectEntityList[24].state
printf "S tcInk %d\\n", ObjectEntityList[24].inkEffect
printf "S replayWords %u\\n", g_ps1ChainReplayWords
printf "S tintPartialFrames %u\\n", g_ps1TintPartial
printf "S tintMaskSkip %u\\n", g_ps1TintMaskSkip
printf "S tintApprox %u\\n", g_ps1TintWindowApprox
set logging enabled off
dump binary value $O.state.hparallax HParallax
dump binary value $O.state.tint TintLookupTable1
tbreak PS1PerfTick if g_ps1PerfFrames == $NEXT
continue
shell python3 tools/vramshot.py $O
GDBEOF
done
echo detach
} > /tmp/layer_capture.gdb
$PCSX -run -fastboot -interpreter -debugger -gdb -gdb-port 3333 -webserver \
      -iso "${DISC:-build/game.cue}" -exe rsdkv2.ps-exe >/dev/null 2>&1 &
sleep 3
timeout 1500 $GDB --batch -x /tmp/layer_capture.gdb 2>&1 | grep -E "CAPTURE|fb[01]|rror"
pkill -f PCSX-Redux
cat $OUT*.state | grep -E "frame|Scroll|chain"
