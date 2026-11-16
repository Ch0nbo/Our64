#include <algorithm>
#include <cstring>
#include <cstdio>
#include "DebugDump.h"
#include "RSP.h"
#include "RDP.h"
#include "N64.h"
#include "uCodes/F3D.h"
#include "uCodes/Turbo3D.h"
#include "uCodes/T3DUX.h"
#include "VI.h"
#include "Combiner.h"
#include "FrameBuffer.h"
#include "DepthBuffer.h"
#include "FrameBufferInfo.h"
#include "GBI.h"
#include "PluginAPI.h"
#include "Config.h"
#include "TextureFilterHandler.h"
#include "DisplayWindow.h"

using namespace std;

#define SP_STATUS_HALT 0x0001
#define SP_STATUS_BROKE 0x0002
#define SP_STATUS_TASKDONE 0x0200

RSPInfo		RSP;

#ifdef OUR64_CMD_TIMING
// Our64: time spent in each display-list command, by opcode (timebase ticks
// on the console, TSC on the PC), for finding the decoder's hot spots.
#if defined(__WIIU__)
static inline u64 our64_now() { return __builtin_ppc_get_timebase(); }
#else
#include <x86intrin.h>
static inline u64 our64_now() { return __rdtsc(); }
#endif
u64 our64_cmd_ticks[256];
u32 our64_cmd_count[256];
#define OUR64_TIMED_CMD(call) do { const u32 c = RSP.cmd & 0xFF; const u64 t0 = our64_now(); call; \
	our64_cmd_ticks[c] += our64_now() - t0; ++our64_cmd_count[c]; } while (0)
#else
#define OUR64_TIMED_CMD(call) call
#endif

// Our64: display list commands run since the last task began, for the
// player's watchdog (our64_dl_state; issue 43, Rayman 2's endless task).
static volatile u32 our64_dl_cmds;

extern "C" void our64_dl_state(char * _out, unsigned _size)
{
	int n = snprintf(_out, _size, "display list: %u commands this task, microcode type %u, depth %u, at",
	                 (unsigned)our64_dl_cmds, (unsigned)GBI.getMicrocodeType(), (unsigned)RSP.PCi);
	for (u32 i = 0; i <= RSP.PCi && i < 18 && n > 0 && n < (int)_size; ++i)
		n += snprintf(_out + n, _size - n, " %06X", (unsigned)RSP.PC[i]);
	if (n > 0 && n < (int)_size)
		snprintf(_out + n, _size - n, "; last %08X %08X; count %d, halt %d, infloop %d",
		         (unsigned)RSP.w0, (unsigned)RSP.w1, (int)RSP.count, (int)RSP.halt, (int)RSP.infloop);
}

// Our64: the stuck task's memory, for walking its display list on the PC
// (issue 43). The file: "O64DL1", RDRAM as the game sees it, the segment
// table, the PC stack and its depth, then DMEM. Returns 0 when written.
extern "C" int our64_dl_dump(const char * _path)
{
	FILE * f = fopen(_path, "wb");
	if (f == nullptr)
		return -1;
	const u32 rdram_bytes = RDRAMSize + 1, pci = RSP.PCi;
	bool ok = fwrite("O64DL1\0\0", 1, 8, f) == 8 &&
		fwrite(&rdram_bytes, 4, 1, f) == 1 &&
		fwrite(RDRAM, 1, rdram_bytes, f) == rdram_bytes &&
		fwrite(gSP.segment, 4, 16, f) == 16 &&
		fwrite(RSP.PC, 4, 18, f) == 18 &&
		fwrite(&pci, 4, 1, f) == 1 &&
		fwrite(DMEM, 1, 0x1000, f) == 0x1000;
	ok = fclose(f) == 0 && ok;
	return ok ? 0 : -2;
}

// Our64: Rayman 2 (issue 43) now and then sends a display list whose calls
// lead back into themselves, so the task runs for seconds or never ends:
// an 11 s task that ended by itself, then one still running after 200 s
// (console log, 2026-11-15). Its tasks take at most about 0.3 s, so one
// past 2 s is ended there: a frame drawn short instead of a freeze. The
// player's our64_task_cut logs it.
static bool our64_cut_long_tasks;
extern "C" void our64_task_cut(unsigned _cmds) __attribute__((weak));
#if defined(__WIIU__)
static inline u64 our64_task_ticks() { return __builtin_ppc_get_timebase(); }
static const u64 OUR64_TICKS_PER_MS = 62156; // the timebase runs at a quarter of the 248.625 MHz bus
#else
#include <chrono>
static inline u64 our64_task_ticks()
{
	return (u64)std::chrono::duration_cast<std::chrono::nanoseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
}
static const u64 OUR64_TICKS_PER_MS = 1000000;
#endif
static u64 our64_task_t0;
static u64 our64_task_limit = 2000 * OUR64_TICKS_PER_MS;
// The player's test switch: a shorter limit, so normal tasks get ended too.
extern "C" void our64_set_task_limit_ms(unsigned _ms) { our64_task_limit = _ms * OUR64_TICKS_PER_MS; }

static
void _ProcessDList()
{
	while (!RSP.halt) {
		our64_dl_cmds = our64_dl_cmds + 1;
		if (our64_cut_long_tasks && (our64_dl_cmds & 0x3FFF) == 0 &&
		    our64_task_ticks() - our64_task_t0 > our64_task_limit) {
			if (our64_task_cut)
				our64_task_cut(our64_dl_cmds);
			RSP.halt = true;
			break;
		}
		if ((RSP.PC[RSP.PCi] + 8) > RDRAMSize) {
#ifdef DEBUG_DUMP
			if ((config.debug.dumpMode & DEBUG_DETAIL) != 0)
				DebugMsg(DEBUG_DETAIL | DEBUG_ERROR, "// Attempting to execute RSP command at invalid RDRAM location\n");
			else if ((config.debug.dumpMode & DEBUG_NORMAL) != 0)
				DebugMsg(DEBUG_NORMAL | DEBUG_ERROR, "Attempting to execute RSP command at invalid RDRAM location\n");
			else if ((config.debug.dumpMode & DEBUG_LOW) != 0)
				DebugMsg(DEBUG_LOW | DEBUG_ERROR, "ATTEMPTING TO EXECUTE RSP COMMAND AT INVALID RDRAM LOCATION\n");
#endif
			break;
		}

		RSP.w0 = *(u32*)&RDRAM[RSP.PC[RSP.PCi]];
		RSP.w1 = *(u32*)&RDRAM[RSP.PC[RSP.PCi] + 4];
		RSP.cmd = _SHIFTR(RSP.w0, 24, 8);

#ifdef DEBUG_DUMP
		DebugMsg(DEBUG_LOW, "0x%08lX: CMD=0x%02lX W0=0x%08lX W1=0x%08lX\n", RSP.PC[RSP.PCi], _SHIFTR(RSP.w0, 24, 8), RSP.w0, RSP.w1);
#endif

		RSP.PC[RSP.PCi] += 8;
		u32 pci = RSP.PCi;
		if (RSP.count == 1)
			--pci;
		RSP.nextCmd = _SHIFTR(*(u32*)&RDRAM[RSP.PC[pci]], 24, 8);

		OUR64_TIMED_CMD(GBI.cmd[RSP.cmd](RSP.w0, RSP.w1));
		RSP_CheckDLCounter();
	}
}

static
void _ProcessDListFactor5()
{
	// Lemmy's note: read first 64 bits of this dlist
	RSP.F5DL[0] = _SHIFTR(*(u32*)&RDRAM[RSP.PC[0]], 0, 24);
	RSP.PC[0] += 8;

	static u32 vAddrToClear[7] = { 0x11C >> 2, 0x120 >> 2, 0x124 >> 2, 0x37C >> 2,
		0x58C >> 2, 0x5B0 >> 2, 0x5B4 >> 2};
	u32 * pDmem32 = reinterpret_cast<u32*>(DMEM);
	for (u32 i = 0; i < 7; ++i)
		pDmem32[vAddrToClear[i]] = 0U;

	while (!RSP.halt) {
		if ((RSP.PC[RSP.PCi] + 8) > RDRAMSize) {
			break;
		}

		RSP.w0 = *(u32*)&RDRAM[RSP.PC[RSP.PCi]];
		RSP.w1 = *(u32*)&RDRAM[RSP.PC[RSP.PCi] + 4];
		RSP.cmd = _SHIFTR(RSP.w0, 24, 8);

#ifdef DEBUG_DUMP
		DebugMsg(DEBUG_LOW, "0x%08lX: CMD=0x%02lX W0=0x%08lX W1=0x%08lX\n", RSP.PC[RSP.PCi], _SHIFTR(RSP.w0, 24, 8), RSP.w0, RSP.w1);
#endif

		RSP.nextCmd = _SHIFTR(*(u32*)&RDRAM[RSP.PC[RSP.PCi] + 8], 24, 8);

		OUR64_TIMED_CMD(GBI.cmd[RSP.cmd](RSP.w0, RSP.w1));
		RSP.PC[RSP.PCi] += 8;
		RSP_CheckDLCounter();
	}
}

void RSP_CheckDLCounter()
{
	if (RSP.count != -1) {
		--RSP.count;
		if (RSP.count == 0) {
			RSP.count = -1;
			--RSP.PCi;
			DebugMsg(DEBUG_NORMAL, "End of DL\n");
		}
	}
}

// Our64: the next ProcessDList carries on a task that stopped on a wait
// (ZSortBOSS's signal waits, a display list branching to itself). The RSP's
// DMEM and IMEM are as that task left them then, so the host does not load
// the CPU's copy over them (player video.cpp, replay engine).
extern "C" int our64_rsp_continues(void)
{
	return RSP.infloop ? 1 : 0;
}

void RSP_ProcessDList()
{
	RSP.LLE = false;

	if (ConfigOpen || dwnd().isResizeWindow()) {
		*REG.MI_INTR |= MI_INTR_DP;
		CheckInterrupts();
		return;
	}

	if (RSP.infloop) {
		RSP.infloop = false;
		RSP.halt = false;
	} else {
		if (*REG.VI_ORIGIN != VI.lastOrigin) {
			VI_UpdateSize();
			dwnd().updateScale();
		}

		RSP.PC[0] = *(u32*)&DMEM[0x0FF0];
		RSP.PCi = 0;
		our64_dl_cmds = 0;
		RSP.count = -1;

		RSP.halt = false;
		RSP.busy = true;

		gSP.matrix.stackSize = min( 32U, *(u32*)&DMEM[0x0FE4] >> 6 );
		if (gSP.matrix.stackSize == 0)
			gSP.matrix.stackSize = 32;
		gSP.matrix.modelViewi = 0;
		gSP.status[0] = gSP.status[1] = gSP.status[2] = gSP.status[3] = 0;
		gSP.geometryMode = 0U;
		memset(&gSP.lookat, 0, sizeof(gSPInfo::lookat));
		gSP.lookat.xyz[0][Y] = gSP.lookat.xyz[1][X] = 1.0f;
		memset(&gSP.lights, 0, sizeof(gSPInfo::lights));
		gSP.ao = { 1.f, 0.625f, 0 };
		gSP.attrOffset = { 0x0100, -0x0100 };
		gSP.fresnel = {};
		gSP.alphaCompareCull = {};
		gSP.changed |= CHANGED_MATRIX | CHANGED_LIGHT | CHANGED_LOOKAT | CHANGED_GEOMETRYMODE;
		gSP.tri_num = 0;
		gSP.cbfd.advancedLighting = false;
		gDP.changed &= ~CHANGED_CPU_FB_WRITE;
		gDPSetTexturePersp(G_TP_PERSP);

		// Get the start of the display list and the length of it
		const u32 dlist_start = *(u32*)(DMEM + 0xFF0);
		const u32 dlist_length = *(u32*)(DMEM + 0xFF4);
		DebugMsg(DEBUG_NORMAL, "--- NEW DLIST --- ucode: %d, fbuf: %08lx, fbuf_width: %d, dlist start: %08lx, dlist_length: %d, x_scale: %f, y_scale: %f\n",
			GBI.getMicrocodeType(), *REG.VI_ORIGIN, *REG.VI_WIDTH, dlist_start, dlist_length, (*REG.VI_X_SCALE & 0xFFF) / 1024.0f, (*REG.VI_Y_SCALE & 0xFFF) / 1024.0f);

		u32 uc_start = *(u32*)&DMEM[0x0FD0];
		u32 uc_dstart = *(u32*)&DMEM[0x0FD8];
		u32 uc_dsize = *(u32*)&DMEM[0x0FDC];

		if ((uc_start != RSP.uc_start) || (uc_dstart != RSP.uc_dstart))
			gSPLoadUcodeEx(uc_start, uc_dstart, uc_dsize);

		depthBufferList().setCleared(false);

		if (GBI.getMicrocodeType() == ZSortBOSS) {
			RSP.PC[1] = *(u32*)&DMEM[0xff8];
			*REG.SP_STATUS &= ~0x300;  // clear sig1 | sig2
			*REG.SP_STATUS |= 0x800;   // set sig4
		}
	}

	switch (GBI.getMicrocodeType()) {
	case Turbo3D:
		RunTurbo3D();
		break;
	case T3DUX:
		RunT3DUX();
		break;
	case F5Rogue:
	case F5Indi_Naboo:
		_ProcessDListFactor5();
		break;
	default:
		our64_task_t0 = our64_task_ticks();
		_ProcessDList();
		break;
	}

	if (RSP.infloop && REG.SP_STATUS) {
		*REG.SP_STATUS &= ~(SP_STATUS_TASKDONE | SP_STATUS_HALT | SP_STATUS_BROKE);
		return;
	}

	if (config.frameBufferEmulation.copyDepthToRDRAM != Config::cdDisable) {
		if ((config.generalEmulation.hacks & hack_rectDepthBufferCopyCBFD) != 0) {
			; // do nothing
		} else if ((config.generalEmulation.hacks & hack_rectDepthBufferCopyPD) != 0) {
			if (rectDepthBufferCopyFrame == dwnd().getBuffersSwapCount())
				FrameBuffer_CopyDepthBuffer(gDP.colorImage.address);
		} else if (!FBInfo::fbInfo.isSupported())
			FrameBuffer_CopyDepthBuffer(gDP.colorImage.address);
	}

	RSP.busy = false;
	gDP.changed |= CHANGED_COLORBUFFER;
}

static
void RSP_SetDefaultState()
{
	memset(&gSP, 0, sizeof(gSPInfo));

	gSPTexture(1.0f, 1.0f, 0, 0, TRUE);
	gDP.loadTile = &gDP.tiles[7];
	gSP.textureTile[0] = &gDP.tiles[0];
	gSP.textureTile[1] = &gDP.tiles[1];
	gSP.lookat.xyz[0][Y] = gSP.lookat.xyz[1][X] = 1.0f;
	gSP.lookatEnable = true;

	gSP.objRendermode = 0;

	for (int i = 0; i < 4; i++)
	for (int j = 0; j < 4; j++)
		gSP.matrix.modelView[0][i][j] = 0.0f;

	gSP.matrix.modelView[0][0][0] = 1.0f;
	gSP.matrix.modelView[0][1][1] = 1.0f;
	gSP.matrix.modelView[0][2][2] = 1.0f;
	gSP.matrix.modelView[0][3][3] = 1.0f;

	gSP.clipRatio = 1U;

	gDP.otherMode._u64 = 0U;
	gDP.otherMode.bi_lerp0 = gDP.otherMode.bi_lerp1 = 1;
}

u32 DepthClearColor = 0xfffcfffc;
extern "C" { char our64_fix_name[21]; }

static
void setDepthClearColor()
{
	if (strstr(RSP.fixname, (const char *)"Elmo's") != nullptr)
		DepthClearColor = 0xFFFFFFFF;
	else if (strstr(RSP.fixname, (const char *)"Taz Express") != nullptr)
		DepthClearColor = 0xFFBCFFBC;
	else if (strstr(RSP.fixname, (const char *)"NFL QBC 2000") != nullptr || strstr(RSP.fixname, (const char *)"NFL Quarterback Club") != nullptr || strstr(RSP.fixname, (const char *)"Jeremy McGrath Super") != nullptr)
		DepthClearColor = 0xFFFDFFFC;
	else
		DepthClearColor = 0xFFFCFFFC;
}

void RSP_Init()
{
	if (RDRAMSize == 0) {
#ifdef OS_WINDOWS
		// Calculate RDRAM size by intentionally causing an access violation
		u32 test;
		try
		{
			test = RDRAM[0x007FFFFF] + 1;
		}
		catch (...)
		{
			test = 0;
		}
		if (test > 0)
			RDRAMSize = 0x7FFFFF;
		else
			RDRAMSize = 0x3FFFFF;
#else // OS_WINDOWS
		RDRAMSize = 1024 * 1024 * 8 - 1;
#endif // OS_WINDOWS
	}

	RSP.uc_start = RSP.uc_dstart = 0;
	RSP.LLE = false;
	RSP.infloop = false;

	// get the name of the ROM
	char romname[21];
	for (int i = 0; i < 20; ++i)
		romname[i] = HEADER[(32 + i) ^ BYTE_ADDR_XOR];
	romname[20] = 0;

	// remove all trailing spaces
	while (romname[strlen(romname) - 1] == ' ')
		romname[strlen(romname) - 1] = 0;

	if (strcmp(RSP.romname, romname) != 0)
		TFH.shutdown();

	strncpy(RSP.romname, romname, 21);
	// Our64: a ROM hack's own name, or the game picked in Game options > Fixes;
	// romname stays the ROM's own for its shader files and logs.
	strncpy(RSP.fixname, our64_fix_name[0] ? our64_fix_name : romname, 21);
	RSP.fixname[20] = 0;
	while (RSP.fixname[0] && RSP.fixname[strlen(RSP.fixname) - 1] == ' ')
		RSP.fixname[strlen(RSP.fixname) - 1] = 0;
	setDepthClearColor();
	our64_cut_long_tasks = strstr(RSP.fixname, (const char *)"Rayman 2") != nullptr;
	config.generalEmulation.hacks = 0;
	if (strstr(RSP.fixname, (const char *)"OgreBattle64") != nullptr)
		config.generalEmulation.hacks |= hack_Ogre64;
	else if (strstr(RSP.fixname, (const char *)"F1 POLE POSITION 64") != nullptr)
		config.generalEmulation.hacks |= hack_noDepthFrameBuffers;
	else if (strstr(RSP.fixname, (const char *)"ROADSTERS TROPHY") != nullptr)
		config.generalEmulation.hacks |= hack_noDepthFrameBuffers;
	else if (strstr(RSP.fixname, (const char *)"VIGILANTE 8") != nullptr)
		config.generalEmulation.hacks |= hack_noDepthFrameBuffers;
	else if (strstr(RSP.fixname, (const char *)"CONKER BFD") != nullptr)
		config.generalEmulation.hacks |= hack_blurPauseScreen | hack_rectDepthBufferCopyCBFD | hack_fbTextureOffset;
	else if (strstr(RSP.fixname, (const char *)"MICKEY USA") != nullptr)
		config.generalEmulation.hacks |= hack_blurPauseScreen;
	else if (strstr(RSP.fixname, (const char *)"GOLDENEYE") != nullptr)
		config.generalEmulation.hacks |= hack_clearAloneDepthBuffer;
	else if (strstr(RSP.fixname, (const char *)"STARCRAFT 64") != nullptr)
		config.generalEmulation.hacks |= hack_StarCraftBackgrounds;
	else if (strstr(RSP.fixname, (const char *)"THE LEGEND OF ZELDA") != nullptr ||
			 strstr(RSP.fixname, (const char *)"ZELDA MASTER QUEST") != nullptr)
		config.generalEmulation.hacks |= hack_subscreen | hack_ZeldaMonochrome;
	else if (strstr(RSP.fixname, (const char *)"DOUBUTSUNOMORI") != nullptr ||
			 strstr(RSP.fixname, (const char *)"ANIMAL FOREST") != nullptr)
		config.generalEmulation.hacks |= hack_subscreen;
	else if (strstr(RSP.fixname, (const char *)"Lode Runner 3D") != nullptr)
		config.generalEmulation.hacks |= hack_LodeRunner;
	else if (strstr(RSP.fixname, (const char *)"Blast") != nullptr)
		config.generalEmulation.hacks |= hack_blastCorps;
	else if (strstr(RSP.fixname, (const char *)"MASK") != nullptr) // Zelda MM
		config.generalEmulation.hacks |= hack_ZeldaMonochrome | hack_ZeldaMM;
	else if (strstr(RSP.fixname, (const char *)"Perfect Dark") != nullptr ||
			 strstr(RSP.fixname, (const char *)"PERFECT DARK") != nullptr)
		config.generalEmulation.hacks |= hack_rectDepthBufferCopyPD | hack_clearAloneDepthBuffer;
	else if (strstr(RSP.fixname, (const char *)"Jeremy McGrath Super") != nullptr)
		config.generalEmulation.hacks |= hack_ModifyVertexXyInShader;
	else if (strstr(RSP.fixname, (const char *)"RAT ATTACK") != nullptr)
		config.generalEmulation.hacks |= hack_ModifyVertexXyInShader;
	else if (strstr(RSP.fixname, (const char *)"Quake") != nullptr)
		config.generalEmulation.hacks |= hack_doNotResetOtherModeH|hack_doNotResetOtherModeL;
	else if (strstr(RSP.fixname, (const char *)"QUAKE II") != nullptr ||
			 strstr(RSP.fixname, (const char *)"GAUNTLET LEGENDS") != nullptr)
		config.generalEmulation.hacks |= hack_doNotResetOtherModeH;
	else if (strstr(RSP.fixname, (const char *)"quarterback_club_98") != nullptr)
		config.generalEmulation.hacks |= hack_LoadDepthTextures;
	else if (strstr(RSP.fixname, (const char *)"WIN BACK") != nullptr ||
		strstr(RSP.fixname, (const char *)"OPERATION WINBACK") != nullptr)
		config.generalEmulation.hacks |= hack_WinBack;
	else if (strstr(RSP.fixname, (const char *)"POKEMON SNAP") != nullptr)
		config.generalEmulation.hacks |= hack_Snap;
	else if (strstr(RSP.fixname, (const char *)"MARIOKART64") != nullptr)
		config.generalEmulation.hacks |= hack_MK64;
	else if (strstr(RSP.fixname, (const char *)"Resident Evil II") ||
			 strstr(RSP.fixname, (const char *)"BioHazard II"))
		config.generalEmulation.hacks |= hack_RE2 | hack_ModifyVertexXyInShader | hack_LoadDepthTextures;
	else if (strstr(RSP.fixname, (const char *)"THPS") != nullptr)
		config.generalEmulation.hacks |= hack_TonyHawk;
	else if (strstr(RSP.fixname, (const char *)"NITRO64") != nullptr)
		config.generalEmulation.hacks |= hack_WCWNitro;
	else if (strstr(RSP.fixname, (const char *)"DR.MARIO 64") != nullptr)
		config.generalEmulation.hacks |= hack_fullRdramCopy;
	else if (strstr(RSP.fixname, (const char *)"Fighting Force") != nullptr)
		config.generalEmulation.hacks |= hack_fastLodSharpen;
	else if (strstr(RSP.fixname, (const char *)"MarioTennis") != nullptr)
		config.generalEmulation.hacks |= hack_fbTextureOffset | hack_refreshRdramCopy;
	else if (strstr(RSP.fixname, (const char *)"Extreme G 2") != nullptr ||
		strstr(RSP.fixname, (const char *)"\xb4\xb8\xbd\xc4\xd8\xb0\xd1\x47\x32") != nullptr)
		config.generalEmulation.hacks |= hack_noDepthFrameBuffers;
	else if (strstr(RSP.fixname, (const char *)"PAPER MARIO") ||
		strstr(RSP.fixname, (const char *)"MARIO STORY"))
		config.generalEmulation.hacks |= hack_paper_mario_subscreen;

	api().FindPluginPath(RSP.pluginpath);

	RSP_SetDefaultState();
}
