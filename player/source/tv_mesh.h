// Our64 player: CRT Curved's grid (tv_pass.cpp, and tools/crt-preview on the
// PC). Our own code.
//
// The picture's rect drawn as a grid of cells, 16 rows and about as many
// columns as make them square (22 for a 4:3 picture, 28 for 16:9), each two
// triangles. The rect stays where it is; the bend is in the texture
// coordinates: each vertex carries the frame's point a tube's curved glass
// shows there, and the GPU interpolates it across the cell, so the pixel
// shader reads the frame as CRT's does, with no bend of its own. The cells
// are big because every triangle edge that is not along a row or column
// costs the 2x2 pixel blocks it cuts (they are shaded once for each side):
// about 2 / (cell size in pixels) more pixel work, 3% here.
//
// The bend: as CRT Curved's pixel shader did it per pixel until 2026-11-04
// (commit 4175528), a barrel bend in the picture's own proportions (its half
// height 1, its half width as = w / h), k = 0.04 * (25 / 9) / (1 + as^2) so a
// 16:9 picture's corners pull in as far as a 4:3 one's, scaled so the middle
// of each edge stays on the picture's edge. Interpolated, the bent point is
// off by at most 0.42 TV pixels at 1080p (4:3; 0.40 at 16:9, 0.28 at 720p)
// against the per-pixel bend (crt-preview --mesh). The error grows with
// the bend's change of slope, which points away from the middle, so each
// cell's diagonal runs across that direction (with diagonals all one way it
// was 0.74).
//
// Each vertex also carries the edge: s1, t1 how far the bent point q is
// past the inner rect the corners round off from, |q| - (as - 0.08, 0.92),
// times sqrt(px) (px = (h / 2) / (2 R): R^2 - r^2 near the corner radius
// R = 0.08 is 2 R (R - r), so px times it is R - r in TV pixels); the
// grid's middle lines sit where |q| turns, so it interpolates as smoothly
// as q. w is R^2 px + 0.5, so the edge fades over about a TV pixel, and z
// the falloff toward the corners (1 - 0.06 r^2, r the unbent point). The
// light is clamp(w - max(s1, 0)^2 - max(t1, 0)^2, 0, 1) times z: the old
// shader's rounded-rect edge, term for term (tv_shaders.h).
//
// The cells where that light is 1 everywhere (the whole middle) come first
// in the index list, drawn with CRT's own pixel shader; the rest (the ones
// along the edges, the corners and the black past them) after, drawn with
// the one that works out the edge. The light is concave across a triangle
// (its e terms are linear there), so a cell whose four corners are fully
// lit is fully lit inside.
#pragma once

#include <cmath>
#include <cstdint>

constexpr float kTvMeshBend = 0.04f; // the curvature
// Overscan (Zach, 2026-11-12, issue #36): as a tube hides the picture's
// edges behind its bezel, every TV filter shows the frame less this share at
// each side, stretched over the same place on the TV, so the black margins
// most N64 games leave (8 of 640 pixels left and right, 6 of 480 below) are
// off the screen. tv_overscan: a frame coordinate 0..1 to the part shown.
constexpr float kTvOverscan = 0.03f; // 2% left part of the bars games draw themselves (Super Mario 64: 14 rows above)
inline float tv_overscan(double s, double o = kTvOverscan) { return float(o + s * (1.0 - 2.0 * o)); }
// The GamePad (Zach, 2026-11-12): its 480 rows give a game line 2 rows, and
// 2.13 with the overscan, so beams that follow the game's lines land on its
// rows at a phase that drifts down the screen: bands, or with no overscan no
// lines at all (both rows of a line the same quarter line from its middle).
// There the shaders are told the frame has as many lines as the screen has
// groups of kTvPadRows rows, the first row of each group on a line's middle
// (f = 0) and the others evenly on to the next: the same beams, locked to
// the screen's rows instead of the game's lines. The lines' count is the
// uniform's (the frame's whole height in lines); the phase is in the
// overscan up and down, the nearest to kTvOverscan that puts the first row
// n whole lines in: o * lines - 0.5 + 0.5 / rows = n with
// lines = (h / rows) / (1 - 2 o).
// 3 rows a line (160 lines, one bright row and two dim, 3.1% overscan): at
// 2 rows a line the console drew every other row at about two thirds of its
// neighbour's value (read back: 157 against 102 of 255), as the PC does, and
// on the GamePad's screen the lines were "not very noticeable" (Zach): a
// dark line one row high does not survive its small, compressed picture.
constexpr unsigned kTvPadRows = 3;
inline double tv_rows_overscan(unsigned h)
{
	const double lines = double(h) / kTvPadRows, shift = 0.5 - 0.5 / kTvPadRows;
	const double n = std::floor(kTvOverscan * lines / (1.0 - 2.0 * kTvOverscan) - shift + 0.5);
	return (n + shift) / (lines + 2.0 * (n + shift));
}
inline double tv_rows_lines(unsigned h) { return double(h) / kTvPadRows / (1.0 - 2.0 * tv_rows_overscan(h)); }
constexpr unsigned kTvMeshRows = 16;
constexpr unsigned kTvMeshMaxCols = 32; // 2 * round(8 * as) up to as = 2
constexpr unsigned kTvMeshMaxVertices = (kTvMeshMaxCols + 1) * (kTvMeshRows + 1);
constexpr unsigned kTvMeshMaxIndices = kTvMeshMaxCols * kTvMeshRows * 6;

// The GX2 backend's RectVertex layout (aRectPosition, aTexCoord0, aTexCoord1).
struct TvMeshVertex {
	float x, y, z, w;
	float s0, t0, s1, t1;
};
static_assert(sizeof(TvMeshVertex) == 32, "RectVertex layout");

struct TvMesh {
	unsigned columns, vertices;
	unsigned inner, edge; // index counts: the inner cells' first, then the edge cells'
};

// The bend at the unbent point (qx, qy) (-1 to 1 across the picture, -1 at
// its left and top): the bent one, in the same units.
inline void tv_mesh_bend(double qx, double qy, double as, double k, double & gx, double & gy)
{
	const double r2 = qx * qx * as * as + qy * qy;
	gx = qx / (1.0 + k * as * as) * (1.0 + k * r2);
	gy = qy / (1.0 + k) * (1.0 + k * r2);
}

// k for a picture as wide as as times its height and the uniform's bend.
inline double tv_mesh_k(double as, double bend) { return bend * (25.0 / 9.0) / (1.0 + as * as); }

// The light the edge leaves at a vertex: below 1 where the pixel shader's
// edge term matters.
inline float tv_mesh_light(const TvMeshVertex & v)
{
	const float ex = v.s1 > 0.0f ? v.s1 : 0.0f, ey = v.t1 > 0.0f ? v.t1 : 0.0f;
	return v.w - ex * ex - ey * ey;
}

// The grid for a w x h picture on the TV (in TV pixels) and the bend (the
// uniform's uSize.w). Grid lines fall on even pixels, so the cells' rows
// and columns cut no 2x2 block. overscan_v: the overscan up and down (the
// GamePad's own, tv_rows_overscan).
inline TvMesh tv_mesh_build(TvMeshVertex * v, uint16_t * index, unsigned w, unsigned h, float bend,
                            double overscan_v = kTvOverscan)
{
	TvMesh m;
	const double as = double(w) / double(h);
	unsigned cols = 2 * unsigned(std::lround(kTvMeshRows / 2 * as));
	if (cols < 2)
		cols = 2;
	if (cols > kTvMeshMaxCols)
		cols = kTvMeshMaxCols;
	m.columns = cols;
	m.vertices = (cols + 1) * (kTvMeshRows + 1);
	const double k = tv_mesh_k(as, bend);
	const double px = h * (0.5 / 0.16), root = std::sqrt(px);
	for (unsigned j = 0; j <= kTvMeshRows; j++) {
		// Rows top first: the frame's first row (t = 0) is at y = 1.
		const double row = j == kTvMeshRows ? h : 2.0 * std::lround(double(j) * h / kTvMeshRows / 2.0);
		const double qy = row / h * 2.0 - 1.0; // the unbent point, -1 at the top
		for (unsigned i = 0; i <= cols; i++) {
			const double col = i == cols ? w : 2.0 * std::lround(double(i) * w / cols / 2.0);
			const double qx = col / w * 2.0 - 1.0;
			const double r2 = qx * qx * as * as + qy * qy;
			double gx, gy;
			tv_mesh_bend(qx, qy, as, k, gx, gy);
			TvMeshVertex & o = v[j * (cols + 1) + i];
			o.x = float(qx);
			o.y = float(-qy);
			o.z = float(1.0 - 0.06 * r2);
			o.w = float(0.0064 * px + 0.5);
			o.s0 = tv_overscan(gx * 0.5 + 0.5);
			o.t0 = tv_overscan(gy * 0.5 + 0.5, overscan_v);
			o.s1 = float((std::fabs(gx * as) - (as - 0.08)) * root);
			o.t1 = float((std::fabs(gy) - 0.92) * root);
		}
	}
	// Each cell's two triangles, its diagonal across the direction from the
	// middle; inner cells first. A little margin on "fully lit" for the
	// GPU's rounding.
	unsigned n = 0;
	for (int pass = 0; pass < 2; pass++) {
		const unsigned start = n;
		for (unsigned j = 0; j < kTvMeshRows; j++)
			for (unsigned i = 0; i < cols; i++) {
				const uint16_t a = uint16_t(j * (cols + 1) + i), b = uint16_t(a + 1);
				const uint16_t c = uint16_t(a + cols + 1), d = uint16_t(c + 1);
				const bool lit = tv_mesh_light(v[a]) >= 1.01f && tv_mesh_light(v[b]) >= 1.01f &&
				                 tv_mesh_light(v[c]) >= 1.01f && tv_mesh_light(v[d]) >= 1.01f;
				if (lit != (pass == 0))
					continue;
				// The cell's middle, from the grid's: the same sign both ways
				// means it lies toward a corner along (1, 1) or (-1, -1).
				const bool across = (2 * int(i) + 1 - int(cols)) * (2 * int(j) + 1 - int(kTvMeshRows)) > 0;
				const uint16_t t[6] = {a, b, across ? c : d, across ? b : a, d, c};
				for (uint16_t x : t)
					index[n++] = x;
			}
		(pass == 0 ? m.inner : m.edge) = n - start;
	}
	return m;
}
