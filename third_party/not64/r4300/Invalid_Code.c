/* Invalid_Code.c - Uses 1/8th the memory as the char hash table
   by Mike Slegeir for Mupen64-GC / MEM2 ver by emu_kidid
 */

#include "Invalid_Code.h"
#ifdef __WIIU__
void jr_cache_clear(void); // ppc/Wrappers.c: register jumps must not reach invalidated code
#define JR_CACHE_CLEAR() jr_cache_clear()
#else
#define JR_CACHE_CLEAR()
#endif
#ifndef HW_RVL  //GC use bit version

static unsigned char invalid_code[0x100000/8];

int inline invalid_code_get(int block_num){
	return invalid_code[block_num>>3] & (1<<(block_num&0x7));
}

void inline invalid_code_set(int block_num, int value){
	if(value){ if(!(invalid_code[block_num>>3] & (1<<(block_num&0x7)))) JR_CACHE_CLEAR(); invalid_code[block_num>>3] |=  (1<<(block_num&0x7)); }
	else      invalid_code[block_num>>3] &= ~(1<<(block_num&0x7));
}

#else //Wii MEM2 1MB char array version
#include "../gc_memory/MEM2.h"

static unsigned char *const invalid_code = (unsigned char*)(INVCODE_LO);

int inline invalid_code_get(int block_num){
	return invalid_code[block_num];
}

void inline invalid_code_set(int block_num, int value){
	if(value && !invalid_code[block_num]) JR_CACHE_CLEAR();
	invalid_code[block_num] = value;
}

#endif

