/* Pokemon Stadium 2's GB Tower RSP program (rsp_lle.c runs it on cxd4) in C:
 * the same RDRAM and DMEM writes, without interpreting 320 000-380 000 RSP
 * instructions a frame. Portable: the PC tests compare it with cxd4. */
#ifndef GB_TOWER_HLE_H
#define GB_TOWER_HLE_H

#include <stdint.h>

/* Runs the task osSpTaskLoad left in DMEM (OSTask at 0xFC0, the program at
 * IMEM 0x1000) to its BREAK. Memory as rsp_lle.h keeps it: native 32-bit
 * words, RDRAM 8 MB. Writes RDRAM and DMEM as the program does (DMEM 0xFFC
 * gets 0, the DPC_CLOCK ticks the program counts, which cxd4 never advances)
 * and leaves SP_STATUS to the caller: the program sets SIG3 once its input is
 * copied, and at its end waits for the CPU to clear SIG3, then sets SIG2 and
 * breaks. Returns 0 without writing anything for a variant it has no code for
 * (Stadium 2's are boot sizes 0xA10, 0xB30 and 0xA60, ucode_boot_size at DMEM
 * 0xFCC; the 0xC0 bytes of tables after the program at ucode_boot must be the
 * ROM's, and its first copy must not land on them), or a task the program
 * would never finish (0xFFE's row bytes 0, or a data_size its rows never
 * reach exactly); 1 when it ran. */
int gb_tower_hle(unsigned char *rdram, unsigned char *dmem);

#endif
