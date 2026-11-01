/* Our64 bench: plugin entry points for a headless run. Not64's RSP HLE plugin
 * handles every RSP task (audio included, so its CPU cost is measured); the
 * graphics, audio-output and input plugins do nothing. */
#include <gctypes.h>
#include <string.h>
#include "main/winlnxdefs.h"
#include "main/plugin.h"
#include "gc_memory/memory.h"
#include "r4300/r4300.h"
#include "bench.h"
#include "input.h"

static void none(void) {}

/* A graphics task ends with a full sync, which raises the DP interrupt. */
static void count_dlist(void)
{
	bench_on_gfx_task();
	g_bench.gfx_tasks++;
	MI_register.mi_intr_reg |= 0x20;
}

static void count_alist(void) { g_bench.audio_tasks++; }

static void check_origin(void)
{
	if (vi_register.vi_origin != g_bench.last_origin) {
		g_bench.last_origin = vi_register.vi_origin;
		g_bench.origin_changes++;
	}
}

/* Controller 1 present, driven by the ROM's input script (input.h). */
static void read_controller(int Control, BYTE *Command) { (void)Control; (void)Command; }
static void get_keys(int Control, BUTTONS *Keys) { Keys->Value = Control == 0 ? input_keys(g_bench.vis) : 0; }
static void controller_command(int Control, BYTE *Command) { (void)Control; (void)Command; }

/* Not64 links its plugins statically, so these are plain functions. */
void processDList(void) { count_dlist(); }
/* The bench runs graphics tasks in the HLE plugin, so no GFX_INT event is ever added. */
int gfx_int_event(void) { return 0; }
void processAList(void) { count_alist(); }
void processRDPList(void) {}
void showCFB(void) {}
void updateScreen(void) { check_origin(); }
void viStatusChanged(void) {}
void viWidthChanged(void) {}
void aiDacrateChanged(int SystemType) { (void)SystemType; }
void aiLenChanged(void) {}
DWORD aiReadLength(void) { return 0; }
void controllerCommand(int Control, BYTE *Command) { controller_command(Control, Command); }
void getKeys(int Control, BUTTONS *Keys) { get_keys(Control, Keys); }
void readController(int Control, BYTE *Command) { read_controller(Control, Command); }
void (*fBRead)(DWORD addr) = 0;
void (*fBWrite)(DWORD addr, DWORD size) = 0;
void (*fBGetFrameBufferInfo)(void *p) = 0;

CONTROL Controls[4];
extern char padType[4]; /* defined below */

static void dummy_check(void) {}

void bench_plugins_init(void)
{
	static RSP_INFO rsp_info;
	static DWORD cycle_count;
	rsp_info.MemoryBswaped = TRUE;
	rsp_info.RDRAM = (BYTE *)rdram;
	rsp_info.DMEM = (BYTE *)SP_DMEM;
	rsp_info.IMEM = (BYTE *)SP_IMEM;
	rsp_info.MI_INTR_REG = &MI_register.mi_intr_reg;
	rsp_info.SP_MEM_ADDR_REG = &sp_register.sp_mem_addr_reg;
	rsp_info.SP_DRAM_ADDR_REG = &sp_register.sp_dram_addr_reg;
	rsp_info.SP_RD_LEN_REG = &sp_register.sp_rd_len_reg;
	rsp_info.SP_WR_LEN_REG = &sp_register.sp_wr_len_reg;
	rsp_info.SP_STATUS_REG = &sp_register.sp_status_reg;
	rsp_info.SP_DMA_FULL_REG = &sp_register.sp_dma_full_reg;
	rsp_info.SP_DMA_BUSY_REG = &sp_register.sp_dma_busy_reg;
	rsp_info.SP_PC_REG = &rsp_register.rsp_pc;
	rsp_info.SP_SEMAPHORE_REG = &sp_register.sp_semaphore_reg;
	rsp_info.DPC_START_REG = &dpc_register.dpc_start;
	rsp_info.DPC_END_REG = &dpc_register.dpc_end;
	rsp_info.DPC_CURRENT_REG = &dpc_register.dpc_current;
	rsp_info.DPC_STATUS_REG = &dpc_register.dpc_status;
	rsp_info.DPC_CLOCK_REG = &dpc_register.dpc_clock;
	rsp_info.DPC_BUFBUSY_REG = &dpc_register.dpc_bufbusy;
	rsp_info.DPC_PIPEBUSY_REG = &dpc_register.dpc_pipebusy;
	rsp_info.DPC_TMEM_REG = &dpc_register.dpc_tmem;
	rsp_info.CheckInterrupts = dummy_check;
	rsp_info.ProcessDlistList = count_dlist;
	rsp_info.ProcessAlistList = count_alist;
	rsp_info.ProcessRdpList = none;
	rsp_info.ShowCFB = none;
	initiateRSP(rsp_info, &cycle_count); /* rsp_hle-ppc, which also provides doRspCycles */
	/* It keeps which code each microcode address runs, until a ROM closes: one
	 * bench run loads many ROMs (16 kept at most: tlb-count's 21st, Blast Corps,
	 * failed an assert). */
	extern void romClosed_RSP(void);
	romClosed_RSP();

	for (int i = 0; i < 4; i++) {
		Controls[i].Present = i == 0;
		Controls[i].RawData = FALSE;
		Controls[i].Plugin = PLUGIN_NONE;
		/* Non-zero: the PIF answers through internal_ControllerCommand and
		 * internal_ReadController (getKeys). With 0, status commands go
		 * unanswered and games report no controller. */
		padType[i] = 1; /* all four through the internal PIF code: 2-4 answer "not connected" */
	}
	g_bench.last_origin = 0;
}

/* Not in the benchmark: savestates. padType is the GUI's pad-type setting,
 * set in bench_plugins_init. */
int  savestates_job;
void savestates_save(void) {}
void savestates_load(void) {}
char padType[4];

/* The player turns Rumble Pak writes into controller motors; the bench has none. */
void our64_rumble(int port, int on) { (void)port; (void)on; }
void our64_pak_wanted(int port, int rumble, int address) { (void)port; (void)rumble; (void)address; }
int our64_pak_pulled(int port) { (void)port; return 0; }
void our64_pak_trace(int port, int write, int address, int value) { (void)port; (void)write; (void)address; (void)value; }

/* Not64's Transfer Pak hooks (the player's tpak_wiiu.c): the bench has no Game Boy cartridges. */
void our64_tpak_read(int control, int address, unsigned char *data) { (void)control; (void)address; memset(data, 0, 32); }
void our64_tpak_write(int control, int address, const unsigned char *data) { (void)control; (void)address; (void)data; }
void our64_tpak_wanted(int control, int address, const unsigned char *data) { (void)control; (void)address; (void)data; }
