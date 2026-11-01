/**
 * Wii64 - Wrappers.h
 * Copyright (C) 2007, 2008, 2009, 2010 Mike Slegeir
 * 
 * Interface between emulator code and recompiled code
 *
 * Wii64 homepage: http://www.emulatemii.com
 * email address: tehpola@gmail.com
 *
 *
 * This program is free software; you can redistribute it and/
 * or modify it under the terms of the GNU General Public Li-
 * cence as published by the Free Software Foundation; either
 * version 2 of the Licence, or any later version.
 *
 * This program is distributed in the hope that it will be use-
 * ful, but WITHOUT ANY WARRANTY; without even the implied war-
 * ranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public Licence for more details.
 *
**/

#ifndef WRAPPERS_H
#define WRAPPERS_H

#include "Recompile.h"
#include "../our64_sda.h"

#define DYNAREG_RDRAM  29
#define DYNAREG_FUNC   30
#define DYNAREG_ZERO   31

#define DYNAOFF_LR     12

/* GCC 16 no longer satisfies the "U" (V.4 small data) asm constraint that the
 * original @sdarel form relied on, and no -msdata/-G combination brings it
 * back. The offset is simply the distance from the small data base, which the
 * linker exposes as _SDA_BASE_, so compute it directly and skip the asm. */
#ifdef __WIIU__
/* Our64: wut has no small data area, so the platform layer picks a base inside
 * the hand-placed .sbss/.sdata variables and loads it into r13 before running
 * generated code (see our64_sda_base in the Our64 platform layer). */
extern char *our64_sda_base;
#define SDAREL(symbol) ((short)((char *)(&symbol) - our64_sda_base))
#else
extern char _SDA_BASE_[];
#define SDAREL(symbol) ((short)((char *)(&symbol) - _SDA_BASE_))
#endif

extern long long int reg[34] __attribute__((section(".sbss")));
extern unsigned long reg_cop0[32] __attribute__((section(".sbss")));
extern float*  reg_cop1_simple[32] __attribute__((section(".sbss")));
extern double* reg_cop1_double[32] __attribute__((section(".sbss")));

extern int noCheckInterrupt;

#ifdef __WIIU__
/* Our64: register jumps (jr, jalr) look their target up in this table from the
 * generated code and only return to the dispatcher on a miss. The dispatcher
 * fills it; anything that frees, moves or invalidates code clears it. */
#define JR_CACHE_SIZE 4096 /* entries; the generated code masks (target >> 2) with it */
typedef struct {
	unsigned int   mips;   /* MIPS address; when empty, one no target looks up here (jr_cache_empty) */
	PowerPC_func*  func;
	PowerPC_instr* code;
	unsigned int   pad;
} jr_cache_entry;
extern jr_cache_entry jr_cache[JR_CACHE_SIZE];
extern int jr_cache_on;    /* read when a block is compiled */
extern unsigned int jr_cache_clears;
void jr_cache_clear(void);
#endif

typedef enum { MEM_LW,   MEM_LH,   MEM_LB,   MEM_LD,
               MEM_ULW,                      MEM_ULD,
               MEM_LWU,  MEM_LHU,  MEM_LBU,
               MEM_LWC1, MEM_LDC1, MEM_LL,   MEM_LLD,
               MEM_LWL,  MEM_LWR,  MEM_LDL,  MEM_LDR,
               MEM_SW,   MEM_SH,   MEM_SB,   MEM_SD,
               MEM_USW,                      MEM_USD,
               MEM_SWC1, MEM_SDC1, MEM_SC,   MEM_SCD,
               MEM_SWL,  MEM_SWR,  MEM_SDL,  MEM_SDR } memType;

void dynarec(unsigned int address);
unsigned int decodeNInterpret(MIPS_instr, unsigned int, int);
#ifdef COMPARE_CORE
int dyna_update_count(unsigned int pc, int isDelaySlot);
#else
int dyna_update_count(unsigned int pc);
#endif
unsigned int dyna_check_cop1_unusable(unsigned int pc, int isDelaySlot);
void invalidate_func(unsigned int addr);
unsigned int dyna_mem(unsigned int addr, unsigned int value, int count,
                      memType type, unsigned int pc, int isDelaySlot);

//cop0 macros
#define Index reg_cop0[0]
#define Random reg_cop0[1]
#define EntryLo0 reg_cop0[2]
#define EntryLo1 reg_cop0[3]
#define Context reg_cop0[4]
#define PageMask reg_cop0[5]
#define Wired reg_cop0[6]
#define BadVAddr reg_cop0[8]
#define Count reg_cop0[9]
#define EntryHi reg_cop0[10]
#define Compare reg_cop0[11]
#define Status reg_cop0[12]
#define Cause reg_cop0[13]
#define EPC reg_cop0[14]
#define PRevID reg_cop0[15]
#define Config reg_cop0[16]
#define LLAddr reg_cop0[17]
#define WatchLo reg_cop0[18]
#define WatchHi reg_cop0[19]
#define XContext reg_cop0[20]
#define PErr reg_cop0[26]
#define CacheErr reg_cop0[27]
#define TagLo reg_cop0[28]
#define TagHi reg_cop0[29]
#define ErrorEPC reg_cop0[30]

#endif

