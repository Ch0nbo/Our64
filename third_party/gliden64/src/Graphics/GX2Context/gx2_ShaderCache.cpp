// Our64 GX2 backend: compiled shader pairs saved to the SD card. CafeGLSL
// takes up to a second per pair on the console, so each pair is compiled
// once and loaded from <our64_gx2_shader_dir>/<key>.bin after that. The key
// hashes both sources, the compile mode and the compiler version.
//
// Before a game starts, its pairs come from <game>.pack, all of them read in
// one go: opening each .bin took about 0.23 s on 2026-10-21 (1857 files in
// the folder, a nearly full card), 97% of an 11-17 s start. The .bin files
// stay the store; a pack missing pairs, or holding pairs no longer used, is
// written again from what the preload read (shaderPackOpen, shaderPackClose).
//
// Layout: <dir>/<first two hex digits of the key>/<key>.bin, and each game's
// .keys and .pack in <dir>/games. With every file in one folder, any open
// there cost about 0.25 s (FAT folders are searched entry by entry): a
// 220 KB pack took 280 ms to read, and each new combiner's cache lookup, a
// miss, stalled the graphics core as long (2026-10-22). Files in the old
// flat layout are still found there and moved on first use
// (tools/shader-cache-migrate.sh moves them all at once).
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <sys/stat.h>

#include <cafeglsl/CafeGLSLCompiler.h>
#include <coreinit/memdefaultheap.h>
#include <coreinit/mutex.h>
#include <coreinit/time.h>

#include "gx2_Internal.h"
#include "gx2_ShaderText.h"

extern const char * our64_gx2_debug;

extern "C" {
const char * our64_gx2_shader_dir = nullptr;
// Time spent opening and reading cache files, for the preload's log line:
// loading a game's saved shaders took 0.2-0.5 s each on 2026-10-21 (Mario
// Tennis: 49 in 12 s), against 0.05 s on 2026-10-15.
volatile u64 our64_gx2_cache_read_ticks = 0;
}

namespace gx2 {

namespace {

const u32 kMagic = 0x4F363453; // "O64S"
const u32 kVersion = 1;

u64 fnv(u64 _h, const void * _data, size_t _size)
{
	const u8 * p = static_cast<const u8*>(_data);
	for (size_t i = 0; i < _size; ++i)
		_h = (_h ^ p[i]) * 0x100000001B3ull;
	return _h;
}

std::string pathOf(u64 _key)
{
	char name[40];
	snprintf(name, sizeof(name), "/%02x/%016llx.bin", unsigned(_key >> 56), (unsigned long long)_key);
	return std::string(our64_gx2_shader_dir) + name;
}

// <dir>/index.txt lists the key of every .bin, one hex number a line, when
// tools/build-shader-packs.sh has written it; saves append to it. With it a
// key it does not list is a miss without opening anything: a failed open
// cost 0.25-1.8 s on the console's card (Majora's Mask, 32 misses at each
// start). Without it every lookup opens the file, as before.
bool g_indexRead = false, g_indexPresent = false;
std::unordered_set<u64> g_index;
// Lookups run on the graphics core, saves on the compile thread.
struct IndexLock {
	OSMutex m;
	IndexLock() { OSInitMutex(&m); }
} g_indexLock;
struct IndexGuard {
	IndexGuard() { OSLockMutex(&g_indexLock.m); }
	~IndexGuard() { OSUnlockMutex(&g_indexLock.m); }
};

std::string indexPath() { return std::string(our64_gx2_shader_dir) + "/index.txt"; }

void readIndex()
{
	if (g_indexRead)
		return;
	g_indexRead = true;
	FILE * f = fopen(indexPath().c_str(), "r");
	if (f == nullptr)
		return;
	g_indexPresent = true;
	char line[64];
	while (fgets(line, sizeof(line), f) != nullptr) {
		unsigned long long key;
		if (sscanf(line, "%llx", &key) == 1)
			g_index.insert(u64(key));
	}
	fclose(f);
}

// The flat layout before 2026-10-22.
std::string legacyPathOf(u64 _key)
{
	char name[32];
	snprintf(name, sizeof(name), "/%016llx.bin", (unsigned long long)_key);
	return std::string(our64_gx2_shader_dir) + name;
}

// Creates the folder holding _path once a session (mkdir on one that exists
// fails, at the cost of a lookup).
void ensureFolderOf(const std::string & _path)
{
	static std::vector<std::string> made;
	const std::string dir = _path.substr(0, _path.rfind('/'));
	if (std::find(made.begin(), made.end(), dir) != made.end())
		return;
	mkdir(dir.c_str(), 0777);
	made.push_back(dir);
}

// A file of the flat layout moved to _path; false if there was none.
bool moveLegacy(const std::string & _legacy, const std::string & _path)
{
	FILE * f = fopen(_legacy.c_str(), "rb");
	if (f == nullptr)
		return false;
	fclose(f);
	ensureFolderOf(_path);
	return rename(_legacy.c_str(), _path.c_str()) == 0;
}

// ---- writing ----

struct Writer {
	FILE * f;
	bool ok = true;
	void bytes(const void * _p, u32 _n) { if (ok && _n > 0) ok = fwrite(_p, 1, _n, f) == _n; }
	void word(u32 _v) { bytes(&_v, 4); }
	void str(const char * _s) {
		const u32 n = _s != nullptr ? u32(strlen(_s)) : 0;
		word(n);
		bytes(_s, n);
	}
};

void writeCommon(Writer & _w, u32 _size, const void * _program, GX2ShaderMode _mode,
	u32 _nBlocks, const GX2UniformBlock * _blocks, u32 _nVars, const GX2UniformVar * _vars,
	u32 _nInit, const GX2UniformInitialValue * _init, u32 _nLoops, const GX2LoopVar * _loops,
	u32 _nSamplers, const GX2SamplerVar * _samplers)
{
	_w.word(_size);
	_w.bytes(_program, _size);
	_w.word(_mode);
	_w.word(_nBlocks);
	for (u32 i = 0; i < _nBlocks; ++i) {
		_w.str(_blocks[i].name);
		_w.word(_blocks[i].offset);
		_w.word(_blocks[i].size);
	}
	_w.word(_nVars);
	for (u32 i = 0; i < _nVars; ++i) {
		_w.str(_vars[i].name);
		_w.word(_vars[i].type);
		_w.word(_vars[i].count);
		_w.word(_vars[i].offset);
		_w.word(u32(_vars[i].block));
	}
	_w.word(_nInit);
	_w.bytes(_init, _nInit * sizeof(GX2UniformInitialValue));
	_w.word(_nLoops);
	_w.bytes(_loops, _nLoops * sizeof(GX2LoopVar));
	_w.word(_nSamplers);
	for (u32 i = 0; i < _nSamplers; ++i) {
		_w.str(_samplers[i].name);
		_w.word(_samplers[i].type);
		_w.word(_samplers[i].location);
	}
}

// ---- reading: every piece is its own heap block, freed by freeCachedShaders ----

struct Reader {
	const u8 * data;
	u32 size, pos = 0;
	bool ok = true;
	void bytes(void * _p, u32 _n) {
		if (!ok || _n == 0) return;
		if (_n > size - pos) { ok = false; return; }
		memcpy(_p, data + pos, _n);
		pos += _n;
	}
	u32 word() { u32 v = 0; bytes(&v, 4); return ok ? v : 0; }
	void * alloc(u32 _n, u32 _align = 8) {
		if (!ok || _n == 0) return nullptr;
		void * p = MEMAllocFromDefaultHeapEx(_n, _align);
		if (p == nullptr) ok = false;
		else memset(p, 0, _n);
		return p;
	}
	template <typename T> T * array(u32 _count) {
		if (_count > 4096) ok = false; // a damaged file, not a shader
		return static_cast<T*>(alloc(_count * sizeof(T)));
	}
	const char * str() {
		const u32 n = word();
		if (n > 1024) ok = false;
		char * s = static_cast<char*>(alloc(n + 1));
		bytes(s, n);
		return s;
	}
};

template <typename S> void readCommon(Reader & _r, S * _s)
{
	_s->size = _r.word();
	if (_s->size > 1024 * 1024) _r.ok = false;
	_s->program = _r.alloc(_s->size, GX2_SHADER_PROGRAM_ALIGNMENT);
	_r.bytes(_s->program, _s->size);
	_s->mode = GX2ShaderMode(_r.word());
	_s->uniformBlockCount = _r.word();
	_s->uniformBlocks = _r.template array<GX2UniformBlock>(_s->uniformBlockCount);
	for (u32 i = 0; _r.ok && i < _s->uniformBlockCount; ++i) {
		_s->uniformBlocks[i].name = _r.str();
		_s->uniformBlocks[i].offset = _r.word();
		_s->uniformBlocks[i].size = _r.word();
	}
	_s->uniformVarCount = _r.word();
	_s->uniformVars = _r.template array<GX2UniformVar>(_s->uniformVarCount);
	for (u32 i = 0; _r.ok && i < _s->uniformVarCount; ++i) {
		_s->uniformVars[i].name = _r.str();
		_s->uniformVars[i].type = GX2ShaderVarType(_r.word());
		_s->uniformVars[i].count = _r.word();
		_s->uniformVars[i].offset = _r.word();
		_s->uniformVars[i].block = s32(_r.word());
	}
	_s->initialValueCount = _r.word();
	_s->initialValues = _r.template array<GX2UniformInitialValue>(_s->initialValueCount);
	_r.bytes(_s->initialValues, _s->initialValueCount * sizeof(GX2UniformInitialValue));
	_s->loopVarCount = _r.word();
	_s->loopVars = _r.template array<GX2LoopVar>(_s->loopVarCount);
	_r.bytes(_s->loopVars, _s->loopVarCount * sizeof(GX2LoopVar));
	_s->samplerVarCount = _r.word();
	_s->samplerVars = _r.template array<GX2SamplerVar>(_s->samplerVarCount);
	for (u32 i = 0; _r.ok && i < _s->samplerVarCount; ++i) {
		_s->samplerVars[i].name = _r.str();
		_s->samplerVars[i].type = GX2SamplerVarType(_r.word());
		_s->samplerVars[i].location = _r.word();
	}
}

void heapFree(const void * _p)
{
	if (_p != nullptr)
		MEMFreeToDefaultHeap(const_cast<void*>(_p));
}

template <typename S> void freeCommon(S * _s)
{
	heapFree(_s->program);
	for (u32 i = 0; _s->uniformBlocks != nullptr && i < _s->uniformBlockCount; ++i)
		heapFree(_s->uniformBlocks[i].name);
	heapFree(_s->uniformBlocks);
	for (u32 i = 0; _s->uniformVars != nullptr && i < _s->uniformVarCount; ++i)
		heapFree(_s->uniformVars[i].name);
	heapFree(_s->uniformVars);
	heapFree(_s->initialValues);
	heapFree(_s->loopVars);
	for (u32 i = 0; _s->samplerVars != nullptr && i < _s->samplerVarCount; ++i)
		heapFree(_s->samplerVars[i].name);
	heapFree(_s->samplerVars);
}

void freeVertex(GX2VertexShader * _vs)
{
	if (_vs == nullptr) return;
	freeCommon(_vs);
	for (u32 i = 0; _vs->attribVars != nullptr && i < _vs->attribVarCount; ++i)
		heapFree(_vs->attribVars[i].name);
	heapFree(_vs->attribVars);
	heapFree(_vs);
}

void freePixel(GX2PixelShader * _ps)
{
	if (_ps == nullptr) return;
	freeCommon(_ps);
	heapFree(_ps);
}

} // namespace

u64 shaderCacheKey(const std::string & _vs, const std::string & _ps, u32 _mode)
{
	u64 h = 0xCBF29CE484222325ull;
	const char * version = GetGLSLCompilerVersion();
	if (version != nullptr)
		h = fnv(h, version, strlen(version) + 1);
	h = fnv(h, &_mode, sizeof(_mode));
	h = fnv(h, _vs.c_str(), _vs.size() + 1);
	return fnv(h, _ps.c_str(), _ps.size() + 1);
}

namespace {

// The pack being read: the whole file, where each pair starts in it and how
// long it is, which of them were used, and the pairs read from .bin files.
struct Pack {
	bool open = false;
	std::string path;
	std::vector<u8> data;
	std::unordered_map<u64, std::pair<u32, u32>> index;
	std::unordered_map<u64, bool> used;
	std::vector<std::pair<u64, std::vector<u8>>> added;
	unsigned fromPack = 0, fromFiles = 0;
} g_pack;

const u32 kPackMagic = 0x4F36344B; // "O64K"

bool readWhole(const std::string & _path, std::vector<u8> & _out)
{
	FILE * f = fopen(_path.c_str(), "rb");
	if (f == nullptr)
		return false;
	bool ok = fseek(f, 0, SEEK_END) == 0;
	const long n = ok ? ftell(f) : -1;
	ok = ok && n > 0 && n < 64 * 1024 * 1024 && fseek(f, 0, SEEK_SET) == 0;
	if (ok) {
		_out.resize(size_t(n));
		ok = fread(_out.data(), 1, size_t(n), f) == size_t(n);
	}
	fclose(f);
	return ok;
}

bool parsePair(const u8 * _data, u32 _size, u64 _key, GX2VertexShader ** _vs, GX2PixelShader ** _ps, GX2ShaderMode * _mode)
{
	Reader r{_data, _size};
	GX2VertexShader * vs = nullptr;
	GX2PixelShader * ps = nullptr;
	if (r.word() != kMagic || r.word() != kVersion)
		r.ok = false;
	u32 hi = r.word(), lo = r.word();
	if ((u64(hi) << 32 | lo) != _key)
		r.ok = false;
	const GX2ShaderMode mode = GX2ShaderMode(r.word());

	vs = static_cast<GX2VertexShader*>(r.alloc(sizeof(GX2VertexShader)));
	if (r.ok) {
		r.bytes(&vs->regs, sizeof(vs->regs));
		readCommon(r, vs);
		vs->attribVarCount = r.word();
		vs->attribVars = r.array<GX2AttribVar>(vs->attribVarCount);
		for (u32 i = 0; r.ok && i < vs->attribVarCount; ++i) {
			vs->attribVars[i].name = r.str();
			vs->attribVars[i].type = GX2ShaderVarType(r.word());
			vs->attribVars[i].count = r.word();
			vs->attribVars[i].location = r.word();
		}
		vs->ringItemsize = r.word();
		vs->hasStreamOut = r.word();
		r.bytes(vs->streamOutStride, sizeof(vs->streamOutStride));
	}
	ps = static_cast<GX2PixelShader*>(r.alloc(sizeof(GX2PixelShader)));
	if (r.ok) {
		r.bytes(&ps->regs, sizeof(ps->regs));
		readCommon(r, ps);
	}
	if (r.ok && r.word() != kMagic)
		r.ok = false;
	if (!r.ok) {
		freeVertex(vs);
		freePixel(ps);
		return false;
	}
	// Files and packs made before the fix (gx2_ShaderText.h) get it here.
	if (our64_gx2_debug == nullptr || strstr(our64_gx2_debug, "notransfix") == nullptr) {
		gx2::fixTransSlotWrites(vs->program, vs->size, vs->regs.sq_pgm_resources_vs);
		gx2::fixTransSlotWrites(ps->program, ps->size, ps->regs.sq_pgm_resources_ps);
	}
	*_vs = vs;
	*_ps = ps;
	*_mode = mode;
	return true;
}

} // namespace

bool shaderCacheLoad(u64 _key, GX2VertexShader ** _vs, GX2PixelShader ** _ps, GX2ShaderMode * _mode)
{
	if (our64_gx2_shader_dir == nullptr)
		return false;
	if (g_pack.open) {
		auto it = g_pack.index.find(_key);
		if (it != g_pack.index.end() &&
			parsePair(g_pack.data.data() + it->second.first, it->second.second, _key, _vs, _ps, _mode)) {
			g_pack.used[_key] = true;
			++g_pack.fromPack;
			return true;
		}
		// Two combiners can make the same shaders: read the file once and
		// pack it once (Majora's Mask's pack held 3 pairs twice).
		for (const auto & a : g_pack.added)
			if (a.first == _key)
				return parsePair(a.second.data(), u32(a.second.size()), _key, _vs, _ps, _mode);
	}
	const OSTime start = OSGetSystemTime();
	bool listed;
	{
		IndexGuard guard;
		readIndex();
		listed = !g_indexPresent || g_index.count(_key) != 0;
	}
	if (!listed) {
		our64_gx2_cache_read_ticks += OSGetSystemTime() - start;
		return false;
	}
	std::vector<u8> bytes;
	bool read = readWhole(pathOf(_key), bytes);
	if (!read && !g_indexPresent && moveLegacy(legacyPathOf(_key), pathOf(_key)))
		read = readWhole(pathOf(_key), bytes);
	our64_gx2_cache_read_ticks += OSGetSystemTime() - start;
	if (!read)
		return false;
	if (!parsePair(bytes.data(), u32(bytes.size()), _key, _vs, _ps, _mode)) {
		log("shader cache: %016llx unreadable, compiling again\n", (unsigned long long)_key);
		return false;
	}
	if (g_pack.open) {
		++g_pack.fromFiles;
		g_pack.added.emplace_back(_key, std::move(bytes));
	}
	return true;
}

void shaderCacheStore(u64 _key, const GX2VertexShader * _vs, const GX2PixelShader * _ps, GX2ShaderMode _mode)
{
	if (our64_gx2_shader_dir == nullptr)
		return;
	// Written under a temporary name, so an interrupted write never leaves a
	// damaged file under the real one.
	const std::string path = pathOf(_key), tmp = path + ".tmp";
	ensureFolderOf(path);
	FILE * f = fopen(tmp.c_str(), "wb");
	if (f == nullptr)
		return;
	Writer w{f};
	w.word(kMagic);
	w.word(kVersion);
	w.word(u32(_key >> 32));
	w.word(u32(_key));
	w.word(_mode);
	w.bytes(&_vs->regs, sizeof(_vs->regs));
	writeCommon(w, _vs->size, _vs->program, _vs->mode, _vs->uniformBlockCount, _vs->uniformBlocks,
		_vs->uniformVarCount, _vs->uniformVars, _vs->initialValueCount, _vs->initialValues,
		_vs->loopVarCount, _vs->loopVars, _vs->samplerVarCount, _vs->samplerVars);
	w.word(_vs->attribVarCount);
	for (u32 i = 0; i < _vs->attribVarCount; ++i) {
		w.str(_vs->attribVars[i].name);
		w.word(_vs->attribVars[i].type);
		w.word(_vs->attribVars[i].count);
		w.word(_vs->attribVars[i].location);
	}
	w.word(_vs->ringItemsize);
	w.word(_vs->hasStreamOut);
	w.bytes(_vs->streamOutStride, sizeof(_vs->streamOutStride));
	w.bytes(&_ps->regs, sizeof(_ps->regs));
	writeCommon(w, _ps->size, _ps->program, _ps->mode, _ps->uniformBlockCount, _ps->uniformBlocks,
		_ps->uniformVarCount, _ps->uniformVars, _ps->initialValueCount, _ps->initialValues,
		_ps->loopVarCount, _ps->loopVars, _ps->samplerVarCount, _ps->samplerVars);
	w.word(kMagic);
	const bool ok = w.ok;
	fclose(f);
	remove(path.c_str());
	if (!ok || rename(tmp.c_str(), path.c_str()) != 0) {
		remove(tmp.c_str());
		log("shader cache: could not save %s\n", path.c_str());
		return;
	}
	IndexGuard guard;
	readIndex();
	if (g_indexPresent && g_index.insert(_key).second)
		if (FILE * i = fopen(indexPath().c_str(), "a")) {
			fprintf(i, "%016llx\n", (unsigned long long)_key);
			fclose(i);
		}
}

void freeCachedShaders(GX2VertexShader * _vs, GX2PixelShader * _ps)
{
	freeVertex(_vs);
	freePixel(_ps);
}

} // namespace gx2

namespace gx2 {

// ---- combiner keys per game ----
//
// <our64_gx2_shader_dir>/<game>.keys lists every combiner key the game has
// used, one hex number per line, so its shaders can be loaded before it
// starts. The keys are N64 combiner state: they stay valid when the shader
// sources change, and then the shaders are compiled again from them.

namespace {

std::string gamePath(const char * _game, const char * _ext)
{
	std::string name;
	for (const char * c = _game; *c != '\0'; ++c)
		name += (isalnum(u8(*c)) ? *c : '_');
	while (!name.empty() && name.back() == '_')
		name.pop_back();
	return std::string(our64_gx2_shader_dir) + "/games/" + (name.empty() ? "unnamed" : name) + _ext;
}

// The game's file, moved from the flat layout if it is still there.
std::string gameFile(const char * _game, const char * _ext)
{
	const std::string path = gamePath(_game, _ext);
	FILE * f = fopen(path.c_str(), "rb");
	if (f != nullptr) {
		fclose(f);
		return path;
	}
	const std::string games = "/games/";
	std::string legacy = path;
	legacy.erase(legacy.rfind(games), games.size() - 1);
	moveLegacy(legacy, path);
	return path;
}

std::string keysPath(const char * _game) { return gameFile(_game, ".keys"); }

} // namespace

std::vector<u64> loadCombinerKeys(const char * _game)
{
	std::vector<u64> keys;
	if (our64_gx2_shader_dir == nullptr)
		return keys;
	FILE * f = fopen(keysPath(_game).c_str(), "r");
	if (f == nullptr)
		return keys;
	char line[64];
	while (fgets(line, sizeof(line), f) != nullptr) {
		unsigned long long key;
		if (line[0] != '#' && sscanf(line, "%llx", &key) == 1)
			keys.push_back(u64(key));
	}
	fclose(f);
	return keys;
}

void saveCombinerKeys(const char * _game, const std::vector<u64> & _keys)
{
	if (our64_gx2_shader_dir == nullptr)
		return;
	std::vector<u64> keys = loadCombinerKeys(_game);
	const size_t before = keys.size();
	keys.insert(keys.end(), _keys.begin(), _keys.end());
	std::sort(keys.begin(), keys.end());
	keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
	if (keys.size() == before)
		return;
	const std::string path = keysPath(_game), tmp = path + ".tmp";
	ensureFolderOf(path);
	FILE * f = fopen(tmp.c_str(), "w");
	if (f == nullptr)
		return;
	bool ok = fprintf(f, "# Our64: combiner keys of %s, loaded before the game starts\n", _game) > 0;
	for (u64 key : keys)
		ok = ok && fprintf(f, "0x%016llx\n", (unsigned long long)key) > 0;
	ok = fclose(f) == 0 && ok;
	remove(path.c_str());
	if (!ok || rename(tmp.c_str(), path.c_str()) != 0) {
		remove(tmp.c_str());
		log("shader cache: could not save %s\n", path.c_str());
	}
}

// ---- a game's shader pairs in one file ----
//
// <game>.pack: "O64K", a version, the number of pairs, then each pair as its
// length and the bytes of its .bin file. Unknown or damaged packs are ignored.

void shaderPackOpen(const char * _game)
{
	g_pack = Pack();
	if (our64_gx2_shader_dir == nullptr)
		return;
	g_pack.open = true;
	g_pack.path = gameFile(_game, ".pack");
	const OSTime start = OSGetSystemTime();
	const bool read = readWhole(g_pack.path, g_pack.data);
	our64_gx2_cache_read_ticks += OSGetSystemTime() - start;
	if (!read)
		return;
	Reader r{g_pack.data.data(), u32(g_pack.data.size())};
	if (r.word() != kPackMagic || r.word() != kVersion) {
		g_pack.data.clear();
		return;
	}
	const u32 count = r.word();
	for (u32 i = 0; r.ok && i < count; ++i) {
		const u32 n = r.word();
		if (!r.ok || n < 16 || n > r.size - r.pos)
			break;
		const u8 * p = r.data + r.pos;
		const u64 key = u64(p[8]) << 56 | u64(p[9]) << 48 | u64(p[10]) << 40 | u64(p[11]) << 32 |
			u64(p[12]) << 24 | u64(p[13]) << 16 | u64(p[14]) << 8 | u64(p[15]); // big-endian, as written
		g_pack.index[key] = std::make_pair(r.pos, n);
		r.pos += n;
	}
}

void shaderPackClose(unsigned * _fromPack, unsigned * _fromFiles)
{
	if (_fromPack != nullptr) *_fromPack = g_pack.fromPack;
	if (_fromFiles != nullptr) *_fromFiles = g_pack.fromFiles;
	if (!g_pack.open)
		return;
	const bool stale = g_pack.used.size() < g_pack.index.size();
	if (!g_pack.added.empty() || stale) {
		const std::string tmp = g_pack.path + ".tmp";
		ensureFolderOf(g_pack.path);
		if (FILE * f = fopen(tmp.c_str(), "wb")) {
			Writer w{f};
			w.word(kPackMagic);
			w.word(kVersion);
			w.word(u32(g_pack.used.size() + g_pack.added.size()));
			for (const auto & u : g_pack.used) {
				const auto & at = g_pack.index[u.first];
				w.word(at.second);
				w.bytes(g_pack.data.data() + at.first, at.second);
			}
			for (const auto & a : g_pack.added) {
				w.word(u32(a.second.size()));
				w.bytes(a.second.data(), u32(a.second.size()));
			}
			const bool ok = w.ok;
			fclose(f);
			remove(g_pack.path.c_str());
			if (!ok || rename(tmp.c_str(), g_pack.path.c_str()) != 0) {
				remove(tmp.c_str());
				log("shader cache: could not save %s\n", g_pack.path.c_str());
			}
		}
	}
	g_pack = Pack();
}

}
