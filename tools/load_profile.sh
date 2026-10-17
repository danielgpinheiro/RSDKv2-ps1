#!/bin/bash
# Load-time profile: log every file-system CD request (PS1FsProfileHook: LBA, sectors, time) and
# every load end (PS1LoadingDoneHook), then print per-load / per-file tables (load_profile.py).
# Usage: tools/load_profile.sh ssz|natural OUT.log [--order ORDER.txt]
#   --order: also write the first-open order of the disc files (tools/disc/load_order.txt, the
#            disc layout build_disc.py follows)
#   ssz     : BOOT=SSZ build; boot load + SSZ stage load + first-frame tile set (stops at frame 3)
#   natural : natural build; boot, NIntro, Start at F700 (intro skip -> NTitle), Start at F1300
#             (title -> SSZ), stops at F1900
# The emulated CD timing is PCSX-Redux's model (not hardware): compare request counts first.
MODE=$1; OUT=${2:-/tmp/load_profile.log}; shift 2 2>/dev/null; EXTRA=("$@")
PCSX=/Applications/PCSX-Redux.app/Contents/MacOS/PCSX-Redux
GDB=${GDB:-tools/gdbw}
case $MODE in ssz) STOP=3 ;; natural) STOP=1900 ;; *) echo "usage: $0 ssz|natural OUT.log"; exit 1 ;; esac
G=/tmp/load_profile.gdb
cat > $G <<GDBEOF
set pagination off
set confirm off
target remote localhost:3333
symbol-file rsdkv2.elf
break PS1FsProfileHook
commands
silent
printf "REQ lba=%u n=%u us=%u frame=%u\n", lba, count, us, g_ps1PerfFrames
continue
end
break PS1FsPreloadDir
commands
silent
printf "PRELOAD %s\n", dir
continue
end
break fopen
commands
silent
printf "OPEN %s\n", path
continue
end
break PS1LoadingDoneHook
commands
silent
printf "LOAD us=%u frame=%u requests=%u sectors=%u readUs=%u headroom=%u\n", us, g_ps1PerfFrames, g_ps1FsRequests, g_ps1FsSectors, g_ps1FsReadUs, g_ps1LoadingHeadroom
continue
end
GDBEOF
if [ "$MODE" = natural ]; then
  for f in "700 0x80" "710 0" "1300 0x80" "1310 0"; do set -- $f
    printf 'break PS1PerfTick if g_ps1PerfFrames == %s\ncommands\nsilent\nset var g_ps1PadTestMask = %s\ncontinue\nend\n' $1 $2 >> $G
  done
fi
cat >> $G <<GDBEOF
tbreak PS1PerfTick if g_ps1PerfFrames == $STOP
continue
printf "DONE frame=%u vsyncs=%u requests=%u sectors=%u readUs=%u headroom=%u videoForced=%u xaStarts=%u\n", g_ps1PerfFrames, g_ps1PerfVsyncs, g_ps1FsRequests, g_ps1FsSectors, g_ps1FsReadUs, g_ps1HeapHeadroom, g_ps1VideoForcedClose, g_ps1XaStarts
detach
GDBEOF
pkill -f PCSX-Redux 2>/dev/null; sleep 1
$PCSX -fastboot -interpreter -debugger -gdb -gdb-port 3333 \
      -iso "${DISC:-build/game.cue}" -exe rsdkv2.ps-exe >/dev/null 2>&1 &
sleep 4
timeout 3000 $GDB --batch -x $G > "$OUT" 2>&1
pkill -f PCSX-Redux
python3 "$(dirname "$0")/load_profile.py" "$OUT" "${DISC_BIN:-build/game.bin}" "${EXTRA[@]}"
