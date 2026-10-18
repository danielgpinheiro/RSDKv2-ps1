#pragma once
#include <stdint.h>

// The main stack, moved out of the heap (NX-070, from RSDKv3-ps1 CD-092, ../docs/44). nugget's allocator sizes the heap
// up to __stack_start, the RAM top the BIOS stack grows down from: malloc never failed while the heap's last free block
// still overlapped the live stack (Sonic CD's R51A time-warp load handed its 32 KB stream windows out on top of it).
// main() runs the app on this .bss array instead: the heap owns everything up to __stack_start, malloc returns NULL at
// the true end (a read-ahead window that finds no room makes fread return short, ps1/cdrom_fs.cpp, instead of landing
// on live frames), and psyqo_heap_end() / g_ps1HeapHeadroom count bytes that are really free. psyqo's exception handler
// has its own stack (vector.s, 0x1000).
// Deepest stack measured 2,916 B (every test-mode scenario's stack checkpoint, 2026-10-18).
#define PS1_STACK_BYTES (16 * 1024)

extern uint32_t g_ps1Stack[PS1_STACK_BYTES / 4];
extern volatile uint32_t g_ps1StackOverflow; // frames whose check found the stack's bottom guard words overwritten

int PS1RunOnStack(int (*fn)(), void *top); // calls fn with $sp = top (8-byte aligned), returns its result
void PS1StackPaint();                        // fill the array with the paint word (call before PS1RunOnStack)
bool PS1StackGuardBroken();                  // the bottom guard words are no longer paint
uint32_t PS1StackLowWater();                 // lowest address the stack has written (scan; for checkpoints / GDB)
