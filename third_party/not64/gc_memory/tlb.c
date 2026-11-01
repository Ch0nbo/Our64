/**
 * Mupen64 - tlb.c
 * Copyright (C) 2002 Hacktarux
 *
 * Mupen64 homepage: http://mupen64.emulation64.com
 * email address: hacktarux@yahoo.fr
 * 
 * If you want to contribute to the project please contact
 * me first (maybe someone is already making what you are
 * planning to do).
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
 * You should have received a copy of the GNU General Public
 * Licence along with this program; if not, write to the Free
 * Software Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139,
 * USA.
 *
**/

#include "memory.h"
#include "../r4300/r4300.h"
#include "../r4300/exception.h"
#include "../r4300/macros.h"
#include "TLB-Cache.h"
#ifdef USE_EXPANSION
	#define MEMMASK 0x7FFFFF
	#define TOPOFMEM 0x800000
#else
	#define MEMMASK 0x3FFFFF
	#define TOPOFMEM 0x400000
#endif


#ifndef USE_TLB_CACHE
#include "MEM2.h"
unsigned long *const tlb_LUT_r = (unsigned long*)(TLBLUT_LO);
unsigned long *const tlb_LUT_w = (unsigned long*)(TLBLUT_LO+0x400000);
#endif

#ifndef USE_TLB_CACHE
void tlb_mem2_init()
{
	long i;
	for(i = 0; i<(0x400000/4); i++)
	{	
		tlb_LUT_r[i] = 0;
		tlb_LUT_w[i] = 0;
	}
}
#endif

extern unsigned long interp_addr;
#ifdef __WIIU__
/* Our64: the physical address a read of addresse would use, or 0 when no TLB
 * entry maps it (no exception taken): for the player's stall report. */
unsigned long our64_tlb_peek(unsigned long addresse)
{
   if (addresse >= 0x80000000 && addresse < 0xC0000000)
     return addresse & 0x1FFFFFFF;
#ifdef USE_TLB_CACHE
   unsigned long paddr = TLBCache_get_r(addresse>>12);
#else
   unsigned long paddr = tlb_LUT_r[addresse>>12];
#endif
   return paddr ? (paddr&0xFFFFF000)|(addresse&0xFFF) : 0;
}
#endif

unsigned long virtual_to_physical_address(unsigned long addresse, int w)
{
   if (w == 1)
     {
#ifdef USE_TLB_CACHE
     	unsigned long paddr = TLBCache_get_w(addresse>>12);
     	if(paddr) return (paddr&0xFFFFF000)|(addresse&0xFFF);
#else
	if (tlb_LUT_w[addresse>>12])
	  return (tlb_LUT_w[addresse>>12]&0xFFFFF000)|(addresse&0xFFF);
#endif
     }
   else
     {
#ifdef USE_TLB_CACHE
	unsigned long paddr = TLBCache_get_r(addresse>>12);
	if(paddr) return (paddr&0xFFFFF000)|(addresse&0xFFF);
#else
	if (tlb_LUT_r[addresse>>12])
	  return (tlb_LUT_r[addresse>>12]&0xFFFFF000)|(addresse&0xFFF);
#endif
     }
   //printf("tlb exception !!! @ %x, %x, add:%x\n", addresse, w, interp_addr);
   //getchar();
   TLB_refill_exception(addresse,w);
   //return 0x80000000;
   return 0x00000000;
   /*int i;
   for (i=0; i<32; i++)
     {
	if ((tlb_e[i].vpn2 & ~(tlb_e[i].mask))
	    == ((addresse >> 13) & ~(tlb_e[i].mask)))
	  {
	     if (tlb_e[i].g || (tlb_e[i].asid == (EntryHi & 0xFF)))
	       {
		  if (addresse & tlb_e[i].check_parity_mask)
		    {
		       if (tlb_e[i].v_odd)
			 {
			    if (tlb_e[i].d_odd && w)
			      {
				 TLB_mod_exception();
				 return 0;
			      }
			    return ((addresse & ((tlb_e[i].mask << 12)|0xFFF))
				    | ((tlb_e[i].pfn_odd << 12) &
				       ~((tlb_e[i].mask << 12)|0xFFF)) 
				    | 0x80000000);
			 }
		       else
			 {
			    TLB_invalid_exception();
			    return 0;
			 }
		    }
		  else
		    {
		       if (tlb_e[i].v_even)
			 {
			    if (tlb_e[i].d_even && w)
			      {
				 TLB_mod_exception();
				 return 0;
			      }
			    return ((addresse & ((tlb_e[i].mask << 12)|0xFFF))
				    | ((tlb_e[i].pfn_even << 12) &
				       ~((tlb_e[i].mask << 12)|0xFFF))
				    | 0x80000000);
			 }
		       else
			 {
			    TLB_invalid_exception();
			    return 0;
			 }
		    }
	       }
	     else
	       {
		  printf("tlb refill inconnu\n");
		  TLB_refill_exception(addresse,w);
	       }
	  }
     }
   BadVAddr = addresse;
   TLB_refill_exception(addresse,w);
   //printf("TLB refill exception\n");
   return 0x80000000;*/
}

int probe_nop(unsigned long address)
{
   return *fast_mem_access(address) == 0;
}

int probe_idle_slot(unsigned long address, unsigned long *op)
{
   const unsigned long w = *fast_mem_access(address);
   const unsigned opcode = w >> 26, rs = (w >> 21) & 31, rt = (w >> 16) & 31;
   *op = w;
   return w == 0 || ((opcode == 0x09 || opcode == 0x19) && rs == rt && rt != 0); /* ADDIU, DADDIU */
}

void idle_slot_skipped(unsigned long op, long skip)
{
   if (!op || skip <= 0)
      return;
   const int rt = (op >> 16) & 31;
   const long long passes = skip / (2 * (long)count_per_op); /* the branch and its delay slot */
   const long long add = passes * (long long)(short)(op & 0xFFFF);
   if ((op >> 26) == 0x19)
      reg[rt] += add;
   else
      reg[rt] = (long long)(int)((int)reg[rt] + (int)add);
}
