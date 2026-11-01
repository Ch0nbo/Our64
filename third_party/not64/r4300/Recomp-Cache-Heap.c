/**
 * Wii64 - Recomp-Cache-Heap.c
 * Copyright (C) 2009, 2010 Mike Slegeir
 * 
 * Cache for recompiled code using a heap to maintain LRU order
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


#include <ogc/lwp_heap.h>
#include <ogc/cache.h>
#ifdef __WIIU__
#include "our64_jit.h"
#endif
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include "../gc_memory/MEM2.h"
#include "r4300.h"
#include "ppc/Recompile.h"
#include "ppc/Wrappers.h"
#include "Invalid_Code.h"
#include "../gc_memory/tlb.h"
#include "Recomp-Cache.h"
#include "ARAM-blocks.h"

typedef struct _meta_node {
	unsigned int  addr;
	PowerPC_func* func;
	unsigned int  size;
} CacheMetaNode;

unsigned int nextLRU OUR64_SDA = 0;

static heap_cntrl* cache = NULL, * meta_cache = NULL;
static int cacheSize = 0;

#define HEAP_CHILD1(i) ((i<<1)+1)
#define HEAP_CHILD2(i) ((i<<1)+2)
#define HEAP_PARENT(i) ((i-1)>>2)

#define INITIAL_HEAP_SIZE (64)
static unsigned int heapSize = 0;
static unsigned int maxHeapSize = 0;
static CacheMetaNode** cacheHeap = NULL;

static void heapSwap(int i, int j){
	CacheMetaNode* t = cacheHeap[i];
	cacheHeap[i] = cacheHeap[j];
	cacheHeap[j] = t;
}

static void heapUp(int i){
	// While the given element is out of order
	while(i && cacheHeap[i]->func->lru < cacheHeap[HEAP_PARENT(i)]->func->lru){
		// Swap the child with its parent
		heapSwap(i, HEAP_PARENT(i));
		// Consider its new position
		i = HEAP_PARENT(i);
	}
}

static void heapDown(int i){
	// While the given element is out of order
	while(1){
		unsigned int lru = cacheHeap[i]->func->lru;
		CacheMetaNode* c1 = (HEAP_CHILD1(i) < heapSize) ?
		                     cacheHeap[HEAP_CHILD1(i)] : NULL;
		CacheMetaNode* c2 = (HEAP_CHILD2(i) < heapSize) ?
		                     cacheHeap[HEAP_CHILD2(i)] : NULL;
		// Check against the children, swapping with the min if parent isn't
		if(c1 && lru > c1->func->lru &&
		   (!c2 || c1->func->lru < c2->func->lru)){
			heapSwap(i, HEAP_CHILD1(i));
			i = HEAP_CHILD1(i);
		} else if(c2 && lru > c2->func->lru){
			heapSwap(i, HEAP_CHILD2(i));
			i = HEAP_CHILD2(i);
		} else break;
	}
}

static void heapify(void){
	int i;
	for(i=1; i<heapSize; ++i) heapUp(i);
}

static void heapPush(CacheMetaNode* node){
	if(heapSize == maxHeapSize){
		maxHeapSize = 3*maxHeapSize/2 + 10;
		cacheHeap = realloc(cacheHeap, maxHeapSize*sizeof(void*));
	}
	// Simply add it to the end of the heap
	// No need to heapUp since its the most recently used
	cacheHeap[heapSize++] = node;
}

static CacheMetaNode* heapPop(void){
	heapSwap(0, --heapSize);
	heapDown(0);
	return cacheHeap[heapSize];
}

static void unlink_func(PowerPC_func* func){
//	start_section(UNLINK_SECTION);
	
	// Remove any incoming links to this func
	PowerPC_func_link_node* link, * next_link;
#ifdef __WIIU__
	our64_jit_write_begin();
#endif
	for(link = func->links_in; link != NULL; link = next_link){
		next_link = link->next;
		
		GEN_ORI(*(link->branch-10), 0, 0, 0);
		GEN_ORI(*(link->branch-9), 0, 0, 0);
		GEN_BLR(*link->branch, 1); // Set the linking branch to blrl
		DCStoreRange(link->branch-10, 11*sizeof(PowerPC_instr));
		ICInvalidateRange(link->branch-10, 11*sizeof(PowerPC_instr));
		
		remove_func(&link->func->links_out, func);
		MetaCache_Free(link);
	}
#ifdef __WIIU__
	our64_jit_write_end();
#endif
	func->links_in = NULL;
	
	// Remove any references to outgoing links from this func
	void remove_outgoing_links(PowerPC_func_node** node){
		if(!*node) return;
		if((*node)->left) remove_outgoing_links(&(*node)->left);
		if((*node)->right) remove_outgoing_links(&(*node)->right);

		// Remove any links this function has which point in the code
		PowerPC_func_link_node** link, ** next;
		for(link = &(*node)->function->links_in; *link != NULL; link = next){
			next = &(*link)->next;
			if((*link)->func == func){
				PowerPC_func_link_node* tmp = (*link)->next;
				MetaCache_Free(*link);
				*link = tmp;
				next = link;
			}
		}
		MetaCache_Free(*node); // Free the PowerPC_func_node*
	}
	remove_outgoing_links(&func->links_out);
	func->links_out = NULL;
	
//	end_section(UNLINK_SECTION);
}

#ifdef __WIIU__
/* Our64: card switch tlb-links (the player): code at TLB addresses is linked
 * and put in the jr cache like any other, and a TLB write that remaps a page
 * cuts the links into it (and empties the jr cache), so jumps there go
 * through the dispatcher's check of the remapped page. Off: such code is
 * never linked (Wrappers.c), safe but slow (Gauntlet Legends, 2026-10-20). */
int our64_tlb_links;

static void unlink_incoming(PowerPC_func* func){
	PowerPC_func_link_node* link, * next_link;
	our64_jit_write_begin();
	for(link = func->links_in; link != NULL; link = next_link){
		next_link = link->next;
		GEN_ORI(*(link->branch-10), 0, 0, 0);
		GEN_ORI(*(link->branch-9), 0, 0, 0);
		GEN_BLR(*link->branch, 1); // Set the linking branch to blrl
		DCStoreRange(link->branch-10, 11*sizeof(PowerPC_instr));
		ICInvalidateRange(link->branch-10, 11*sizeof(PowerPC_instr));
		remove_func(&link->func->links_out, func);
		MetaCache_Free(link);
	}
	our64_jit_write_end();
	func->links_in = NULL;
}

static void unlink_tree(PowerPC_func_node* node){
	if(!node) return;
	unlink_tree(node->left);
	unlink_tree(node->right);
	unlink_incoming(node->function);
}

/* Code changing in a physical page (a DMA or a CPU write) is noticed for
 * its TLB pages only by the dispatcher (update_invalid_addr); links into them
 * skip it. So a change in a page some TLB entry maps marks those virtual pages
 * invalid and cuts the links into them. Which physical pages are mapped: a
 * table rebuilt after each TLB write. */
unsigned char our64_tlb_phys[0x800]; /* 8 MB in 4 KB pages; generated code reads it (genCheckMemory) */
int our64_tlb_phys_dirty = 1;

void our64_tlb_written(void){ our64_tlb_phys_dirty = 1; }
void our64_tlb_remap(void* b);

static void tlb_phys_rebuild(void){
	memset(our64_tlb_phys, 0, sizeof(our64_tlb_phys));
	for(int e = 0; e < 32; e++){
		if(tlb_e[e].v_even)
			for(unsigned long a = tlb_e[e].phys_even; a <= tlb_e[e].phys_even + (tlb_e[e].end_even - tlb_e[e].start_even); a += 0x1000)
				our64_tlb_phys[(a & 0x7FFFFF) >> 12] = 1;
		if(tlb_e[e].v_odd)
			for(unsigned long a = tlb_e[e].phys_odd; a <= tlb_e[e].phys_odd + (tlb_e[e].end_odd - tlb_e[e].start_odd); a += 0x1000)
				our64_tlb_phys[(a & 0x7FFFFF) >> 12] = 1;
	}
	our64_tlb_phys_dirty = 0;
}

/* Without tlb-links too: only the links are cut there (our64_tlb_remap). A
 * DMA marked no physical page invalid, so code compiled at TLB addresses
 * outlived the code it came from: Mario Tennis loads overlays into an 8 KB
 * window at 0x10000000 and its main thread ran the old one's code and died
 * on a store to 0 as the ball crossed the net after a serve (2026-10-21). */
void our64_phys_code_changed(unsigned long addr){
	static unsigned long last_p = ~0ul, last_vpage;
	if(our64_tlb_phys_dirty){ tlb_phys_rebuild(); last_p = ~0ul; }
	const unsigned long p = addr & 0x7FFFF000;
	if(p >= 0x800000 || !our64_tlb_phys[p >> 12]) return;
	/* A DMA calls once a byte: done while the page last marked stays marked. */
	if(p == last_p && invalid_code_get(last_vpage)) return;
	last_p = ~0ul;
	for(int e = 0; e < 32; e++){
		for(int odd = 0; odd < 2; odd++){
			const unsigned long phys = odd ? tlb_e[e].phys_odd : tlb_e[e].phys_even;
			const unsigned long start = odd ? tlb_e[e].start_odd : tlb_e[e].start_even;
			const unsigned long end = odd ? tlb_e[e].end_odd : tlb_e[e].end_even;
			if(!(odd ? tlb_e[e].v_odd : tlb_e[e].v_even)) continue;
			if(p < (phys & 0x7FFFF000) || p > (phys & 0x7FFFF000) + (end - start)) continue;
			const unsigned long vpage = (start + (p - (phys & 0x7FFFF000))) >> 12;
			last_p = p;
			last_vpage = vpage;
			if(invalid_code_get(vpage)) continue; /* already cut, not run since */
			invalid_code_set(vpage, 1);
			our64_tlb_remap(blocks_get(vpage));
		}
	}
}

void our64_tlb_remap(void* b){
	PowerPC_block* block = (PowerPC_block*)b;
	if(!our64_tlb_links || !block) return;
	unlink_tree(block->funcs);
	jr_cache_clear();
}
#endif

static void free_func(PowerPC_func* func, unsigned int addr){
#ifdef __WIIU__
	jr_cache_clear(); // it may point into this func's code
#endif
	// Free the code associated with the func
	__lwp_heap_free(cache, func->code);
#ifdef __WIIU__
	free(func->our64_mips);
#endif
	MetaCache_Free(func->code_addr);
	// Remove any holes into this func
	PowerPC_func_hole_node* hole, * next_hole;
	for(hole = func->holes; hole != NULL; hole = next_hole){
		next_hole = hole->next;
		free(hole);
	}

	// Remove any pointers to this code
	// Remove the func from the block
	PowerPC_block* block = blocks_get(addr>>12);
	remove_func(&block->funcs, func);
	// Remove func links
	unlink_func(func);

	free(func);
}

static inline void update_lru(PowerPC_func* func){
	func->lru = nextLRU++;

	if(!nextLRU || nextLRU>0x80000000){
		// Handle nextLRU overflows
		// By heap-sorting and assigning new LRUs
		heapify();
		// Since you can't do an in-place min-heap ascending-sort
		//   I have to create a new heap
		CacheMetaNode** newHeap = malloc(maxHeapSize * sizeof(CacheMetaNode*));
		int i, savedSize = heapSize;
		for(i=0; heapSize > 0; ++i){
			newHeap[i] = heapPop();
			newHeap[i]->func->lru = i;
		}
		free(cacheHeap);
		cacheHeap = newHeap;

		nextLRU = heapSize = savedSize;
	}
}

static void release(int minNeeded){
	// Frees alloc'ed blocks so that at least minNeeded bytes are available
	int toFree = minNeeded * 2; // Free 2x what is needed
	// Restore the heap properties to pop the LRU
	heapify();
	// Release nodes' memory until we've freed enough
	while(toFree > 0 && cacheSize){
		// Pop the LRU to be freed
		CacheMetaNode* n = heapPop();
		// Free the function it contains
		free_func(n->func, n->addr);
		toFree    -= n->size;
		cacheSize -= n->size;
		// And the cache node itself
		free(n);
	}
}

void RecompCache_Alloc(unsigned int size, unsigned int address, PowerPC_func* func){
	// Allocate new memory for this code
	void* code = __lwp_heap_allocate(cache, size);
	while(!code){
		release(size);
		code = __lwp_heap_allocate(cache, size);
	}
	int num_instrs = (func->end_addr - func->start_addr) >> 2;
	void* code_addr = MetaCache_Alloc(num_instrs * sizeof(void*));

	CacheMetaNode* newBlock = malloc(sizeof(CacheMetaNode));
	newBlock->addr = address;
	newBlock->size = size;
	newBlock->func = func;

	cacheSize += size;
	newBlock->func->code = code;
	newBlock->func->code_addr = code_addr;
	// Add it to the heap
	heapPush(newBlock);
	// Make this function the LRU
	update_lru(func);
}

void RecompCache_Realloc(PowerPC_func* func, unsigned int new_size){
#ifdef __WIIU__
	jr_cache_clear(); // the func's code moves
#endif
	// There should be no need for the code to be preserved
	__lwp_heap_free(cache, func->code);
	func->code = __lwp_heap_allocate(cache, new_size);
	while(!func->code){
		release(new_size);
		func->code = __lwp_heap_allocate(cache, new_size);
	}
	
	// Update the size for the cache
	int i;
	for(i=heapSize-1; i>=0; --i){
		if(cacheHeap[i]->func == func){
			cacheSize += new_size - cacheHeap[i]->size;
			cacheHeap[i]->size = new_size;
			break;
		}
	}
	
	// Remove any func links since the code has changed
	unlink_func(func);
}

void RecompCache_Free(unsigned int addr){
	int i;
	CacheMetaNode* n = NULL;
	// Find the corresponding node
	for(i=heapSize-1; i>=0; --i){
		if(cacheHeap[i]->addr == addr){
			n = cacheHeap[i];
			// Remove from the heap
			heapSwap(i, --heapSize);
			// Free n's func
			free_func(n->func, addr);
			cacheSize -= n->size;
			// Free the cache node
			free(n);
			return;
		}
	}
}

void RecompCache_Update(PowerPC_func* func){
	update_lru(func);
}

void RecompCache_Link(PowerPC_func* src_func, PowerPC_instr* src_instr,
                      PowerPC_func* dst_func, PowerPC_instr* dst_instr){
//	start_section(LINK_SECTION);
	
	// Setup book-keeping
	// Create the incoming link info
	PowerPC_func_link_node* fln =
		MetaCache_Alloc(sizeof(PowerPC_func_link_node));
	fln->branch = src_instr;
	fln->func = src_func;
	fln->next = dst_func->links_in;
	dst_func->links_in = fln;
	// Create the outgoing link info
	insert_func(&src_func->links_out, dst_func);
	
	// Actually link the funcs
#ifdef __WIIU__
	our64_jit_write_begin();
#endif
	GEN_LIS(*(src_instr-10), DYNAREG_FUNC, (unsigned int)dst_func>>16);
	GEN_ORI(*(src_instr-9), DYNAREG_FUNC,DYNAREG_FUNC, (unsigned int)dst_func);
	GEN_B(*src_instr, (PowerPC_instr*)dst_instr-src_instr, 0, 0);
	DCStoreRange(src_instr-10, 11*sizeof(PowerPC_instr));
#ifdef __WIIU__
	our64_jit_write_end();
#endif
	ICInvalidateRange(src_instr-10, 11*sizeof(PowerPC_instr));
	
//	end_section(LINK_SECTION);
}

void RecompCache_Init(void){
	if(!cache){
		cache = malloc(sizeof(heap_cntrl));
#ifdef __WIIU__
		// Our64: generated code lives in the Wii U JIT area
		cache->in_jit = 1;
		__lwp_heap_init(cache, our64_jit_heap_start(),
		                our64_jit_heap_size(), 32);
#else
		__lwp_heap_init(cache, memalign(32,RECOMP_CACHE_SIZE),
		                RECOMP_CACHE_SIZE, 32);
#endif
	}
	if(!meta_cache){
		meta_cache = malloc(sizeof(heap_cntrl));
#ifdef __WIIU__
		meta_cache->in_jit = 0;
#endif
#ifdef HW_RVL		
		__lwp_heap_init(meta_cache, RECOMPMETA_LO,
		                RECOMPMETA_SIZE, 32);
#else
    __lwp_heap_init(meta_cache, memalign(32,1*1024*1024),
		                1*1024*1024, 32);
#endif
	}
}

void* MetaCache_Alloc(unsigned int size){
	void* ptr = __lwp_heap_allocate(meta_cache, size);
	// While there's no room to allocate, call release
	while(!ptr){
		release(size);
		ptr = __lwp_heap_allocate(meta_cache, size);
	}
	
	return ptr;
}

void MetaCache_Free(void* ptr){
	__lwp_heap_free(meta_cache, ptr);
}
