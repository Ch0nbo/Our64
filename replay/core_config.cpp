// Our64 replay: the mupen64plus core's config functions, which a video plugin
// looks up by name. An in-memory store that holds defaults, plus overrides
// (our64_config_override) that win over any value. Shared by the PC
// replay_core.so and the console replay app.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

#define M64P_CORE_PROTOTYPES 1
#include "m64p_types.h"
#include "m64p_common.h"
#include "m64p_config.h"

// ---- config store: in memory, defaults only ----

namespace {
struct Param { int type; int i; float f; std::string s; };
std::map<std::string, std::map<std::string, Param>> g_config;
std::string g_dataDir = "/tmp";
std::map<std::string, std::string> g_overrides; // by parameter name, any section
// Where GLideN64.custom.ini (per-game settings) lives: set by the app, or
// OUR64_SHARED_DIR on the PC. Empty: no per-game settings.
std::string g_sharedDir = getenv("OUR64_SHARED_DIR") ? getenv("OUR64_SHARED_DIR") : "";
// A parsed INI file: section -> key -> value.
typedef std::map<std::string, std::map<std::string, std::string>> Ini;
}

// GLideN64 keeps global function pointers with these same names, so a build
// that links GLideN64 in renames them (CORE_FN, see source/core_wiiu.cpp).
#ifndef CORE_FN
#define CORE_FN(name) name
#endif

extern "C" {

EXPORT m64p_error CALL CORE_FN(ConfigOpenSection)(const char *name, m64p_handle *h)
{
	*h = &g_config[name];
	return M64ERR_SUCCESS;
}
EXPORT m64p_error CALL CORE_FN(ConfigDeleteSection)(const char *name) { g_config.erase(name); return M64ERR_SUCCESS; }
EXPORT m64p_error CALL CORE_FN(ConfigSaveSection)(const char *) { return M64ERR_SUCCESS; }
EXPORT m64p_error CALL CORE_FN(ConfigSaveFile)(void) { return M64ERR_SUCCESS; }

static Param &param(m64p_handle h, const char *name) { return (*(std::map<std::string, Param> *)h)[name]; }
static bool has(m64p_handle h, const char *name) { return ((std::map<std::string, Param> *)h)->count(name); }

EXPORT m64p_error CALL CORE_FN(ConfigSetDefaultInt)(m64p_handle h, const char *n, int v, const char *)
{
	if (!has(h, n)) param(h, n) = {M64TYPE_INT, v, (float)v, ""};
	return M64ERR_SUCCESS;
}
EXPORT m64p_error CALL CORE_FN(ConfigSetDefaultFloat)(m64p_handle h, const char *n, float v, const char *)
{
	if (!has(h, n)) param(h, n) = {M64TYPE_FLOAT, (int)v, v, ""};
	return M64ERR_SUCCESS;
}
EXPORT m64p_error CALL CORE_FN(ConfigSetDefaultBool)(m64p_handle h, const char *n, int v, const char *)
{
	if (!has(h, n)) param(h, n) = {M64TYPE_BOOL, v, (float)v, ""};
	return M64ERR_SUCCESS;
}
EXPORT m64p_error CALL CORE_FN(ConfigSetDefaultString)(m64p_handle h, const char *n, const char *v, const char *)
{
	if (!has(h, n)) param(h, n) = {M64TYPE_STRING, 0, 0, v};
	return M64ERR_SUCCESS;
}
EXPORT int CALL CORE_FN(ConfigGetParamInt)(m64p_handle h, const char *n)
{
	if (g_overrides.count(n)) return atoi(g_overrides[n].c_str());
	return has(h, n) ? param(h, n).i : 0;
}
EXPORT float CALL CORE_FN(ConfigGetParamFloat)(m64p_handle h, const char *n)
{
	if (g_overrides.count(n)) return (float)atof(g_overrides[n].c_str());
	return has(h, n) ? param(h, n).f : 0;
}
EXPORT int CALL CORE_FN(ConfigGetParamBool)(m64p_handle h, const char *n)
{
	if (g_overrides.count(n)) return atoi(g_overrides[n].c_str());
	return has(h, n) ? param(h, n).i : 0;
}
EXPORT const char *CALL CORE_FN(ConfigGetParamString)(m64p_handle h, const char *n)
{
	return has(h, n) ? param(h, n).s.c_str() : "";
}
EXPORT const char *CALL CORE_FN(ConfigGetSharedDataFilepath)(const char *name)
{
	static std::string path;
	if (g_sharedDir.empty() || name == nullptr) return nullptr;
	path = g_sharedDir + "/" + name;
	FILE *f = fopen(path.c_str(), "r");
	if (!f) return nullptr;
	fclose(f);
	return path.c_str();
}
EXPORT const char *CALL CORE_FN(ConfigGetUserConfigPath)(void) { return g_dataDir.c_str(); }
EXPORT const char *CALL CORE_FN(ConfigGetUserDataPath)(void) { return g_dataDir.c_str(); }
EXPORT const char *CALL CORE_FN(ConfigGetUserCachePath)(void) { return g_dataDir.c_str(); }
// "[SECTION]" lines, then "key=value" lines; ';' and '#' start comments.
EXPORT m64p_error CALL CORE_FN(ConfigExternalOpen)(const char *path, m64p_handle *h)
{
	FILE *f = path ? fopen(path, "r") : nullptr;
	if (!f) return M64ERR_FILES;
	Ini *ini = new Ini;
	std::string section;
	char line[512];
	while (fgets(line, sizeof(line), f)) {
		std::string l = line;
		while (!l.empty() && (l.back() == '\n' || l.back() == '\r' || l.back() == ' ' || l.back() == '\t')) l.pop_back();
		size_t start = l.find_first_not_of(" \t");
		if (start == std::string::npos || l[start] == ';' || l[start] == '#') continue;
		l = l.substr(start);
		if (l[0] == '[') {
			section = l.substr(1, l.find(']') - 1);
			continue;
		}
		const size_t eq = l.find('=');
		if (eq != std::string::npos && eq > 0)
			(*ini)[section][l.substr(0, eq)] = l.substr(eq + 1);
	}
	fclose(f);
	*h = ini;
	return M64ERR_SUCCESS;
}
EXPORT m64p_error CALL CORE_FN(ConfigExternalClose)(m64p_handle h)
{
	delete (Ini *)h;
	return M64ERR_SUCCESS;
}
EXPORT m64p_error CALL CORE_FN(ConfigExternalGetParameter)(m64p_handle h, const char *section, const char *key,
                                                           char *value, int size)
{
	const Ini *ini = (const Ini *)h;
	if (!ini || !section || !key || !value || size <= 0) return M64ERR_INPUT_INVALID;
	auto s = ini->find(section);
	if (s == ini->end()) return M64ERR_INPUT_NOT_FOUND;
	auto k = s->second.find(key);
	if (k == s->second.end()) return M64ERR_INPUT_NOT_FOUND;
	snprintf(value, size, "%s", k->second.c_str());
	return M64ERR_SUCCESS;
}

EXPORT m64p_error CALL CORE_FN(PluginGetVersion)(m64p_plugin_type *type, int *version, int *api, const char **name, int *caps)
{
	if (type) *type = M64PLUGIN_CORE;
	if (version) *version = 0x020600;
	if (api) *api = 0x020001;
	if (name) *name = "Our64 replay64";
	if (caps) *caps = 0;
	return M64ERR_SUCCESS;
}

// The folder holding GLideN64.custom.ini (per-game settings).
EXPORT void CALL our64_config_shared_dir(const char *dir) { g_sharedDir = dir ? dir : ""; }

// "Name=value,Name=value": each value wins over the plugin's own for that
// parameter name, in any section. Replaces earlier overrides; returns how
// many were set.
EXPORT int CALL our64_config_override(const char *list)
{
	g_overrides.clear();
	int n = 0;
	std::string all = list ? list : "";
	size_t pos = 0;
	while (pos < all.size()) {
		size_t end = all.find_first_of(",\n", pos);
		if (end == std::string::npos) end = all.size();
		const std::string item = all.substr(pos, end - pos);
		const size_t eq = item.find('=');
		if (eq != std::string::npos && eq > 0) {
			g_overrides[item.substr(0, eq)] = item.substr(eq + 1);
			++n;
		}
		pos = end + 1;
	}
	return n;
}

} // extern "C"
