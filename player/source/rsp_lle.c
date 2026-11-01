/* RSP tasks the HLE has no code for, on cxd4's interpreter.
 *
 * third_party/cxd4 is cxd4's RSP (CC0, https://github.com/cxd4/rsp at
 * 00906a92641c540a64e4e7505012157315314810): the scalar and vector units
 * (su.c, vu/) without its plugin glue (module.c: config files, signal
 * handlers, a message box that waits for ENTER), which this file replaces.
 * Our64's changes to su.c, each marked "Our64": MFC0 of SP_STATUS can read
 * chosen bits as clear, and run_task counts its instructions and stops after
 * a limit. No SSE path is built (ARCH_MIN_SSE2 is never defined) and byte
 * order follows __BIG_ENDIAN__, so on the Wii U memory is plain big-endian
 * words, as Not64 keeps it. In the player its symbols are made local after
 * linking (player/Makefile), so its SR, DMEM, message and the rest cannot
 * meet rsp_hle's or GLideN64's. */
#include "rsp_lle.h"
#include "su.h"
#include <string.h>

RSP_INFO RSP_INFO_NAME;
p_func GBI_phase;

extern u32 our64_mf_status_hidden;
extern int our64_sig3_set, our64_stop_at_sig3;
extern unsigned long our64_run_instructions, our64_run_limit;

static unsigned g_messages;

/* cxd4 calls this for reserved instructions and odd register writes. */
void message(const char *body)
{
	(void)body;
	g_messages++;
}

static void no_rdp(void) {}

void rsp_lle_init(const rsp_lle_regs *r)
{
	memset(&RSP_INFO_NAME, 0, sizeof(RSP_INFO_NAME));
	rsp_lle_bind(r);
	GBI_phase = no_rdp; /* an RDP list the task starts is not drawn */
	MF_SP_STATUS_TIMEOUT = 32767; /* as cxd4's plugin: a task polling SP_STATUS that long halts */
	su_max_address = 0x7FFFFF;    /* 8 MB */
}

void rsp_lle_bind(const rsp_lle_regs *r)
{
	RSP_INFO_NAME.MemorySwapped = 1;
	RSP_INFO_NAME.RDRAM = DRAM = r->rdram;
	RSP_INFO_NAME.DMEM = DMEM = r->dmem;
	RSP_INFO_NAME.IMEM = IMEM = r->imem;
	RSP_INFO_NAME.MI_INTR_REG = r->mi_intr;
	pu32 *const sp[8] = {&RSP_INFO_NAME.SP_MEM_ADDR_REG, &RSP_INFO_NAME.SP_DRAM_ADDR_REG,
	                     &RSP_INFO_NAME.SP_RD_LEN_REG, &RSP_INFO_NAME.SP_WR_LEN_REG,
	                     &RSP_INFO_NAME.SP_STATUS_REG, &RSP_INFO_NAME.SP_DMA_FULL_REG,
	                     &RSP_INFO_NAME.SP_DMA_BUSY_REG, &RSP_INFO_NAME.SP_SEMAPHORE_REG};
	pu32 *const dp[8] = {&RSP_INFO_NAME.DPC_START_REG, &RSP_INFO_NAME.DPC_END_REG,
	                     &RSP_INFO_NAME.DPC_CURRENT_REG, &RSP_INFO_NAME.DPC_STATUS_REG,
	                     &RSP_INFO_NAME.DPC_CLOCK_REG, &RSP_INFO_NAME.DPC_BUFBUSY_REG,
	                     &RSP_INFO_NAME.DPC_PIPEBUSY_REG, &RSP_INFO_NAME.DPC_TMEM_REG};
	for (int i = 0; i < 8; i++) {
		*sp[i] = CR[i] = (pu32)r->sp[i];
		*dp[i] = CR[8 + i] = (pu32)r->dpc[i];
	}
	RSP_INFO_NAME.SP_PC_REG = (pu32)r->sp_pc;
	RSP_INFO_NAME.CheckInterrupts = r->check_interrupts;
}

static uint32_t word(const unsigned char *p, uint32_t off)
{
	uint32_t w;
	memcpy(&w, p + off, 4);
	return w;
}

static const uint32_t gb_w0 = 0x401A6000, gb_w1 = 0xC800207C; /* mfc0 $26, DPC_CLOCK; lqv $v0[0], -0x40($zero) */

int rsp_lle_gb_loaded(const unsigned char *imem) { return word(imem, 0) == gb_w0 && word(imem, 4) == gb_w1; }

uint32_t rsp_lle_gb_tower(const unsigned char *rdram, const unsigned char *dmem, const unsigned char *imem, int *code_seen)
{
	/* Stadium 2's three, then Stadium 1's gbTowerMain and gbTowerColorMain. */
	static const uint32_t sizes[] = {0xA10, 0xB30, 0xA60, 0xA40, 0x9E4};
	const uint32_t boot = word(dmem, 0xFC8) & 0x7FFFFC, size = word(dmem, 0xFCC);
	const int seen = (boot + 8 <= 0x800000 && word(rdram, boot) == gb_w0 && word(rdram, boot + 4) == gb_w1) ||
	                 rsp_lle_gb_loaded(imem); /* already loaded at IMEM 0x1000 */
	if (code_seen)
		*code_seen = seen;
	if (!seen)
		return 0;
	for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++)
		if (size == sizes[i])
			return size;
	return 0;
}

unsigned long rsp_lle_run(uint32_t hidden, unsigned long limit, int stop_at_sig3, int *result)
{
	for (int i = 0; i < NUMBER_OF_SCALAR_REGISTERS; i++)
		MFC0_count[i] = 0;
	our64_mf_status_hidden = hidden;
	our64_run_limit = limit;
	our64_run_instructions = 0;
	our64_sig3_set = 0;
	our64_stop_at_sig3 = stop_at_sig3;
	run_task();
	our64_mf_status_hidden = 0;
	our64_stop_at_sig3 = 0;
	*result = (*CR[4] & SP_STATUS_BROKE) ? RSP_LLE_BROKE
	        : !(*CR[4] & SP_STATUS_HALT) && our64_sig3_set ? RSP_LLE_AT_SIG3 : RSP_LLE_STOPPED;
	return our64_run_instructions;
}

unsigned rsp_lle_messages(void) { return g_messages; }
