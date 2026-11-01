/* Our64: Wii U JIT area for Not64's recompiler.
 *
 * The area granted by Aroma runs code only while it is not writable, and only
 * the codegen core may switch it. Every write to it sits inside a
 * our64_jit_write_begin/end pair; pairs nest, and only the outermost one
 * switches modes (about 0.3 us per round trip on hardware).
 *
 * Generated code cannot reach the app's C code with a relative branch (it is
 * 168 MB away on hardware), so calls go through 4-instruction stubs kept at
 * the start of the area, where a relative branch from anywhere in it reaches. */
#ifndef OUR64_JIT_H
#define OUR64_JIT_H

int   our64_jit_init(void);          /* 0 on success */
void *our64_jit_heap_start(void);
unsigned int our64_jit_heap_size(void);
void  our64_jit_write_begin(void);
void  our64_jit_write_end(void);
void *our64_jit_call_stub(void *target);
/* Code that returns to the dispatcher with r3 = mips (a MIPS address): for a
 * branch whose target has no code of its own. NULL when the area is full. */
void *our64_jit_exit_stub(unsigned int mips);
unsigned int our64_jit_stub_count(void);
unsigned int our64_jit_switches(void);
int   our64_jit_selftest(unsigned *got, unsigned *want); /* 0 = a stub call delivered all six arguments */
#endif
