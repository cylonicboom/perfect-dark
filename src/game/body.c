#include <ultra64.h>
#include "constants.h"
#include "game/cheats.h"
#include "game/chraction.h"
#include "game/chr.h"
#include "game/body.h"
#include "game/prop.h"
#include "game/atan2f.h"
#include "game/modelmgr.h"
#include "game/lv.h"
#include "game/modeldef.h"
#include "game/mplayer/mplayer.h"
#include "game/pad.h"
#include "game/propobj.h"
#include "bss.h"
#include "lib/memp.h"
#include "lib/model.h"
#include "game/texdecompress.h" // texInitPool for the model-swap private tex pool
#include "lib/mema.h"
#include "lib/rng.h"
#include "lib/mtx.h"
#include "lib/ailist.h"
#include "lib/anim.h"
#include "lib/collision.h"
#include "data.h"
#include "types.h"
#ifndef PLATFORM_N64
#include "system.h" // sysLogPrintf for the bodyAllocateModel NULL-definition guard
#endif

#ifndef PLATFORM_N64
// Set by botmgrAllocateBot around its bodyAllocateModel call when the
// "Randomise Heights" sim toggle is off: the height RNG is still consumed (so
// the deterministic bot-allocation stream stays in lockstep on every peer and
// build) but the resulting scale is neutralised. Scoped to sim bodies only.
extern u8 g_MpSimFixedHeight;
#endif

s32 g_NumActiveHeadsPerGender;
u32 var8009cd24;
s32 g_ActiveMaleHeads[8];
s32 g_ActiveFemaleHeads[8];

s32 g_NumBondBodies = 0;
s32 g_NumMaleGuardHeads = 0;
s32 g_NumFemaleGuardHeads = 0;
s32 g_NumMaleGuardTeamHeads = 0;
s32 g_NumFemaleGuardTeamHeads = 0;
s32 var80062b14 = 0;
s32 var80062b18 = 0;

s32 g_BondBodies[] = {
	BODY_DJBOND,
	BODY_CONNERY,
	BODY_DALTON,
	BODY_MOORE,
	-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
};

s32 g_MaleGuardHeads[] = {
	HEAD_BEAU1,
	HEAD_CHRIST,
	HEAD_DARLING,
	HEAD_JON,
	HEAD_ROSS,
	HEAD_RUSS,
	HEAD_MARK2,
	HEAD_JAMIE,
	HEAD_DUNCAN2,
	HEAD_BRIAN,
	HEAD_STEVEM,
	HEAD_KEITH,
	HEAD_GRANT,
	HEAD_PENNY,
	HEAD_DAVEC,
	HEAD_JONES,
	HEAD_GRAHAM,
	HEAD_SHAUN,
	HEAD_NEIL2,
	HEAD_EDMCG,
	HEAD_MATT_C,
	HEAD_PEER_S,
	HEAD_ANDY_R,
	HEAD_BEN_R,
	HEAD_STEVE_K,
	HEAD_SCOTT_H,
	HEAD_SANCHEZ,
	HEAD_COOK,
	HEAD_PRYCE,
	HEAD_SILKE,
	HEAD_SMITH,
	HEAD_GARETH,
	HEAD_MURCHIE,
	HEAD_WONG,
	HEAD_CARTER,
	HEAD_TINTIN,
	HEAD_MUNTON,
	HEAD_PHELPS,
	HEAD_KEN,
	HEAD_JOEL,
	HEAD_TIM,
	HEAD_ROBIN,
	-1,
};

s32 g_MaleGuardTeamHeads[] = {
	HEAD_BEAU1,
	HEAD_CHRIST,
	HEAD_DARLING,
	HEAD_JON,
	HEAD_ROSS,
	HEAD_RUSS,
	HEAD_MARK2,
	HEAD_JAMIE,
	HEAD_DUNCAN2,
	HEAD_BRIAN,
	HEAD_STEVEM,
	HEAD_KEITH,
	HEAD_GRANT,
	HEAD_PENNY,
	HEAD_DAVEC,
	HEAD_JONES,
	-1,
};

s32 g_FemaleGuardHeads[] = {
	HEAD_LESLIE_S,
	HEAD_ANKA,
	HEAD_EILEEN_T,
	HEAD_EILEEN_H,
	-1,
};

s32 g_FemaleGuardTeamHeads[] = {
	HEAD_LESLIE_S,
	HEAD_ANKA,
	HEAD_EILEEN_T,
	HEAD_EILEEN_H,
	-1,
};

s32 var80062c80 = 0;
s32 g_ActiveMaleHeadsIndex = 0;
s32 g_ActiveFemaleHeadsIndex = 0;

s32 g_FemGuardHeads[3] = {
	HEAD_ALEX,
	HEAD_JULIANNE,
	HEAD_LAURA,
};

u32 bodyGetRace(s32 bodynum)
{
	switch (bodynum) {
	case BODY_SKEDAR:
	case BODY_MINISKEDAR:
	case BODY_SKEDARKING:
		return RACE_SKEDAR;
	case BODY_DRCAROLL:
		return RACE_DRCAROLL;
	case BODY_EYESPY:
		return RACE_EYESPY;
	case BODY_CHICROB:
		return RACE_ROBOT;
	}

	return RACE_HUMAN;
}

bool bodyLoad(s32 bodynum)
{
	if (!g_HeadsAndBodies[bodynum].modeldef) {
		g_HeadsAndBodies[bodynum].modeldef = modeldefLoadToNew(g_HeadsAndBodies[bodynum].filenum);
		return true;
	}

	return false;
}

#ifndef PLATFORM_N64
// Chaos live model swap (see port/src/romdata.c). These live in the port loader.
extern s32 g_ModelRomActive;   // an overlay ROM (--model-rom) is loaded
extern s32 g_ModelSwapActive;  // character models sourced from the overlay now
extern u8 g_ModelSwapFiles[];  // per-file: redirect this file to the overlay
extern s32 g_ModelSwapRedirects; // # overlay serves this toggle (diagnostics)
extern s32 g_ModelSwapMisses;    // # armed-but-couldn't-serve (diagnostics)
extern s32 romdataChainFileGetNumForName(const char *name);
extern const char *romdataFileGetName(s32 fileNum);

// Model-swap texture overlay (port/src/romdata.c). g_ModelSwapTexList != NULL
// once the overlay ROM's texture table was located; g_ModelSwapTexActive gates
// texLoad's per-number redirect AND the private-pool selection in modeldefLoad.
extern struct texture *g_ModelSwapTexList;
extern s32 g_ModelSwapTexActive;

// Private texture pool for swapped models: overlay textures load here (once,
// shared across all swapped models) so they don't alias the base game's cached
// textures in g_TexSharedPool. Referenced by modeldefLoad. Allocated per swap-on
// from MEMPOOL_STAGE (bounded per-toggle leak, freed at stage end).
struct texpool g_ModelSwapTexPool;
static bool g_ModelSwapTexPoolReady = false;
#define MODELSWAP_TEXPOOL_BYTES (6 * 1024 * 1024)

// # of live chrs whose overlay body/head produced no modeldef this rebuild
// (they keep the base model). Reported by the model-swap toggle log.
static s32 g_ModelSwapNullLoads = 0;

// A chr's held items (weapons_held[0]=right gun, [1]=left gun, [2]=hat) are
// child props whose model is ATTACHED to the chr's body model — attachedtomodel
// points at chr->model and attachedtonode at a node inside that model's
// definition (see chrEquipWeapon / hatApplyToChr in propobj.c). When we swap the
// body to a fresh model with a DIFFERENT definition, those two pointers dangle
// into the old (freed) definition; chr0f022214's per-child modelFindNodeMtx then
// reads freed memory and crashes. Re-point each held item at the new body model
// and re-resolve its attach node against the new definition (same bodynum, so the
// skeleton — and thus the hand/hat parts — is identical). Called after the body
// re-link but before the old model is freed.
static void modelSwapReattachHeld(struct chrdata *chr)
{
	struct model *body = chr->model;
	bool skedar;
	s32 h;

	if (body == NULL || body->definition == NULL) {
		return;
	}

	skedar = (body->definition->skel == &g_SkelSkedar);

	for (h = 0; h < 3; h++) {
		struct prop *held = chr->weapons_held[h];
		struct modelnode *node = NULL;

		if (held == NULL || held->obj == NULL || held->obj->model == NULL) {
			continue;
		}

		if (h == HAND_RIGHT) {
			node = modelGetPart(body->definition,
					skedar ? MODELPART_SKEDAR_RIGHTHAND : MODELPART_CHR_RIGHTHAND);
		} else if (h == HAND_LEFT) {
			node = modelGetPart(body->definition,
					skedar ? MODELPART_SKEDAR_LEFTHAND : MODELPART_CHR_LEFTHAND);
		} else if (!skedar) {
			// Hat slot — skedar has no hats (hatApplyToChr is g_SkelChr-only).
			node = modelGetPart(body->definition, MODELPART_CHR_0006);
		}

		if (node) {
			held->obj->model->attachedtomodel = body;
			held->obj->model->attachedtonode = node;
		}
	}
}

// Rebuild every currently-spawned chr's body+head model IN PLACE, so the swap
// takes effect immediately instead of only on respawn. For each live non-player
// chr: allocate a fresh model (which now loads from the overlay or base per
// g_ModelSwapActive), re-link it with chr0f020b14 — which SKIPS chrInit for an
// existing chr, so health/AI/inventory are preserved (it re-grounds the chr and
// resets to the idle pose) — re-attach its held weapons/hat to the new model,
// then free the old model the same way chr death does. Corpses and the player
// are skipped. Called at effect toggle from the Lua tick (after tick, before
// render), which is a safe boundary.
static s32 modelSwapRebuildLiveChrs(void)
{
	s32 i;
	s32 rebuilt = 0;

	if (g_ChrSlots == NULL) {
		return 0;
	}

	for (i = 0; i < g_NumChrSlots; i++) {
		struct chrdata *chr = &g_ChrSlots[i];
		struct model *old;
		struct model *neu;
		struct coord pos;
		RoomNum rooms[8];
		f32 faceangle;

		if (chr->prop == NULL || chr->model == NULL) {
			continue; // no live model
		}
		if (chr->prop->type != PROPTYPE_CHR) {
			continue; // players / non-chr props handled elsewhere
		}
		if (chrIsDead(chr)) {
			continue; // leave corpses as they are
		}

		// Same guard as chraiLuaChrSetBody: chr0f020b14 re-grounds what it
		// re-links, and cdFindGroundInfoAtCyl reports "no floor" as the
		// -4294967296 sentinel rather than failing — which chr0f020b14 turns
		// straight into prop->pos.y, dropping the chr four billion units under the
		// map. Skip anyone standing where no floor can be found (airborne after an
		// explosion, on a lift); they get swapped on their next respawn.
		{
			struct coord testpos;
			f32 ground;

			testpos.x = chr->prop->pos.x;
			testpos.y = chr->prop->pos.y + 100.0f;
			testpos.z = chr->prop->pos.z;

			ground = cdFindGroundInfoAtCyl(&testpos, chr->radius, chr->prop->rooms,
					NULL, NULL, NULL, NULL, NULL, NULL);

			if (ground <= -100000.0f) {
				continue;
			}
		}

		old = chr->model;
		faceangle = chrGetInverseTheta(chr);
		pos.x = chr->prop->pos.x;
		pos.y = chr->prop->pos.y;
		pos.z = chr->prop->pos.z;
		roomsCopy(chr->prop->rooms, rooms);

		// bodyAllocateModel brackets the overlay-texture redirect itself (so
		// respawns get it too), so no bracketing is needed here.
		neu = bodyAllocateModel(chr->bodynum, chr->headnum, 0);
		if (neu) {
			chr0f020b14(chr->prop, neu, &pos, rooms, faceangle, NULL);
			// chr0f020b14 set chr->model = neu; re-hang the held items on it
			// before the old model (their current attach target) is freed.
			modelSwapReattachHeld(chr);

			// chr0f020b14 is the spawn-time linker: it leaves the fresh model
			// with a zeroed anim (T-pose) and a default-initialised chrinfo
			// rwdata (root position / facing / root-motion accumulator). On a
			// live chr that renders as a T-pose that then integrates garbage
			// root motion each tick — the chr teleports, spins, or drifts out
			// of view ("invisible"). Both models are the same bodynum, so the
			// anim struct and the root chrinfo rwdata are layout-identical:
			// copy them straight across so the swap is seamless.
			modelCopyAnimData(old, neu);

			if ((old->definition->rootnode->type & 0xff) == MODELNODETYPE_CHRINFO
					&& (neu->definition->rootnode->type & 0xff) == MODELNODETYPE_CHRINFO) {
				union modelrwdata *oldrw =
						modelGetNodeRwData(old, old->definition->rootnode);
				union modelrwdata *newrw =
						modelGetNodeRwData(neu, neu->definition->rootnode);

				if (oldrw && newrw) {
					newrw->chrinfo = oldrw->chrinfo;
				}
			}

			modelmgrFreeModel(old);
			rebuilt++;
		} else {
			// bodyAllocateModel returned NULL: the overlay file for this
			// body/head produced no usable modeldef (bad/absent slot). The chr
			// keeps its old model, so it shows as un-swapped rather than gone —
			// count it so the toggle log can report how many failed to load.
			g_ModelSwapNullLoads++;
		}
	}

	return rebuilt;
}

// Turn the Chaos character-model swap on/off. Flags every character body/head
// (and first-person hands) file for redirection to the overlay ROM, invalidates
// the shared modeldef cache + drops the file cache so the NEXT load of each
// body/head re-reads (overlay when on, base when off), then rebuilds every live
// chr in place so the swap is visible immediately (Phase 2), not just on respawn.
//
// Only invalidates/rebuilds on an actual state change to bound the per-toggle
// MEMPOOL_STAGE modeldef leak (nulling the cache without freeing keeps existing
// pointers valid; the live rebuild frees each chr's OLD instance as it goes).
void modelSwapSetActive(bool on)
{
	s32 i;
	bool changed = (on != 0) != (g_ModelSwapActive != 0);

	if (!g_ModelRomActive) {
		return; // no --model-rom overlay loaded; nothing to swap
	}

	// Rebuild the redirect set from the live body/head table (tracks whatever
	// bodies/heads the current stage actually uses).
	for (i = 0; i < 2048 /* ROMDATA_MAX_FILES */; i++) {
		g_ModelSwapFiles[i] = 0;
	}
	if (on) {
		for (i = 0; g_HeadsAndBodies[i].filenum; i++) {
			g_ModelSwapFiles[g_HeadsAndBodies[i].filenum] = 1;
			if (g_HeadsAndBodies[i].handfilenum) {
				g_ModelSwapFiles[g_HeadsAndBodies[i].handfilenum] = 1;
			}
		}
	}

	g_ModelSwapActive = on ? 1 : 0;

	if (changed) {
		s32 rebuilt;
		s32 probefile;
		s32 probecn;

		// Ensure the private overlay-texture pool exists (allocated once, from
		// PERMANENT memory so it survives stage changes and acts as a persistent
		// overlay-texture cache). If it can't be had, texturing stays base and
		// only geometry swaps.
		if (on && g_ModelSwapTexList != NULL && !g_ModelSwapTexPoolReady) {
			u8 *poolmem = mempAlloc(ALIGN16(MODELSWAP_TEXPOOL_BYTES), MEMPOOL_PERMANENT);
			if (poolmem) {
				texInitPool(&g_ModelSwapTexPool, poolmem, MODELSWAP_TEXPOOL_BYTES);
				g_ModelSwapTexPoolReady = true;
			}
		}

		// Drop the shared modeldef cache + the file cache so the next load
		// re-reads the (now redirected) file data.
		for (i = 0; g_HeadsAndBodies[i].filenum; i++) {
			g_HeadsAndBodies[i].modeldef = NULL;
			g_FileInfo[g_HeadsAndBodies[i].filenum].loadedsize = 0;
			if (g_HeadsAndBodies[i].handfilenum) {
				g_FileInfo[g_HeadsAndBodies[i].handfilenum].loadedsize = 0;
			}
		}

		// Diagnostics: zero the redirect counters so they reflect just this
		// toggle's rebuild loads, then probe whether the first body file even
		// resolves to a same-named file in the overlay ROM.
		g_ModelSwapRedirects = 0;
		g_ModelSwapMisses = 0;
		g_ModelSwapNullLoads = 0;
		probefile = g_HeadsAndBodies[0].filenum;
		probecn = romdataChainFileGetNumForName(romdataFileGetName(probefile));

		// Immediately swap everyone already on screen.
		rebuilt = modelSwapRebuildLiveChrs();

		sysLogPrintf(LOG_NOTE,
				"modelswap: %s romactive=%d chrs=%d rebuilt=%d nullloads=%d redirects=%d misses=%d probe(file=%d '%s' -> chain=%d)",
				on ? "ON" : "OFF", g_ModelRomActive, g_NumChrSlots, rebuilt,
				g_ModelSwapNullLoads, g_ModelSwapRedirects, g_ModelSwapMisses,
				probefile, romdataFileGetName(probefile), probecn);
	}
}

bool modelSwapRomLoaded(void)
{
	return g_ModelRomActive != 0;
}
#endif

struct model *body0f02ce8c(s32 bodynum, s32 headnum, struct modeldef *bodymodeldef, struct modeldef *headmodeldef, bool sunglasses, struct model *model, bool isplayer, u8 varyheight)
{
	f32 scale = g_HeadsAndBodies[bodynum].scale * 0.10000001f;
	f32 animscale = g_HeadsAndBodies[bodynum].animscale;
	struct modelnode *node = NULL;
	u32 stack[2];

	if (cheatIsActive(CHEAT_DKMODE)) {
		scale *= 0.8f;
	}

	if (bodymodeldef == NULL) {
		if (g_HeadsAndBodies[bodynum].modeldef == NULL) {
			g_HeadsAndBodies[bodynum].modeldef = modeldefLoadToNew(g_HeadsAndBodies[bodynum].filenum);
		}

		bodymodeldef = g_HeadsAndBodies[bodynum].modeldef;
	}

#ifndef PLATFORM_N64
	// modeldefLoadToNew returns NULL for files with no data source (port-added
	// mod model ids — FILE_CSKEDAR2 etc — on installs without the mod data
	// dir; crash ledger #22). Bail before modelAllocateRwData derefs it; the
	// bodyAllocateModel guard passes the NULL up and botmgrAllocateBot / the
	// chr spawn paths skip the body.
	if (bodymodeldef == NULL) {
		sysLogPrintf(LOG_ERROR,
				"body0f02ce8c: body %d (file %d) has no modeldef (missing mod file?) — skipping",
				bodynum, g_HeadsAndBodies[bodynum].filenum);
		return NULL;
	}
#endif

	modelAllocateRwData(bodymodeldef);

	if (!g_HeadsAndBodies[bodynum].unk00_01) {
		if (bodymodeldef->skel == &g_SkelChr) {
			node = modelGetPart(bodymodeldef, MODELPART_CHR_HEADSPOT);

			if (node != NULL) {
				if (headnum < 0) {
					headmodeldef = func0f18e57c(-1 - headnum, &headnum);
					bodymodeldef->rwdatalen += headmodeldef->rwdatalen;
				} else if (headnum > 0) {
					if (headmodeldef == NULL) {
						if (g_Vars.normmplayerisrunning && !IS4MB()) {
							headmodeldef = modeldefLoadToNew(g_HeadsAndBodies[headnum].filenum);
							g_HeadsAndBodies[headnum].modeldef = headmodeldef;
							g_FileInfo[g_HeadsAndBodies[headnum].filenum].loadedsize = 0;
							bodyCalculateHeadOffset(headmodeldef, headnum, bodynum);
						} else {
							if (g_HeadsAndBodies[headnum].modeldef == NULL) {
								g_HeadsAndBodies[headnum].modeldef = modeldefLoadToNew(g_HeadsAndBodies[headnum].filenum);
							}

							headmodeldef = g_HeadsAndBodies[headnum].modeldef;
						}
					}

#ifndef PLATFORM_N64
					// Same missing-data-source bail as the body load above,
					// for the head modeldef (ledger #22).
					if (headmodeldef == NULL) {
						sysLogPrintf(LOG_ERROR,
								"body0f02ce8c: head %d (file %d) has no modeldef (missing mod file?) — skipping",
								headnum, g_HeadsAndBodies[headnum].filenum);
						return NULL;
					}
#endif

					modelAllocateRwData(headmodeldef);

					bodymodeldef->rwdatalen += headmodeldef->rwdatalen;

					if (g_HeadsAndBodies[bodynum].canvaryheight && varyheight) {
						// Set height to between 95% and 115%
						f32 frac = RANDOMFRAC() * 0.05f;
						f32 mult = 2.0f * frac - 0.05f + 1.0f;
#ifndef PLATFORM_N64
						// Sim "Randomise Heights" off: RNG already consumed above
						// (keeps peers in sync) — just don't apply the variation.
						if (g_MpSimFixedHeight) {
							mult = 1.0f;
						}
#endif
						scale *= mult;
					}
				}

				if (!isplayer) {
					if (cheatIsActive(CHEAT_SMALLCHARACTERS)) {
						scale *= 0.4f;
					}

					if (cheatIsActive(CHEAT_DKMODE)) {
						scale *= 1.25f;
					}
				} else {
					if (cheatIsActive(CHEAT_SMALLJO)) {
						scale *= 0.4f;
					}
				}
			}
		} else if (bodymodeldef->skel == &g_SkelSkedar) {
			if (g_HeadsAndBodies[bodynum].canvaryheight && varyheight && bodynum == BODY_SKEDAR) {
				// Set height to between 65% and 85%
				f32 frac = RANDOMFRAC();
				f32 mult = 2.0f * (0.1f * frac) - 0.1f + 0.75f;
#ifndef PLATFORM_N64
				// Sim "Randomise Heights" off: RNG already consumed — don't apply.
				if (g_MpSimFixedHeight) {
					mult = 1.0f;
				}
#endif
				scale *= mult;
			}

			if (1);
		}
	}

	if (model) {
		if (model->rwdatalen < bodymodeldef->rwdatalen);
	} else {
		model = modelmgrInstantiateModelWithAnim(bodymodeldef);
	}

	if (model) {
		modelSetScale(model, scale);
		modelSetAnimScale(model, animscale);

		if (headmodeldef && !g_HeadsAndBodies[bodynum].unk00_01) {
			bodymodeldef->rwdatalen -= headmodeldef->rwdatalen;

			modelmgrAttachHead(model, node, headmodeldef);

			if ((s16)*(s32 *)&headmodeldef->skel == SKEL_HEAD) {
				struct modelnode *node2;

				if (!sunglasses) {
					node2 = modelGetPart(headmodeldef, MODELPART_HEAD_SUNGLASSES);

					if (node2) {
						union modelrwdata *rwdata = modelGetNodeRwData(model, node2);
						rwdata->toggle.visible = false;
					}
				}

				node2 = modelGetPart(headmodeldef, MODELPART_HEAD_HUDPIECE);

				if (node2) {
					union modelrwdata *rwdata = modelGetNodeRwData(model, node2);
					rwdata->toggle.visible = false;
				}
			}
		}
	}

	return model;
}

struct model *body0f02d338(s32 bodynum, s32 headnum, struct modeldef *bodymodeldef, struct modeldef *headmodeldef, bool sunglasses, u8 varyheight)
{
	return body0f02ce8c(bodynum, headnum, bodymodeldef, headmodeldef, sunglasses, NULL, false, varyheight);
}

struct model *bodyAllocateModel(s32 bodynum, s32 headnum, u32 spawnflags)
{
	bool sunglasses = false;
	u8 varyheight = true;
#ifndef PLATFORM_N64
	struct model *model;
#endif

	if (spawnflags & SPAWNFLAG_FORCESUNGLASSES) {
		sunglasses = true;
	} else if (spawnflags & SPAWNFLAG_MAYBESUNGLASSES) {
		sunglasses = rngRandom() % 2 == 0;
	}

	if (spawnflags & SPAWNFLAG_FIXEDHEIGHT) {
		varyheight = false;
	}

#ifndef PLATFORM_N64
	// Chaos model-swap: this is the single choke point every character body/head
	// load passes through (initial spawn, respawn, and the live rebuild), so
	// bracket the overlay-texture redirect here — swapped models draw the
	// overlay's textures from the private pool for their whole lifetime, not just
	// on the toggle. Gated on the pool + overlay table being ready; otherwise
	// geometry swaps and textures stay base. World/gun textures don't pass
	// through here, so they're unaffected.
	bool texswap = (g_ModelSwapActive && g_ModelSwapTexPoolReady && g_ModelSwapTexList != NULL);

	// A model whose definition (or its contents) is bad crashes far away from
	// here — chrAllocate -> chrSetLookAngle -> modelSetChrRotY read-at-0 on a
	// headless server at match start (timing/pressure-dependent, 2026-06-10;
	// suspected file-cache reclaim of the modeldef backing memory while
	// g_HeadsAndBodies[] still points at it: zeroed contents = rootnode NULL
	// at offset 0). Catch BOTH shapes here, name the body/head loudly, and
	// return NULL — botmgrAllocateBot and the chr spawn paths handle NULL by
	// skipping the chr. The model slot is deliberately leaked (freeing a
	// half-built model walks its definition); this is a crash-grade event,
	// once-per-incident, and the slot returns at stage end.
	if (texswap) {
		g_ModelSwapTexActive = 1;
	}
	model = body0f02d338(bodynum, headnum, NULL, NULL, sunglasses, varyheight);
	g_ModelSwapTexActive = 0;

	if (model && (model->definition == NULL || model->definition->rootnode == NULL)) {
		sysLogPrintf(LOG_ERROR,
				"bodyAllocateModel: body %d head %d produced model with NULL definition/rootnode — dropping (file evicted?)",
				bodynum, headnum);
		return NULL;
	}

	return model;
#else
	return body0f02d338(bodynum, headnum, NULL, NULL, sunglasses, varyheight);
#endif
}

s32 body0f02d3f8(void)
{
	return g_BondBodies[var80062c80];
}

#ifndef PLATFORM_N64
// Number of entries in g_HeadsAndBodies[] (the table is sentinel-terminated,
// filenum == 0). Used by netmsg.c to validate wire body/head indices before
// anything dereferences the table (audit M1 — a bad SVC_CHR_SPAWN index was
// an OOB read/crash).
s32 bodyGetHeadsAndBodiesCount(void)
{
	static s32 count = -1;

	if (count < 0) {
		s32 i = 0;
		while (g_HeadsAndBodies[i].filenum) {
			i++;
		}
		count = i;
	}
	return count;
}
#endif

s32 bodyChooseHead(s32 bodynum)
{
	s32 head;

	if (g_HeadsAndBodies[bodynum].ismale) {
		head = g_ActiveMaleHeads[g_ActiveMaleHeadsIndex++];

		if (g_ActiveMaleHeadsIndex == g_NumActiveHeadsPerGender) {
			g_ActiveMaleHeadsIndex = 0;
		}
	} else if (bodynum == BODY_FEM_GUARD) {
		head = g_FemGuardHeads[rngRandom() % 3];
	} else {
		head = g_ActiveFemaleHeads[g_ActiveFemaleHeadsIndex++];

		if (g_ActiveFemaleHeadsIndex == g_NumActiveHeadsPerGender) {
			g_ActiveFemaleHeadsIndex = 0;
		}
	}

	return head;
}

/**
 * Read a "packed" chr definition and create a runtime chr from it.
 *
 * Chr definitions are stored in a packed format in each stage's setup file.
 * The packed format is used for space saving reasons.
 */
void bodyAllocateChr(s32 stagenum, struct packedchr *packed, s32 cmdindex)
{
	struct pad pad;
	RoomNum rooms[2];
	struct chrdata *chr;
	struct modeldef *headmodeldef;
	struct model *model;
	struct prop *prop;
	s32 bodynum;
	s32 headnum;
	f32 angle;
	s32 index;

	padUnpack(packed->padnum, PADFIELD_POS | PADFIELD_LOOK | PADFIELD_ROOM, &pad);

	rooms[0] = pad.room;
	rooms[1] = -1;

	if (cdTestVolume(&pad.pos, 20, rooms, CDTYPE_ALL, CHECKVERTICAL_YES, 200, -200) == CDRESULT_COLLISION
			&& packed->chair == -1
			&& (packed->spawnflags & SPAWNFLAG_IGNORECOLLISION) == 0) {
		return;
	}

	if (packed->spawnflags & (SPAWNFLAG_ONLYONA | SPAWNFLAG_ONLYONSA | SPAWNFLAG_ONLYONPA)) {
		if ((packed->spawnflags & (SPAWNFLAG_ONLYONA | SPAWNFLAG_ONLYONSA | SPAWNFLAG_ONLYONPA)) == 0) {
			return;
		}

		if (((packed->spawnflags & SPAWNFLAG_ONLYONA) && lvGetDifficulty() == DIFF_A)
				|| ((packed->spawnflags & SPAWNFLAG_ONLYONSA) && lvGetDifficulty() == DIFF_SA)
				|| ((packed->spawnflags & SPAWNFLAG_ONLYONPA) && lvGetDifficulty() == DIFF_PA)) {
			// ok
		} else {
			return;
		}
	}

	headnum = -55555;
	headmodeldef = NULL;

	if (packed->bodynum == 255) {
		bodynum = body0f02d3f8();
	} else {
		bodynum = packed->bodynum;
	}

	if (!g_HeadsAndBodies[bodynum].unk00_01) {
		if (packed->headnum >= 0) {
			headnum = packed->headnum;
		} else if (headnum == -55555) {
			headnum = bodyChooseHead(bodynum);
		}
	}

	if (headnum < 0) {
		index = -1 - headnum;

		if (index >= 0 && index < 22) {
			headmodeldef = func0f18e57c(index, &headnum);
		}

		model = body0f02ce8c(bodynum, headnum, NULL, headmodeldef, false, NULL, false, false);
	} else {
		model = bodyAllocateModel(bodynum, headnum, packed->spawnflags);
	}

	if (model != NULL) {
		angle = atan2f(pad.look.x, pad.look.z);
		prop = chrAllocate(model, &pad.pos, rooms, angle, ailistFindById(packed->ailistnum));

		if (prop != NULL) {
			propActivate(prop);
			propEnable(prop);

			chr = prop->chr;
			chrSetChrnum(chr, packed->chrnum);
			chr->hearingscale = packed->hearscale / 1000.0f;
			chr->visionrange = packed->viewdist;
			chr->padpreset1 = packed->padpreset;
			chr->chrpreset1 = packed->chrpreset;
			chr->headnum = headnum;
			chr->bodynum = bodynum;
			chr->race = bodyGetRace(chr->bodynum);

			chr->rtracked = false;

			if (bodynum == BODY_DRCAROLL) {
				chr->drcarollimage_left = 0;
				chr->drcarollimage_right = 0;
				chr->height = 185;
				chr->radius = 30;
			} else if (bodynum == BODY_CHICROB) {
				chr->unk348[0] = mempAlloc(sizeof(struct fireslotthing), MEMPOOL_STAGE);
				chr->unk348[1] = mempAlloc(sizeof(struct fireslotthing), MEMPOOL_STAGE);
				chr->unk348[0]->beam = mempAlloc(ALIGN16(sizeof(struct beam)), MEMPOOL_STAGE);
				chr->unk348[1]->beam = mempAlloc(ALIGN16(sizeof(struct beam)), MEMPOOL_STAGE);
				chr->unk348[0]->beam->age = -1;
				chr->unk348[1]->beam->age = -1;
				chr->height = 200;
				chr->radius = 42;
			}

			if (packed->spawnflags & SPAWNFLAG_INVINCIBLE) {
				chr->chrflags |= CHRCFLAG_INVINCIBLE;
			}

			if (packed->spawnflags & SPAWNFLAG_BASICGUARD) {
				chr->hidden |= CHRHFLAG_BASICGUARD;
			}

			if (packed->spawnflags & SPAWNFLAG_ANTINONINTERACTABLE) {
				chr->hidden |= CHRHFLAG_ANTINONINTERACTABLE;
			}

			if (packed->spawnflags & SPAWNFLAG_DONTSHOOTME) {
				chr->hidden |= CHRHFLAG_DONTSHOOTME;
			}

			if (packed->spawnflags & SPAWNFLAG_HIDDEN) {
				chr->chrflags |= CHRCFLAG_HIDDEN;
			}

			if (packed->spawnflags & SPAWNFLAG_RTRACKED) {
				chr->rtracked = true;
			}

			if (packed->spawnflags & SPAWNFLAG_NOBLOOD) {
				chr->noblood = true;
			}

			if (packed->spawnflags & SPAWNFLAG_BLUESIGHT) {
				chr->hidden2 |= CHRH2FLAG_BLUESIGHT;
			}

			chr->flags = packed->flags;
			chr->flags2 = packed->flags2;

			if (cheatIsActive(CHEAT_MARQUIS)) {
				chr->flags2 &= ~CHRFLAG1_NOHANDCOMBAT;
				chr->flags2 |= CHRFLAG1_HANDCOMBATONLY;
			}

			chr->team = packed->team;
			chr->squadron = packed->squadron;
			chr->aibot = NULL;

			if (packed->tude != 4) {
				chr->tude = packed->tude;
			} else {
				chr->tude = rngRandom() % 4;
			}

			chr->voicebox = rngRandom() % 3;

			if (!g_HeadsAndBodies[chr->bodynum].ismale) {
				chr->voicebox = VOICEBOX_FEMALE;
			}

			chr->naturalanim = packed->naturalanim;
			chr->myspecial = packed->chair;
			chr->yvisang = packed->yvisang;

			packed->chrindex = chr - g_ChrSlots;

			chr->teamscandist = packed->teamscandist;
			chr->convtalk = packed->convtalk;

			if (chr->flags & CHRFLAG0_CAN_HEARSPAWN) {
				chr->chrflags |= CHRCFLAG_CLONEABLE;
			}

			if (!g_Vars.normmplayerisrunning && g_MissionConfig.iscoop && g_Vars.numaibuddies > 0) {
				chr->flags |= CHRFLAG0_AIVSAI;
			}

			if (rngRandom() % 5 == 0) {
				// Make chr punch slower
				chr->flags2 |= CHRFLAG1_ADJUSTPUNCHSPEED;
			}

			if (CHRRACE(chr) == RACE_SKEDAR) {
				chr->chrflags |= CHRCFLAG_FORCEAUTOAIM;
			}
		}
	}
}

struct prop *bodyAllocateEyespy(struct pad *pad, RoomNum room)
{
	RoomNum rooms[2];
	struct prop *prop;
	struct chrdata *chr;
	struct model *model;
	s32 inlift;
	struct prop *lift;
	f32 ground;

	rooms[0] = room;
	rooms[1] = -1;

#if PIRACYCHECKS
	{
		u32 stack[2];
		u32 checksum = 0;
		s32 *ptr = (s32 *)&lvReset;
		s32 *end = (s32 *)&lvConfigureFade;

		while (ptr < end) {
			checksum <<= 1;
			checksum ^= *ptr;
			ptr++;
		}

		if (checksum != CHECKSUM_PLACEHOLDER) {
			s32 *ptr2 = (s32 *)_memaFree;
			s32 *end2 = (s32 *)memaInit;

			while (ptr2 < end2) {
				ptr2[0] = 0;
				ptr2++;
			}
		}
	}
#endif

	model = bodyAllocateModel(BODY_EYESPY, 0, 0);

	if (model) {
		prop = chrAllocate(model, &pad->pos, rooms, 0, ailistFindById(GAILIST_IDLE));

		if (prop) {
			propActivate(prop);
			propEnable(prop);
			chr = prop->chr;
			chrSetChrnum(chr, chrsGetNextUnusedChrnum());
			chr->bodynum = BODY_EYESPY;
			chr->padpreset1 = 0;
			chr->chrpreset1 = 0;
			chr->headnum = 0;
			chr->hearingscale = 0;
			chr->visionrange = 0;
			chr->race = bodyGetRace(chr->bodynum);

			ground = cdFindGroundInfoAtCyl(&pad->pos, 30, rooms, NULL, NULL, NULL, NULL, &inlift, &lift);
			chr->ground = ground;
			chr->manground = ground;

			chr->flags = 0;
			chr->flags2 = 0;
			chr->team = 0;
			chr->squadron = 0;
			chr->maxdamage = 2;
			chr->tude = rngRandom() & 3;
			chr->voicebox = rngRandom() % 3;
			chr->naturalanim = 0;
			chr->myspecial = 0;
			chr->yvisang = 0;
			chr->teamscandist = 0;
			chr->convtalk = 0;
			chr->radius = 26;
			chr->height = 200;
			func0f02e9a0(chr, 0);
			chr->chrflags |= CHRCFLAG_HIDDEN;

#if VERSION >= VERSION_NTSC_1_0
			chr->hidden2 |= CHRH2FLAG_CONSIDERPROXIES;
#else
			chr->hidden |= CHRHFLAG_CONSIDERPROXIES;
#endif

			return prop;
		}
	}

	return NULL;
}

void body0f02ddbf(void)
{
	// empty
}

/**
 * Tweak the head's Y offset to suit the body.
 *
 * By default, heads and their matching bodies align perfectly and don't need
 * any tweaking. This function is used in multiplayer where players can put any
 * heads on any bodies.
 */
void bodyCalculateHeadOffset(struct modeldef *headmodeldef, s32 headnum, s32 bodynum)
{
	struct modelnode *node;
	struct modelnode *prev;
	Gfx *gdl;
	s32 offset;
	struct modelrodata_bbox *bbox;
	s32 i;

#if VERSION >= VERSION_JPN_FINAL
	offset = 0;

	switch (headnum) {
	case HEAD_DARK_COMBAT:
	case HEAD_DARK_FROCK:
	case HEAD_DARKAQUA:
	case HEAD_DARK_SNOW:
		switch (bodynum) {
		case BODY_DARK_COMBAT:
		case BODY_DARK_FROCK:
		case BODY_DARK_TRENCH:
		case BODY_DARK_RIPPED:
		case BODY_DARK_AF1:
		case BODY_DARKWET:
		case BODY_DARKAQUALUNG:
		case BODY_DARKSNOW:
		case BODY_DARKLAB:
		case BODY_DARK_LEATHER:
		case BODY_DARK_NEGOTIATOR:
			break;
		default:
			offset = -12;
			break;
		}
		break;
	}
#endif

	if ((s16)(*(s32 *)&headmodeldef->skel) == SKEL_HEAD) {
#if VERSION >= VERSION_JPN_FINAL
		if (g_HeadsAndBodies[headnum].type == g_HeadsAndBodies[bodynum].type && offset == 0) {
			return;
		}
#else
		if (g_HeadsAndBodies[headnum].type == g_HeadsAndBodies[bodynum].type) {
			return;
		}
#endif

#if VERSION >= VERSION_JPN_FINAL
		switch (g_HeadsAndBodies[headnum].type) {
		default:
		case HEADBODYTYPE_FEMALE:
			offset += 0;
			break;
		case HEADBODYTYPE_MAIAN:
			offset += 0;
			break;
		case HEADBODYTYPE_DEFAULT:
			offset -= 35;
			break;
		case HEADBODYTYPE_MRBLONDE:
			offset += 0;
			break;
		case HEADBODYTYPE_CASS:
			offset -= 20;
			break;
		case HEADBODYTYPE_FEMALEGUARD:
			offset -= 40;
			break;
		}
#else
		// Same as JPN, but sets the value rather than adjusts
		switch (g_HeadsAndBodies[headnum].type) {
		default:
		case HEADBODYTYPE_FEMALE:
			offset = 0;
			break;
		case HEADBODYTYPE_MAIAN:
			offset = 0;
			break;
		case HEADBODYTYPE_DEFAULT:
			offset = -35;
			break;
		case HEADBODYTYPE_MRBLONDE:
			offset = 0;
			break;
		case HEADBODYTYPE_CASS:
			offset = -20;
			break;
		case HEADBODYTYPE_FEMALEGUARD:
			offset = -40;
			break;
		}
#endif

		switch (g_HeadsAndBodies[bodynum].type) {
		case HEADBODYTYPE_FEMALE:
			break;
		case HEADBODYTYPE_MAIAN:
			offset -= 30;
			break;
		case HEADBODYTYPE_DEFAULT:
			offset += 35;
			break;
		case HEADBODYTYPE_MRBLONDE:
			break;
		case HEADBODYTYPE_CASS:
			offset += 20;
			break;
		case HEADBODYTYPE_FEMALEGUARD:
			offset += 40;
			break;
		}

		if (g_HeadsAndBodies[bodynum].type == HEADBODYTYPE_FEMALE) {
			if (g_HeadsAndBodies[headnum].type == HEADBODYTYPE_DEFAULT
					|| g_HeadsAndBodies[headnum].type == HEADBODYTYPE_MRBLONDE) {
				offset -= 10;
			} else if (g_HeadsAndBodies[headnum].type == HEADBODYTYPE_CASS
					|| g_HeadsAndBodies[headnum].type == HEADBODYTYPE_FEMALEGUARD) {
				offset -= 5;
			}
		} else if (g_HeadsAndBodies[bodynum].type == HEADBODYTYPE_CASS
				&& (g_HeadsAndBodies[headnum].type == HEADBODYTYPE_DEFAULT
					|| g_HeadsAndBodies[headnum].type == HEADBODYTYPE_MRBLONDE)) {
			offset -= 5;
		}

		// Apply the offset
		if (offset != 0) {
			node = NULL;

			do {
				prev = node;

				modelIterateDisplayLists(headmodeldef, &node, &gdl);

				if (node && node != prev && node->type == MODELNODETYPE_DL) {
					struct modelrodata_dl *rodata = &node->rodata->dl;

					for (i = 0; i < rodata->numvertices; i++) {
						rodata->vertices[i].y += offset;
					}
				}
			} while (node);

			bbox = modeldefFindBboxRodata(headmodeldef);

			if (bbox != NULL) {
				bbox->ymin += offset;
				bbox->ymax += offset;
			}
		}
	}
}
