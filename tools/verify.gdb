set pagination off
set confirm off
target remote localhost:3333
symbol-file rsdkv2.elf
break PS1VideoNextFrame
commands 1
silent
printf "hit frame=%d readDone=%d readIdx=%d dispIdx=%d\n", s_frameIndex, s_readDone, s_readIdx, s_dispIdx
continue
end
continue
