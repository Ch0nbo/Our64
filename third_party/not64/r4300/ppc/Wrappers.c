/**
 * Wii64 - Wrappers.c
 * Copyright (C) 2008, 2009, 2010 Mike Slegeir
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

#include <stdlib.h>
#include <string.h>
#include "../../gui/DEBUG.h"
#include "../Invalid_Code.h"
#include "../ARAM-blocks.h"
#include "../../gc_memory/memory.h"
#include "../interupt.h"
#include "../exception.h"
#include "../r4300.h"
#include "../macros.h"
#include "../Recomp-Cache.h"
#include "Recompile.h"
#include "Wrappers.h"

extern int stop;
extern unsigned long instructionCount;
extern void (*interp_ops[64])(void);
inline unsigned long update_invalid_addr(unsigned long addr);
unsigned int dyna_check_cop1_unusable(unsigned int, int);
unsigned int dyna_mem(unsigned int, unsigned int, int, memType, unsigned int, int);

int noCheckInterrupt OUR64_SDA = 0;
#ifdef __WIIU__
unsigned our64_count_writes; /* MTC0 Count (pure_interp.c): the count guard leaves those alone */
#endif

static PowerPC_instr* link_branch = NULL;
static PowerPC_func* last_func;

#ifdef __WIIU__
jr_cache_entry jr_cache[JR_CACHE_SIZE] __attribute__((aligned(64)));
int jr_cache_on = 1;
unsigned int jr_cache_clears;
static int jr_cache_used;

/* Empty entries hold an odd address whose slot is the next one, so no jump
 * target matches them. Zeroed entries matched jr $zero (Bomberman 64), and the
 * generated code went on with a NULL func and code: a store through NULL and a
 * branch to 0 took the whole console down. */
static void jr_cache_empty(void){
	int i;
	for(i = 0; i < JR_CACHE_SIZE; i++)
		jr_cache[i].mips = (((i + 1) & (JR_CACHE_SIZE - 1)) << 2) | 1;
	jr_cache_used = 0;
}

void jr_cache_clear(void){
	if(!jr_cache_used) return;
	jr_cache_empty();
	jr_cache_clears++;
}
#endif

/* Recompiled code stack frame:
 *  $sp+12  |
 *  $sp+8   | old cr
 *  $sp+4   | old lr
 *  $sp	    | old sp
 */

inline unsigned int dyna_run(PowerPC_func* func, PowerPC_instr* code){
	PowerPC_instr* return_addr;

	register void* r3  __asm__("r3");
	register void* r29 __asm__("r29") = rdram;
	register void* r30 __asm__("r30") = func;
	register void* r31 __asm__("r31") = NULL;

	end_section(TRAMP_SECTION);

	__asm__ volatile(
		"mtctr  %4        \n"
		"bl     1f        \n"
		"mflr   %3        \n"
		"lwz    %2, 12(1) \n"
		"addi   1, 1, 8   \n"
		"b      2f        \n"
		"1:               \n"
		"stwu   1, -8(1)  \n"
		"mflr   0         \n"
		"stw    0, 12(1)  \n"
		"bctr             \n"
		"2:               \n"
		: "=r" (r3), "=r" (r30), "=r" (return_addr), "=r" (link_branch)
		: "r" (code), "r" (r29), "r" (r30), "r" (r31)
		: "r0", "r2", "r4", "r5", "r6", "r7", "r8", "r9", "r10", "r11", "r12",
		  "fr0", "fr1", "fr2", "fr3", "fr4", "fr5", "fr6", "fr7", "fr8", "fr9", "fr10", "fr11", "fr12", "fr13",
		  "lr", "ctr", "cr0", "cr1", "cr2", "cr3", "cr4", "cr5", "cr6", "cr7", "ca");

	last_func = r30;
	link_branch = link_branch == return_addr ? NULL : link_branch - 1;

	return r3;
}

#ifdef __WIIU__
/* Set while the dispatcher calls gen_interupt. Our64 loads and captures
 * save states only then: nothing of an interrupted instruction runs after
 * gen_interupt returns (MTC0 Status, for one, takes 4 off interp_addr), so
 * the state is exactly "about to run interp_addr, with last_addr there". */
int our64_at_dispatcher;
/* Set then too, and while an interpreted branch or ERET calls gen_interupt
 * as its last act (pure_interp.c): the same state. Captures may happen at
 * either; loads only at the dispatcher, since a load invalidates the code
 * an interpreted instruction returns into. */
int our64_at_boundary;
/* Addresses run in the interpreter because their compiled code was missing
 * (see dynarec), and the first one: the player logs them. */
unsigned int our64_dyna_fallbacks, our64_dyna_fallback_first;
/* Funcs at TLB addresses found changed in memory and compiled again (dynarec). */
unsigned int our64_tlb_stale_funcs;
int our64_full_code_check; /* Our64: compare whole functions at TLB addresses (see the dispatcher) */
/* Our64 stops go() to load a save state or reset, then starts it again:
 * the block that ran last must not be linked to whatever runs first. */
void our64_dynarec_unlink(void)
{
	link_branch = NULL;
	last_func = NULL;
}
#endif

void dynarec(unsigned int address){
#ifdef __WIIU__
	jr_cache_empty();
#endif
	while(!stop){
		refresh_stat();
		
		start_section(TRAMP_SECTION);
		PowerPC_block* dst_block = blocks_get(address>>12);
		unsigned long paddr = update_invalid_addr(address);
		/*
		sprintf(txtbuffer, "trampolining to 0x%08x\n", address);
		DEBUG_print(txtbuffer, DBG_USBGECKO);
		*/
		if(!paddr){
			link_branch = NULL;
			address = interp_addr;
			dst_block = blocks_get(address>>12); 
			paddr = update_invalid_addr(address);
		}
		
		if(!dst_block){
			/*sprintf(txtbuffer, "block at %08x doesn't exist\n", address&~0xFFF);
			DEBUG_print(txtbuffer, DBG_USBGECKO);*/
			dst_block = calloc(1, sizeof(PowerPC_block));
			blocks_set(address>>12, dst_block);
			dst_block->start_address = address & ~0xFFF;
			dst_block->end_address   = (address & ~0xFFF) + 0x1000;
			init_block(dst_block);
		} else if(invalid_code_get(address>>12)){
			invalidate_block(dst_block);
		}

		PowerPC_func* func = find_func(&dst_block->funcs, address);
#ifdef __WIIU__
		/* Our64: code at a TLB address whose memory no longer holds what it was
		 * compiled from. Mario Tennis swaps overlays in an 8 KB window at
		 * 0x10000000, and a remap can keep the old code (TLBWI's adler32 check
		 * sees the page after the new overlay is in already, and Not64's stores
		 * check no code): its main thread ran an old overlay's code and died on
		 * a store to 0 in a rally (2026-10-21). The words at the address entered
		 * and the 3 after it are compared each time the dispatcher enters; with
		 * our64_full_code_check (the player sets it per game), every word from
		 * there to the function's end within the page. Castlevania's overlays at
		 * 0x0F000000 begin with the same four words and differ further in: an old
		 * overlay's code ran, loaded through a table address the new one has not
		 * mapped, and its main thread died at the start of play (GitHub issue
		 * 15, 2026-11-02). Not for every game: Conker enters code at TLB
		 * addresses so often that comparing whole functions took it from 60 to
		 * 49-53 VIs a second. */
		if(func && func->our64_mips){
			extern int our64_full_code_check;
			const unsigned int at = (address - func->start_addr) >> 2;
			const unsigned int last = our64_full_code_check ? func->our64_count : at + 4;
			const MIPS_instr* now = (const MIPS_instr*)fast_mem_access(paddr);
			int same = 1;
			for(unsigned int k = at; same && k < func->our64_count && k < last && ((paddr + (k - at) * 4) & 0xFFF) >= (paddr & 0xFFF); k++)
				same = now[k - at] == func->our64_mips[k];
			if(!same){
				our64_tlb_stale_funcs++;
				if(our64_tlb_stale_funcs <= 8){
					extern void report_line(const char *fmt, ...);
					report_line("CPU: code at 0x%08X (TLB, physical 0x%08lX) changed since it was compiled: compiled again",
					            address, paddr);
				}
				invalidate_block(dst_block);
				func = NULL;
			}
		}
#endif

		if(!func || !func->code_addr[(address-func->start_addr)>>2]){
			/*sprintf(txtbuffer, "code at %08x is not compiled\n", address);
			DEBUG_print(txtbuffer, DBG_USBGECKO);*/
			dst_block->mips_code = fast_mem_access(paddr & ~0xFFF);
			start_section(COMPILER_SECTION);
			func = recompile_block(dst_block, address);
			end_section(COMPILER_SECTION);
		} else {
#ifdef USE_RECOMP_CACHE
			RecompCache_Update(func);
#endif
		}

		int index = (address - func->start_addr)>>2;

		// Recompute the block offset
		PowerPC_instr* code = func->code_addr[index];
#ifdef __WIIU__
		if(!code){
			/* Our64: an address the recompiler gave no code of its own (one it
			 * folded into the instruction before it, like a delay slot that is
			 * also a jump target) was jumped to as NULL: an ISI at 0 in Smash
			 * Remix's character select (2026-10-17). Run that one instruction in
			 * the interpreter instead, and count it. */
			MIPS_instr mips = *(MIPS_instr*)fast_mem_access(paddr);
			if(!our64_dyna_fallbacks)
				our64_dyna_fallback_first = address;
			our64_dyna_fallbacks++;
			link_branch = NULL;
			unsigned int next = decodeNInterpret(mips, address, 0);
			Count += count_per_op;
			interp_addr = address = next ? next : address + 4;
			if(!noCheckInterrupt){
				last_addr = interp_addr;
				if(next_interupt <= Count){
					our64_at_dispatcher = our64_at_boundary = 1;
					gen_interupt();
					our64_at_dispatcher = our64_at_boundary = 0;
					address = interp_addr;
				}
			}
			noCheckInterrupt = 0;
			continue;
		}
#endif
		
#ifdef __WIIU__
		/* Our64: code at TLB addresses (Conker, Perfect Dark) can be remapped
		 * under the same address; only the dispatcher checks a page's code is
		 * still valid (invalid_code), so jumps into or out of it come back here
		 * instead of through a link or the jr cache. With either, Conker's timer
		 * went missing (its stalls; none on the pure interpreter). */
		extern int our64_tlb_links; /* Recomp-Cache-Heap.c: links cut at each remap instead */
		const int our64_tlb_code = !our64_tlb_links && (address < 0x80000000 || address >= 0xC0000000 ||
		                           (last_func && !func_was_freed(last_func) && (last_func->start_addr < 0x80000000 || last_func->start_addr >= 0xC0000000)));
		if(our64_tlb_code)
			link_branch = NULL;
#endif
		// Create a link if possible
		if(link_branch && !func_was_freed(last_func))
			RecompCache_Link(last_func, link_branch, func, code);
		clear_freed_funcs();

#ifdef __WIIU__
		if(jr_cache_on && !our64_tlb_code){
			jr_cache_entry* e = &jr_cache[(address >> 2) & (JR_CACHE_SIZE - 1)];
			e->mips = address;
			e->func = func;
			e->code = code;
			jr_cache_used = 1;
		}
#endif
		
#ifdef __WIIU__
		unsigned long our64_count_was = Count, our64_last_was = last_addr;
		unsigned int our64_entry = address, our64_writes_was = our64_count_writes;
#endif
		interp_addr = address = dyna_run(func, code);
#ifdef __WIIU__
		/* Our64: a VI or another event ends every run within a VI period, so
		 * Count can't move 16M counts (21 VIs) in one; when it does, a count
		 * update measured from a stale last_addr. Suspected in Conker's and
		 * Perfect Dark's stalls (code at TLB addresses; a timer left behind
		 * Count waits for it to wrap). Undone, and the first few logged. */
		if (Count - our64_count_was > 0x01000000 && our64_count_writes == our64_writes_was) {
			extern void report_line(const char *fmt, ...);
			static int told;
			if (told < 12) {
				told++;
				report_line("CPU: Count leapt %08lX in one run entered at 0x%08X, out at 0x%08X; last_addr 0x%08lX before, 0x%08lX after; undone",
				            Count - our64_count_was, our64_entry, address, our64_last_was, last_addr);
			}
			Count = our64_count_was + 1000 * count_per_op;
		}
#endif

		if(!noCheckInterrupt){
			last_addr = interp_addr;
			// Check for interrupts
			if(next_interupt <= Count){
#ifdef __WIIU__
				our64_at_dispatcher = our64_at_boundary = 1;
#endif
				gen_interupt();
#ifdef __WIIU__
				our64_at_dispatcher = our64_at_boundary = 0;
#endif
				address = interp_addr;
			}
		}
		noCheckInterrupt = 0;
	}
	interp_addr = address;
}

unsigned int decodeNInterpret(MIPS_instr mips, unsigned int pc,
                              int isDelaySlot){
	delay_slot = isDelaySlot; // Make sure we set delay_slot properly
	PC->addr = interp_addr = pc;
	start_section(INTERP_SECTION);
	prefetch_opcode(mips);
	interp_ops[MIPS_GET_OPCODE(mips)]();
	end_section(INTERP_SECTION);
	delay_slot = 0;

	if(interp_addr != pc + 4) noCheckInterrupt = 1;

	return interp_addr != pc + 4 ? interp_addr : 0;
}

#ifdef COMPARE_CORE
int dyna_update_count(unsigned int pc, int isDelaySlot){
#else
int dyna_update_count(unsigned int pc){
#endif
	Count += ((pc - last_addr) >> 2) * count_per_op;
	last_addr = pc;

#ifdef COMPARE_CORE
	if(isDelaySlot){
		interp_addr = pc;
		compare_core();
	}
#endif

	return next_interupt - Count;
}

unsigned int dyna_check_cop1_unusable(unsigned int pc, int isDelaySlot){
	// Set state so it can be recovered after exception
	delay_slot = isDelaySlot;
	PC->addr = interp_addr = pc;
	// Take a FP unavailable exception
	Cause = (11 << 2) | 0x10000000;
	exception_general();
	// Reset state
	delay_slot = 0;
	noCheckInterrupt = 1;
	// Return the address to trampoline to
	return interp_addr;
}

void invalidate_func(unsigned int addr){
#ifdef __WIIU__
	{ extern void our64_phys_code_changed(unsigned long addr); /* Recomp-Cache-Heap.c: TLB pages mapping it */
	  our64_phys_code_changed(addr); }
#endif
	if(!invalid_code_get(addr>>12)){
		PowerPC_block* block = blocks_get(addr>>12);
		PowerPC_func* func = find_func(&block->funcs, addr);
		if(func)
			RecompCache_Free(func->start_addr);
	}
}

unsigned int dyna_mem(unsigned int addr, unsigned int value, int count,
                      memType type, unsigned int pc, int isDelaySlot){
	int i;

	PC->addr = interp_addr = pc;
	delay_slot = isDelaySlot;
#ifdef OUR64_COUNT_MEM
	/* Our64's bench (tlb-count): which games take this slow path, and for what. */
	{ extern unsigned int our64_mem_tlb, our64_mem_other;
	  if(addr < 0x80000000 || addr >= 0xC0000000) our64_mem_tlb++; else our64_mem_other++; }
#endif

	if(count < 1 || value + count > 32){
#ifdef __WIIU__
		void our64_bad_mem_call(unsigned, unsigned, int, int, unsigned, int);
		our64_bad_mem_call(addr, value, count, type, pc, isDelaySlot);
#endif
		abort();
	}

	switch(type){
		case MEM_LW:
			addr &= ~3;
		case MEM_ULW:
			for(i = 0; i < count; i++){
				address = addr + i*4;
				read_word_in_memory();
				if(!address) break;
				reg[value + i] = (signed long)word;
			}
			break;
		case MEM_LWU:
			addr &= ~3;
			for(i = 0; i < count; i++){
				address = addr + i*4;
				read_word_in_memory();
				if(!address) break;
				reg[value + i] = word;
			}
			break;
		case MEM_LH:
			addr &= ~1;
			for(i = 0; i < count; i++){
				address = addr + i*2;
				read_hword_in_memory();
				if(!address) break;
				reg[value + i] = (signed short)hword;
			}
			break;
		case MEM_LHU:
			addr &= ~1;
			for(i = 0; i < count; i++){
				address = addr + i*2;
				read_hword_in_memory();
				if(!address) break;
				reg[value + i] = hword;
			}
			break;
		case MEM_LB:
			for(i = 0; i < count; i++){
				address = addr + i;
				read_byte_in_memory();
				if(!address) break;
				reg[value + i] = (signed char)byte;
			}
			break;
		case MEM_LBU:
			for(i = 0; i < count; i++){
				address = addr + i;
				read_byte_in_memory();
				if(!address) break;
				reg[value + i] = byte;
			}
			break;
		case MEM_LD:
			addr &= ~7;
		case MEM_ULD:
			for(i = 0; i < count; i++){
				address = addr + i*8;
				read_dword_in_memory();
				if(!address) break;
				reg[value + i] = dword;
			}
			break;
		case MEM_LWC1:
			addr &= ~3;
			for(i = 0; i < count; i++){
				address = addr + i*4;
				read_word_in_memory();
				if(!address) break;
				*((long*)reg_cop1_simple[value + i*2]) = word;
			}
			break;
		case MEM_LDC1:
			addr &= ~7;
			for(i = 0; i < count; i++){
				address = addr + i*8;
				read_dword_in_memory();
				if(!address) break;
				*((long long*)reg_cop1_double[value + i*2]) = dword;
			}
			break;
		case MEM_LL:
			address = addr &= ~3;
			read_word_in_memory();
			if(!address) break;
			reg[value] = (signed long)word;
			llbit = 1;
			break;
		case MEM_LLD:
			address = addr &= ~7;
			read_dword_in_memory();
			if(!address) break;
			reg[value] = dword;
			llbit = 1;
			break;
		case MEM_LWL:
			address = addr & ~3;
			read_word_in_memory();
			if(!address) break;
			if((addr & 3) == 0){
				reg[value] = (signed long)word;
			} else {
				u32 shift = (addr & 3) * 8;
				u32 mask = 0xFFFFFFFF << shift;
				reg[value] = (signed long)((reg[value] & ~mask) | ((word << shift) & mask));
			}
			break;
		case MEM_LWR:
			address = (addr + 3) & ~3;
			read_word_in_memory();
			if(!address) break;
			if((-addr & 3) == 0){
				reg[value] = (signed long)word;
			} else {
				u32 shift = (-addr & 3) * 8;
				u32 mask = 0xFFFFFFFF >> shift;
				reg[value] = (signed long)((reg[value] & ~mask) | ((word >> shift) & mask));
			}
			break;
		case MEM_LDL:
			address = addr & ~7;
			read_dword_in_memory();
			if(!address) break;
			if((addr & 7) == 0){
				reg[value] = dword;
			} else {
				u32 shift = (addr & 7) * 8;
				u64 mask = 0xFFFFFFFFFFFFFFFFLL << shift;
				reg[value] = (reg[value] & ~mask) | ((dword << shift) & mask);
			}
			break;
		case MEM_LDR:
			address = (addr + 7) & ~7;
			read_dword_in_memory();
			if(!address) break;
			if((-addr & 7) == 0){
				reg[value] = dword;
			} else {
				u32 shift = (-addr & 7) * 8;
				u64 mask = 0xFFFFFFFFFFFFFFFFLL >> shift;
				reg[value] = (reg[value] & ~mask) | ((dword >> shift) & mask);
			}
			break;
		case MEM_SW:
			addr &= ~3;
		case MEM_USW:
			for(i = 0; i < count; i++){
				address = addr + i*4;
				word = reg[value + i];
				write_word_in_memory();
			}
			invalidate_func(addr);
			break;
		case MEM_SH:
			addr &= ~1;
			for(i = 0; i < count; i++){
				address = addr + i*2;
				hword = reg[value + i];
				write_hword_in_memory();
			}
			invalidate_func(addr);
			break;
		case MEM_SB:
			for(i = 0; i < count; i++){
				address = addr + i;
				byte = reg[value + i];
				write_byte_in_memory();
			}
			invalidate_func(addr);
			break;
		case MEM_SD:
			addr &= ~7;
		case MEM_USD:
			for(i = 0; i < count; i++){
				address = addr + i*8;
				dword = reg[value + i];
				write_dword_in_memory();
			}
			invalidate_func(addr);
			break;
		case MEM_SWC1:
			addr &= ~3;
			for(i = 0; i < count; i++){
				address = addr + i*4;
				word = *((long*)reg_cop1_simple[value + i*2]);
				write_word_in_memory();
			}
			invalidate_func(addr);
			break;
		case MEM_SDC1:
			addr &= ~7;
			for(i = 0; i < count; i++){
				address = addr + i*8;
				dword = *((long long*)reg_cop1_double[value + i*2]);
				write_dword_in_memory();
			}
			invalidate_func(addr);
			break;
		case MEM_SC:
			if(llbit){
				address = addr &= ~3;
				word = reg[value];
				write_word_in_memory();
				invalidate_func(addr);
			}
			reg[value] = !!llbit;
			llbit = 0;
			break;
		case MEM_SCD:
			if(llbit){
				address = addr &= ~7;
				dword = reg[value];
				write_dword_in_memory();
				invalidate_func(addr);
			}
			reg[value] = !!llbit;
			llbit = 0;
			break;
		case MEM_SWL:
			address = addr & ~3;
			read_word_in_memory();
			if((addr & 3) == 0){
				word = reg[value];
			} else {
				u32 shift = (addr & 3) * 8;
				u32 mask = 0xFFFFFFFF >> shift;
				word = (word & ~mask) | ((reg[value] >> shift) & mask);
			}
			write_word_in_memory();
			invalidate_func(addr);
			break;
		case MEM_SWR:
			address = (addr + 3) & ~3;
			read_word_in_memory();
			if((-addr & 3) == 0){
				word = reg[value];
			} else {
				u32 shift = (-addr & 3) * 8;
				u32 mask = 0xFFFFFFFF << shift;
				word = (word & ~mask) | ((reg[value] << shift) & mask);
			}
			write_word_in_memory();
			invalidate_func(addr);
			break;
		case MEM_SDL:
			address = addr & ~7;
			read_dword_in_memory();
			if((addr & 7) == 0){
				dword = reg[value];
			} else {
				u32 shift = (addr & 7) * 8;
				u64 mask = 0xFFFFFFFFFFFFFFFFLL >> shift;
				dword = (dword & ~mask) | ((reg[value] >> shift) & mask);
			}
			write_dword_in_memory();
			invalidate_func(addr);
			break;
		case MEM_SDR:
			address = (addr + 7) & ~7;
			read_dword_in_memory();
			if((-addr & 7) == 0){
				dword = reg[value];
			} else {
				u32 shift = (-addr & 7) * 8;
				u64 mask = 0xFFFFFFFFFFFFFFFFLL << shift;
				dword = (dword & ~mask) | ((reg[value] << shift) & mask);
			}
			write_dword_in_memory();
			invalidate_func(addr);
			break;
		default:
#ifdef __WIIU__
			{
				void our64_bad_mem_call(unsigned, unsigned, int, int, unsigned, int);
				our64_bad_mem_call(addr, value, count, type, pc, isDelaySlot);
			}
#endif
			abort();
			break;
	}
	delay_slot = 0;

	if(interp_addr != pc) noCheckInterrupt = 1;

	return interp_addr != pc ? interp_addr : 0;
}

