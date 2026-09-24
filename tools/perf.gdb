# Attach to PCSX-Redux (-gdb), print the PS1 perf/memory counters, detach (emulation continues).
set pagination off
set confirm off
target remote localhost:3333
symbol-file rsdkv2.elf
printf "PERF frames=%u vsyncs=%u last=%u heapTotal=%u headroom=%u\n", g_ps1PerfFrames, g_ps1PerfVsyncs, g_ps1PerfLastFrameVsyncs, g_ps1HeapTotal, g_ps1HeapHeadroom
printf "CHAIN overflow=%u highwater_words=%u last_words=%u\n", g_ps1ChainOverflow, g_ps1ChainHighWater, g_ps1ChainLastWords
printf "CPU last_hbl=%u max_hbl=%u (262 = 1 frame) layers_hbl=%u layer_prims=%u\n", g_ps1PerfLastHbl, g_ps1PerfMaxHbl, g_ps1LayerHbl, g_ps1LayerPrims
detach
