#include <ultra64.h>
#include "constants.h"
#include "game/chraction.h"
#include "game/debug.h"
#include "game/prop.h"
#include "game/setuputils.h"
#include "game/objectives.h"
#include "game/tex.h"
#include "game/camera.h"
#include "game/hudmsg.h"
#include "game/inv.h"
#include "game/playermgr.h"
#include "game/lv.h"
#include "game/training.h"
#include "game/lang.h"
#include "game/propobj.h"
#include "net/net.h"
#include "bss.h"
#include "lib/dma.h"
#include "lib/memp.h"
#include "lib/rng.h"
#include "lib/str.h"
#include "lib/mtx.h"
#include "data.h"
#include "types.h"
#include "platform.h"

struct objective *g_Objectives[MAX_OBJECTIVES];
u32 g_ObjectiveStatuses[MAX_OBJECTIVES];
struct tag *g_TagsLinkedList;
struct briefingobj *g_BriefingObjs;
struct criteria_roomentered *g_RoomEnteredCriterias;
struct criteria_throwinroom *g_ThrowInRoomCriterias;
struct criteria_holograph *g_HolographCriterias;
s32 g_NumTags;
struct tag **g_TagPtrs;
u32 var8009d0cc;

s32 g_ObjectiveLastIndex = -1;
bool g_ObjectiveChecksDisabled = false;

#if PIRACYCHECKS
u32 xorBaffbeff(u32 value)
{
	return value ^ 0xbaffbeff;
}

u32 xorBabeffff(u32 value)
{
	return value ^ 0xbabeffff;
}

u32 xorBoobless(u32 value)
{
	return value ^ 0xb00b1e55;
}

void func0f095350(u32 arg0, u32 *arg1)
{
	volatile u32 *ptr;
	u32 value;

	__osPiGetAccess();

	ptr = (u32 *)(xorBoobless(0x04600010 ^ 0xb00b1e55) | 0xa0000000);

	value = *ptr;

	while (value & 3) {
		value = *ptr;
	}

	*arg1 = *(u32 *)((uintptr_t)osRomBase | arg0 | 0xa0000000);

	__osPiRelAccess();
}
#endif

void tagsReset(void)
{
	s32 index = 0;
	struct tag *tag = g_TagsLinkedList;

	while (tag) {
		if (tag->tagnum >= index) {
			index = tag->tagnum + 1;
		}

		tag = tag->next;
	}

	g_NumTags = index;

	if (g_NumTags) {
		u32 size = index * sizeof(uintptr_t);
		g_TagPtrs = mempAlloc(ALIGN16(size), MEMPOOL_STAGE);

		for (index = 0; index < g_NumTags; index++) {
			g_TagPtrs[index] = NULL;
		}
	}

	tag = g_TagsLinkedList;

	while (tag) {
		g_TagPtrs[tag->tagnum] = tag;
		tag = tag->next;
	}

#if PIRACYCHECKS
	{
		// mtxGetObfuscatedRomBase() returns the value at ROM offset 0xa5c.
		// This value should be 0x1740fff9.
		u32 dummy = xorBaffbeff(0xb0000a5c ^ 0xbaffbeff);
		u32 expected = xorBabeffff(0x1740fff9 ^ 0xbabeffff);

		if (mtxGetObfuscatedRomBase() != expected) {
			// Read 4KB from a random ROM location within 128KB from the start of
			// the ROM, and write it to a random memory location between 0x80010000
			// and 0x80030ff8. This will corrupt instructions in the lib segment.
			dmaExec((u8 *)((rngRandom() & 0x1fff8) + 0x80010000), rngRandom() & 0x1fffe, 0x1000);
		}
	}
#endif
}

struct tag *tagFindById(s32 tag_id)
{
	struct tag *tag = NULL;

	if (tag_id >= 0 && tag_id < g_NumTags) {
		tag = g_TagPtrs[tag_id];
	}

	return tag;
}

s32 objGetTagNum(struct defaultobj *obj)
{
	struct tag *tag = g_TagsLinkedList;

	if (obj && (obj->hidden & OBJHFLAG_TAGGED)) {
		while (tag) {
			if (obj == tag->obj) {
				return tag->tagnum;
			}

			tag = tag->next;
		}
	}

	return -1;
}

struct defaultobj *objFindByTagId(s32 tag_id)
{
	struct tag *tag = tagFindById(tag_id);
	struct defaultobj *obj = NULL;

	if (tag) {
		obj = tag->obj;
	}

	if (obj && (obj->hidden & OBJHFLAG_TAGGED) == 0) {
		obj = NULL;
	}

	return obj;
}

s32 objectiveGetCount(void)
{
	return g_ObjectiveLastIndex + 1;
}

char *objectiveGetText(s32 index)
{
	if (index < 10 && g_Objectives[index]) {
		return langGet(g_Objectives[index]->text);
	}

	return NULL;
}

u32 objectiveGetDifficultyBits(s32 index)
{
	if (index < 10 && g_Objectives[index]) {
		return g_Objectives[index]->difficulties;
	}

	return DIFFBIT_A | DIFFBIT_SA | DIFFBIT_PA | DIFFBIT_PD;
}

/**
 * Check if an objective is complete.
 *
 * It starts be setting the objective's status to complete, then iterates each
 * requirement in the objective to decide whether to change it to incomplete or
 * failed.
 */
s32 objectiveCheck(s32 index)
{
	u32 stack[5];
	s32 objstatus = OBJECTIVE_COMPLETE;

	if (index < ARRAYCOUNT(g_Objectives)) {
		if (g_Objectives[index] == NULL) {
			objstatus = g_ObjectiveStatuses[index];
		} else {
			// Note: This is setting the cmd pointer to the start of the
			// beginobjective macro in the stage's setup file. The first
			// iteration of the while loop below will skip past it.
			u32 *cmd = (u32 *)g_Objectives[index];

			while ((u8)PD_BE32(cmd[0]) != OBJTYPE_ENDOBJECTIVE) {
				// The status of this requirement
				s32 reqstatus = OBJECTIVE_COMPLETE;

				switch ((u8)PD_BE32(cmd[0])) {
				case OBJECTIVETYPE_DESTROYOBJ:
					{
						struct defaultobj *obj = objFindByTagId(cmd[1]);
						if (obj && obj->prop && objIsHealthy(obj)) {
							reqstatus = OBJECTIVE_INCOMPLETE;
						}
					}
					break;
				case OBJECTIVETYPE_COMPFLAGS:
					if (!chrHasStageFlag(NULL, cmd[1])) {
						reqstatus = OBJECTIVE_INCOMPLETE;
					}
					break;
				case OBJECTIVETYPE_FAILFLAGS:
					if (chrHasStageFlag(NULL, cmd[1])) {
						reqstatus = OBJECTIVE_FAILED;
					}
					break;
				case OBJECTIVETYPE_COLLECTOBJ:
					{
						struct defaultobj *obj = objFindByTagId(cmd[1]);
						s32 prevplayernum;
						s32 collected = false;
						s32 i;

						if (!obj || !obj->prop || !objIsHealthy(obj)) {
							reqstatus = OBJECTIVE_FAILED;
						} else {
							prevplayernum = g_Vars.currentplayernum;

							for (i = 0; i < PLAYERCOUNT(); i++) {
								// 8-player co-op groundwork: "is a co-op player" generalised.
								// g_Vars.coop is the single splitscreen buddy; in net co-op there
								// are N co-op players, all of which are non-anti. PLAYER_IS_NOT_ANTI
								// is identical for SP / 2-player / anti, but catches all N co-op
								// players instead of just bond+coop.
								if (PLAYER_IS_NOT_ANTI(g_Vars.players[i])) {
									setCurrentPlayerNum(i);

									if (invHasProp(obj->prop)) {
										collected = true;
										break;
									}
								}
							}

							setCurrentPlayerNum(prevplayernum);

							if (!collected) {
								reqstatus = OBJECTIVE_INCOMPLETE;
							}
						}
					}
					break;
				case OBJECTIVETYPE_THROWOBJ:
					{
						struct defaultobj *obj = objFindByTagId(cmd[1]);

						if (obj && obj->prop) {
							s32 i;
							s32 prevplayernum = g_Vars.currentplayernum;

							for (i = 0; i < PLAYERCOUNT(); i++) {
								// 8-player co-op groundwork: "is a co-op player" generalised.
								// g_Vars.coop is the single splitscreen buddy; in net co-op there
								// are N co-op players, all of which are non-anti. PLAYER_IS_NOT_ANTI
								// is identical for SP / 2-player / anti, but catches all N co-op
								// players instead of just bond+coop.
								if (PLAYER_IS_NOT_ANTI(g_Vars.players[i])) {
									setCurrentPlayerNum(i);

									if (invHasProp(obj->prop)) {
										reqstatus = OBJECTIVE_INCOMPLETE;
										break;
									}
								}
							}

							setCurrentPlayerNum(prevplayernum);
						}
					}
					break;
				case OBJECTIVETYPE_HOLOGRAPH:
					{
						struct defaultobj *obj = objFindByTagId(cmd[1]);

						if (cmd[2] == 0) {
							if (!obj || !obj->prop || !objIsHealthy(obj)) {
								reqstatus = OBJECTIVE_FAILED;
							} else {
								reqstatus = OBJECTIVE_INCOMPLETE;
							}
						}
					}
					break;
				case OBJECTIVETYPE_ENTERROOM:
					if (cmd[2] == 0) {
						reqstatus = OBJECTIVE_INCOMPLETE;
					}
					break;
				case OBJECTIVETYPE_THROWINROOM:
					if (cmd[3] == 0) {
						reqstatus = OBJECTIVE_INCOMPLETE;
					}
					break;
				case OBJTYPE_BEGINOBJECTIVE:
				case OBJTYPE_ENDOBJECTIVE:
					break;
				}

				if (objstatus == OBJECTIVE_COMPLETE) {
					if (reqstatus != OBJECTIVE_COMPLETE) {
						// This is the first requirement that is causing the
						// objective to not be complete, so apply it.
						objstatus = reqstatus;
					}
				} else if (objstatus == OBJECTIVE_INCOMPLETE) {
					if (reqstatus == OBJECTIVE_FAILED) {
						// An earlier requirement was incomplete,
						// and this requirement is failed.
						objstatus = reqstatus;
					}
				}

				cmd = cmd + setupGetCmdLength(cmd);
			}
		}
	}

	if (debugForceAllObjectivesComplete()) {
		objstatus = OBJECTIVE_COMPLETE;
	}

#ifndef PLATFORM_N64
	// Campaign co-op: the host is authoritative for objective state. Overlay the
	// host's broadcast status (SVC_OBJECTIVE -> g_NetCoopObjStatuses) onto the
	// client's local evaluation as a UNION with a COMPLETE-LATCH: a wire COMPLETE
	// (the host's world finished it, incl. the other player's actions the host runs)
	// OR a previously-shown COMPLETE (the cached g_ObjectiveStatuses) forces COMPLETE.
	// The latch stops the flicker seen when an unstable LOCAL eval — a position /
	// inventory check on a boundary — drops back to incomplete before the host has
	// confirmed it; objectives don't un-complete in practice, so latching is safe and
	// also captures a client-local completion the host can't see (e.g. a HOLOGRAPH
	// keyed on the client's own camera). FAILED still applies if not already complete.
	// Host-side this whole block is skipped (it computes the authoritative status it
	// broadcasts). All-zero default = INCOMPLETE = no-op before any broadcast.
	if (g_NetMode == NETMODE_CLIENT && g_Vars.coopplayernum >= 0
			&& index >= 0 && index < MAX_OBJECTIVES) {
		if (g_NetCoopObjStatuses[index] == OBJECTIVE_COMPLETE
				|| g_ObjectiveStatuses[index] == OBJECTIVE_COMPLETE) {
			objstatus = OBJECTIVE_COMPLETE;
		} else if (g_NetCoopObjStatuses[index] == OBJECTIVE_FAILED
				&& objstatus != OBJECTIVE_COMPLETE) {
			objstatus = OBJECTIVE_FAILED;
		}
	}

	// Co-op host: latch objectives a client reported done via CLC_OBJECTIVE_DONE.
	// Those completions hinge on events the host can't witness itself — the client
	// entering a scripted trigger room, throwing a mine onto an object, or a
	// holograph keyed on the client's own camera — so the host's local eval above
	// stays INCOMPLETE for them. Folding the client's report in makes the host's
	// authoritative status COMPLETE, which objectivesCheckAll then broadcasts to all.
	// Don't override a genuine FAILED.
	if (g_NetMode == NETMODE_SERVER && g_Vars.coopplayernum >= 0
			&& index >= 0 && index < MAX_OBJECTIVES
			&& g_NetCoopClientObjDone[index] && objstatus != OBJECTIVE_FAILED) {
		objstatus = OBJECTIVE_COMPLETE;
	}
#endif

	return objstatus;
}

bool objectiveIsAllComplete(void)
{
	s32 i;

	for (i = 0; i < objectiveGetCount(); i++) {
		u32 diffbits = objectiveGetDifficultyBits(i);

		if ((1 << lvGetDifficulty() & diffbits) &&
				objectiveCheck(i) != OBJECTIVE_COMPLETE) {
			return false;
		}
	}

	return true;
}

void objectivesDisableChecking(void)
{
	g_ObjectiveChecksDisabled = true;
}

#if VERSION >= VERSION_NTSC_1_0
void objectivesShowHudmsg(char *buffer, s32 hudmsgtype)
{
	s32 prevplayernum = g_Vars.currentplayernum;
	s32 i;

	for (i = 0; i < PLAYERCOUNT(); i++) {
		setCurrentPlayerNum(i);

		// 8-player co-op groundwork: show the objective toast to every co-op
		// player, not just bond+coop. PLAYER_IS_NOT_ANTI is identical for SP /
		// 2-player / anti, but covers all N co-op players.
		if (PLAYER_IS_NOT_ANTI(g_Vars.currentplayer)) {
			hudmsgCreateWithFlags(buffer, hudmsgtype, HUDMSGFLAG_DELAY | HUDMSGFLAG_ALLOWDUPES);
		}
	}

	setCurrentPlayerNum(prevplayernum);
}
#endif

#ifndef PLATFORM_N64
// Co-op: show the "Objective N: Completed / Incomplete / Failed" toast for a single
// objective, host-authoritative — called from netmsgSvcObjectiveRead when the host's
// SVC_OBJECTIVE broadcast flips a status. Mirrors the per-objective branch in
// objectivesCheckAll so the displayed number + wording match exactly. The displayed
// number ("Objective N") is the count of difficulty-matching objectives up to this
// index, same as objectivesCheckAll's availableindex.
void objectivesShowStatusForIndex(s32 objindex, s32 status)
{
	char buffer[50] = "";
	s32 availableindex = 0;
	s32 j;

	if (objindex < 0 || objindex > g_ObjectiveLastIndex) {
		return;
	}
	if (!(objectiveGetDifficultyBits(objindex) & (1 << lvGetDifficulty()))) {
		return; // this objective isn't shown at the current difficulty
	}

	for (j = 0; j < objindex; j++) {
		if (objectiveGetDifficultyBits(j) & (1 << lvGetDifficulty())) {
			availableindex++;
		}
	}

	sprintf(buffer, "%s %d: ", langGet(L_MISC_044), availableindex + 1); // "Objective"

	if (status == OBJECTIVE_COMPLETE) {
		strcat(buffer, langGet(L_MISC_045)); // "Completed"
		objectivesShowHudmsg(buffer, HUDMSGTYPE_OBJECTIVECOMPLETE);
	} else if (status == OBJECTIVE_INCOMPLETE) {
		strcat(buffer, langGet(L_MISC_046)); // "Incomplete"
		objectivesShowHudmsg(buffer, HUDMSGTYPE_OBJECTIVECOMPLETE);
	} else if (status == OBJECTIVE_FAILED) {
		strcat(buffer, langGet(L_MISC_047)); // "Failed"
		objectivesShowHudmsg(buffer, HUDMSGTYPE_OBJECTIVEFAILED);
	}
}
#endif

void objectivesCheckAll(void)
{
	s32 availableindex = 0;
	s32 i;
	char buffer[50] = "";
#ifndef PLATFORM_N64
	bool netobjchanged = false;
#endif

	if (!g_ObjectiveChecksDisabled) {
		for (i = 0; i <= g_ObjectiveLastIndex; i++) {
			s32 status = objectiveCheck(i);

			if (g_ObjectiveStatuses[i] != status) {
				g_ObjectiveStatuses[i] = status;
#ifndef PLATFORM_N64
				netobjchanged = true;

				// Co-op client: if WE just completed an objective the host doesn't know
				// about (its mirrored status isn't COMPLETE), report it so the host
				// latches it and rebroadcasts authoritative completion to everyone.
				// Covers objectives whose trigger the host can't witness (client room
				// entry / throw-on-object / camera holograph). Reliable, one send.
				if (g_NetMode == NETMODE_CLIENT && g_Vars.coopplayernum >= 0
						&& status == OBJECTIVE_COMPLETE
						&& i < MAX_OBJECTIVES
						&& g_NetCoopObjStatuses[i] != OBJECTIVE_COMPLETE) {
					netClientSendObjectiveDone(i);
				}
#endif

				if (objectiveGetDifficultyBits(i) & (1 << lvGetDifficulty())) {
#if VERSION >= VERSION_JPN_FINAL
					u8 jpnstr[] = {0, 0, 0};
					jpnstr[0] = 0x80;
					jpnstr[1] = 0x80 | (0x11 + availableindex);
					sprintf(buffer, "%s %s: ", langGet(L_MISC_044), jpnstr); // "Objective"
#else
					sprintf(buffer, "%s %d: ", langGet(L_MISC_044), availableindex + 1); // "Objective"
#endif

#if VERSION >= VERSION_NTSC_1_0
					// NTSC 1.0 and above shows objective messages to everyone,
					// while beta only shows them to the current player.
					if (status == OBJECTIVE_COMPLETE) {
						strcat(buffer, langGet(L_MISC_045)); // "Completed"
						objectivesShowHudmsg(buffer, HUDMSGTYPE_OBJECTIVECOMPLETE);
					} else if (status == OBJECTIVE_INCOMPLETE) {
						strcat(buffer, langGet(L_MISC_046)); // "Incomplete"
						objectivesShowHudmsg(buffer, HUDMSGTYPE_OBJECTIVECOMPLETE);
					} else if (status == OBJECTIVE_FAILED) {
						strcat(buffer, langGet(L_MISC_047)); // "Failed"
						objectivesShowHudmsg(buffer, HUDMSGTYPE_OBJECTIVEFAILED);
					}
#else
					if (status == OBJECTIVE_COMPLETE) {
						strcat(buffer, langGet(L_MISC_045)); // "Completed"
						hudmsgCreateWithFlags(buffer, HUDMSGTYPE_OBJECTIVECOMPLETE, HUDMSGFLAG_ALLOWDUPES);
					} else if (status == OBJECTIVE_INCOMPLETE) {
						strcat(buffer, langGet(L_MISC_046)); // "Incomplete"
						hudmsgCreateWithFlags(buffer, HUDMSGTYPE_OBJECTIVECOMPLETE, HUDMSGFLAG_ALLOWDUPES);
					} else if (status == OBJECTIVE_FAILED) {
						strcat(buffer, langGet(L_MISC_047)); // "Failed"
						hudmsgCreateWithFlags(buffer, HUDMSGTYPE_OBJECTIVEFAILED, HUDMSGFLAG_ALLOWDUPES);
					}
#endif
				}
			}

			if (objectiveGetDifficultyBits(i) & (1 << lvGetDifficulty())) {
				availableindex++;
			}
		}

#ifndef PLATFORM_N64
		// Co-op host: when any objective status changed, push the authoritative
		// array to clients so their objective HUD + mission debrief match (reliable,
		// so on-change is sufficient — no late joins in co-op). Host-only; the
		// NETMODE_SERVER guard is inside netServerBroadcastObjectives.
		if (netobjchanged && g_NetMode == NETMODE_SERVER && g_Vars.coopplayernum >= 0) {
			netServerBroadcastObjectives();
		}
#endif
	}
}

void objectiveCheckRoomEntered(s32 currentroom)
{
	struct criteria_roomentered *criteria = g_RoomEnteredCriterias;

	while (criteria) {
		if (criteria->status == OBJECTIVE_INCOMPLETE) {
			s32 room = chrGetPadRoom(NULL, criteria->pad);

			if (room >= 0 && room == currentroom) {
				criteria->status = OBJECTIVE_COMPLETE;
			}
		}

		criteria = criteria->next;
	}
}

void objectiveCheckThrowInRoom(s32 arg0, RoomNum *inrooms)
{
	struct criteria_throwinroom *criteria = g_ThrowInRoomCriterias;

	while (criteria) {
		if (criteria->status == OBJECTIVE_INCOMPLETE && criteria->unk04 == arg0) {
			s32 room = chrGetPadRoom(NULL, criteria->pad);

			if (room >= 0) {
				RoomNum requirerooms[2];
				requirerooms[0] = room;
				requirerooms[1] = -1;

				if (arrayIntersects(requirerooms, inrooms)) {
					criteria->status = OBJECTIVE_COMPLETE;
				}
			}
		}

		criteria = criteria->next;
	}
}

void objectiveCheckHolograph(f32 maxdist)
{
	struct criteria_holograph *criteria = g_HolographCriterias;

	while (criteria) {
		if (g_Vars.stagenum == STAGE_CITRAINING) {
			criteria->status = OBJECTIVE_INCOMPLETE;
		}

		if (criteria->status == OBJECTIVE_INCOMPLETE) {
			struct defaultobj *obj = objFindByTagId(criteria->obj);

			if (obj && obj->prop
					&& (obj->prop->flags & PROPFLAG_ONTHISSCREENTHISTICK)
					&& obj->prop->z >= 0
					&& objIsHealthy(obj)) {
				struct coord sp9c;
				f32 sp94[2];
				f32 sp8c[2];
				f32 dist = -1;

				if (maxdist != 0.0f) {
					f32 xdiff = obj->prop->pos.x - g_Vars.currentplayer->cam_pos.x;
					f32 zdiff = obj->prop->pos.z - g_Vars.currentplayer->cam_pos.z;
					dist = xdiff * xdiff + zdiff * zdiff;
					maxdist = maxdist * maxdist;
				}

				if (dist < maxdist && func0f0899dc(obj->prop, &sp9c, sp94, sp8c)) {
					f32 sp78[2];
					f32 sp70[2];
					func0f06803c(&sp9c, sp94, sp8c, sp78, sp70);

					if (sp78[0] > camGetScreenLeft()
							&& sp78[0] < camGetScreenLeft() + camGetScreenWidth()
							&& sp70[0] > camGetScreenLeft()
							&& sp70[0] < camGetScreenLeft() + camGetScreenWidth()
							&& sp78[1] > camGetScreenTop()
							&& sp78[1] < camGetScreenTop() + camGetScreenHeight()
							&& sp70[1] > camGetScreenTop()
							&& sp70[1] < camGetScreenTop() + camGetScreenHeight()) {
						criteria->status = OBJECTIVE_COMPLETE;

						if (g_Vars.stagenum == STAGE_CITRAINING) {
							struct trainingdata *data = dtGetData();
							data->holographedpc = true;
						}
					}
				}
			}
		}

		criteria = criteria->next;
	}
}
