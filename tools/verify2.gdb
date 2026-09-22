set pagination off
set confirm off
target remote localhost:3333
symbol-file rsdkv2.elf
break PS1VideoNextFrame
commands 1
silent
printf "f=%u frame=%d readDone=%d\n", g_ps1GPU->getFrameCount(), s_frameIndex, s_readDone
continue
end
continue
