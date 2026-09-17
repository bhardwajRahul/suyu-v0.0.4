#pragma once
#include <stdint.h>
/* Synthetic instructions only: no executable or cartridge input. */
static const uint32_t smoke_code[] = {
    0x91000400, 0x17ffffff, /* 1000: add x0,x0,#1; b 1000 */
    0xd40000e1,             /* 1008: svc #7 */
    0x91000421, 0xd4200000, /* 100c: add x1,x1,#1; brk */
    0xd2800022, 0xd503201f, 0xd65f03c0, /* 1014: mov x2,#1; nop; ret */
    0xd63f03c0,             /* 1020: blr x30 */
    0xd4200000,             /* 1024: return address */
    0x91000463, 0xd4200000  /* 1028: add x3,x3,#1; brk */
};
