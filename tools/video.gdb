# FMV self-test counters (PS1_VIDEO_TEST 1).
set pagination off
set confirm off
target remote localhost:3333
symbol-file rsdkv2.elf
printf "VIDEO ticks=%u vsyncs=%u frames=%u resync=%u restarts=%u discarded=%u\n", g_ps1PerfFrames, g_ps1PerfVsyncs, g_ps1VideoFrames, g_ps1VideoResync, g_ps1StreamRestarts, g_ps1StreamDiscarded
detach
