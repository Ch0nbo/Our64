/* Our64: the libogc processor helpers Not64 uses. */
#ifndef OUR64_PROCESSOR_H
#define OUR64_PROCESSOR_H
#include <gctypes.h>

static inline u32 __lwbrx(void *base, u32 off)
{
	u32 v;
	__asm__ volatile("lwbrx %0,%1,%2" : "=r"(v) : "b"(base), "r"(off) : "memory");
	return v;
}

static inline u16 __lhbrx(void *base, u32 off)
{
	u32 v;
	__asm__ volatile("lhbrx %0,%1,%2" : "=r"(v) : "b"(base), "r"(off) : "memory");
	return (u16)v;
}

/* Interrupt masking is not available to a Wii U app; the callers only use it
 * around GameCube serial transfers, which are stubbed out. */
#define _CPU_ISR_Disable(level) ((level) = 0)
#define _CPU_ISR_Restore(level) ((void)(level))
#define _CPU_ISR_Flash(level)   ((void)(level))
#endif
