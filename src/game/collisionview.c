/**
 * Collision View — port-only debug visualiser for collision geometry and chr
 * hit volumes. Extended Options > Experiments > Collision View, or /collision.
 *
 * This draws the data the SIMULATION uses, not the data the renderer uses, so
 * it is the tool for "why did that shot miss / why am I stuck on nothing":
 *
 *   - World collision (`g_TileFileData` per-room geo lists, the same bytes
 *     `cdCollectGeoForCylFromList` walks) colour-coded floor / ceiling / wall /
 *     ladder / lethal. PD has no ceiling FLAG — the engine picks floor-vs-
 *     ceiling by approach direction (SURFACE_FLOOR / SURFACE_CEILING in
 *     `cdFindClosestVertical`) — so the class here is derived from the tile's
 *     surface normal, exactly as the collision code effectively does.
 *   - Object / door collision volumes (GEOTYPE_BLOCK prisms, GEOTYPE_CYL
 *     cylinders) via `propUpdateGeometry`, the same per-prop lists collision
 *     uses.
 *   - Chr hit boxes: every MODELNODETYPE_BBOX node of a chr's model, coloured
 *     by its `hitpart` group (head / torso / arms / legs / tail). These are the
 *     literal boxes `modelTestForHit` tests a shot against.
 *
 * Everything is drawn as solid translucent polygons (G_CC_SHADE — untextured,
 * vertex-coloured, the hoverbike speed-bar recipe in bondview.c) so volumes
 * read as volumes.
 *
 * Two matrix regimes are used, and the difference matters:
 *
 *   - World geometry is emitted CAMERA-RELATIVE under the world-to-screen
 *     matrix with its translation zeroed and scaled by 1/COLVIEW_VTXSCALE —
 *     the `lasersightRenderBeam` trick (gunfx.c). Vtx coords are s16, so this
 *     buys sub-unit precision near the camera without overflowing far away.
 *   - Chr hit boxes are emitted in NODE-LOCAL space under the model's own
 *     `model->matrices[]` entry, because those matrices are already
 *     model-to-SCREEN (they're built during render prep against the current
 *     camera — see the `isdifferentmtx` re-base in chraction.c's hitpart
 *     search). That's how the engine draws the model itself, so the boxes land
 *     exactly on the animated limbs with no manual transform.
 *
 * Purely cosmetic: nothing here writes game state, so it is save-safe and
 * net-safe. It is NOT chaos-gated — it's a standalone debug feature.
 */

#include <ultra64.h>
#include <math.h>
#include "constants.h"
#include "game/camera.h"
#include "game/collisionview.h"
#include "game/gfxmemory.h"
#include "game/prop.h"
#include "bss.h"
#include "lib/model.h"
#include "lib/mtx.h"
#include "data.h"
#include "types.h"

#ifndef PLATFORM_N64

s32 g_ColViewEnabled = 0;
s32 g_ColViewGeo = 1;
/**
 * PARKED, DEFAULT OFF (2026-07-28). The chr-hitbox layer is the source of the
 * remaining "triangles flickering across the viewport" corruption — the user
 * confirmed that turning this layer off alone clears it, so the world-geometry
 * and prop layers are sound. Leave it off by default until the cause is found;
 * `/collision hitboxes` still turns it on for debugging.
 * See the open-issue section of docs/PORT_COLLISION_VIEW.md.
 */
s32 g_ColViewHitboxes = 0;
s32 g_ColViewOutlines = 1;
s32 g_ColViewProps = 1;
s32 g_ColViewXray = 0;
s32 g_ColViewAlpha = 96;
s32 g_ColViewRange = 2500;

/**
 * World geometry is emitted in WORLD coordinates under the plain world-to-
 * screen matrix.
 *
 * An earlier version used the lasersightRenderBeam trick — camera-relative
 * coords scaled x4 under a translation-zeroed matrix — for sub-unit precision.
 * That was wrong here and produced "corrupted polygons": Vtx.x/y/z are s16, so
 * x4 caps the representable radius at +-8191 units from the camera, and one big
 * outdoor collision tile (a helipad, a hangar floor) easily spans more than
 * that. The far vertices wrapped and threw giant triangles across the screen.
 *
 * World coords have no such cap: geotilei vertices are ALREADY stored as s16
 * world values in the tile file, so this is both lossless and jitter-free,
 * which the camera-relative form never was — it re-quantised every vertex
 * against a moving fractional camera position every frame.
 */

/**
 * Collision surfaces are COPLANAR with the geometry they describe — a floor
 * tile lies exactly on the rendered floor — so a depth-tested translucent fill
 * z-fights it into shimmer. Every vertex is therefore pulled this fraction of
 * the way toward the camera.
 *
 * Scaling a point along its own view ray does not move it on screen at all (it
 * stays on the same ray through the eye), so this is a pure depth bias with no
 * positional distortion. Proportional rather than absolute is the right shape:
 * depth precision is finest near the eye, so near surfaces need less offset to
 * win the comparison.
 */
#define COLVIEW_DEPTHBIAS 0.995f

#define COLVIEW_MAXRANGE  8000
#define COLVIEW_MINRANGE  200

/** Rooms considered per frame (a terminated RoomNum list for roomGetProps). */
#define COLVIEW_MAXROOMS  48
/** Props considered per frame. */
#define COLVIEW_MAXPROPS  256
/** Segments in a drawn GEOTYPE_CYL. */
#define COLVIEW_CYLSEGS   10

/**
 * Leave this many BYTES of the per-frame vertex pool for the actual game.
 *
 * `gfxAllocateVertices` does NOT bounds-check — it just bumps `g_GfxMemPos` and
 * returns the old value, so overrunning the pool silently corrupts whatever
 * follows it. And `gfxGetFreeVtx()` reports BYTES REMAINING, not a vertex count
 * (it's a pointer difference). Everything here must therefore be budgeted in
 * bytes, via colviewHasVtxBudget. The pool is shared with all world/HUD
 * geometry (it's what the /fps overlay reports) and a big open room holds
 * thousands of collision tiles, so the reserve is generous: dropping part of a
 * debug overlay is nothing, dropping real geometry is a bug.
 */
#define COLVIEW_VTXRESERVE 32768

/**
 * ...and this many master-display-list COMMANDS. Same reasoning, different
 * pool: `gfxGetFreeGfx(gdl)` counts remaining Gfx entries.
 */
#define COLVIEW_GFXRESERVE 4096

// Palette slots. Vertex `colour` is a BYTE OFFSET into the Col array, so the
// slot index is multiplied by sizeof(Col) when written.
#define COLSLOT_FLOOR    0
#define COLSLOT_CEILING  1
#define COLSLOT_WALL     2
#define COLSLOT_SLOPE    3
#define COLSLOT_LADDER   4
#define COLSLOT_DIE      5
#define COLSLOT_BLOCK    6
#define COLSLOT_CYL      7
#define COLSLOT_HEAD     8
#define COLSLOT_TORSO    9
#define COLSLOT_ARM      10
#define COLSLOT_LEG      11
#define COLSLOT_TAIL     12
#define COLSLOT_OTHER    13
#define COLSLOT_EDGE     14
#define COLSLOT_COUNT    15

/** 0xRRGGBB per slot; alpha is applied from g_ColViewAlpha at build time. */
static const u32 g_ColViewPalette[COLSLOT_COUNT] = {
	0x33cc55, // floor    green
	0x3366ff, // ceiling  blue
	0xff3333, // wall     red
	0xffcc33, // slope    amber  (walkable but steep / ramp / step)
	0x33e5e5, // ladder   cyan
	0xff33ff, // die      magenta (kill volume)
	0xff9933, // block    orange (object / door collision prism)
	0xcc66ff, // cyl      violet (chr collision cylinder)
	0xff2222, // head     red
	0xffdd22, // torso    yellow
	0x22ddff, // arm      cyan
	0x22ff44, // leg      green
	0xff22aa, // tail     pink
	0xdddddd, // other    grey
	0x000000, // edge     black (hitbox outlines)
};

static Col *g_ColViewCols;

static s32 colviewCanEmit(Gfx *gdl, u32 verts, s32 cmds);

/**
 * Build the frame's colour array and point the RSP at it. Vertices then select
 * a colour by byte offset, so one array serves every shape this frame.
 */
static Gfx *colviewSetupColours(Gfx *gdl)
{
	s32 i;
	s32 alpha = g_ColViewAlpha;

	if (alpha < 8) {
		alpha = 8;
	} else if (alpha > 255) {
		alpha = 255;
	}

	// Colours come out of the same unchecked bump allocator as vertices.
	if (!colviewCanEmit(gdl, COLSLOT_COUNT, 1)) {
		g_ColViewCols = NULL;
		return gdl;
	}

	g_ColViewCols = gfxAllocateColours(COLSLOT_COUNT);

	if (!g_ColViewCols) {
		return gdl;
	}

	for (i = 0; i < COLSLOT_COUNT; i++) {
		s32 a = alpha;

		// Outlines want to stay crisp when the fills are set very faint —
		// a black edge at 30% alpha over a dark scene is invisible, which
		// defeats the point of having them.
		if (i == COLSLOT_EDGE) {
			a = alpha + 128;

			if (a > 255) {
				a = 255;
			}
		}

		g_ColViewCols[i].word = PD_BE32((g_ColViewPalette[i] << 8) | (u32)a);
	}

	gSPColor(gdl++, osVirtualToPhysical(g_ColViewCols), COLSLOT_COUNT);

	return gdl;
}

/**
 * World-space vertex with the view-ray depth bias applied. `campos` is the eye,
 * which is the point the bias scales about.
 */
static void colviewVtx(Vtx *v, struct coord *campos, f32 x, f32 y, f32 z, s32 slot)
{
	v->x = (s16)(campos->x + (x - campos->x) * COLVIEW_DEPTHBIAS);
	v->y = (s16)(campos->y + (y - campos->y) * COLVIEW_DEPTHBIAS);
	v->z = (s16)(campos->z + (z - campos->z) * COLVIEW_DEPTHBIAS);
	v->flags = 0;
	v->colour = slot * sizeof(Col);
	v->s = 0;
	v->t = 0;
}

/** Node-local variant for the chr hit boxes (matrix already carries camera). */
static void colviewVtxLocal(Vtx *v, f32 x, f32 y, f32 z, s32 slot)
{
	v->x = (s16)x;
	v->y = (s16)y;
	v->z = (s16)z;
	v->flags = 0;
	v->colour = slot * sizeof(Col);
	v->s = 0;
	v->t = 0;
}

/**
 * Can we afford `verts` vertices AND `cmds` display-list commands?
 *
 * BOTH pools must be checked. They are separate bump allocators and NEITHER
 * bounds-checks — `gfxAllocateVertices`/`gfxAllocateMatrix` just advance
 * `g_GfxMemPos`, and the master display list just advances `gdl`. Overrunning
 * either writes into the buffer the GPU is currently displaying, which is what
 * produced the triangles-flickering-across-the-viewport corruption (the same
 * failure gfxmemory.c documents for `/octree bigroom`: "triangles out of
 * order"). A garbage matrix or garbage vertex data throws geometry to arbitrary
 * screen positions, which is why it looked like a coordinate bug.
 *
 * The vertex overlay is command-hungry rather than vertex-hungry — one hit box
 * is 8 verts but a whole display list of draw commands — so the command budget
 * is the one that actually bites.
 *
 * The signed casts are load-bearing. `gfxGetFreeVtx()` returns a pointer
 * difference as u32, so once the pool HAS overrun it reports a huge positive
 * number and an unsigned comparison silently passes forever — the guard
 * inverts exactly when it's needed most.
 */
static s32 colviewCanEmit(Gfx *gdl, u32 verts, s32 cmds)
{
	if ((s32)gfxGetFreeVtx() < (s32)(verts * sizeof(Vtx)) + COLVIEW_VTXRESERVE) {
		return 0;
	}

	if (gfxGetFreeGfx(gdl) < cmds + COLVIEW_GFXRESERVE) {
		return 0;
	}

	return 1;
}

/**
 * Emit a convex polygon (up to 8 vertices) as a triangle fan. Winding is
 * irrelevant — culling is off, so both faces draw.
 */
static Gfx *colviewPoly(Gfx *gdl, struct coord *campos, struct coord *pts, s32 count, s32 slot)
{
	// Zero-padded so a group of 4 can always be read; gSPTri4 skips a triangle
	// whose three indices are all zero, and a real fan triangle (0, i, i+1)
	// never is.
	u8 idx[8][3];
	Vtx *vertices;
	s32 tris;
	s32 i;

	if (count < 3) {
		return gdl;
	}

	if (count > 8) {
		count = 8;
	}

	tris = count - 2;

	if (!colviewCanEmit(gdl, count, 1 + (tris + 3) / 4)) {
		return gdl;
	}

	vertices = gfxAllocateVertices(count);

	if (!vertices) {
		return gdl;
	}

	for (i = 0; i < count; i++) {
		colviewVtx(&vertices[i], campos, pts[i].x, pts[i].y, pts[i].z, slot);
	}

	gSPVertex(gdl++, osVirtualToPhysical(vertices), count, 0);

	for (i = 0; i < tris; i++) {
		idx[i][0] = 0;
		idx[i][1] = i + 1;
		idx[i][2] = i + 2;
	}

	for (; i < 8; i++) {
		idx[i][0] = idx[i][1] = idx[i][2] = 0;
	}

	for (i = 0; i < tris; i += 4) {
		gSPTri4(gdl++,
				idx[i][0], idx[i][1], idx[i][2],
				idx[i + 1][0], idx[i + 1][1], idx[i + 1][2],
				idx[i + 2][0], idx[i + 2][1], idx[i + 2][2],
				idx[i + 3][0], idx[i + 3][1], idx[i + 3][2]);
	}

	return gdl;
}

/**
 * Flatness of a polygon: |normal.y|, 0 = vertical, 1 = horizontal.
 *
 * The SIGN is deliberately discarded. It depends on winding, and PD's collision
 * tiles are not consistently wound — the engine never asks a tile which way it
 * faces either, it asks whether a searcher approaching from above or below hits
 * it (SURFACE_FLOOR / SURFACE_CEILING in cdFindClosestVertical). So the caller
 * resolves floor-vs-ceiling by height relative to the viewer, which is also
 * what a player means by "floor" and "ceiling".
 */
static f32 colviewFlatness(struct coord *pts, s32 count)
{
	struct coord a;
	struct coord b;
	f32 nx;
	f32 ny;
	f32 nz;
	f32 len;

	if (count < 3) {
		return 1.0f;
	}

	a.x = pts[1].x - pts[0].x;
	a.y = pts[1].y - pts[0].y;
	a.z = pts[1].z - pts[0].z;

	b.x = pts[2].x - pts[0].x;
	b.y = pts[2].y - pts[0].y;
	b.z = pts[2].z - pts[0].z;

	nx = a.y * b.z - a.z * b.y;
	ny = a.z * b.x - a.x * b.z;
	nz = a.x * b.y - a.y * b.x;

	len = sqrtf(nx * nx + ny * ny + nz * nz);

	if (len < 0.0001f) {
		return 1.0f;
	}

	ny = ny / len;

	return ny < 0.0f ? -ny : ny;
}

/** Mean height of a polygon, for the floor-vs-ceiling decision. */
static f32 colviewMeanY(struct coord *pts, s32 count)
{
	f32 sum = 0.0f;
	s32 i;

	for (i = 0; i < count; i++) {
		sum += pts[i].y;
	}

	return sum / (f32)count;
}

/**
 * Classify a collision tile for colouring. DIE and LADDER win over the surface
 * class because they're the interesting ones; otherwise flatness decides
 * horizontal-vs-wall, with a middle band for ramps and steps, and height
 * relative to the eye splits horizontal into floor and ceiling.
 */
static s32 colviewClassifyTile(u16 flags, f32 flatness, f32 meany, f32 eyey)
{
	if (flags & GEOFLAG_DIE) {
		return COLSLOT_DIE;
	}

	if (flags & (GEOFLAG_LADDER | GEOFLAG_LADDER_PLAYERONLY)) {
		return COLSLOT_LADDER;
	}

	if (flatness > 0.7f) {
		return meany > eyey ? COLSLOT_CEILING : COLSLOT_FLOOR;
	}

	// Near-vertical is a wall; the band between is a ramp/step/slope, which is
	// exactly the geometry that produces "why can't I walk up this" reports.
	if (flatness < 0.25f) {
		return COLSLOT_WALL;
	}

	return COLSLOT_SLOPE;
}

/**
 * Range cull against a volume's own XZ bounds.
 *
 * Testing a single vertex (which this used to do) is wrong for big geometry: a
 * helipad-sized tile can have its first vertex well outside the draw radius
 * while you are standing in the middle of it, so the tile popped in and out as
 * the camera crossed the boundary. Distance to the nearest point of the AABB is
 * the correct test and matches how the collision code broad-phases.
 */
static s32 colviewAabbInRange(struct coord *campos, f32 range, f32 xmin, f32 xmax, f32 zmin, f32 zmax)
{
	f32 dx = 0.0f;
	f32 dz = 0.0f;

	if (campos->x < xmin) {
		dx = xmin - campos->x;
	} else if (campos->x > xmax) {
		dx = campos->x - xmax;
	}

	if (campos->z < zmin) {
		dz = zmin - campos->z;
	} else if (campos->z > zmax) {
		dz = campos->z - zmax;
	}

	return dx * dx + dz * dz < range * range;
}

/** XZ bounds of a point set, for the volumes that don't carry their own. */
static void colviewPtsBounds(struct coord *pts, s32 count, f32 *xmin, f32 *xmax, f32 *zmin, f32 *zmax)
{
	s32 i;

	*xmin = *xmax = pts[0].x;
	*zmin = *zmax = pts[0].z;

	for (i = 1; i < count; i++) {
		if (pts[i].x < *xmin) { *xmin = pts[i].x; }
		if (pts[i].x > *xmax) { *xmax = pts[i].x; }
		if (pts[i].z < *zmin) { *zmin = pts[i].z; }
		if (pts[i].z > *zmax) { *zmax = pts[i].z; }
	}
}

/**
 * Walk one geo list (a room's BG tiles, or one prop's volumes) and draw it.
 * Stride arithmetic is copied verbatim from cdCollectGeoForCylFromList — this
 * is the same byte stream, so it must advance identically or it desyncs.
 */
static Gfx *colviewDrawGeoList(Gfx *gdl, struct coord *campos, u8 *start, u8 *end)
{
	struct geo *geo = (struct geo *) start;
	struct coord pts[8];
	f32 range = (f32)g_ColViewRange;

	while (geo < (struct geo *) end) {
		if (geo->type == GEOTYPE_TILE_I) {
			struct geotilei *tile = (struct geotilei *) geo;
			s32 count = tile->header.numvertices;
			s32 i;

			if (count > 8) {
				count = 8;
			}

			// The tile's min/max fields are BYTE OFFSETS into the tile itself
			// (see the struct comment) — this is how cdCollectGeoForCylFromList
			// reads them.
			if (g_ColViewGeo && count >= 3
					&& colviewAabbInRange(campos, range,
						*(s16 *)(tile->xmin + (uintptr_t)tile), *(s16 *)(tile->xmax + (uintptr_t)tile),
						*(s16 *)(tile->zmin + (uintptr_t)tile), *(s16 *)(tile->zmax + (uintptr_t)tile))) {
				for (i = 0; i < count; i++) {
					pts[i].x = tile->vertices[i][0];
					pts[i].y = tile->vertices[i][1];
					pts[i].z = tile->vertices[i][2];
				}

				gdl = colviewPoly(gdl, campos, pts, count,
						colviewClassifyTile(tile->header.flags,
							colviewFlatness(pts, count), colviewMeanY(pts, count), campos->y));
			}

			geo = (struct geo *)((uintptr_t)geo + tile->header.numvertices * 6 + 0xe);
		} else if (geo->type == GEOTYPE_TILE_F) {
			struct geotilef *tile = (struct geotilef *) geo;
			s32 count = tile->header.numvertices;
			s32 i;

			if (count > 8) {
				count = 8;
			}

			if (g_ColViewGeo && count >= 3) {
				f32 xmin;
				f32 xmax;
				f32 zmin;
				f32 zmax;

				for (i = 0; i < count; i++) {
					pts[i].x = tile->vertices[i].x;
					pts[i].y = tile->vertices[i].y;
					pts[i].z = tile->vertices[i].z;
				}

				colviewPtsBounds(pts, count, &xmin, &xmax, &zmin, &zmax);

				if (colviewAabbInRange(campos, range, xmin, xmax, zmin, zmax)) {
					gdl = colviewPoly(gdl, campos, pts, count,
							colviewClassifyTile(tile->header.flags,
								colviewFlatness(pts, count), colviewMeanY(pts, count), campos->y));
				}
			}

			geo = (struct geo *)((uintptr_t)geo + (tile->header.numvertices - 0x40) * 0xc + 0x310);
		} else if (geo->type == GEOTYPE_BLOCK) {
			struct geoblock *block = (struct geoblock *) geo;
			s32 count = block->header.numvertices;

			if (count > 8) {
				count = 8;
			}

			if (g_ColViewProps && count >= 3) {
				f32 xmin;
				f32 xmax;
				f32 zmin;
				f32 zmax;
				s32 i;

				// Reuse pts as a scratch footprint for the bounds test; the
				// draw loops below rebuild it per face.
				for (i = 0; i < count; i++) {
					pts[i].x = block->vertices[i][0];
					pts[i].y = block->ymax;
					pts[i].z = block->vertices[i][1];
				}

				colviewPtsBounds(pts, count, &xmin, &xmax, &zmin, &zmax);

				if (colviewAabbInRange(campos, range, xmin, xmax, zmin, zmax)) {
					// Cap first (pts already holds it), then the sides.
					gdl = colviewPoly(gdl, campos, pts, count, COLSLOT_BLOCK);

					for (i = 0; i < count; i++) {
						s32 next = (i + 1) % count;

						pts[0].x = block->vertices[i][0];
						pts[0].y = block->ymin;
						pts[0].z = block->vertices[i][1];

						pts[1].x = block->vertices[next][0];
						pts[1].y = block->ymin;
						pts[1].z = block->vertices[next][1];

						pts[2].x = block->vertices[next][0];
						pts[2].y = block->ymax;
						pts[2].z = block->vertices[next][1];

						pts[3].x = block->vertices[i][0];
						pts[3].y = block->ymax;
						pts[3].z = block->vertices[i][1];

						gdl = colviewPoly(gdl, campos, pts, 4, COLSLOT_BLOCK);
					}
				}
			}

			geo = (struct geo *)((uintptr_t)geo + 0x4c);
		} else if (geo->type == GEOTYPE_CYL) {
			struct geocyl *cyl = (struct geocyl *) geo;

			if (g_ColViewProps) {
				if (colviewAabbInRange(campos, range,
						cyl->x - cyl->radius, cyl->x + cyl->radius,
						cyl->z - cyl->radius, cyl->z + cyl->radius)) {
					s32 i;

					for (i = 0; i < COLVIEW_CYLSEGS; i++) {
						f32 a0 = (f32)i / COLVIEW_CYLSEGS * 6.2831855f;
						f32 a1 = (f32)(i + 1) / COLVIEW_CYLSEGS * 6.2831855f;
						f32 x0 = cyl->x + cosf(a0) * cyl->radius;
						f32 z0 = cyl->z + sinf(a0) * cyl->radius;
						f32 x1 = cyl->x + cosf(a1) * cyl->radius;
						f32 z1 = cyl->z + sinf(a1) * cyl->radius;

						pts[0].x = x0; pts[0].y = cyl->ymin; pts[0].z = z0;
						pts[1].x = x1; pts[1].y = cyl->ymin; pts[1].z = z1;
						pts[2].x = x1; pts[2].y = cyl->ymax; pts[2].z = z1;
						pts[3].x = x0; pts[3].y = cyl->ymax; pts[3].z = z0;

						gdl = colviewPoly(gdl, campos, pts, 4, COLSLOT_CYL);
					}
				}
			}

			geo = (struct geo *)((uintptr_t)geo + 0x18);
		} else {
			// Unknown type: the stride is unknown too, so bail rather than walk
			// off into unrelated memory.
			break;
		}
	}

	return gdl;
}

/** Map a HITPART_* id to a palette slot. */
static s32 colviewHitpartSlot(s32 hitpart)
{
	switch (hitpart) {
	case HITPART_HEAD:
		return COLSLOT_HEAD;
	case HITPART_TORSO:
	case HITPART_PELVIS:
		return COLSLOT_TORSO;
	case HITPART_LHAND:
	case HITPART_LFOREARM:
	case HITPART_LBICEP:
	case HITPART_RHAND:
	case HITPART_RFOREARM:
	case HITPART_RBICEP:
		return COLSLOT_ARM;
	case HITPART_LFOOT:
	case HITPART_LSHIN:
	case HITPART_LTHIGH:
	case HITPART_RFOOT:
	case HITPART_RSHIN:
	case HITPART_RTHIGH:
		return COLSLOT_LEG;
	case HITPART_TAIL:
		return COLSLOT_TAIL;
	}

	return COLSLOT_OTHER;
}

/** The 6 faces of an axis-aligned box, in node-local space. */
static Gfx *colviewLocalAabb(Gfx *gdl, f32 xmin, f32 xmax, f32 ymin, f32 ymax,
		f32 zmin, f32 zmax, s32 slot)
{
	// 6 quads as 12 triangles, packed 4 to a gSPTri4 command. Corner indices
	// are the (x,y,z) min/max bit pattern used to fill the vertices below.
	// No triple is all-zero, so none is silently dropped.
	static const u8 tris[12][3] = {
		{ 0, 1, 3 }, { 0, 3, 2 }, // -z
		{ 4, 5, 7 }, { 4, 7, 6 }, // +z
		{ 0, 1, 5 }, { 0, 5, 4 }, // -y
		{ 2, 3, 7 }, { 2, 7, 6 }, // +y
		{ 0, 2, 6 }, { 0, 6, 4 }, // -x
		{ 1, 3, 7 }, { 1, 7, 5 }, // +x
	};
	Vtx *vertices;
	s32 i;

	if (!colviewCanEmit(gdl, 8, 4)) {
		return gdl;
	}

	vertices = gfxAllocateVertices(8);

	if (!vertices) {
		return gdl;
	}

	for (i = 0; i < 8; i++) {
		colviewVtxLocal(&vertices[i],
				(i & 1) ? xmax : xmin,
				(i & 2) ? ymax : ymin,
				(i & 4) ? zmax : zmin,
				slot);
	}

	gSPVertex(gdl++, osVirtualToPhysical(vertices), 8, 0);

	for (i = 0; i < 12; i += 4) {
		gSPTri4(gdl++,
				tris[i][0], tris[i][1], tris[i][2],
				tris[i + 1][0], tris[i + 1][1], tris[i + 1][2],
				tris[i + 2][0], tris[i + 2][1], tris[i + 2][2],
				tris[i + 3][0], tris[i + 3][1], tris[i + 3][2]);
	}

	return gdl;
}

/**
 * Black outlines for a hit box: the 12 edges as thin solid bars.
 *
 * Bars are real geometry, not lines — the fast3d line path is desktop-GL only
 * (see PORT_WIREFRAME.md) and wouldn't survive on the SDL_GPU backend, and a
 * bar reads better anyway because it thickens as you approach.
 *
 * Each bar is centred ON its edge, so it straddles the box surface: the outer
 * half sits proud of the translucent fill (no coplanar z-fighting) and the
 * inner half is overdrawn on top of it, since XLU render modes compare depth
 * but don't write it — draw order decides, and the fill goes first.
 *
 * Thickness is a FRACTION of the box, not a fixed world size: hit boxes range
 * from a foot to a torso, and one absolute thickness would either swallow the
 * small ones or vanish on the large ones.
 */
static Gfx *colviewLocalBoxEdges(Gfx *gdl, struct modelrodata_bbox *bbox)
{
	f32 ex = bbox->xmax - bbox->xmin;
	f32 ey = bbox->ymax - bbox->ymin;
	f32 ez = bbox->zmax - bbox->zmin;
	f32 smallest = ex < ey ? ex : ey;
	f32 t;
	s32 i;

	if (ez < smallest) {
		smallest = ez;
	}

	// Scale off the SMALLEST extent, not the largest: limb boxes are long and
	// thin (a shin is ~3x longer than it is wide), and a thickness derived from
	// the long axis would be wider than the box's short axis — the bars would
	// meet in the middle and the whole box would read as a black blob.
	t = smallest * 0.12f;

	if (t < 0.4f) {
		t = 0.4f;
	}

	// 4 edges along each axis. The bars overlap at the corners, which fills
	// them in — cheaper and cleaner than mitring.
	for (i = 0; i < 4; i++) {
		f32 y = (i & 1) ? bbox->ymax : bbox->ymin;
		f32 z = (i & 2) ? bbox->zmax : bbox->zmin;

		gdl = colviewLocalAabb(gdl, bbox->xmin, bbox->xmax,
				y - t, y + t, z - t, z + t, COLSLOT_EDGE);
	}

	for (i = 0; i < 4; i++) {
		f32 x = (i & 1) ? bbox->xmax : bbox->xmin;
		f32 z = (i & 2) ? bbox->zmax : bbox->zmin;

		gdl = colviewLocalAabb(gdl, x - t, x + t,
				bbox->ymin, bbox->ymax, z - t, z + t, COLSLOT_EDGE);
	}

	for (i = 0; i < 4; i++) {
		f32 x = (i & 1) ? bbox->xmax : bbox->xmin;
		f32 y = (i & 2) ? bbox->ymax : bbox->ymin;

		gdl = colviewLocalAabb(gdl, x - t, x + t, y - t, y + t,
				bbox->zmin, bbox->zmax, COLSLOT_EDGE);
	}

	return gdl;
}

/**
 * Draw every hit box of one chr. The model's matrices are model-to-screen, so
 * each box is emitted in node-local space under its own matrix.
 */
static Gfx *colviewDrawChrHitboxes(Gfx *gdl, struct chrdata *chr)
{
	struct modelnode *node;

	if (!chr || !chr->model || !chr->model->definition) {
		return gdl;
	}

	// modelInit leaves `matrices` NULL until a model's first render prep, and
	// modelFindNodeMtx returns `&matrices[index]` — NULL + offset is a garbage
	// pointer, not NULL. Never dereference it without this check.
	if (!chr->model->matrices) {
		return gdl;
	}

	node = chr->model->definition->rootnode;

	while (node) {
		if ((node->type & 0xff) == MODELNODETYPE_BBOX) {
			Mtxf *mtxf = modelFindNodeMtx(chr->model, node, 0);

			// A hit box costs one matrix + up to 13 boxes (fill + 12 edge
			// bars) at 8 verts / 4 commands each. gfxAllocateMatrix is a bare
			// pointer bump with no bounds check, so it must be budgeted too —
			// an unchecked matrix write past the pool end is precisely what
			// throws geometry to random screen positions.
			if (mtxf && node->rodata && colviewCanEmit(gdl, 8 + 13 * 8, 1 + 13 * 4)) {
				// gfxAllocateMatrix returns the scratch slot mtxF2L writes the
				// fixed-point form into; both sides are typed Mtxf * here.
				Mtxf *mtx = gfxAllocateMatrix();

				if (mtx) {
					struct modelrodata_bbox *bbox = &node->rodata->bbox;

					mtxF2L(mtxf, mtx);
					gSPMatrix(gdl++, osVirtualToPhysical(mtx), G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);

					// Fill first, outlines over it — XLU doesn't write depth,
					// so draw order is what layers them.
					gdl = colviewLocalAabb(gdl, bbox->xmin, bbox->xmax,
							bbox->ymin, bbox->ymax, bbox->zmin, bbox->zmax,
							colviewHitpartSlot(bbox->hitpart));

					if (g_ColViewOutlines) {
						gdl = colviewLocalBoxEdges(gdl, bbox);
					}
				}
			}
		}

		// Same traversal as the hitpart search in chraction.c: everything
		// except a headspot's children (those carry their own bboxes that the
		// hit test doesn't walk from here).
		if (node->child && (node->type & 0xff) != MODELNODETYPE_HEADSPOT) {
			node = node->child;
		} else {
			while (node) {
				if (node->next) {
					node = node->next;
					break;
				}

				node = node->parent;
			}
		}
	}

	return gdl;
}

/**
 * Entry point, called from playerRenderHud with the world rendered and the
 * depth buffer still intact (bgunRender clears it afterwards, which is why
 * this must run before the viewmodel).
 */
Gfx *colviewRender(Gfx *gdl)
{
	struct player *player = g_Vars.currentplayer;
	struct coord campos;
	RoomNum rooms[COLVIEW_MAXROOMS + 1];
	s16 propnums[COLVIEW_MAXPROPS];
	s32 numrooms = 0;
	s32 range;
	f32 rangesq;
	Mtxf worldmtx;
	Mtxf *mtx;
	s32 r;
	s32 i;

	if (!g_ColViewEnabled || !player || !player->prop) {
		return gdl;
	}

	if (!g_ColViewGeo && !g_ColViewProps && !g_ColViewHitboxes) {
		return gdl;
	}

	range = g_ColViewRange;

	if (range < COLVIEW_MINRANGE) {
		range = COLVIEW_MINRANGE;
	} else if (range > COLVIEW_MAXRANGE) {
		range = COLVIEW_MAXRANGE;
	}

	g_ColViewRange = range;
	rangesq = (f32)range * (f32)range;

	campos.x = player->cam_pos.x;
	campos.y = player->cam_pos.y;
	campos.z = player->cam_pos.z;

	// --- Render state: untextured, vertex-coloured, translucent, two-sided.
	gDPSetCycleType(gdl++, G_CYC_1CYCLE);
	gDPSetTexturePersp(gdl++, G_TP_NONE);
	gDPSetTextureLUT(gdl++, G_TT_NONE);
	gDPSetAlphaCompare(gdl++, G_AC_NONE);
	gDPSetColorDither(gdl++, G_CD_DISABLE);
	gDPSetCombineMode(gdl++, G_CC_SHADE, G_CC_SHADE);
	gSPClearGeometryMode(gdl++, G_CULL_BOTH | G_LIGHTING | G_TEXTURE_GEN | G_TEXTURE_GEN_LINEAR | G_FOG);
	gSPSetGeometryMode(gdl++, G_SHADE | G_SHADING_SMOOTH);

	if (g_ColViewXray) {
		// No Z compare: volumes show through walls.
		gDPSetRenderMode(gdl++, G_RM_AA_XLU_SURF, G_RM_AA_XLU_SURF2);
	} else {
		gDPSetRenderMode(gdl++, G_RM_AA_ZB_XLU_SURF, G_RM_AA_ZB_XLU_SURF2);
	}

	gdl = colviewSetupColours(gdl);

	if (!g_ColViewCols) {
		return gdl;
	}

	// --- Collect the rooms in range. Room bboxes are world-space, so this is a
	// cheap broad phase before touching any geo bytes.
	for (r = 1; r < g_Vars.roomcount && numrooms < COLVIEW_MAXROOMS; r++) {
		f32 dx = 0.0f;
		f32 dz = 0.0f;

		if (campos.x < g_Rooms[r].bbmin[0]) {
			dx = g_Rooms[r].bbmin[0] - campos.x;
		} else if (campos.x > g_Rooms[r].bbmax[0]) {
			dx = campos.x - g_Rooms[r].bbmax[0];
		}

		if (campos.z < g_Rooms[r].bbmin[2]) {
			dz = g_Rooms[r].bbmin[2] - campos.z;
		} else if (campos.z > g_Rooms[r].bbmax[2]) {
			dz = campos.z - g_Rooms[r].bbmax[2];
		}

		if (dx * dx + dz * dz < rangesq) {
			rooms[numrooms++] = r;
		}
	}

	rooms[numrooms] = -1;

	// One prop query for both the object-collision and hitbox passes. It
	// terminates with a negative entry within COLVIEW_MAXPROPS, but every walk
	// below still bounds-checks first so a full array can't be read past.
	propnums[0] = -1;

	if (g_ColViewProps || g_ColViewHitboxes) {
		roomGetProps(rooms, propnums, COLVIEW_MAXPROPS);
	}

	// --- World geometry, under the camera-relative world matrix.
	if (g_ColViewGeo || g_ColViewProps) {
		// The plain world-to-screen matrix, translation INTACT — vertices are
		// world coordinates. Do not zero the translation and switch to
		// camera-relative coords here; that's what overflowed s16 on large
		// outdoor tiles (see the COLVIEW_DEPTHBIAS block).
		mtx4LoadIdentity(&worldmtx);
		mtx00015be0(camGetWorldToScreenMtxf(), &worldmtx);

		if (!colviewCanEmit(gdl, 8, 1)) {
			return gdl;
		}

		mtx = gfxAllocateMatrix();

		if (!mtx) {
			return gdl;
		}

		mtxF2L(&worldmtx, mtx);
		gSPMatrix(gdl++, osVirtualToPhysical(mtx), G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);

		if (g_ColViewGeo) {
			for (i = 0; i < numrooms; i++) {
				s32 roomnum = rooms[i];

				if (roomnum < g_TileNumRooms) {
					gdl = colviewDrawGeoList(gdl, &campos,
							g_TileFileData.u8 + g_TileRooms[roomnum],
							g_TileFileData.u8 + g_TileRooms[roomnum + 1]);
				}
			}
		}

		if (g_ColViewProps) {
			for (i = 0; i < COLVIEW_MAXPROPS && propnums[i] >= 0; i++) {
				struct prop *prop = &g_Vars.props[propnums[i]];
				u8 *start;
				u8 *end;

				if (propUpdateGeometry(prop, &start, &end)) {
					gdl = colviewDrawGeoList(gdl, &campos, start, end);
				}
			}
		}
	}

	// --- Chr hit boxes, each under its own model-to-screen node matrix.
	if (g_ColViewHitboxes) {
		for (i = 0; i < COLVIEW_MAXPROPS && propnums[i] >= 0; i++) {
			struct prop *prop = &g_Vars.props[propnums[i]];
			f32 dx;
			f32 dz;

			// prop->chr / obj / door alias one union slot, so the type test is
			// mandatory before touching chr fields.
			if (prop->type != PROPTYPE_CHR && prop->type != PROPTYPE_PLAYER) {
				continue;
			}

			dx = prop->pos.x - campos.x;
			dz = prop->pos.z - campos.z;

			if (dx * dx + dz * dz >= rangesq) {
				continue;
			}

			gdl = colviewDrawChrHitboxes(gdl, prop->chr);
		}
	}

	return gdl;
}

#endif
