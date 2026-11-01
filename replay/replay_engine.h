// Our64 replay engine: plays captured frames (.o64f) through a mupen64plus
// video plugin. Shared by the PC tool (tools/replay-pc/replay64, plugin found
// with dlsym) and the console replay app (replay/source, plugin linked in).
//
// It stands in for the mupen64plus core: it fills GFX_INFO from each capture
// and calls ProcessDList and UpdateScreen. Frames from one game are played in
// order, so the plugin's frame-buffer tracking sees a continuous sequence.
#pragma once
#include <zlib.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "m64p_types.h"
#include "m64p_common.h"
#include "m64p_plugin.h"

#include "../bench/source/capture.h"

namespace replay {

struct Frame {
	std::string name; // file name without directory or extension
	o64f_header h;
	std::vector<uint8_t> dmem, imem, rdram; // as N64 bytes
};

inline uint32_t be32(const uint8_t *p) { return uint32_t(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

inline bool load(const char *path, Frame &f)
{
	FILE *fp = fopen(path, "rb");
	if (!fp) return false;
	f.name = path;
	f.name = f.name.substr(f.name.find_last_of('/') + 1);
	f.name = f.name.substr(0, f.name.find_last_of('.'));
	uint8_t raw[sizeof(o64f_header)];
	bool ok = fread(raw, sizeof(raw), 1, fp) == 1;
	// Every field after the magic and the ROM name is a big-endian word.
	memcpy(&f.h, raw, sizeof(raw));
	uint32_t *w = (uint32_t *)&f.h;
	for (size_t i = 1; i < sizeof(f.h) / 4; i++)
		if (i < 2 || i >= 7) w[i] = be32(raw + 4 * i);
	ok = ok && !memcmp(f.h.magic, "O64F", 4) && f.h.version == O64F_VERSION;
	f.dmem.resize(0x1000);
	f.imem.resize(0x1000);
	ok = ok && fread(f.dmem.data(), 0x1000, 1, fp) == 1 && fread(f.imem.data(), 0x1000, 1, fp) == 1;
	if (ok) {
		std::vector<uint8_t> z(f.h.rdram_zsize);
		f.rdram.resize(f.h.rdram_size);
		uLongf n = f.rdram.size();
		ok = fread(z.data(), z.size(), 1, fp) == 1 && uncompress(f.rdram.data(), &n, z.data(), z.size()) == Z_OK;
	}
	fclose(fp);
	return ok;
}

// mupen64plus keeps N64 memory as host-endian 32-bit words.
inline void to_words(uint8_t *dst, const uint8_t *src, size_t size)
{
	for (size_t i = 0; i < size; i += 4) {
		const uint32_t v = be32(src + i);
		memcpy(dst + i, &v, 4);
	}
}

struct Plugin {
	ptr_PluginStartup PluginStartup;
	ptr_InitiateGFX InitiateGFX;
	ptr_RomOpen RomOpen;
	ptr_RomClosed RomClosed;
	ptr_ProcessDList ProcessDList;
	ptr_UpdateScreen UpdateScreen;
	ptr_ViStatusChanged ViStatusChanged;
	ptr_ViWidthChanged ViWidthChanged;
	ptr_ReadScreen2 ReadScreen2;
	ptr_PluginShutdown PluginShutdown;
};

class Host {
public:
	// Starts the plugin for one game, from the game's first frame.
	// core_handle is what the plugin passes back to look up core functions.
	bool start(const Plugin &p, void *core_handle, const Frame &first, void (*debug)(void *, int, const char *))
	{
		m_p = p;
		uint8_t hdr_be[0x40] = {0x80, 0x37, 0x12, 0x40};
		for (int k = 0; k < 2; k++)
			for (int b = 0; b < 4; b++) hdr_be[0x10 + 4 * k + b] = first.h.rom_crc[k] >> (24 - 8 * b);
		memcpy(hdr_be + 0x20, first.h.rom_name, 20);
		to_words(m_header, hdr_be, sizeof(hdr_be));

		GFX_INFO gi = {};
		unsigned *r = m_regs;
		gi.HEADER = m_header;
		gi.RDRAM = m_rdram;
		gi.DMEM = m_dmem;
		gi.IMEM = m_imem;
		gi.MI_INTR_REG = r + 0;
		gi.DPC_START_REG = r + 1; gi.DPC_END_REG = r + 2; gi.DPC_CURRENT_REG = r + 3; gi.DPC_STATUS_REG = r + 4;
		gi.DPC_CLOCK_REG = r + 5; gi.DPC_BUFBUSY_REG = r + 6; gi.DPC_PIPEBUSY_REG = r + 7; gi.DPC_TMEM_REG = r + 8;
		unsigned **vi[14] = {&gi.VI_STATUS_REG, &gi.VI_ORIGIN_REG, &gi.VI_WIDTH_REG, &gi.VI_INTR_REG,
		                     &gi.VI_V_CURRENT_LINE_REG, &gi.VI_TIMING_REG, &gi.VI_V_SYNC_REG, &gi.VI_H_SYNC_REG,
		                     &gi.VI_LEAP_REG, &gi.VI_H_START_REG, &gi.VI_V_START_REG, &gi.VI_V_BURST_REG,
		                     &gi.VI_X_SCALE_REG, &gi.VI_Y_SCALE_REG};
		for (int k = 0; k < 14; k++) *vi[k] = r + 16 + k;
		gi.CheckInterrupts = [] {};
		gi.version = 2;
		gi.SP_STATUS_REG = r + 32;
		gi.RDRAM_SIZE = &m_rdram_size;

		if (m_p.PluginStartup(core_handle, nullptr, debug) != M64ERR_SUCCESS) { m_error = "PluginStartup failed"; return false; }
		if (!m_p.InitiateGFX(gi)) { m_error = "InitiateGFX failed"; return false; }
		m_first = true;
		apply(first);
		m_first = true;
		if (!m_p.RomOpen()) { m_error = "RomOpen failed"; return false; }
		m_p.ViStatusChanged();
		m_p.ViWidthChanged();
		return true;
	}

	// Loads a frame's memory and registers. The plugin marks its frame buffers
	// in RDRAM and checks the marks later, so after the first frame only the
	// words the CPU changed between captures are applied.
	void apply(const Frame &f)
	{
		apply_rdram(f);
		to_words(m_dmem, f.dmem.data(), 0x1000);
		to_words(m_imem, f.imem.data(), 0x1000);
		unsigned *r = m_regs;
		r[0] = f.h.mi_intr;
		for (int k = 0; k < 8; k++) r[1 + k] = f.h.dpc[k];
		for (int k = 0; k < 14; k++) r[16 + k] = f.h.vi[k];
#ifndef __WIIU__
		// REPLAY_ORIGIN=<hex>: the VI shows that address instead, so the PC
		// replay can show a buffer the game is drawing rather than displaying.
		if (const char *origin = getenv("REPLAY_ORIGIN"))
			r[16 + 1] = (unsigned)strtoul(origin, nullptr, 16);
#endif
		r[32] = f.h.sp_status;
	}

	// Only the RDRAM words the CPU changed up to this frame. Applying the next
	// frame's RDRAM before decoding a frame shows what a decoder running behind
	// the CPU would read.
	void apply_rdram(const Frame &f)
	{
		to_words(m_next, f.rdram.data(), f.rdram.size() < sizeof(m_next) ? f.rdram.size() : sizeof(m_next));
		if (m_first) {
			memcpy(m_rdram, m_next, sizeof(m_rdram));
			m_first = false;
		} else {
			uint32_t *dst = (uint32_t *)m_rdram, *a = (uint32_t *)m_prev, *b = (uint32_t *)m_next;
			for (size_t w = 0; w < sizeof(m_rdram) / 4; w++)
				if (a[w] != b[w]) dst[w] = b[w];
		}
		memcpy(m_prev, m_next, sizeof(m_prev));
	}

	void stop()
	{
		if (m_p.RomClosed) m_p.RomClosed();
		if (m_p.PluginShutdown) m_p.PluginShutdown();
	}

	const char *error() const { return m_error; }

private:
	Plugin m_p = {};
	const char *m_error = "";
	bool m_first = true;
	unsigned m_regs[64] = {};
	uint8_t m_header[0x40] = {};
	unsigned m_rdram_size = 0x800000;
	alignas(64) uint8_t m_rdram[0x800000];
	alignas(64) uint8_t m_next[0x800000];
	alignas(64) uint8_t m_prev[0x800000];
	uint8_t m_dmem[0x1000], m_imem[0x1000];
};

} // namespace replay
