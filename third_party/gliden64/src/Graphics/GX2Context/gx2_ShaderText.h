#pragma once
// Our64 GX2 backend: the text rewrites every GLideN64 shader pair gets before
// CafeGLSL compiles it, and one fix to the code it makes (fixTransSlotWrites).
// Plain code, so tools/shader-dump in Our64 can run them on the PC and show
// what the console compiles.
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace gx2 {

struct Varying {
	std::string interpolation;  // "noperspective", "flat" or empty
	std::string type;
	std::string name;
};

// The varyings a shader declares with _keyword ("in"/"IN" or "out"/"OUT"):
// lines such as "noperspective IN lowp vec4 vShadeColorNoperspective;".
inline std::vector<Varying> declaredVaryings(const std::string & _source, const char * _lower, const char * _upper)
{
	std::vector<Varying> result;
	size_t start = 0;
	while (start < _source.size()) {
		size_t end = _source.find('\n', start);
		if (end == std::string::npos)
			end = _source.size();
		std::vector<std::string> words;
		std::string word;
		for (size_t i = start; i < end; ++i) {
			const char c = _source[i];
			if (c == ' ' || c == '\t' || c == ';' || c == '\r') {
				if (!word.empty())
					words.push_back(word);
				word.clear();
				if (c == ';')
					break;
			} else {
				word += c;
			}
		}
		start = end + 1;
		size_t k = 0;
		Varying v;
		if (k < words.size() && (words[k] == "noperspective" || words[k] == "flat" || words[k] == "smooth"))
			v.interpolation = words[k++];
		if (k >= words.size() || (words[k] != _lower && words[k] != _upper))
			continue;
		++k;
		if (k < words.size() && (words[k] == "lowp" || words[k] == "mediump" || words[k] == "highp"))
			++k;
		if (k + 2 != words.size())
			continue;
		v.type = words[k];
		v.name = words[k + 1];
		if (v.name.find('(') != std::string::npos || v.name.find('[') != std::string::npos)
			continue;
		result.push_back(v);
	}
	return result;
}

// CafeGLSL compiles the two shaders of a pair separately and does not match
// vertex outputs to pixel inputs by name. GLideN64's rect vertex shaders
// write fewer varyings than the shared pixel shader reads, so every pixel
// input the vertex shader lacks becomes a vertex output set to zero.
inline std::string addMissingOutputs(const std::string & _vs, const std::string & _ps)
{
	const std::vector<Varying> outputs = declaredVaryings(_vs, "out", "OUT");
	std::string declarations, writes;
	for (const Varying & in : declaredVaryings(_ps, "in", "IN")) {
		bool found = false;
		for (const Varying & out : outputs)
			found = found || out.name == in.name;
		if (found)
			continue;
		declarations += (in.interpolation.empty() ? "" : in.interpolation + " ") + "out " + in.type + " " + in.name + ";\n";
		writes += "  " + in.name + " = " + in.type + "(0);\n";
	}
	if (declarations.empty())
		return _vs;
	const size_t main = _vs.find("void main");
	const size_t end = _vs.rfind('}');
	if (main == std::string::npos || end == std::string::npos || end < main)
		return _vs;
	return _vs.substr(0, main) + declarations + _vs.substr(main, end - main) + writes + _vs.substr(end);
}

// Every vertex shader negates gl_Position.y, so surfaces hold rows in
// OpenGL's order (row 0 at the bottom): see docs/feasibility.md, stage 3.
inline std::string flipVertexShader(const std::string & _source)
{
	const size_t end = _source.rfind('}');
	if (end == std::string::npos)
		return _source;
	return _source.substr(0, end) + "  gl_Position.y = -gl_Position.y;\n" + _source.substr(end);
}

inline void replaceAll(std::string & _s, const std::string & _from, const std::string & _to)
{
	for (size_t at = _s.find(_from); at != std::string::npos; at = _s.find(_from, at + _to.size()))
		_s.replace(at, _from.size(), _to);
}

// RE2's N64 depth table is an RGBA8 texture here, high byte in red, low byte
// in green (gx2_ContextImpl.cpp, GX2Formats and convertRow). As a UINT_R32
// texture read through an integer sampler its values came back byte-swapped and
// the characters vanished behind the background; as an uploaded FLOAT_R32
// texture it read 0 and they drew over each other in draw order (Cemu GX2
// replay, results/frames-re2-v897, 2026-10-26). RGBA8 uploads are what every
// game's textures use.
inline std::string zlutAsRGBA8(const std::string & _ps)
{
	if (_ps.find("uniform lowp usampler2D uZlutImage;") == std::string::npos)
		return _ps;
	std::string ps = _ps;
	replaceAll(ps, "uniform lowp usampler2D uZlutImage;", "uniform highp sampler2D uZlutImage;");
	replaceAll(ps, "texelFetch(uZlutImage,ivec2(x0,y0), 0).r",
		"(uint(texelFetch(uZlutImage,ivec2(x0,y0), 0).r * 255.0 + 0.5) * 256u + uint(texelFetch(uZlutImage,ivec2(x0,y0), 0).g * 255.0 + 0.5))");
	return ps;
}

// Latte blends with the second source color by reading the shader's second
// color export (target 1), so the GLSL dual-source output (location 0,
// index 1) is compiled as a plain location 1 output.
inline std::string dualSourceAsTarget1(const std::string & _ps)
{
	std::string ps = _ps;
	replaceAll(ps, "layout(location = 0, index = 0)", "layout(location = 0)");
	replaceAll(ps, "layout(location = 0, index = 1)", "layout(location = 1)");
	return ps;
}

// On the Wii U GPU a pixel shader that writes gl_FragDepth and discards
// part way through loses pixels it never discards: holes in translucent
// draws (Mario Party 3's board arrows, Mario Golf's swing meter; console GX2
// replays in Our64's results/gx2-mp3-*). So every discard in main() only
// sets a flag, and one discard at the end acts on it, after depth and
// colour are computed. The flag is a float: as a bool, its discard compiles
// to KILLNE_INT, and the GPU then dropped scattered pixels with the flag
// clear (Mario Party 3's bank house, results/gx2-bank2); KILLGT on a float
// keeps both the arrows and the bank whole (results/gx2-kill). The one
// discard goes after the render-target code at the end of main(), just
// before the gl_FragDepth write, and the depth-image branches of that code
// (uRenderTarget 2 and 4) keep their own discards. Found by console replay
// with the player's settings (results/gx2-place), Mario Golf's grey swing
// meter sliders and the ball in the hole: after the gl_FragDepth write, with
// the depth-image discards deferred too, the GPU kept the ball's pixels with
// the flag set (273 pixels off the PC); before the render-target code, the
// compiler folded the flag into the alpha test and the KILLNE_INT, mid-way
// through an instruction clause, dropped slider pixels it never discards
// (1245); here both match the PC (the kill is a KILLNE_INT again, but the
// last instruction of its clause).
// Shaders with a discard outside main() are left alone.
inline std::string deferDiscards(const std::string & _ps)
{
	if (_ps.find("discard;") == std::string::npos)
		return _ps;
	const size_t mainAt = _ps.find("void main()");
	const size_t open = mainAt == std::string::npos ? std::string::npos : _ps.find('{', mainAt);
	if (open == std::string::npos)
		return _ps;
	size_t close = std::string::npos, depth = 0;
	for (size_t i = open; i < _ps.size(); ++i) {
		if (_ps[i] == '{')
			++depth;
		else if (_ps[i] == '}' && --depth == 0) {
			close = i;
			break;
		}
	}
	if (close == std::string::npos || _ps.find("discard;") < open || _ps.rfind("discard;") > close)
		return _ps;
	std::string body = _ps.substr(open + 1, close - open - 1);
	const size_t rt = body.find("  if (uRenderTarget == 1");
	std::string before = body.substr(0, rt), after = rt == std::string::npos ? "" : body.substr(rt);
	replaceAll(before, "discard;", "{ our64Discard = 1.0; }");
	const std::string kill = "  if (our64Discard > 0.5) discard;\n";
	const size_t depthWrite = after.find("  gl_FragDepth = fragDepth;");
	if (depthWrite != std::string::npos)
		after.insert(depthWrite, kill);
	else
		before += kill;
	return _ps.substr(0, open + 1) + "\n  float our64Discard = 0.0;\n" + before + after + _ps.substr(close);
}

// The simple texture coordinates' 3-point filter reads its texels with
// texelFetch at whole-texel positions instead of texture() at texel centres:
// through the sampler, the console read wrong texels (blue speckles over
// Pokemon Stadium 2's arena floor, clean on the PC; console GX2 replays in
// Our64's results/gx2-ps2*). A texel past an edge is found as the sampler
// would: clamped, mirrored or repeated, from the same tile state that sets
// the sampler (uTexClampN's lower bound is 0 exactly when it clamps,
// uTexMirrorN is 1 when it mirrors). Textures read without those uniforms
// (frame buffer copies) clamp.
// Textures 320 or more texels wide (frame buffers, read back by effects such
// as Majora's Mask's Clock Town haze) keep the texture() reads: through
// texelFetch the console passed the alpha test on scattered pixels there,
// sparkles of the old frame (the PC gives the same picture both ways).
// _all (debug word "fetchall") fetches whole texels from every texture.
// _mode (debug words, 2026-10-19: Jet Force Gemini's gems speckled, pixels
// discarded that the old reads keep): 0 the edge in branches and texelFetch;
// 1 "fetchsel" the edge without branches; 2 "fetchtex" that edge, then
// texture() at the texel's centre (both speckled as 0 does); 3 "fetchclamp"
// every edge clamped: gems and floor right, but faint lines along the floor's
// tile seams; 4, the one in use, 0 where the alpha test is off and texture()
// where it is on: the gems (alpha tested) as on the PC, Pokemon Stadium 2's
// floor as fixed ("fetchedge" is 0 everywhere).
inline std::string fetchTexelFilter(const std::string & _ps, bool _all = false, int _mode = 4)
{
	const std::string offset = "#define TEX_OFFSET(off, tex, texCoord) texture(tex, texCoord - (off)/texSize)";
	const size_t at = _ps.find(offset);
	const size_t mainAt = _ps.find("void main()");
	if (at == std::string::npos || mainAt == std::string::npos)
		return _ps;
	std::string ps = _ps;
	ps.replace(at, offset.size(), std::string(
		"bvec2 our64Clamp = bvec2(true), our64Mirror = bvec2(false);\n"
		"int our64Edge(in int i, in int n, in bool clampEdge, in bool mirror)\n"
		"{\n") + (_mode == 3 ? "  return clamp(i, 0, n - 1);\n" : _mode == 0 || _mode == 4 ?
		"  if (clampEdge) return clamp(i, 0, n - 1);\n"
		"  if (!mirror) return i - n * int(floor(float(i) / float(n)));\n"
		"  int k = i - 2 * n * int(floor(float(i) / float(2 * n)));\n"
		"  return k < n ? k : 2 * n - 1 - k;\n" :
		"  int c = clamp(i, 0, n - 1);\n"
		"  int r = i - n * int(floor(float(i) / float(n)));\n"
		"  int k = i - 2 * n * int(floor(float(i) / float(2 * n)));\n"
		"  int m = k < n ? k : 2 * n - 1 - k;\n"
		"  return clampEdge ? c : (mirror ? m : r);\n") +
		"}\n"
		"lowp vec4 our64Fetch(in sampler2D tex, in ivec2 size, in highp vec2 pos)\n"
		"{\n"
		"  ivec2 i = ivec2(floor(pos));\n"
		"  ivec2 e = ivec2(our64Edge(i.x, size.x, our64Clamp.x, our64Mirror.x),\n"
		"    our64Edge(i.y, size.y, our64Clamp.y, our64Mirror.y));\n" +
		(_mode == 2 ? "  return texture(tex, (vec2(e) + vec2(0.5)) / vec2(size));\n" : "  return texelFetch(tex, e, 0);\n") +
		"}\n" +
		(_all ? std::string("#define TEX_OFFSET(off, tex, texCoord) our64Fetch(tex, ivec2(texSize), texCoord*texSize - (off))") :
		std::string(_mode == 4 && ps.find("uniform lowp int uEnableAlphaTest;") != std::string::npos ?
		"#define TEX_OFFSET(off, tex, texCoord) (texSize.x < 320.0 && uEnableAlphaTest == 0 ? our64Fetch(tex, ivec2(texSize), texCoord*texSize - (off)) " :
		"#define TEX_OFFSET(off, tex, texCoord) (texSize.x < 320.0 ? our64Fetch(tex, ivec2(texSize), texCoord*texSize - (off)) ") +
		": texture(tex, texCoord - (off)/texSize))"));
	for (int t = 0; t < 2; ++t) {
		const std::string n = std::to_string(t);
		const std::string read = "READ_TEX(readtex" + n + ", uTex" + n + ",";
		if (_mode == 3 || ps.find("uniform highp vec4 uTexClamp" + n + ";") == std::string::npos ||
				ps.find("uniform lowp vec2 uTexMirror" + n + ";") == std::string::npos)
			continue;
		for (size_t r = ps.find(read, ps.find("void main()")); r != std::string::npos; r = ps.find(read, r)) {
			const std::string set = "our64Clamp = greaterThan(uTexClamp" + n + ".xy, vec2(-1.0)); "
				"our64Mirror = greaterThan(uTexMirror" + n + ", vec2(0.5));\n  ";
			ps.insert(r, set);
			r += set.size() + read.size();
		}
	}
	return ps;
}

// Ocarina of Time (and Master Quest, same internal name) keeps its discards
// where GLideN64 put them. Deferred, the GPU dropped pixels of the Deku Tree's
// mustache it never discarded, showing the trunk through it (console GX2
// replay, results/gx2-oot-v349 and gx2-early, 2026-10-26); in place, they
// match the PC. Indiana Jones's speckled hills are the same fault, gone in
// place (results/gx2-indy, d1 and d2). Battle for Naboo, Indy's engine, shows
// the same holes live (2026-10-26). Deferred stays for every other game: in
// place, Mario Party 3's translucent board arrows got holes (results/gx2-early,
// mp3-v15802).
// "ZELDA MASTER QUEST" is the name of Master Quest hacks of Ocarina of Time
// (retail Master Quest keeps "THE LEGEND OF ZELDA").
inline bool keepDiscardsInPlace(const char * _romName)
{
	return _romName != nullptr && (strstr(_romName, "THE LEGEND OF ZELDA") != nullptr ||
		strstr(_romName, "ZELDA MASTER QUEST") != nullptr ||
		strstr(_romName, "Indiana Jones") != nullptr || strstr(_romName, "Battle for Naboo") != nullptr);
}

// Battle for Naboo's snowy hills lost pixels on the console only, in diagonal
// stripes that moved every frame, with its discards in place as well as
// deferred. Any change to the shader text hid them (marking the discards, no
// discards, no gl_FragDepth); with every lowp and mediump value highp, the
// hills were whole in every frame tried (console GX2 replay, results v39422
// d1-d5, 2026-10-26). Naboo's fragment shaders are all highp.
// Army Men: Sarge's Heroes showed parts behind the player's soldier (face
// through the back of his head, the hand through the gun) as speckles once
// he ran away from the camera, console only; all highp matches the PC
// (console GX2 replay, results/gx2-army-v2 d1 and d2, 2026-10-28). The lowp
// values alone do it (gx2-army-v3: "hplow" fixes it, "hpmed" and "highpdepth"
// change nothing); Army Men: Sarge's Heroes 2 shows the same speckles live.
// Not for every game: with every lowp value highp, Diddy Kong Racing's track
// fills with blue speckles and Mario Golf and Banjo-Kazooie move away from
// the PC (results/gx2-lowpscreen, 32 captures, 2026-10-29).
// NASCAR 2000 needs it too (Zach on the console, 2026-10-29), and so do
// Castlevania and Legacy of Darkness ("CASTLEVANIA2"), whose torches showed
// the same wrong texels (Zach on the console, 2026-11-02).
// Kirby 64's round shadows (under Kirby and the items) broke into scattered
// colored speckles on the console only; "hplow" draws them as soft blobs,
// matching the PC (console GX2 replay of a 24-frame capture, 2026-11-08).
// "Kirby64" is every region's header name.
// Lode Runner 3D's blocks and rails were full of speckles on the console; "hplow"
// matches the PC (console GX2 replay, 12 frames, 2026-11-08). "Lode Runner 3D"
// is every region's header name.
// Conker's Bad Fur Day: the panther king's face and fist and the floor showed
// speckles in the game over scene; "hplow" matches the PC (console GX2 replay,
// 21 frames, 2026-11-08). "CONKER BFD" covers Europe, USA and the debug beta.
// Super Smash Bros.: the locked characters' "?" slots on the character select
// screen were covered in orange and red speckles; "hplow" matches the PC
// (console GX2 replay, 12 frames, 2026-11-08). "SMASH BROTHERS" is every
// region's header name.
inline bool allHighp(const char * _romName)
{
	return _romName != nullptr && (strstr(_romName, "Battle for Naboo") != nullptr ||
		strstr(_romName, "Army Men Sarge") != nullptr || strstr(_romName, "ARMYMEN SARGE 2") != nullptr ||
		strstr(_romName, "NASCAR 2000") != nullptr || strstr(_romName, "CASTLEVANIA") != nullptr ||
		strstr(_romName, "DRACULA MOKUSHIROKU") != nullptr || // the Castlevanias in Japan
		strstr(_romName, "Kirby64") != nullptr || strstr(_romName, "Lode Runner 3D") != nullptr ||
		strstr(_romName, "CONKER BFD") != nullptr || strstr(_romName, "SMASH BROTHERS") != nullptr);
}

inline std::string highpEverywhere(std::string _ps)
{
	replaceAll(_ps, "lowp ", "highp ");
	replaceAll(_ps, "mediump ", "highp ");
	return _ps;
}

// Only the values the pixel's depth is made from highp (debug word
// "highpdepth"; "nogamehighp" turns allHighp off, so a variant can show the
// game without it).
inline std::string highpDepth(std::string _ps)
{
	replaceAll(_ps, "uniform mediump vec2 uDepthScale;", "uniform highp vec2 uDepthScale;");
	replaceAll(_ps, "uniform lowp float uPolygonOffset;", "uniform highp float uPolygonOffset;");
	replaceAll(_ps, "uniform mediump float uDeltaZ;", "uniform highp float uDeltaZ;");
	return _ps;
}

// Debug variants of the deferred discard (gx2-debug.txt words), to find
// which form the GPU handles: "flagmark" paints the pixel magenta instead of
// discarding it; "killbool" keeps the flag as a bool (KILLNE_INT, the form
// that dropped pixels); "killatend" puts the discard back after the
// gl_FragDepth write, the depth-image ones deferred too (the form before
// Mario Golf's ball fix); "killbeforert" puts it before the render-target
// code (the form that dropped swing meter slider pixels); "depthafterkill" writes gl_FragDepth after the discard; flagne,
// flagnan and killne below.
inline std::string deferredDiscardVariant(const std::string & _ps, const char * _words)
{
	const std::string kill = "if (our64Discard > 0.5) discard;";
	std::string ps = _ps;
	if (ps.find(kill) == std::string::npos)
		return ps;
	if (strstr(_words, "flagmark") != nullptr) {
		replaceAll(ps, kill, "if (our64Discard > 0.5) { fragColor = vec4(1.0, 0.0, 1.0, 1.0); fragColor1 = vec4(0.0); }");
		return ps;
	}
	// "flagne" paints magenta where the flag is not 0.0 (so also a NaN flag,
	// which "> 0.5" lets through); "flagnan" paints cyan where it is NaN and
	// discards as usual elsewhere; "killne" discards where it is not 0.0.
	if (strstr(_words, "flagne") != nullptr) {
		replaceAll(ps, kill, "if (our64Discard != 0.0) { fragColor = vec4(1.0, 0.0, 1.0, 1.0); fragColor1 = vec4(0.0); }");
		return ps;
	}
	if (strstr(_words, "flagnan") != nullptr) {
		replaceAll(ps, kill, "if (isnan(our64Discard)) { fragColor = vec4(0.0, 1.0, 1.0, 1.0); fragColor1 = vec4(0.0); }\n  " + kill);
		return ps;
	}
	if (strstr(_words, "killne") != nullptr)
		replaceAll(ps, kill, "if (our64Discard != 0.0) discard;");
	if (strstr(_words, "killatend") != nullptr && ps.find("gl_FragDepth = fragDepth;") != std::string::npos) {
		replaceAll(ps, "  " + kill + "\n", "");
		replaceAll(ps, "discard;", "{ our64Discard = 1.0; }");
		const size_t eol = ps.find('\n', ps.find("gl_FragDepth = fragDepth;"));
		ps.insert(eol + 1, "  " + kill + "\n");
	}
	if (strstr(_words, "killbeforert") != nullptr && ps.find("  if (uRenderTarget == 1") != std::string::npos) {
		replaceAll(ps, "  " + kill + "\n", "");
		ps.insert(ps.find("  if (uRenderTarget == 1"), "  " + kill + "\n");
	}
	if (strstr(_words, "depthafterkill") != nullptr && ps.find("gl_FragDepth = fragDepth;") != std::string::npos) {
		replaceAll(ps, "gl_FragDepth = fragDepth;", "");
		replaceAll(ps, kill, kill + "\n  gl_FragDepth = fragDepth;");
	}
	if (strstr(_words, "killbool") != nullptr) {
		replaceAll(ps, "float our64Discard = 0.0;", "bool our64Discard = false;");
		replaceAll(ps, "{ our64Discard = 1.0; }", "{ our64Discard = true; }");
		replaceAll(ps, kill, "if (our64Discard) discard;");
	}
	return ps;
}

// Debug paint of RE2's triangles ("showlut" in the GX2 backend, OUR64_SHOW=showlut
// on the PC): red the N64 depth table's value / 256, green its low byte, blue the
// depth it was looked up with, so the PC and the console can be compared.
inline std::string showLut(const std::string & _ps)
{
	if (_ps.find("iN64z") == std::string::npos)
		return _ps;
	std::string ps = _ps;
	const size_t top = ps.find('\n', ps.find("#version")) + 1;
	ps.insert(top, "highp float our64Fd = 0.0; highp uint our64Lut = 0u;\n");
	replaceAll(ps, "return clamp(float(iN64z)/65532.0, 0.0, 1.0);",
		"our64Fd = FragDepth; our64Lut = iN64z; return clamp(float(iN64z)/65532.0, 0.0, 1.0);");
	replaceAll(ps, "gl_FragDepth = fragDepth;",
		"if (uRenderTarget == 0) { fragColor = vec4(float(our64Lut / 256u) / 255.0, float(our64Lut % 256u) / 255.0, our64Fd, 1.0); "
		"fragColor1 = vec4(0.0); } gl_FragDepth = fragDepth;");
	return ps;
}

// CafeGLSL sometimes gives a vector slot and the trans slot of one ALU group
// the same destination, R123.c: the register it writes results to that only
// the next group reads (as PV.c and PS). One channel written twice in one
// group is not allowed, and the next group's PV.c then read the trans slot's
// result. In Perfect Dark's wall and security camera shaders the blender's
// red input got the green (Cemu GX2 replay of capture v9950, 2026-10-30:
// grey cameras and walls where the PC has the level's blue-green light);
// the same pattern was in 191 of the 2755 shaders built from 141 games' key
// lists. The trans slot's write goes to a spare register instead (the
// shader's GPR count, raised by one), which nothing reads; PS still carries
// it. _program is the GPU's little-endian code; _resources is the shader's
// SQ_PGM_RESOURCES (GPR count in bits 0-7). Running it twice changes nothing.
// Returns the number of writes moved.
inline uint32_t fixTransSlotWrites(void * _program, uint32_t _size, uint32_t & _resources)
{
	uint8_t * const code = static_cast<uint8_t*>(_program);
	const uint32_t words = _size / 4;
	auto word = [&](uint32_t _i) {
		const uint8_t * b = code + 4 * _i;
		return uint32_t(b[0]) | uint32_t(b[1]) << 8 | uint32_t(b[2]) << 16 | uint32_t(b[3]) << 24;
	};
	auto setWord = [&](uint32_t _i, uint32_t _v) {
		uint8_t * b = code + 4 * _i;
		b[0] = uint8_t(_v); b[1] = uint8_t(_v >> 8); b[2] = uint8_t(_v >> 16); b[3] = uint8_t(_v >> 24);
	};
	const uint32_t spare = _resources & 0xFF;
	if (code == nullptr || spare >= 120)
		return 0;
	uint32_t moved = 0;
	for (uint32_t cf = 0; 2 * cf + 1 < words; ++cf) {
		const uint32_t cf0 = word(2 * cf), cf1 = word(2 * cf + 1);
		if ((cf1 >> 29 & 1) == 0) {  // not an ALU clause
			if (cf1 >> 21 & 1)       // end of program
				break;
			continue;
		}
		const uint32_t start = (cf0 & 0x3FFFFF) * 2, end = start + ((cf1 >> 18 & 0x7F) + 1) * 2;
		if (end > words)
			break;
		for (uint32_t i = start; i < end;) {
			uint32_t at[5], w1[5], n = 0;
			int literal = -1;
			for (bool last = false; !last;) {
				if (n == 5 || i + 1 >= end)
					return moved;  // not code this understands: leave the rest
				const uint32_t a = word(i), b = word(i + 1);
				const bool op3 = (b >> 15 & 7) != 0;
				const uint32_t sel[3] = {a & 0x1FF, a >> 13 & 0x1FF, op3 ? b & 0x1FF : 0};
				const uint32_t chan[3] = {a >> 10 & 3, a >> 23 & 3, b >> 10 & 3};
				for (int s = 0; s < 3; ++s)
					if (sel[s] == 253 && int(chan[s]) > literal)  // a literal constant after the group
						literal = int(chan[s]);
				at[n] = i + 1;
				w1[n++] = b;
				last = (a >> 31) != 0;
				i += 2;
			}
			// Slots as the GPU assigns them: each instruction to its
			// destination channel's vector slot, to the trans slot if that
			// slot is taken or the instruction runs only there.
			bool taken[4] = {};
			int trans = -1;
			for (uint32_t k = 0; k < n; ++k) {
				const uint32_t b = w1[k];
				const bool op3 = (b >> 15 & 7) != 0;
				const uint32_t inst = op3 ? b >> 13 & 0x1F : b >> 7 & 0x7FF;
				const bool transOnly = op3 ? inst >= 0x0C && inst <= 0x0F :
					(inst >= 0x61 && inst <= 0x6F) || (inst >= 0x73 && inst <= 0x79);
				const uint32_t c = b >> 29 & 3;
				if (transOnly || taken[c])
					trans = int(k);
				else
					taken[c] = true;
			}
			auto writesR123 = [](uint32_t _b) {
				const bool writes = (_b >> 15 & 7) != 0 || (_b >> 4 & 1) != 0;
				return writes && (_b >> 21 & 0x7F) == 123 && (_b >> 28 & 1) == 0;
			};
			if (trans >= 0 && writesR123(w1[trans]))
				for (uint32_t k = 0; k < n; ++k)
					if (int(k) != trans && writesR123(w1[k]) && (w1[k] >> 29 & 3) == (w1[trans] >> 29 & 3)) {
						setWord(at[trans], (w1[trans] & ~(0x7Fu << 21)) | spare << 21);
						++moved;
						break;
					}
			if (literal >= 0)
				i += literal >= 2 ? 4 : 2;
		}
	}
	if (moved > 0)
		_resources = (_resources & ~0xFFu) | (spare + 1);
	return moved;
}

} // namespace gx2
