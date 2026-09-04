#include "dolphin/types.h"

u32 lbl_1_bss_A48;

void fn_1_8934 (u32 seed) {
    lbl_1_bss_A48 = seed;
}

s32 fn_1_8944(void) {
    return (lbl_1_bss_A48 = (lbl_1_bss_A48 * 0x41C64E6D) + 0x3039) / 65536 & 0x7FFF;
}