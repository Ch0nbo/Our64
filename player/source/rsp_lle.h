/* RSP tasks the HLE has no code for, run instruction by instruction on cxd4's
 * interpreter (third_party/cxd4). Portable: the PC tests run it too. */
#ifndef RSP_LLE_H
#define RSP_LLE_H

#include <stdint.h>

/* Memory as the CPU core keeps it: native 32-bit words (big-endian on the
 * Wii U; on a little-endian PC each word's bytes are swapped), IMEM right
 * after DMEM. RDRAM is 8 MB. */
typedef struct {
	unsigned char *rdram, *dmem, *imem;
	uint32_t *mi_intr;
	uint32_t *sp[8];  /* MEM_ADDR, DRAM_ADDR, RD_LEN, WR_LEN, STATUS, DMA_FULL, DMA_BUSY, SEMAPHORE */
	uint32_t *sp_pc;
	uint32_t *dpc[8]; /* START, END, CURRENT, STATUS, CLOCK, BUFBUSY, PIPEBUSY, TMEM */
	void (*check_interrupts)(void);
} rsp_lle_regs;

void rsp_lle_init(const rsp_lle_regs *r);
/* Points cxd4 at other registers (the memory stays), keeping where the task is. */
void rsp_lle_bind(const rsp_lle_regs *r);

/* Pokemon Stadium's and Stadium 2's GB Tower: the RSP program that turns the
 * Game Boy picture into framebuffer pixels. It is its own boot code (an
 * M_AUDTASK whose ucode_boot, loaded at IMEM 0x1000, starts 0x401A6000
 * 0xC800207C). The boot size of the variant found, or 0. *code_seen (when
 * not NULL) says whether the code was there, whatever the boot size. */
uint32_t rsp_lle_gb_tower(const unsigned char *rdram, const unsigned char *dmem, const unsigned char *imem, int *code_seen);

/* Whether IMEM holds that program (it is still there after its task). */
int rsp_lle_gb_loaded(const unsigned char *imem);

/* Runs the task from SP_PC until it breaks. MFC0 reads of SP_STATUS see the
 * bits in hidden as clear (the register keeps them). Stops with the RSP
 * halted after limit instructions (0: no limit). With stop_at_sig3, returns
 * right after an MTC0 that sets SIG3, the RSP still running; calling again
 * carries on. Returns the number of instructions run; *result says how it
 * ended. */
enum { RSP_LLE_BROKE, RSP_LLE_AT_SIG3, RSP_LLE_STOPPED };
unsigned long rsp_lle_run(uint32_t hidden, unsigned long limit, int stop_at_sig3, int *result);

/* Reserved instructions and odd register writes cxd4 met so far. */
unsigned rsp_lle_messages(void);

#endif
