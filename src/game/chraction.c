#include <ultra64.h>
#include "constants.h"
#include "game/acosfasinf.h"
#include "game/atan2f.h"
#include "game/bg.h"
#include "game/body.h"
#include "game/bondgun.h"
#include "game/cheats.h"
#include "game/bot.h"
#include "game/botact.h"
#include "game/botcmd.h"
#include "game/botinv.h"
#include "game/camera.h"
#include "game/chr.h"
#include "game/chraction.h"
#include "game/chrai.h"
#include "lib/memp.h"
#include "game/luaai.h"
#include "game/debug.h"
#include "game/dlights.h"
#include "game/env.h"
#include "game/explosions.h"
#include "game/footstep.h"
#include "game/game_006900.h"
#include "game/game_0b0fd0.h"
#include "game/game_1531a0.h"
#include "game/gunfx.h"
#include "game/inv.h"
#include "game/lv.h"
#include "game/modelmgr.h"
#include "game/mplayer/mplayer.h"
#include "game/music.h"
#include "game/nbomb.h"
#include "game/setup.h"
#include "game/setuputils.h"
#include "game/mpstats.h"
#include "game/objectives.h"
#include "game/options.h"
#include "game/pad.h"
#include "game/padhalllv.h"
#include "game/pak.h"
#include "game/pdmode.h"
#include "game/player.h"
#include "game/playermgr.h"
#include "game/prop.h"
#include "game/propobj.h"
#include "game/propsnd.h"
#include "game/sparks.h"
#include "game/stagetable.h"
#include "game/tex.h"
#include "game/wallhit.h"
#include "game/weather.h"
#include "bss.h"
#include "lib/joy.h"
#include "lib/lib_17ce0.h"
#include "lib/main.h"
#include "lib/model.h"
#include "lib/snd.h"
#include "lib/rng.h"
#include "lib/mtx.h"
#include "lib/ailist.h"
#include "lib/anim.h"
#include "lib/collision.h"
#include "lib/vi.h"
#include "data.h"
#include "types.h"
#ifndef PLATFORM_N64
#include "net/net.h"
#include "net/netmsg.h"
#include "net/netprop.h"
#endif

#ifndef PLATFORM_N64
// Chaos "one punch" (docs/PORT_CHAOS.md, pd.one_punch): while set, a player's
// unarmed strike in chrDamage is lethal + launches the victim. Defined here
// (not in the pd helper block below) because chrDamage reads it first.
s32 g_ChaosOnePunch = 0;
// Chaos "Space Program" (pd.space_program): like one_punch but for GUN shots —
// every player bullet is a one-hit kill through armour and launches the victim
// with massive knockback. Read in chrDamage next to one_punch.
s32 g_ChaosSpaceProgram = 0;
// Chaos "Frag Out" (pd.frag_out): human enemies lob a grenade whenever they
// would fire — chrConsiderGrenadeThrow skips its probability/range gates.
s32 g_ChaosFragOut = 0;
// Chaos "Paintball" (pd.damage_scale): multiplies every chrDamage amount.
// 1.0 = off. Applied in chrDamage before the net broadcast.
f32 g_ChaosDamageScale = 1.0f;
// Chaos "No drops" (pd.no_drops): dead chrs keep their weapons in hand
// instead of dropping them (gate in chrBeginDeath's drop-items block).
s32 g_ChaosNoDrops = 0;
#endif

s32 g_RecentQuipsPlayed[5];
u32 var8009cd84;
u32 var8009cd88;
u32 var8009cd8c;
u32 var8009cd90;
u32 var8009cd94;
u8 g_RecentQuipsIndex;

f32 g_EnemyAccuracyScale = 1;
f32 g_PlayerDamageRxScale = 1;
f32 g_PlayerDamageTxScale = 1;
f32 g_AttackWalkDurationScale = 1;

#if VERSION >= VERSION_NTSC_1_0
s32 g_NumChrsWithPlayerTarget = 0;
s32 g_NumChrsSeenPlayer = 0;
s32 g_NumChrsSeenPlayerRecently = 0;
#endif

s32 g_NumChrsSeenPlayerRecently2 = 0;

#if VERSION < VERSION_NTSC_1_0
const char *g_ChrActionNames[] = {
	"ACT_INIT",
	"ACT_STAND",
	"ACT_KNEEL",
	"ACT_ANIM",
	"ACT_DIE",
	"ACT_DEAD",
	"ACT_ARGH",
	"ACT_PREARGH",
	"ACT_ATTACK",
	"ACT_ATTACKWALK",
	"ACT_ATTACKROLL",
	"ACT_SIDESTEP",
	"ACT_JUMPOUT",
	"ACT_RUNPOS",
	"ACT_PATROL",
	"ACT_GOPOS",
	"ACT_SURRENDER",
	"ACT_LOOKATTARGET",
	"ACT_SURPRISED",
	"ACT_STARTALARM",
	"ACT_THROWGRENADE",
	"ACT_TURNDIR",
	"ACT_TEST",
	"ACT_BONDINTRO",
	"ACT_BONDDIE",
	"ACT_BONDMULTI",
	"ACT_NULL",
	"ACT_BOT_ATTACKSTAND",
	"ACT_BOT_ATTACKKNEEL",
	"ACT_BOT_ATTACKSTRAFE",
	"ACT_DRUGGEDDROP",
	"ACT_DRUGGEDKO",
	"ACT_DRUGGEDCOMINGUP",
	"ACT_ATTACKAMOUNT",
	"ACT_ROBOTATTACK",
	"ACT_SKJUMP",
	"ACT_PUNCH",
	"ACT_CUTFIRE",
};

const char *g_ChrMyActionNames[] = {
	"NONE",
	"NORMAL",
	"COVERWAIT",
	"GRENADEWAIT",
	"WAITING",
	"COVERGOTO",
	"COVERBREAK",
	"COVERSEEN",
	"FLANKLEFT",
	"FLANKRIGHT",
	"DODGE",
	"GRENADE",
	"WAITSEEN",
	"WITHDRAW",
	"SHOOTING",
	"SYNCSHOOT",
	"WAITTIMEOUT",
	"COVERTIMEOUT",
	"TRACKING",
	"RETREAT",
	"SURRENDER",
	"TALKING",
	"LISTENING",
	"GOTOALARM",
	"BOTFRIENDFOLLOW",
	"BOTHIDE",
	"BOTPATH",
	"BOTINJURED",
	"BOTNORMAL",
	"BOTSHOOTING",
	"DRUGGED",
	"PANIC",
	"RUNFROMGRENADE",
	"UNARMEDATTACK",
	"MA_SKJUMP_START",
	"MA_SKJUMP_AIR",
	"MA_SKJUMP_LAND",
	"MA_SKJUMP_LANDLOOP",
	"MA_SKJUMP_SHOT",
	"AIBOTDEADLIST",
	"AIBOTINIT",
	"AIBOTMAINLOOP",
	"AIBOTGETITEM",
	"AIBOTGOTOPOS",
	"AIBOTGOTOPROP",
	"AIBOTRUNAWAY",
	"AIBOTDOWNLOAD",
	"AIBOTATTACK",
	"unused1",
	"unused2",
	"AIBOTFOLLOW",
	"AIBOTDEFEND",
	"FLANKBEST",
	"FACING",
	"PUNCHING",
};

const char *g_ChrActionNames2[] = {
	"ACT_INIT",
	"ACT_STAND",
	"ACT_KNEEL",
	"ACT_ANIM",
	"ACT_DIE",
	"ACT_DEAD",
	"ACT_ARGH",
	"ACT_PREARGH",
	"ACT_ATTACK",
	"ACT_ATTACKWALK",
	"ACT_ATTACKROLL",
	"ACT_SIDESTEP",
	"ACT_JUMPOUT",
	"ACT_RUNPOS",
	"ACT_PATROL",
	"ACT_GOPOS",
	"ACT_SURRENDER",
	"ACT_LOOKATTARGET",
	"ACT_SURPRISED",
	"ACT_STARTALARM",
	"ACT_THROWGRENADE",
	"ACT_TURNDIR",
	"ACT_TEST",
	"ACT_BONDINTRO",
	"ACT_BONDDIE",
	"ACT_BONDMULTI",
	"ACT_NULL",
	"ACT_BOT_ATTACKSTAND",
	"ACT_BOT_ATTACKKNEEL",
	"ACT_BOT_ATTACKSTRAFE",
	"ACT_DRUGGEDDROP",
	"ACT_DRUGGEDKO",
	"ACT_DRUGGEDCOMINGUP",
	"ACT_ATTACKAMOUNT",
	"ACT_ROBOTATTACK",
	"ACT_SKJUMP",
};

const char *g_ChrLiftActionNames[] = {
	"NOTUSINGLIFT",
	"WAITINGFORLIFT",
	"ONLIFT",
	"WAITINGONLIFT",
};
#endif

struct animtablerow g_DeathAnimsHumanLfoot[] = {
	{ ANIM_DEATH_0020, 0, -1, 0.5, 0, 26, -1 },
	{ 0,      0,  0, 0.5, 0, -1, -1 },
};

struct animtablerow g_DeathAnimsHumanLshin[] = {
	{ ANIM_DEATH_0020, 0, -1, 0.5, 0, 26, -1 },
	{ 0,      0,  0, 0.5, 0, -1, -1 },
};

struct animtablerow g_DeathAnimsHumanLthigh[] = {
	{ ANIM_DEATH_0020, 0, -1, 0.5, 1, 26, -1  },
	{ ANIM_DEATH_STOMACH_LONG, 1, -1, 0.5, 0, -1, -1  },
	{ ANIM_0092, 1, -1, 0.4, 0, 42, 103 },
	{ ANIM_0258, 1, -1, 0.5, 0, 43, 100 },
	{ 0,      0, 0,  0.5, 0, -1, -1  },
};

struct animtablerow g_DeathAnimsHumanRfoot[] = {
	{ ANIM_DEATH_0020, 1, -1, 0.5, 0, 26, -1 },
	{ 0,      0, 0,  0.5, 0, -1, -1 },
};

struct animtablerow g_DeathAnimsHumanRshin[] = {
	{ ANIM_DEATH_0020, 1, -1, 0.5, 0, 26, -1 },
	{ 0,      0, 0,  0.5, 0, -1, -1 },
};

struct animtablerow g_DeathAnimsHumanRthigh[] = {
	{ ANIM_DEATH_0020, 1, -1, 0.5, 1, 26, -1  },
	{ ANIM_DEATH_STOMACH_LONG, 0, -1, 0.5, 0, -1, -1  },
	{ ANIM_0092, 0, -1, 0.4, 0, 42, 103 },
	{ ANIM_0258, 0, -1, 0.5, 0, 43, 100 },
	{ 0,      0, 0,  0.5, 0, -1, -1  },
};

struct animtablerow g_DeathAnimsHumanPelvis[] = {
	{ ANIM_DEATH_001A, 0, -1, 0.5, 0, 55,  39  },
	{ ANIM_DEATH_001A, 1, -1, 0.5, 0, 55,  39  },
	{ ANIM_DEATH_001C, 0, -1, 0.5, 1, 29,  -1  },
	{ ANIM_DEATH_001C, 1, -1, 0.5, 1, 29,  -1  },
	{ ANIM_DEATH_0021, 0, -1, 0.5, 0, 97,  64  },
	{ ANIM_DEATH_0021, 1, -1, 0.5, 0, 97,  64  },
	{ ANIM_DEATH_0023, 0, -1, 0.5, 0, 31,  -1  },
	{ ANIM_DEATH_0023, 1, -1, 0.5, 0, 31,  -1  },
	{ ANIM_DEATH_0024, 0, -1, 0.5, 0, 36,  -1  },
	{ ANIM_DEATH_0024, 1, -1, 0.5, 0, 36,  -1  },
	{ ANIM_DEATH_0025, 0, -1, 0.5, 0, 28,  -1  },
	{ ANIM_DEATH_0025, 1, -1, 0.5, 0, 28,  -1  },
	{ ANIM_0090, 0, -1, 0.6, 0, 157, 234 },
	{ ANIM_0090, 1, -1, 0.6, 0, 157, 234 },
	{ ANIM_0091, 0, -1, 0.6, 0, 75,  265 },
	{ ANIM_0091, 1, -1, 0.6, 0, 75,  265 },
	{ ANIM_0250, 0, -1, 0.5, 0, 65,  105 },
	{ ANIM_0250, 1, -1, 0.5, 0, 65,  105 },
	{ 0,      0, -1, 0.5, 0, -1,  -1  },
};

struct animtablerow g_DeathAnimsHumanHead[] = {
	{ ANIM_DEATH_001A, 0, -1, 0.5, 0, 55,  39  },
	{ ANIM_DEATH_001A, 1, -1, 0.5, 0, 55,  39  },
	{ ANIM_DEATH_001C, 0, -1, 0.5, 1, 29,  -1  },
	{ ANIM_DEATH_001C, 1, -1, 0.5, 1, 29,  -1  },
	{ ANIM_DEATH_0020, 0, -1, 0.5, 1, 26,  -1  },
	{ ANIM_DEATH_0020, 1, -1, 0.5, 1, 26,  -1  },
	{ ANIM_DEATH_0021, 0, -1, 0.5, 0, 97,  64  },
	{ ANIM_DEATH_0021, 1, -1, 0.5, 0, 97,  64  },
	{ ANIM_DEATH_0022, 0, -1, 0.5, 0, 94,  66  },
	{ ANIM_DEATH_0022, 1, -1, 0.5, 0, 94,  66  },
	{ ANIM_DEATH_0023, 0, -1, 0.5, 0, 31,  -1  },
	{ ANIM_DEATH_0023, 1, -1, 0.5, 0, 31,  -1  },
	{ ANIM_DEATH_0024, 0, -1, 0.5, 0, 36,  -1  },
	{ ANIM_DEATH_0024, 1, -1, 0.5, 0, 36,  -1  },
	{ ANIM_DEATH_0025, 0, -1, 0.5, 0, 28,  -1  },
	{ ANIM_DEATH_0025, 1, -1, 0.5, 0, 28,  -1  },
	{ ANIM_0038, 0, -1, 0.5, 0, -1,  -1  },
	{ ANIM_0038, 1, -1, 0.5, 0, -1,  -1  },
	{ ANIM_0251, 0, -1, 0.5, 0, 132, 201 },
	{ ANIM_0251, 1, -1, 0.5, 0, 132, 201 },
	{ ANIM_0252, 0, -1, 0.5, 0, 83,  150 },
	{ ANIM_0252, 1, -1, 0.5, 0, 83,  150 },
	{ ANIM_0256, 0, -1, 0.5, 0, 63,  -1  },
	{ ANIM_0256, 1, -1, 0.5, 0, 63,  -1  },
	{ 0,      0, -1, 0.5, 0, -1,  -1  },
};

struct animtablerow g_DeathAnimsHumanLhand[] = {
	{ ANIM_DEATH_0020, 0, -1, 0.5, 0, 26, -1 },
	{ 0,      0, -1, 0.5, 0, -1, -1 },
};

struct animtablerow g_DeathAnimsHumanForearm[] = {
	{ ANIM_DEATH_0020, 0, -1, 0.5, 0, 26, -1 },
	{ 0,      0, -1, 0.5, 0, -1, -1 },
};

struct animtablerow g_DeathAnimsHumanLbicep[] = {
	{ ANIM_DEATH_0020, 0, -1, 0.5,  1, 26, -1 },
	{ ANIM_008F, 1, -1, 0.45, 1, 52, -1 },
	{ 0,      0, -1, 0.5,  0, -1, -1 },
};

struct animtablerow g_DeathAnimsHumanRhand[] = {
	{ ANIM_DEATH_0020, 1, -1, 0.5, 0, 26, -1 },
	{ 0,      0, -1, 0.5, 0, -1, -1 },
};

struct animtablerow g_DeathAnimsHumanRforearm[] = {
	{ ANIM_DEATH_0020, 1, -1, 0.5, 0, 26, -1 },
	{ 0,      0, -1, 0.5, 0, -1, -1 },
};

struct animtablerow g_DeathAnimsHumanRbicep[] = {
	{ ANIM_DEATH_0020, 1, -1, 0.5,  1, 26, -1 },
	{ ANIM_008F, 0, -1, 0.45, 1, 52, -1 },
	{ 0,      0, -1, 0.5,  0, -1, -1 },
};

struct animtablerow g_DeathAnimsHumanTorso[] = {
	{ ANIM_DEATH_001A, 0, -1, 0.5, 0, 55, 39 },
	{ ANIM_DEATH_001A, 1, -1, 0.5, 0, 55, 39 },
	{ ANIM_DEATH_001C, 0, -1, 0.5, 1, 29, -1 },
	{ ANIM_DEATH_001C, 1, -1, 0.5, 1, 29, -1 },
	{ ANIM_DEATH_0020, 0, -1, 0.5, 1, 26, -1 },
	{ ANIM_DEATH_0020, 1, -1, 0.5, 1, 26, -1 },
	{ ANIM_DEATH_0021, 0, -1, 0.5, 0, 97, 64 },
	{ ANIM_DEATH_0021, 1, -1, 0.5, 0, 97, 64 },
	{ ANIM_DEATH_0022, 0, -1, 0.5, 0, 94, 66 },
	{ ANIM_DEATH_0022, 1, -1, 0.5, 0, 94, 66 },
	{ ANIM_DEATH_0023, 0, -1, 0.5, 0, 31, -1 },
	{ ANIM_DEATH_0023, 1, -1, 0.5, 0, 31, -1 },
	{ ANIM_DEATH_0024, 0, -1, 0.5, 0, 36, -1 },
	{ ANIM_DEATH_0024, 1, -1, 0.5, 0, 36, -1 },
	{ ANIM_DEATH_0025, 0, -1, 0.5, 0, 28, -1 },
	{ ANIM_DEATH_0025, 1, -1, 0.5, 0, 28, -1 },
	{ ANIM_024E, 0, -1, 0.4, 0, 60, -1 },
	{ ANIM_024E, 1, -1, 0.4, 0, 60, -1 },
	{ ANIM_024F, 0, -1, 0.5, 0, 49, 80 },
	{ ANIM_024F, 1, -1, 0.5, 0, 49, 80 },
	{ ANIM_0253, 0, -1, 0.5, 1, 22, -1 },
	{ ANIM_0253, 1, -1, 0.5, 1, 22, -1 },
	{ ANIM_0254, 0, -1, 0.5, 0, 52, 75 },
	{ ANIM_0254, 1, -1, 0.5, 0, 52, 75 },
	{ 0,      0, -1, 0.5, 0, -1, -1 },
};

struct animtablerow g_DeathAnimsHumanGun[] = {
	{ ANIM_DEATH_001A, 0, -1, 0.5, 0, 55, 39 },
	{ ANIM_DEATH_001A, 1, -1, 0.5, 0, 55, 39 },
	{ ANIM_DEATH_001C, 0, -1, 0.5, 1, 29, -1 },
	{ ANIM_DEATH_001C, 1, -1, 0.5, 1, 29, -1 },
	{ ANIM_DEATH_0021, 0, -1, 0.5, 0, 97, 64 },
	{ ANIM_DEATH_0021, 1, -1, 0.5, 0, 97, 64 },
	{ ANIM_DEATH_0022, 0, -1, 0.5, 0, 94, 66 },
	{ ANIM_DEATH_0022, 1, -1, 0.5, 0, 94, 66 },
	{ ANIM_DEATH_0023, 0, -1, 0.5, 0, 31, -1 },
	{ ANIM_DEATH_0023, 1, -1, 0.5, 0, 31, -1 },
	{ ANIM_DEATH_0024, 0, -1, 0.5, 0, 36, -1 },
	{ ANIM_DEATH_0024, 1, -1, 0.5, 0, 36, -1 },
	{ ANIM_DEATH_0025, 0, -1, 0.5, 0, 28, -1 },
	{ ANIM_DEATH_0025, 1, -1, 0.5, 0, 28, -1 },
	{ 0,      0, -1, 0.5, 0, -1, -1 },
};

struct animtablerow g_AnimTableHumanSlumped[] = {
	{ ANIM_0019, 0, -1, 0.5, 0, 67, 54 },
	{ ANIM_0019, 1, -1, 0.5, 0, 67, 54 },
	{ ANIM_0257, 0, -1, 0.5, 0, 15, 80 },
	{ ANIM_0257, 1, -1, 0.5, 0, 15, 80 },
	{ 0,      0, -1, 0.5, 0, -1, -1 },
};

struct animtablerow g_InjuryAnimsHumanLfoot[] = {
	{ ANIM_0014, 0, -1, 0.5, 0, -1, -1 },
	{ ANIM_0015, 1, -1, 0.5, 0, -1, -1 },
	{ 0,      0, -1, 0.5, 0, -1, -1 },
};

struct animtablerow g_InjuryAnimsHumanLshin[] = {
	{ ANIM_0014, 0, -1, 0.5, 0, -1, -1 },
	{ ANIM_0015, 1, -1, 0.5, 0, -1, -1 },
	{ ANIM_00BC, 0, -1, 0.5, 0, -1, -1 },
	{ ANIM_00BD, 0, -1, 0.5, 0, -1, -1 },
	{ 0,      0, -1, 0.5, 0, -1, -1 },
};

struct animtablerow g_InjuryAnimsHumanLthigh[] = {
	{ ANIM_0014, 0, -1, 0.5, 0, -1, -1 },
	{ ANIM_0015, 1, -1, 0.5, 0, -1, -1 },
	{ ANIM_DEATH_STOMACH_LONG, 1, 20, 0.4, 0, -1, -1 },
	{ ANIM_00BA, 0, -1, 0.5, 0, -1, -1 },
	{ 0,      0, -1, 0.5, 0, -1, -1 },
};

struct animtablerow g_InjuryAnimsHumanRfoot[] = {
	{ ANIM_0015, 0, -1, 0.5, 0, -1, -1 },
	{ ANIM_0014, 1, -1, 0.5, 0, -1, -1 },
	{ ANIM_0236, 0, -1, 0.5, 0, -1, -1 },
	{ 0,      0, -1, 0.5, 0, -1, -1 },
};

struct animtablerow g_InjuryAnimsHumanRshin[] = {
	{ ANIM_0015, 0, -1, 0.5, 0, -1, -1 },
	{ ANIM_0014, 1, -1, 0.5, 0, -1, -1 },
	{ ANIM_00BE, 0, -1, 0.5, 0, -1, -1 },
	{ 0,      0, -1, 0.5, 0, -1, -1 },
};

struct animtablerow g_InjuryAnimsHumanRthigh[] = {
	{ ANIM_0015, 0, -1, 0.5, 0, -1, -1 },
	{ ANIM_0014, 1, -1, 0.5, 0, -1, -1 },
	{ ANIM_DEATH_STOMACH_LONG, 0, 20, 0.4, 0, -1, -1 },
	{ ANIM_00BF, 0, -1, 0.5, 0, -1, -1 },
	{ 0,      0, -1, 0.5, 0, -1, -1 },
};

struct animtablerow g_InjuryAnimsHumanPelvis[] = {
	{ ANIM_DEATH_0022, 0, 20, 0.5,  0, -1, -1 },
	{ ANIM_DEATH_0022, 1, 20, 0.5,  0, -1, -1 },
	{ ANIM_DEATH_001A, 0, 15, 0.5,  0, -1, -1 },
	{ ANIM_DEATH_001A, 1, 15, 0.5,  0, -1, -1 },
	{ ANIM_DEATH_0023, 0, 10, 0.25, 0, -1, -1 },
	{ ANIM_DEATH_0023, 1, 10, 0.25, 0, -1, -1 },
	{ ANIM_00DA, 0, -1, 0.5,  0, -1, -1 },
	{ ANIM_00F4, 0, -1, 0.5,  0, -1, -1 },
	{ 0,      0, -1, 0.5,  0, -1, -1 },
};

struct animtablerow g_InjuryAnimsHumanHead[] = {
	{ ANIM_DEATH_0022, 0, 20, 0.5, 0, -1, -1 },
	{ ANIM_DEATH_0022, 1, 20, 0.5, 0, -1, -1 },
	{ ANIM_DEATH_001A, 0, 15, 0.5, 0, -1, -1 },
	{ ANIM_DEATH_001A, 1, 15, 0.5, 0, -1, -1 },
	{ ANIM_00F8, 0, -1, 0.5, 0, -1, -1 },
	{ ANIM_00FB, 0, -1, 0.5, 0, -1, -1 },
	{ ANIM_0101, 0, -1, 0.5, 0, -1, -1 },
	{ ANIM_0113, 0, -1, 0.5, 0, -1, -1 },
	{ 0,      0, -1, 0.5, 0, -1, -1 },
};

struct animtablerow g_InjuryAnimsHumanLhand[] = {
	{ ANIM_0012, 0, -1, 0.5, 0, -1, -1 },
	{ ANIM_0013, 1, -1, 0.5, 0, -1, -1 },
	{ ANIM_00B8, 0, -1, 0.5, 0, -1, -1 },
	{ 0,      0, -1, 0.5, 0, -1, -1 },
};

struct animtablerow g_InjuryAnimsHumanForearm[] = {
	{ ANIM_0010, 0, -1, 0.5, 0, -1, -1 },
	{ ANIM_0011, 1, -1, 0.5, 0, -1, -1 },
	{ ANIM_00B4, 0, -1, 0.5, 0, -1, -1 },
	{ ANIM_021B, 0, -1, 0.5, 0, -1, -1 },
	{ 0,      0, -1, 0.5, 0, -1, -1 },
};

struct animtablerow g_InjuryAnimsHumanLbicep[] = {
	{ ANIM_000E, 0, -1, 0.5, 0, -1, -1 },
	{ ANIM_000F, 1, -1, 0.5, 0, -1, -1 },
	{ ANIM_DEATH_0022, 0, 20, 0.5, 0, -1, -1 },
	{ ANIM_00B0, 0, -1, 0.5, 0, -1, -1 },
	{ ANIM_00B1, 0, -1, 0.5, 0, -1, -1 },
	{ ANIM_021C, 0, -1, 0.5, 0, -1, -1 },
	{ ANIM_00B5, 0, -1, 0.5, 0, -1, -1 },
	{ 0,      0, -1, 0.5, 0, -1, -1 },
};

struct animtablerow g_InjuryAnimsHumanRhand[] = {
	{ ANIM_0013, 0, -1, 0.5, 0, -1, -1 },
	{ ANIM_0012, 1, -1, 0.5, 0, -1, -1 },
	{ ANIM_00B9, 0, -1, 0.5, 0, -1, -1 },
	{ 0,      0, -1, 0.5, 0, -1, -1 },
};

struct animtablerow g_InjuryAnimsHumanRforearm[] = {
	{ ANIM_0011, 0, -1, 0.5, 0, -1, -1 },
	{ ANIM_0010, 1, -1, 0.5, 0, -1, -1 },
	{ 0,      0, -1, 0.5, 0, -1, -1 },
};

struct animtablerow g_InjuryAnimsHumanRbicep[] = {
	{ ANIM_000F, 0, -1, 0.5, 0, -1, -1 },
	{ ANIM_000E, 1, -1, 0.5, 0, -1, -1 },
	{ ANIM_DEATH_0022, 1, 20, 0.5, 0, -1, -1 },
	{ ANIM_0190, 0, -1, 0.5, 0, -1, -1 },
	{ ANIM_00B2, 0, -1, 0.5, 0, -1, -1 },
	{ ANIM_00B3, 0, -1, 0.5, 0, -1, -1 },
	{ ANIM_00B6, 0, -1, 0.5, 0, -1, -1 },
	{ ANIM_00B7, 0, -1, 0.5, 0, -1, -1 },
	{ 0,      0, -1, 0.5, 0, -1, -1 },
};

struct animtablerow g_InjuryAnimsHumanTorso[] = {
	{ ANIM_DEATH_0022, 0, 20, 0.5, 0, -1, -1 },
	{ ANIM_DEATH_0022, 1, 20, 0.5, 0, -1, -1 },
	{ ANIM_DEATH_001A, 0, 15, 0.5, 0, -1, -1 },
	{ ANIM_DEATH_001A, 1, 15, 0.5, 0, -1, -1 },
	{ ANIM_0114, 1, -1, 0.5, 0, -1, -1 },
	{ ANIM_0130, 0, -1, 0.5, 0, -1, -1 },
	{ 0,      0, -1, 0.5, 0, -1, -1 },
};

struct animtablerow g_InjuryAnimsHumanGun[] = {
	{ ANIM_DEATH_0022, 0, 20, 0.5, 0, -1, -1 },
	{ ANIM_DEATH_0022, 1, 20, 0.5, 0, -1, -1 },
	{ ANIM_DEATH_001A, 0, 15, 0.5, 0, -1, -1 },
	{ ANIM_DEATH_001A, 1, 15, 0.5, 0, -1, -1 },
	{ 0,      0, -1, 0.5, 0, -1, -1 },
};

struct animtablerow g_DeathAnimsSkedarLfoot[] = {
	{ ANIM_0337, 1, -1, 0.5, 0, -1, -1 },
	{ ANIM_033C, 1, -1, 0.5, 0, -1, -1 },
	{ ANIM_033B, 1, -1, 0.5, 0, -1, -1 },
	{ 0,      0, -1, 0.5, 0, -1, -1 },
};

struct animtablerow g_DeathAnimsSkedarLshin[] = {
	{ ANIM_0337, 1, -1, 0.5, 0, -1, -1 },
	{ ANIM_033C, 1, -1, 0.5, 0, -1, -1 },
	{ ANIM_033B, 1, -1, 0.5, 0, -1, -1 },
	{ 0,      0, -1, 0.5, 0, -1, -1 },
};

struct animtablerow g_DeathAnimsSkedarLthigh[] = {
	{ ANIM_0337, 1, -1, 0.5, 0, -1, -1 },
	{ ANIM_033C, 1, -1, 0.5, 0, -1, -1 },
	{ ANIM_033B, 1, -1, 0.5, 0, -1, -1 },
	{ 0,      0, -1, 0.5, 0, -1, -1 },
};

struct animtablerow g_DeathAnimsSkedarRfoot[] = {
	{ ANIM_0337, 0, -1, 0.5, 0, -1, -1 },
	{ ANIM_033C, 0, -1, 0.5, 0, -1, -1 },
	{ ANIM_033B, 0, -1, 0.5, 0, -1, -1 },
	{ 0,      0, -1, 0.5, 0, -1, -1 },
};

struct animtablerow g_DeathAnimsSkedarRshin[] = {
	{ ANIM_0337, 0, -1, 0.5, 0, -1, -1 },
	{ ANIM_033C, 0, -1, 0.5, 0, -1, -1 },
	{ ANIM_033B, 0, -1, 0.5, 0, -1, -1 },
	{ 0,      0, -1, 0.5, 0, -1, -1 },
};

struct animtablerow g_DeathAnimsSkedarRthigh[] = {
	{ ANIM_0337, 0, -1, 0.5, 0, -1, -1 },
	{ ANIM_033C, 0, -1, 0.5, 0, -1, -1 },
	{ ANIM_033B, 0, -1, 0.5, 0, -1, -1 },
	{ 0,      0, -1, 0.5, 0, -1, -1 },
};

struct animtablerow g_DeathAnimsSkedarPelvis[] = {
	{ ANIM_0336, 0, -1, 0.5, 0, -1, -1 },
	{ 0,      0, -1, 0.5, 0, -1, -1 },
};

struct animtablerow g_DeathAnimsSkedarHead[] = {
	{ ANIM_0339, 0, -1, 0.5, 0, -1, -1 },
	{ ANIM_0338, 0, -1, 0.5, 0, -1, -1 },
	{ ANIM_033A, 0, -1, 0.5, 0, -1, -1 },
	{ 0,      0, -1, 0.5, 0, -1, -1 },
};

struct animtablerow g_DeathAnimsSkedarLhand[] = {
	{ ANIM_0336, 0, -1, 0.5, 0, -1, -1 },
	{ ANIM_033D, 0, -1, 0.5, 0, -1, -1 },
	{ 0,      0, -1, 0.5, 0, -1, -1 },
};

struct animtablerow g_DeathAnimsSkedarLforearm[] = {
	{ ANIM_0336, 0, -1, 0.5, 0, -1, -1 },
	{ ANIM_033D, 0, -1, 0.5, 0, -1, -1 },
	{ 0,      0, -1, 0.5, 0, -1, -1 },
};

struct animtablerow g_DeathAnimsSkedarLbicep[] = {
	{ ANIM_0336, 0, -1, 0.5, 0, -1, -1 },
	{ ANIM_033D, 0, -1, 0.5, 0, -1, -1 },
	{ 0,      0, -1, 0.5, 0, -1, -1 },
};

struct animtablerow g_DeathAnimsSkedarRhand[] = {
	{ ANIM_0336, 0, -1, 0.5, 0, -1, -1 },
	{ ANIM_033D, 0, -1, 0.5, 0, -1, -1 },
	{ 0,      0, -1, 0.5, 0, -1, -1 },
};

struct animtablerow g_DeathAnimsSkedarRforearm[] = {
	{ ANIM_0336, 0, -1, 0.5, 0, -1, -1 },
	{ ANIM_033D, 0, -1, 0.5, 0, -1, -1 },
	{ 0,      0, -1, 0.5, 0, -1, -1 },
};

struct animtablerow g_DeathAnimsSkedarRbicep[] = {
	{ ANIM_0336, 0, -1, 0.5, 0, -1, -1 },
	{ ANIM_033D, 0, -1, 0.5, 0, -1, -1 },
	{ 0,      0, -1, 0.5, 0, -1, -1 },
};

struct animtablerow g_DeathAnimsSkedarTorso[] = {
	{ ANIM_0336, 0, -1, 0.5, 0, -1, -1 },
	{ ANIM_033D, 0, -1, 0.5, 0, -1, -1 },
	{ 0,      0, -1, 0.5, 0, -1, -1 },
};

struct animtablerow g_DeathAnimsSkedarGun[] = {
	{ ANIM_0336, 0, -1, 0.5, 0, -1, -1 },
	{ ANIM_033D, 0, -1, 0.5, 0, -1, -1 },
	{ 0,      0, -1, 0.5, 0, -1, -1 },
};

struct animtablerow g_DeathAnimsSkedarTail[] = {
	{ ANIM_0336, 0, -1, 0.5, 0, -1, -1 },
	{ ANIM_033D, 0, -1, 0.5, 0, -1, -1 },
	{ 0,      0, -1, 0.5, 0, -1, -1 },
};

struct animtablerow g_InjuryAnimsSkedarLfoot[] = {
	{ ANIM_038E, 0, -1, 1,   0, -1, -1 },
	{ 0,      0, -1, 0.5, 0, -1, -1 },
};

struct animtablerow g_InjuryAnimsSkedarLshin[] = {
	{ ANIM_0390, 0, -1, 0.9, 0, -1, -1 },
	{ 0,      0, -1, 0.5, 0, -1, -1 },
};

struct animtablerow g_InjuryAnimsSkedarLthigh[] = {
	{ ANIM_0390, 0, -1, 0.9, 0, -1, -1 },
	{ 0,      0, -1, 0.5, 0, -1, -1 },
};

struct animtablerow g_InjuryAnimsSkedarRfoot[] = {
	{ ANIM_038E, 1, -1, 0.5, 0, -1, -1 },
	{ 0,      0, -1, 0.5, 0, -1, -1 },
};

struct animtablerow g_InjuryAnimsSkedarRshin[] = {
	{ ANIM_0390, 1, -1, 0.9, 0, -1, -1 },
	{ 0,      0, -1, 0.5, 0, -1, -1 },
};

struct animtablerow g_InjuryAnimsSkedarRthigh[] = {
	{ ANIM_0390, 1, -1, 0.9, 0, -1, -1 },
	{ 0,      0, -1, 0.5, 0, -1, -1 },
};

struct animtablerow g_InjuryAnimsSkedarPelvis[] = {
	{ ANIM_038C, 0, -1, 0.9, 0, -1, -1 },
	{ ANIM_038C, 1, -1, 0.9, 0, -1, -1 },
	{ ANIM_0341, 0, -1, 0.5, 0, -1, -1 },
	{ 0,      0, -1, 0.5, 0, -1, -1 },
};

struct animtablerow g_InjuryAnimsSkedarHead[] = {
	{ ANIM_0341, 0, -1, 0.5, 0, -1, -1 },
	{ 0,      0, -1, 0.5, 0, -1, -1 },
};

struct animtablerow g_InjuryAnimsSkedarLhand[] = {
	{ ANIM_0343, 0, -1, 0.5, 0, -1, -1 },
	{ 0,      0, -1, 0.5, 0, -1, -1 },
};

struct animtablerow g_InjuryAnimsSkedarLforearm[] = {
	{ ANIM_038B, 0, -1, 0.9, 0, -1, -1 },
	{ 0,      0, -1, 0.5, 0, -1, -1 },
};

struct animtablerow g_InjuryAnimsSkedarLbicep[] = {
	{ ANIM_038B, 0, -1, 0.9, 0, -1, -1 },
	{ 0,      0, -1, 0.5, 0, -1, -1 },
};

struct animtablerow g_InjuryAnimsSkedarRhand[] = {
	{ ANIM_0343, 1, -1, 0.5, 0, -1, -1 },
	{ 0,      0, -1, 0.5, 0, -1, -1 },
};

struct animtablerow g_InjuryAnimsSkedarRforearm[] = {
	{ ANIM_038B, 1, -1, 0.9, 0, -1, -1 },
	{ 0,      0, -1, 0.5, 0, -1, -1 },
};

struct animtablerow g_InjuryAnimsSkedarRbicep[] = {
	{ ANIM_038B, 1, -1, 0.9, 0, -1, -1 },
	{ 0,      0, -1, 0.5, 0, -1, -1 },
};

struct animtablerow g_InjuryAnimsSkedarTorso[] = {
	{ ANIM_038C, 0, -1, 0.9, 0, -1, -1 },
	{ ANIM_038C, 1, -1, 0.9, 0, -1, -1 },
	{ ANIM_0341, 0, -1, 0.5, 0, -1, -1 },
	{ 0,      0, -1, 0.5, 0, -1, -1 },
};

struct animtablerow g_InjuryAnimsSkedarGun[] = {
	{ ANIM_0343, 0, -1, 0.5, 0, -1, -1 },
	{ 0,      0, -1, 0.5, 0, -1, -1 },
};

struct animtablerow g_InjuryAnimsSkedarTail[] = {
	{ ANIM_0341, 0, -1, 0.5, 0, -1,  -1  },
	{ 0,      0, -1, 0.5, 0, -1,  -1  },
};

struct animtablerow g_SpecialDieAnims[] = {
	{ ANIM_SPECIALDIE_FALLBACK,    0, -1, 0.5, 0, 149, 175 },
	{ ANIM_SPECIALDIE_ROLL1,       0, -1, 0.5, 0, 115, 152 },
	{ ANIM_SPECIALDIE_ROLL2,       0, -1, 0.5, 0, 115, 152 },
	{ ANIM_SPECIALDIE_ROLL3,       0, -1, 0.5, 0, 115, 152 },
	{ ANIM_SPECIALDIE_OVERRAILING, 0, -1, 0.5, 0, 83,  99  },
	{ ANIM_022B,                   0, -1, 0.5, 0, 0,   0   },
	{ ANIM_022C,                   0, -1, 0.5, 0, 0,   0   },
	{ ANIM_022D,                   0, -1, 0.5, 0, 0,   0   },
	{ ANIM_022E,                   0, -1, 0.5, 0, 0,   0   },
	{ 0,                           0, -1, 0.5, 0, -1,  -1  },
};

struct animtable g_AnimTablesHuman[] = {
	{ 0,                NULL,                      NULL,                       0, 0 },
	{ HITPART_LFOOT,    g_DeathAnimsHumanLfoot,    g_InjuryAnimsHumanLfoot,    0, 0 },
	{ HITPART_LSHIN,    g_DeathAnimsHumanLshin,    g_InjuryAnimsHumanLshin,    0, 0 },
	{ HITPART_LTHIGH,   g_DeathAnimsHumanLthigh,   g_InjuryAnimsHumanLthigh,   0, 0 },
	{ HITPART_RFOOT,    g_DeathAnimsHumanRfoot,    g_InjuryAnimsHumanRfoot,    0, 0 },
	{ HITPART_RSHIN,    g_DeathAnimsHumanRshin,    g_InjuryAnimsHumanRshin,    0, 0 },
	{ HITPART_RTHIGH,   g_DeathAnimsHumanRthigh,   g_InjuryAnimsHumanRthigh,   0, 0 },
	{ HITPART_PELVIS,   g_DeathAnimsHumanPelvis,   g_InjuryAnimsHumanPelvis,   0, 0 },
	{ HITPART_HEAD,     g_DeathAnimsHumanHead,     g_InjuryAnimsHumanHead,     0, 0 },
	{ HITPART_LHAND,    g_DeathAnimsHumanLhand,    g_InjuryAnimsHumanLhand,    0, 0 },
	{ HITPART_LFOREARM, g_DeathAnimsHumanForearm,  g_InjuryAnimsHumanForearm,  0, 0 },
	{ HITPART_LBICEP,   g_DeathAnimsHumanLbicep,   g_InjuryAnimsHumanLbicep,   0, 0 },
	{ HITPART_RHAND,    g_DeathAnimsHumanRhand,    g_InjuryAnimsHumanRhand,    0, 0 },
	{ HITPART_RFOREARM, g_DeathAnimsHumanRforearm, g_InjuryAnimsHumanRforearm, 0, 0 },
	{ HITPART_RBICEP,   g_DeathAnimsHumanRbicep,   g_InjuryAnimsHumanRbicep,   0, 0 },
	{ HITPART_TORSO,    g_DeathAnimsHumanTorso,    g_InjuryAnimsHumanTorso,    0, 0 },
	{ HITPART_GUN,      g_DeathAnimsHumanGun,      g_InjuryAnimsHumanGun,      0, 0 },
	{ HITPART_HAT,      NULL,                      NULL,                       0, 0 },
	{ -1,               NULL,                      NULL,                       0, 0 },
};

struct animtable g_AnimTablesSkedar[] = {
	{ 0,                NULL,                       NULL,                        0, 0 },
	{ HITPART_LFOOT,    g_DeathAnimsSkedarLfoot,    g_InjuryAnimsSkedarLfoot,    0, 0 },
	{ HITPART_LSHIN,    g_DeathAnimsSkedarLshin,    g_InjuryAnimsSkedarLshin,    0, 0 },
	{ HITPART_LTHIGH,   g_DeathAnimsSkedarLthigh,   g_InjuryAnimsSkedarLthigh,   0, 0 },
	{ HITPART_RFOOT,    g_DeathAnimsSkedarRfoot,    g_InjuryAnimsSkedarRfoot,    0, 0 },
	{ HITPART_RSHIN,    g_DeathAnimsSkedarRshin,    g_InjuryAnimsSkedarRshin,    0, 0 },
	{ HITPART_RTHIGH,   g_DeathAnimsSkedarRthigh,   g_InjuryAnimsSkedarRthigh,   0, 0 },
	{ HITPART_PELVIS,   g_DeathAnimsSkedarPelvis,   g_InjuryAnimsSkedarPelvis,   0, 0 },
	{ HITPART_HEAD,     g_DeathAnimsSkedarHead,     g_InjuryAnimsSkedarHead,     0, 0 },
	{ HITPART_LHAND,    g_DeathAnimsSkedarLhand,    g_InjuryAnimsSkedarLhand,    0, 0 },
	{ HITPART_LFOREARM, g_DeathAnimsSkedarLforearm, g_InjuryAnimsSkedarLforearm, 0, 0 },
	{ HITPART_LBICEP,   g_DeathAnimsSkedarLbicep,   g_InjuryAnimsSkedarLbicep,   0, 0 },
	{ HITPART_RHAND,    g_DeathAnimsSkedarRhand,    g_InjuryAnimsSkedarRhand,    0, 0 },
	{ HITPART_RFOREARM, g_DeathAnimsSkedarRforearm, g_InjuryAnimsSkedarRforearm, 0, 0 },
	{ HITPART_RBICEP,   g_DeathAnimsSkedarRbicep,   g_InjuryAnimsSkedarRbicep,   0, 0 },
	{ HITPART_TORSO,    g_DeathAnimsSkedarTorso,    g_InjuryAnimsSkedarTorso,    0, 0 },
	{ HITPART_GUN,      g_DeathAnimsSkedarGun,      g_InjuryAnimsSkedarGun,      0, 0 },
	{ HITPART_HAT,      g_DeathAnimsSkedarHead,     g_InjuryAnimsSkedarHead,     0, 0 },
	{ HITPART_TAIL,     g_DeathAnimsSkedarTail,     g_InjuryAnimsSkedarTail,     0, 0 },
	{ -1,               NULL,                       NULL,                        0, 0 },
};

struct animtable g_AnimTablesDrCaroll[] = {
	{ 0,  NULL, NULL, 0, 0 },
	{ -1, NULL, NULL, 0, 0 },
};

struct animtable g_AnimTablesEyespy[] = {
	{ 0,  NULL, NULL, 0, 0 },
	{ -1, NULL, NULL, 0, 0 },
};

struct animtable g_AnimTablesRobot[] = {
	{ 0,  NULL, NULL, 0, 0 },
	{ -1, NULL, NULL, 0, 0 },
};

struct animtable *g_AnimTablesByRace[] = {
	g_AnimTablesHuman,
	g_AnimTablesSkedar,
	g_AnimTablesDrCaroll,
	g_AnimTablesEyespy,
	g_AnimTablesRobot,
};

/**
 * A yeet anim is an animation config for a chr being launched by an explosion.
 */
struct yeetanim {
	s16 animnum;
	bool flip;
	f32 speed;
	f32 startframe;
	f32 thudframe;
	f32 endframe;
};

struct yeetanim g_YeetAnimsHuman[] = {
	/* 0*/ { ANIM_0082, 0, 0.5, 9,  29, -1 },
	/* 1*/ { ANIM_0082, 1, 0.5, 9,  29, -1 },
	/* 2*/ { ANIM_008A, 0, 0.5, 11, 31, -1 },
	/* 3*/ { ANIM_008A, 1, 0.5, 11, 31, -1 },
	/* 4*/ { ANIM_0089, 0, 0.5, 6,  27, -1 },
	/* 5*/ { ANIM_0089, 1, 0.5, 6,  27, -1 },
	/* 6*/ { ANIM_008C, 0, 0.5, 29, 48, -1 },
	/* 7*/ { ANIM_008C, 1, 0.5, 29, 48, -1 },
	/* 8*/ { ANIM_008D, 0, 0.5, 29, 49, -1 },
	/* 9*/ { ANIM_008D, 1, 0.5, 29, 49, -1 },
	/*10*/ { ANIM_008E, 0, 0.5, 19, 42, -1 },
	/*11*/ { ANIM_008E, 1, 0.5, 19, 42, -1 },
	/*12*/ { ANIM_0086, 0, 0.5, 0,  60, -1 },
	/*13*/ { ANIM_0086, 1, 0.5, 0,  60, -1 },
	/*14*/ { ANIM_0087, 0, 0.5, 6,  29, -1 },
	/*15*/ { ANIM_0087, 1, 0.5, 6,  29, -1 },
	/*16*/ { ANIM_0084, 0, 0.5, 8,  25, -1 },
	/*17*/ { ANIM_0084, 1, 0.5, 8,  25, -1 },
	/*18*/ { ANIM_0085, 0, 0.5, 8,  25, -1 },
	/*19*/ { ANIM_0085, 1, 0.5, 8,  25, -1 },
	/*20*/ { ANIM_0088, 0, 0.5, 12, 29, -1 },
	/*21*/ { ANIM_0088, 1, 0.5, 12, 29, -1 },
	/*22*/ { ANIM_008B, 0, 0.5, 22, 41, -1 },
	/*23*/ { ANIM_008B, 1, 0.5, 22, 41, -1 },
	/*24*/ { 0,         0, 0.5, 0,  0,  -1 },
};

s8 g_YeetAnimIndexesHumanAngle0[] = { 0,  1,  2,  3, 4, 5 };
s8 g_YeetAnimIndexesHumanAngle1[] = { 7,  9,  11          };
s8 g_YeetAnimIndexesHumanAngle2[] = { 6,  8,  10          };
s8 g_YeetAnimIndexesHumanAngle3[] = { 12, 15, 0           };
s8 g_YeetAnimIndexesHumanAngle4[] = { 13, 14, 0           };
s8 g_YeetAnimIndexesHumanAngle5[] = { 18, 19, 20, 21      };
s8 g_YeetAnimIndexesHumanAngle6[] = { 16, 22              };
s8 g_YeetAnimIndexesHumanAngle7[] = { 17, 23              };

struct yeetanim g_YeetAnimsSkedar[] = {
	/* 0*/ { ANIM_033F, 0, 0.5, 0, -1, -1 },
	/* 1*/ { ANIM_033F, 0, 0.5, 0, -1, -1 },
	/* 2*/ { ANIM_033F, 0, 0.5, 0, -1, -1 },
	/* 3*/ { ANIM_033F, 0, 0.5, 0, -1, -1 },
	/* 4*/ { ANIM_033F, 0, 0.5, 0, -1, -1 },
	/* 5*/ { ANIM_033F, 0, 0.5, 0, -1, -1 },
	/* 6*/ { ANIM_033F, 0, 0.5, 0, -1, -1 },
	/* 7*/ { ANIM_033F, 0, 0.5, 0, -1, -1 },
	/* 8*/ { 0,         0, 0.5, 0, 0,  -1 },
};

s8 g_YeetAnimIndexesSkedarAngle0[] = { 0 };
s8 g_YeetAnimIndexesSkedarAngle1[] = { 1 };
s8 g_YeetAnimIndexesSkedarAngle2[] = { 2 };
s8 g_YeetAnimIndexesSkedarAngle3[] = { 3 };
s8 g_YeetAnimIndexesSkedarAngle4[] = { 4 };
s8 g_YeetAnimIndexesSkedarAngle5[] = { 7 };
s8 g_YeetAnimIndexesSkedarAngle6[] = { 5 };
s8 g_YeetAnimIndexesSkedarAngle7[] = { 6 };

struct yeetanimindexlist {
	s8 *indexes;
	s32 count;
};

struct yeetanimindexlist g_YeetAnimIndexesByRaceAngle[][8] = {
	{
		{ g_YeetAnimIndexesHumanAngle0, 6 },
		{ g_YeetAnimIndexesHumanAngle1, 3 },
		{ g_YeetAnimIndexesHumanAngle3, 3 },
		{ g_YeetAnimIndexesHumanAngle6, 2 },
		{ g_YeetAnimIndexesHumanAngle5, 4 },
		{ g_YeetAnimIndexesHumanAngle7, 2 },
		{ g_YeetAnimIndexesHumanAngle4, 3 },
		{ g_YeetAnimIndexesHumanAngle2, 3 },
	}, {
		{ g_YeetAnimIndexesSkedarAngle0, 1 },
		{ g_YeetAnimIndexesSkedarAngle1, 1 },
		{ g_YeetAnimIndexesSkedarAngle3, 1 },
		{ g_YeetAnimIndexesSkedarAngle6, 1 },
		{ g_YeetAnimIndexesSkedarAngle5, 1 },
		{ g_YeetAnimIndexesSkedarAngle7, 1 },
		{ g_YeetAnimIndexesSkedarAngle4, 1 },
		{ g_YeetAnimIndexesSkedarAngle2, 1 },
	},
};

struct attackanimconfig var800656c0[] = {
	{ ANIM_0002, 28, 0, 0, 0, -1, 23, 54, -1, -1, 18, 54, 0.87252569198608, -0.52351540327072, 1.0470308065414, -0.34901028871536, 1.6000000238419, 1.7999999523163 },
	{ 0, 0,  0, 0, 0, -1, 0,  0,  0,  0,  0,  0,  0,                0,                 0,               0,                 0,               0               },
};

struct attackanimgroup var80065750 = { var800656c0, 0xffffffff };

struct attackanimconfig var80065758[] = {
	{ ANIM_0032, 37, 0, 0, 0, -1, 30, 81, -1, -1, 25, 81, 0.87252569198608, -0.69802057743073, 0.69802057743073, -0.69802057743073, 1.6000000238419, 1.75 },
	{ ANIM_0003, 27, 0, 0, 0, -1, 22, 61, -1, -1, 17, 61, 0.87252569198608, -0.26175770163536, 0.69802057743073, -0.69802057743073, 2,               1    },
	{ 0, 0,  0, 0, 0, -1, 0,  0,  0,  0,  0,  0,  0,                0,                 0,                0,                 0,               0    },
};

struct attackanimgroup var80065830 = { var80065758, 0xffffffff };

struct attackanimconfig var80065838[] = {
	{ ANIM_0032, 37, 0, 0, 0, -1, 30, 81, -1, -1, 25, 81, 0.87252569198608, -0.69802057743073, 0.69802057743073, -0.69802057743073, 1.6000000238419, 1.75 },
	{ ANIM_0003, 27, 0, 0, 0, -1, 22, 61, -1, -1, 17, 61, 0.87252569198608, -0.26175770163536, 0.69802057743073, -0.69802057743073, 2,               1    },
	{ 0, 0,  0, 0, 0, -1, 0,  0,  0,  0,  0,  0,  0,                0,                 0,                0,                 0,               0    },
};

struct attackanimgroup var80065910 = { var80065838, 0xffffffff };

struct attackanimconfig var80065918[] = {
	{ ANIM_0004, 19, 0, 1.5707963705063, 0, -1, 19, 61, -1, -1, 14, 61, 0.87252569198608, -0.34901028871536, 0.43626284599304, -1.0470308065414, 2.5, 2.5 },
	{ 0, 0,  0, 0,               0, -1, 0,  0,  0,  0,  0,  0,  0,                0,                 0,                0,                0,   0   },
};

struct attackanimgroup var800659a8 = { var80065918, 0xffffffff };

struct attackanimconfig var800659b0[] = {
	{ ANIM_0006, 27, 0, 0, 0, -1, 39, 74, -1, -1, 34, 74, 0.87252569198608, -0.69802057743073, 0.7852731347084, -0.69802057743073, 1.5, 1.5 },
	{ 0, 0,  0, 0, 0, -1, 0,  0,  0,  0,  0,  0,  0,                0,                 0,               0,                 0,   0   },
};

struct attackanimgroup var80065a40 = { var800659b0, 0xffffffff };

struct attackanimconfig var80065a48[] = {
	{ ANIM_034A, 20, 0, 0, 0, -1, 25, 50, -1, -1, 10, 50, 0.34901028871536, -0.34901028871536, 0.52351540327072, -0.52351540327072, 1, 1 },
	{ 0, 0,  0, 0, 0, -1, 0,  0,  0,  0,  0,  0,  0,                0,                 0,                0,                 0, 0 },
};

struct attackanimgroup var80065ad8 = { var80065a48, 0xffffffff };

struct attackanimgroup *g_StandHeavyAttackAnims[][32] = {
	{
		// RACE_HUMAN
		&var80065750, &var80065830, &var80065830, &var80065830,
		&var80065830, &var80065830, &var80065830, &var80065830,
		&var80065830, &var80065830, &var800659a8, &var800659a8,
		&var800659a8, &var800659a8, &var800659a8, &var800659a8,
		&var80065a40, &var80065a40, &var80065a40, &var80065a40,
		&var80065a40, &var80065a40, &var80065910, &var80065910,
		&var80065910, &var80065910, &var80065910, &var80065910,
		&var80065910, &var80065910, &var80065910, &var80065750,
	}, {
		// RACE_SKEDAR
		&var80065ad8, &var80065ad8, &var80065ad8, &var80065ad8,
		&var80065ad8, &var80065ad8, &var80065ad8, &var80065ad8,
		&var80065ad8, &var80065ad8, &var80065ad8, &var80065ad8,
		&var80065ad8, &var80065ad8, &var80065ad8, &var80065ad8,
		&var80065ad8, &var80065ad8, &var80065ad8, &var80065ad8,
		&var80065ad8, &var80065ad8, &var80065ad8, &var80065ad8,
		&var80065ad8, &var80065ad8, &var80065ad8, &var80065ad8,
		&var80065ad8, &var80065ad8, &var80065ad8, &var80065ad8,
	},
};

struct attackanimconfig var80065be0[] = {
	{ ANIM_0041, 26, 0, 0, 12, 140, 58, 92, 60, 79, 20, 120, 0.87252569198608, -0.69802057743073, 0.69802057743073, -0.69802057743073, 0, 0 },
	{ ANIM_0044, 0,  0, 0, 17, 100, 25, 87, 30, 55, 20, 93,  0.87252569198608, -0.69802057743073, 0.69802057743073, -1.0470308065414,  0, 0 },
	{ ANIM_0045, 0,  0, 0, 12, 64,  19, 51, 24, 46, 14, 58,  0.87252569198608, -0.69802057743073, 0.52351540327072, -0.7852731347084,  0, 0 },
	{ ANIM_0046, 22, 0, 0, 4,  69,  22, 49, 22, 33, 8,  58,  0.87252569198608, -0.69802057743073, 0.43626284599304, -0.7852731347084,  0, 0 },
	{ 0, 0,  0, 0, 0,  -1,  0,  0,  0,  0,  0,  0,   0,                0,                 0,                0,                 0, 0 },
};

struct attackanimgroup var80065d48 = { var80065be0, 0xffffffff };

struct attackanimconfig var80065d50[] = {
	{ ANIM_0041, 26, 0, 0, 12, 140, 58, 92, 60, 79, 20, 120, 0.87252569198608, -0.69802057743073, 0.69802057743073, -0.69802057743073, 0, 0 },
	{ ANIM_0046, 22, 0, 0, 4,  69,  22, 49, 22, 33, 8,  58,  0.87252569198608, -0.69802057743073, 0.43626284599304, -0.7852731347084,  0, 0 },
	{ 0, 0,  0, 0, 0,  -1,  0,  0,  0,  0,  0,  0,   0,                0,                 0,                0,                 0, 0 },
};

struct attackanimgroup var80065e28 = { var80065d50, 0xffffffff };

struct attackanimconfig var80065e30[] = {
	{ ANIM_0041, 26, 0, 0,               12, 140, 58, 92, 60, 79, 20, 120, 0.87252569198608, -0.69802057743073, 0.69802057743073, -0.69802057743073, 0, 0 },
	{ ANIM_0046, 22, 0, 0,               4,  69,  22, 49, 22, 33, 8,  58,  0.87252569198608, -0.69802057743073, 0.43626284599304, -0.7852731347084,  0, 0 },
	{ ANIM_0049, 0,  0, 1.5707963705063, 7,  130, 45, 93, 56, 73, 26, 107, 0.87252569198608, -0.69802057743073, 0.34901028871536, -0.52351540327072, 0, 0 },
	{ ANIM_004A, 15, 0, 1.5707963705063, 5,  76,  20, 31, 31, 38, 15, 49,  0.87252569198608, -0.69802057743073, 0.52351540327072, -1.0470308065414,  0, 0 },
	{ 0, 0,  0, 0,               0,  -1,  0,  0,  0,  0,  0,  0,   0,                0,                 0,                0,                 0, 0 },
};

struct attackanimgroup var80065f98 = { var80065e30, 0xffffffff };

struct attackanimconfig var80065fa0[] = {
	{ ANIM_0041, 26, 0, 0,               12, 140, 58, 92,  60, 79, 20, 120, 0.87252569198608, -0.69802057743073, 0.69802057743073, -0.69802057743073, 0, 0 },
	{ ANIM_0046, 22, 0, 0,               4,  69,  22, 49,  22, 33, 8,  58,  0.87252569198608, -0.69802057743073, 0.43626284599304, -0.7852731347084,  0, 0 },
	{ ANIM_0047, 0,  0, 4.7123889923096, 7,  139, 54, 105, 61, 88, 26, 120, 0.87252569198608, -0.69802057743073, 0.69802057743073, -0.61076802015305, 0, 0 },
	{ ANIM_0048, 19, 0, 4.7123889923096, 4,  79,  21, 50,  26, 42, 10, 64,  0.87252569198608, -0.69802057743073, 0.69802057743073, -0.61076802015305, 0, 0 },
	{ 0, 0,  0, 0,               0,  -1,  0,  0,   0,  0,  0,  0,   0,                0,                 0,                0,                 0, 0 },
};

struct attackanimgroup var80066108 = { var80065fa0, 0xffffffff };

struct attackanimconfig var80066110[] = {
	{ ANIM_004A, 19, 0, 1.5707963705063, 5, 76, 20, 31, 31, 38, 15, 49, 0.87252569198608, -0.69802057743073, 0.52351540327072, -1.0470308065414, 0, 0 },
	{ 0, 0,  0, 0,               0, -1, 0,  0,  0,  0,  0,  0,  0,                0,                 0,                0,                0, 0 },
};

struct attackanimgroup var800661a0 = { var80066110, 0xffffffff };

struct attackanimconfig var800661a8[] = {
	{ ANIM_0048, 19, 0, 4.7123889923096, 4, 79, 21, 50, 26, 42, 10, 64, 0.87252569198608, -0.69802057743073, 0.69802057743073, -0.61076802015305, 0, 0 },
	{ 0, 0,  0, 0,               0, -1, 0,  0,  0,  0,  0,  0,  0,                0,                 0,                0,                 0, 0 },
};

struct attackanimgroup var80066238 = { var800661a8, 0xffffffff };

struct attackanimconfig var80066240[] = {
	{ ANIM_034A, 20, 0, 0, 0, -1, 25, 50, -1, -1, 10, 50, 0.34901028871536, -0.34901028871536, 0.52351540327072, -0.52351540327072, 1, 1 },
	{ 0, 0,  0, 0, 0, -1, 0,  0,  0,  0,  0,  0,  0,                0,                 0,                0,                 0, 0 },
};

struct attackanimgroup var800662d0 = { var80066240, 0xffffffff };

struct attackanimgroup *g_StandLightAttackAnims[][32] = {
	{
		// RACE_HUMAN
		&var80065d48, &var80065d48, &var80065e28, &var80065e28,
		&var80065e28, &var80065f98, &var80065f98, &var80065f98,
		&var80065f98, &var80065f98, &var800661a0, &var800661a0,
		&var800661a0, &var800661a0, &var800661a0, &var800661a0,
		&var80066238, &var80066238, &var80066238, &var80066238,
		&var80066238, &var80066238, &var80066108, &var80066108,
		&var80066108, &var80066108, &var80066108, &var80065e28,
		&var80065e28, &var80065e28, &var80065d48, &var80065d48,
	}, {
		// RACE_SKEDAR
		&var800662d0, &var800662d0, &var800662d0, &var800662d0,
		&var800662d0, &var800662d0, &var800662d0, &var800662d0,
		&var800662d0, &var800662d0, &var800662d0, &var800662d0,
		&var800662d0, &var800662d0, &var800662d0, &var800662d0,
		&var800662d0, &var800662d0, &var800662d0, &var800662d0,
		&var800662d0, &var800662d0, &var800662d0, &var800662d0,
		&var800662d0, &var800662d0, &var800662d0, &var800662d0,
		&var800662d0, &var800662d0, &var800662d0, &var800662d0,
	},
};


struct attackanimconfig var800663d8[] = {
	{ ANIM_007A, 26, 0, 0, 7, 92, 28, 68, -1, -1, 11, 73, 0.87252569198608, -0.69802057743073, 0.69802057743073, -0.69802057743073, 0, 0 },
	{ 0, 0,  0, 0, 0, -1, 0,  0,  0,  0,  0,  0,  0,                0,                 0,                0,                 0, 0 },
};

struct attackanimgroup var80066468 = { var800663d8, 0xffffffff };

struct attackanimconfig var80066470[] = {
	{ ANIM_007B, 26, 0, 1.5707963705063, 9,  112, 38, 87, -1, -1, 19, 98, 0.87252569198608, -0.69802057743073, 0.43626284599304, -0.43626284599304, 0, 0 },
	{ ANIM_007D, 25, 0, 1.5707963705063, 10, 112, 32, 86, -1, -1, 19, 97, 0.87252569198608, -0.69802057743073, 0.43626284599304, -0.43626284599304, 0, 0 },
	{ 0, 0,  0, 0,               0,  -1,  0,  0,  0,  0,  0,  0,  0,                0,                 0,                0,                 0, 0 },
};

struct attackanimgroup var80066548 = { var80066470, 0xffffffff };

struct attackanimconfig var80066550[] = {
	{ ANIM_007C, 39, 0, 4.7123889923096, 22, 127, 44, 102, -1, -1, 28, 112, 0.87252569198608, -0.69802057743073, 0.43626284599304, -0.43626284599304, 0, 0 },
	{ ANIM_007E, 39, 0, 4.7123889923096, 23, 130, 46, 100, -1, -1, 30, 110, 0.87252569198608, -0.69802057743073, 0.43626284599304, -0.43626284599304, 0, 0 },
	{ 0, 0,  0, 0,               0,  -1,  0,  0,   0,  0,  0,  0,   0,                0,                 0,                0,                 0, 0 },
};

struct attackanimgroup var80066628 = { var80066550, 0xffffffff };

struct attackanimconfig var80066630[] = {
	{ ANIM_034A, 20, 0, 0, 0, -1, 25, 50, -1, -1, 10, 50, 0.34901028871536, -0.34901028871536, 0.52351540327072, -0.52351540327072, 1, 1 },
	{ 0, 0,  0, 0, 0, -1, 0,  0,  0,  0,  0,  0,  0,                0,                 0,                0,                 0, 0 },
};

struct attackanimgroup var800666c0 = { var80066630, 0xffffffff };

struct attackanimgroup *g_StandDualAttackAnims[][32] = {
	{
		// RACE_HUMAN
		&var80066468, &var80066468, &var80066468, &var80066468,
		&var80066468, &var80066548, &var80066548, &var80066548,
		&var80066548, &var80066548, &var80066548, &var80066548,
		&var80066548, &var80066548, &var80066548, &var80066548,
		&var80066628, &var80066628, &var80066628, &var80066628,
		&var80066628, &var80066628, &var80066628, &var80066628,
		&var80066628, &var80066628, &var80066628, &var80066468,
		&var80066468, &var80066468, &var80066468, &var80066468,
	}, {
		// RACE_SKEDAR
		&var800666c0, &var800666c0, &var800666c0, &var800666c0,
		&var800666c0, &var800666c0, &var800666c0, &var800666c0,
		&var800666c0, &var800666c0, &var800666c0, &var800666c0,
		&var800666c0, &var800666c0, &var800666c0, &var800666c0,
		&var800666c0, &var800666c0, &var800666c0, &var800666c0,
		&var800666c0, &var800666c0, &var800666c0, &var800666c0,
		&var800666c0, &var800666c0, &var800666c0, &var800666c0,
		&var800666c0, &var800666c0, &var800666c0, &var800666c0,
	},
};

struct attackanimconfig var800667c8[] = {
	{ ANIM_0007, 27, 0, 0, 0, -1, 35, 75, -1, -1, 31, 75, 0.87252569198608, -0.69802057743073, 0.90742671489716, -0.69802057743073, 1.5,             1.5             },
	{ ANIM_KNEEL_TWO_HANDED_GUN, 24, 0, 0, 0, -1, 46, 98, -1, -1, 41, 98, 0.87252569198608, -0.52351540327072, 1.1342834234238,  -0.69802057743073, 1.6000000238419, 1.6000000238419 },
	{ 0, 0,  0, 0, 0, -1, 0,  0,  0,  0,  0,  0,  0,                0,                 0,                0,                 0,               0               },
};

struct attackanimgroup var800668a0 = { var800667c8, 0xffffffff };

struct attackanimconfig var800668a8[] = {
	{ ANIM_0009, 26, 0, 0, 0, -1, 34, 87, -1, -1, 29, 87, 0.87252569198608, -0.52351540327072, 0.69802057743073, -0.95977824926376, 1.6000000238419, 2 },
	{ 0, 0,  0, 0, 0, -1, 0,  0,  0,  0,  0,  0,  0,                0,                 0,                0,                 0,               0 },
};

struct attackanimgroup var80066938 = { var800668a8, 0xffffffff };

struct attackanimconfig var80066940[] = {
	{ ANIM_000A, 28, 0, 0, 0, -1, 36, 88, -1, -1, 31, 88, 0.87252569198608, -0.69802057743073, 0.87252569198608, -0.43626284599304, 1.6000000238419, 1.5 },
	{ 0, 0,  0, 0, 0, -1, 0,  0,  0,  0,  0,  0,  0,                0,                 0,                0,                 0,               0   },
};

struct attackanimgroup var800669d0 = { var80066940, 0xffffffff };

struct attackanimconfig var800669d8[] = {
	{ ANIM_034A, 20, 0, 0, 0, -1, 25, 50, -1, -1, 10, 50, 0.34901028871536, -0.34901028871536, 0.52351540327072, -0.52351540327072, 1, 1 },
	{ 0, 0,  0, 0, 0, -1, 0,  0,  0,  0,  0,  0,  0,                0,                 0,                0,                 0, 0 },
};

struct attackanimgroup var80066a68 = { var800669d8, 0xffffffff };

struct attackanimgroup *g_KneelHeavyAttackAnims[][32] = {
	{
		&var800668a0, &var800668a0, &var800668a0, &var800668a0,
		&var800668a0, &var800668a0, &var800668a0, &var800668a0,
		&var800668a0, &var800668a0, &var80066938, &var80066938,
		&var80066938, &var80066938, &var80066938, &var80066938,
		&var800669d0, &var800669d0, &var800669d0, &var800669d0,
		&var800669d0, &var800669d0, &var800668a0, &var800668a0,
		&var800668a0, &var800668a0, &var800668a0, &var800668a0,
		&var800668a0, &var800668a0, &var800668a0, &var800668a0,
	}, {
		&var80066a68, &var80066a68, &var80066a68, &var80066a68,
		&var80066a68, &var80066a68, &var80066a68, &var80066a68,
		&var80066a68, &var80066a68, &var80066a68, &var80066a68,
		&var80066a68, &var80066a68, &var80066a68, &var80066a68,
		&var80066a68, &var80066a68, &var80066a68, &var80066a68,
		&var80066a68, &var80066a68, &var80066a68, &var80066a68,
		&var80066a68, &var80066a68, &var80066a68, &var80066a68,
		&var80066a68, &var80066a68, &var80066a68, &var80066a68,
	},
};

struct attackanimconfig var80066b70[] = {
	{ ANIM_KNEEL_SHOOT_RIGHT_HAND, 25, 0, 0, 12, 132, 55, 87, 67, 87, 26, 111, 0.87252569198608, -0.69802057743073, 0.61076802015305, -0.7852731347084, 0, 0 },
	{ ANIM_004C, 26, 0, 0, 8,  89,  31, 63, 41, 51, 21, 80,  0.87252569198608, -0.69802057743073, 0.34901028871536, -1.1342834234238, 0, 0 },
	{ 0, 0,  0, 0, 0,  -1,  0,  0,  0,  0,  0,  0,   0,                0,                 0,                0,                0, 0 },
};

struct attackanimgroup var80066c48 = { var80066b70, 0xffffffff };

struct attackanimconfig var80066c50[] = {
	{ ANIM_004F, 47, 0, 1.5707963705063, 7,  128, 33, 86, 47, 74, 23, 106, 0.87252569198608, -0.52351540327072,  0.52351540327072, -0.7852731347084,  0,   0 },
	{ ANIM_0050, 18, 0, 1.5707963705063, 7,  78,  28, 52, 35, 45, 15, 66,  0.87252569198608, -0.087252572178841, 0.69802057743073, -0.7852731347084,  1.5, 1 },
	{ ANIM_0051, 20, 0, 1.5707963705063, 13, 92,  37, 67, 42, 55, 25, 84,  0.87252569198608, -0.52351540327072,  0.34901028871536, -0.69802057743073, 0,   0 },
	{ 0, 0,  0, 0,               0,  -1,  0,  0,  0,  0,  0,  0,   0,                0,                  0,                0,                 0,   0 },
};

struct attackanimgroup var80066d70 = { var80066c50, 0xffffffff };

struct attackanimconfig var80066d78[] = {
	{ ANIM_004D, 28, 0, 4.7123889923096, 15, 124, 38, 97, 60, 84, 20, 106, 0.87252569198608, -0.69802057743073, 0.52351540327072, -0.87252569198608, 0, 0 },
	{ ANIM_004E, 23, 0, 4.7123889923096, 0,  85,  32, 38, 38, 60, 14, 71,  0.87252569198608, -0.69802057743073, 0.61076802015305, -0.95977824926376, 0, 0 },
	{ 0, 0,  0, 0,               0,  -1,  0,  0,  0,  0,  0,  0,   0,                0,                 0,                0,                 0, 0 },
};

struct attackanimgroup var80066e50 = { var80066d78, 0xffffffff };

struct attackanimconfig var80066e58[] = {
	{ ANIM_034A, 20, 0, 0, 0, -1, 25, 50, -1, -1, 10, 50, 0.34901028871536, -0.34901028871536, 0.52351540327072, -0.52351540327072, 1, 1 },
	{ 0, 0,  0, 0, 0, -1, 0,  0,  0,  0,  0,  0,  0,                0,                 0,                0,                 0, 0 },
};

struct attackanimgroup var80066ee8 = { var80066e58, 0xffffffff };

struct attackanimgroup *g_KneelLightAttackAnims[][32] = {
	{
		&var80066c48, &var80066c48, &var80066c48, &var80066c48,
		&var80066c48, &var80066c48, &var80066c48, &var80066c48,
		&var80066c48, &var80066c48, &var80066d70, &var80066d70,
		&var80066d70, &var80066d70, &var80066d70, &var80066d70,
		&var80066e50, &var80066e50, &var80066e50, &var80066e50,
		&var80066e50, &var80066e50, &var80066c48, &var80066c48,
		&var80066c48, &var80066c48, &var80066c48, &var80066c48,
		&var80066c48, &var80066c48, &var80066c48, &var80066c48,
	}, {
		&var80066ee8, &var80066ee8, &var80066ee8, &var80066ee8,
		&var80066ee8, &var80066ee8, &var80066ee8, &var80066ee8,
		&var80066ee8, &var80066ee8, &var80066ee8, &var80066ee8,
		&var80066ee8, &var80066ee8, &var80066ee8, &var80066ee8,
		&var80066ee8, &var80066ee8, &var80066ee8, &var80066ee8,
		&var80066ee8, &var80066ee8, &var80066ee8, &var80066ee8,
		&var80066ee8, &var80066ee8, &var80066ee8, &var80066ee8,
		&var80066ee8, &var80066ee8, &var80066ee8, &var80066ee8,
	},
};

struct attackanimconfig var80066ff0[] = {
	{ ANIM_0074, 22, 0, 0, 10, 111, 34, 87, -1, -1, 17, 104, 0.87252569198608, -0.69802057743073, 0.61076802015305, -0.7852731347084, 0, 0 },
	{ ANIM_0077, 25, 0, 0, 9,  92,  33, 62, -1, -1, 18, 69,  0.87252569198608, -0.69802057743073, 0.61076802015305, -0.7852731347084, 0, 0 },
	{ 0, 0,  0, 0, 0,  -1,  0,  0,  0,  0,  0,  0,   0,                0,                 0,                0,                0, 0 },
};

struct attackanimgroup var800670c8 = { var80066ff0, 0xffffffff };

struct attackanimconfig var800670d0[] = {
	{ ANIM_0075, 28, 0, 1.5707963705063, 15, 108, 34, 73, -1, -1, 17, 93, 0.87252569198608, -0.69802057743073, 0.52351540327072, -0.7852731347084, 0,   0 },
	{ ANIM_0078, 19, 0, 1.5707963705063, 3,  95,  30, 64, -1, -1, 14, 71, 0.87252569198608, -0.69802057743073, 0.52351540327072, -0.7852731347084, 1.5, 1 },
	{ 0, 0,  0, 0,               0,  -1,  0,  0,  0,  0,  0,  0,  0,                0,                 0,                0,                0,   0 },
};

struct attackanimgroup var800671a8 = { var800670d0, 0xffffffff };

struct attackanimconfig var800671b0[] = {
	{ ANIM_0076, 31, 0, 4.7123889923096, 14, 111, 40, 83, -1, -1, 21, 94, 0.87252569198608, -0.69802057743073, 0.52351540327072, -0.7852731347084, 0, 0 },
	{ ANIM_0079, 26, 0, 4.7123889923096, 7,  89,  34, 60, -1, -1, 20, 68, 0.87252569198608, -0.69802057743073, 0.52351540327072, -0.7852731347084, 0, 0 },
	{ 0, 0,  0, 0,               0,  -1,  0,  0,  0,  0,  0,  0,  0,                0,                 0,                0,                0, 0 },
};

struct attackanimgroup var80067288 = { var800671b0, 0xffffffff };

struct attackanimconfig var80067290[] = {
	{ ANIM_034A, 20, 0, 0, 0, -1, 25, 50, -1, -1, 10, 50, 0.34901028871536, -0.34901028871536, 0.52351540327072, -0.52351540327072, 1, 1 },
	{ 0, 0,  0, 0, 0, -1, 0,  0,  0,  0,  0,  0,  0,                0,                 0,                0,                 0, 0 },
};

struct attackanimgroup var80067320 = { var80067290, 0xffffffff };

struct attackanimgroup *g_KneelDualAttackAnims[][32] = {
	{
		&var800670c8, &var800670c8, &var800670c8, &var800670c8,
		&var800670c8, &var800670c8, &var800670c8, &var800670c8,
		&var800670c8, &var800670c8, &var800671a8, &var800671a8,
		&var800671a8, &var800671a8, &var800671a8, &var800671a8,
		&var80067288, &var80067288, &var80067288, &var80067288,
		&var80067288, &var80067288, &var800670c8, &var800670c8,
		&var800670c8, &var800670c8, &var800670c8, &var800670c8,
		&var800670c8, &var800670c8, &var800670c8, &var800670c8,
	}, {
		&var80067320, &var80067320, &var80067320, &var80067320,
		&var80067320, &var80067320, &var80067320, &var80067320,
		&var80067320, &var80067320, &var80067320, &var80067320,
		&var80067320, &var80067320, &var80067320, &var80067320,
		&var80067320, &var80067320, &var80067320, &var80067320,
		&var80067320, &var80067320, &var80067320, &var80067320,
		&var80067320, &var80067320, &var80067320, &var80067320,
		&var80067320, &var80067320, &var80067320, &var80067320,
	},
};

struct attackanimconfig g_RollAttackAnims[] = {
	{ ANIM_000B, 76, 0, 0,               20, -1,  98, 161, -1, -1, 93, 161, 0.87252569198608, -0.52351540327072,  0.69802057743073, -0.69802057743073, 1.7000000476837, 2               },
	{ ANIM_000C, 58, 0, 0,               10, -1,  77, 104, -1, -1, 72, 104, 0.87252569198608, -0.34901028871536,  0.61076802015305, -0.69802057743073, 1.5499999523163, 1.5             },
	{ ANIM_000D, 61, 0, 0,               10, -1,  83, 128, -1, -1, 78, 128, 0.87252569198608, -0.52351540327072,  0.87252569198608, -0.52351540327072, 1.2000000476837, 1.2999999523163 },
	{ ANIM_0027, 63, 0, 0,               10, -1,  73, 114, -1, -1, 68, 114, 0.87252569198608, -0.52351540327072,  0.61076802015305, -0.61076802015305, 1.6499999761581, 1.5             },
	{ ANIM_000B, 76, 0, 0,               20, 76,  98, 161, -1, -1, 93, 161, 0.87252569198608, -0.52351540327072,  0.69802057743073, -0.69802057743073, 1.7000000476837, 2               },
	{ ANIM_000C, 58, 0, 0,               10, 63,  77, 104, -1, -1, 72, 104, 0.87252569198608, -0.34901028871536,  0.61076802015305, -0.69802057743073, 1.5499999523163, 1.5             },
	{ ANIM_000D, 61, 0, 0,               10, 56,  83, 128, -1, -1, 78, 128, 0.87252569198608, -0.52351540327072,  0.87252569198608, -0.52351540327072, 1.2000000476837, 1.2999999523163 },
	{ ANIM_0027, 63, 0, 0,               10, 50,  73, 114, -1, -1, 68, 114, 0.87252569198608, -0.52351540327072,  0.61076802015305, -0.61076802015305, 1.6499999761581, 1.5             },
	{ ANIM_0045, 0,  0, 0,               7,  64,  19, 51,  24, 46, 14, 58,  0.87252569198608, -0.69802057743073,  0.52351540327072, -0.7852731347084,  0,               0               },
	{ ANIM_004A, 0,  0, 1.5707963705063, 14, 76,  26, 31,  31, 38, 15, 49,  0.87252569198608, -0.69802057743073,  0.52351540327072, -1.0470308065414,  0,               0               },
	{ ANIM_004C, 26, 0, 0,               25, 89,  41, 63,  41, 51, 21, 80,  0.87252569198608, -0.69802057743073,  0.34901028871536, -1.1342834234238,  0,               0               },
	{ ANIM_0050, 18, 0, 1.5707963705063, 11, 78,  33, 52,  35, 45, 15, 66,  0.87252569198608, -0.087252572178841, 0.69802057743073, -0.7852731347084,  1.5,             1               },
	{ ANIM_007A, 26, 0, 0,               7,  92,  28, 68,  -1, -1, 11, 73,  0.87252569198608, -0.69802057743073,  0.69802057743073, -0.69802057743073, 0,               0               },
	{ ANIM_007B, 26, 0, 1.5707963705063, 9,  112, 38, 87,  -1, -1, 19, 98,  0.87252569198608, -0.69802057743073,  0.43626284599304, -0.43626284599304, 0,               0               },
	{ ANIM_0074, 22, 0, 0,               10, 11,  34, 87,  -1, -1, 17, 104, 0.87252569198608, -0.69802057743073,  0.61076802015305, -0.7852731347084,  0,               0               },
	{ ANIM_0075, 28, 0, 1.5707963705063, 15, 108, 34, 73,  -1, -1, 17, 93,  0.87252569198608, -0.69802057743073,  0.52351540327072, -0.7852731347084,  0,               0               },
	{ ANIM_007A, 26, 0, 0,               7,  92,  28, 68,  -1, -1, 11, 73,  0.87252569198608, -0.69802057743073,  0.69802057743073, -0.69802057743073, 0,               0               },
	{ ANIM_007D, 25, 0, 1.5707963705063, 10, 112, 32, 86,  -1, -1, 19, 97,  0.87252569198608, -0.69802057743073,  0.43626284599304, -0.43626284599304, 0,               0               },
	{ ANIM_0077, 25, 0, 0,               9,  92,  33, 62,  -1, -1, 18, 69,  0.87252569198608, -0.69802057743073,  0.61076802015305, -0.7852731347084,  0,               0               },
	{ ANIM_0078, 19, 0, 1.5707963705063, 3,  95,  30, 64,  -1, -1, 14, 71,  0.87252569198608, -0.69802057743073,  0.52351540327072, -0.7852731347084,  1.5,             1               },
	{ 0, 0,  0, 0,               0,  -1,  0,  0,   0,  0,  0,  0,   0,                0,                  0,                0,                 0,               0               },
};

struct attackanimconfig g_WalkAttackAnims[] = {
	{ ANIM_0030, 0, 0, 0, 0, -1, 0, 0, -1, -1, 0, 0, 0.87252569198608, -0.52351540327072, 0.52351540327072, -0.52351540327072, 1.3999999761581, 1.2999999523163 },
	{ ANIM_0031, 0, 0, 0, 0, -1, 0, 0, -1, -1, 0, 0, 0.87252569198608, -0.52351540327072, 0.52351540327072, -0.52351540327072, 1.1000000238419, 1.2000000476837 },
	{ ANIM_0052, 0, 0, 0, 0, -1, 0, 0, -1, -1, 0, 0, 0.87252569198608, -0.52351540327072, 0.52351540327072, -0.52351540327072, 0,               0               },
	{ ANIM_0055, 0, 0, 0, 0, -1, 0, 0, -1, -1, 0, 0, 0.87252569198608, -0.52351540327072, 0.52351540327072, -0.52351540327072, 0,               0               },
	{ ANIM_006C, 0, 0, 0, 0, -1, 0, 0, -1, -1, 0, 0, 0.87252569198608, -0.52351540327072, 0.52351540327072, -0.52351540327072, 0,               0               },
	{ ANIM_006E, 0, 0, 0, 0, -1, 0, 0, -1, -1, 0, 0, 0.87252569198608, -0.52351540327072, 0.52351540327072, -0.52351540327072, 0,               0               },
	{ ANIM_006D, 0, 0, 0, 0, -1, 0, 0, -1, -1, 0, 0, 0.87252569198608, -0.52351540327072, 0.52351540327072, -0.52351540327072, 0,               0               },
	{ ANIM_006F, 0, 0, 0, 0, -1, 0, 0, -1, -1, 0, 0, 0.87252569198608, -0.52351540327072, 0.52351540327072, -0.52351540327072, 0,               0               },
#ifndef PLATFORM_N64
	// ensure this is terminated
	{ 0, 0, 0, 0,               0, -1, 0, 0, 0,  0,  0, 0, 0,                0,                 0,                0,                 0, 0 },
#endif
};

struct attackanimconfig var80067c50[] = {
	{ ANIM_0057, 0, 0, 1.5707963705063, 0, -1, 0, 0, -1, -1, 0, 0, 0.87252569198608, -0.52351540327072, 0.52351540327072, -0.52351540327072, 0, 0 },
	{ ANIM_0056, 0, 0, 4.7123889923096, 0, -1, 0, 0, -1, -1, 0, 0, 0.87252569198608, -0.52351540327072, 0.52351540327072, -0.52351540327072, 0, 0 },
	{ 0, 0, 0, 0,               0, -1, 0, 0, 0,  0,  0, 0, 0,                0,                 0,                0,                 0, 0 },
};

struct attackanimconfig var80067d28[] = { ANIM_SNIPING_GETDOWN, 0, 0, 0, 0, 236, 0, 0, -1, -1, 0, 0, 0.87252569198608, -0.52351540327072, 0.52351540327072, -0.52351540327072, 0, 0 };

struct attackanimconfig var80067d70[] = {
	{ ANIM_SNIPING_ONGROUND, 0, 0, 0, 0, -1, 0, 0, -1, -1, 0, 0, 0.87252569198608, -0.52351540327072, 0.52351540327072, -0.52351540327072, 0, 0 },
	{ ANIM_SNIPING_GETUP, 0, 0, 0, 0, -1, 0, 0, -1, -1, 0, 0, 0.87252569198608, -0.52351540327072, 0.52351540327072, -0.52351540327072, 0, 0 },
	{ 0, 0, 0, 0, 0, -1, 0, 0, 0,  0,  0, 0, 0,                0,                 0,                0,                 0, 0 },
};

struct attackanimgroup var80067e48 = { var80067d28, 0xffffffff };

struct attackanimgroup *g_LieAttackAnims = &var80067e48;

u32 g_StageFlags = 0;
#ifndef PLATFORM_N64
u32 g_NetCoopLocalStageFlags = 0; // co-op client: flags this client's scripts set locally (OR-merged over the host mirror)
#endif

struct chrdata *g_BgChrs = NULL;
s16 *g_BgChrnums = NULL;
s32 g_NumBgChrs = 0;

s16 *g_TeamList = NULL;
s16 *g_SquadronList = NULL;

struct var80067e6c var80067e6c[] = {
	{ ANIM_0028, 0 },
	{ ANIM_RUNNING_TWOHANDGUN, 0 },
	{ ANIM_0029, 0 },
	{ ANIM_006B, 0 },
	{ ANIM_RUNNING_ONEHANDGUN, 0 },
	{ ANIM_005A, 0 },
	{ ANIM_0072, 0 },
	{ ANIM_0073, 0 },
	{ ANIM_005A, 0 },
	{ ANIM_006C, 0 },
	{ ANIM_0030, 0 },
	{ ANIM_0031, 0 },
	{ ANIM_0052, 0 },
	{ ANIM_0055, 0 },
	{ ANIM_006E, 0 },
	{ ANIM_006F, 0 },
	{ ANIM_0057, 0 },
	{ ANIM_0056, 0 },
	{ ANIM_006D, 0 },
	{ ANIM_RUNNING_ONEHANDGUN, 0 },
	{ ANIM_020A, 0 },
	{ ANIM_020D, 0 },
	{ ANIM_01F9, 0 },
	{ ANIM_01F8, 0 },
	{ ANIM_021D, 0 },
	{ ANIM_0016, 0 },
	{ ANIM_0018, 0 },
	{ ANIM_001B, 0 },
	{ ANIM_001D, 0 },
	{ ANIM_001E, 0 },
	{ ANIM_005C, 0 },
	{ ANIM_005D, 0 },
	{ ANIM_005E, 0 },
	{ ANIM_005F, 0 },
	{ -1 },
};

struct var80067e6c var80067f84[] = {
	{ ANIM_0392, 0 },
	{ ANIM_0393, 0 },
	{ ANIM_SKEDAR_RUNNING, 0 },
	{ -1 },
};

struct var80067e6c var80067fa4[] = {
	{ ANIM_015F, 0 },
	{ ANIM_0160, 0 },
	{ -1 },
};

struct var80067e6c var80067fbc[] = {
	{ ANIM_015F, 0 },
	{ -1 },
};

struct var80067e6c var80067fcc[] = {
	{ ANIM_0238, 0 },
	{ -1 },
};

struct var80067e6c *var80067fdc[] = {
	var80067e6c,
	var80067f84,
	var80067fa4,
	var80067fbc,
	var80067fcc,
};

f32 func0f02dff0(s16 animnum)
{
	s32 i;

	for (i = 0; i < ARRAYCOUNT(var80067fdc); i++) {
		s32 j = 0;
		s16 thisanimnum = var80067fdc[i][j].animnum;

		while (thisanimnum >= 0) {
			if (thisanimnum == animnum) {
				return var80067fdc[i][j].value;
			}

			j++;
			thisanimnum = var80067fdc[i][j].animnum;
		}
	}

	return 1.0f;
}

bool chrGoPosIsWaiting(struct chrdata *chr)
{
	static s16 list1[] = { ANIM_TWO_GUN_HOLD, ANIM_006A, -1 };
	static s16 list2[] = { ANIM_00C0, -1 };
	static s16 list3[] = { ANIM_013E, -1 };
	static s16 list4[] = { ANIM_013E, -1 };
	static s16 list5[] = { ANIM_0237, -1 };
	static s16 *waitableanims[] = { list1, list2, list3, list4, list5 };

	if (chr->aibot) {
		if (chr->actiontype == ACT_STAND
				|| (chr->actiontype == ACT_GOPOS && (chr->act_gopos.flags & GOPOSFLAG_WAITING))) {
			return true;
		}
	} else {
		s16 animnum = modelGetAnimNum(chr->model);
		s32 i;

		for (i = 0; i < ARRAYCOUNT(waitableanims); i++) {
			s16 thisanimnum;
			s32 j;

			for (j = 0; (thisanimnum = waitableanims[i][j]) >= 0; j++) {
				if (thisanimnum == animnum) {
					return true;
				}
			}
		}
	}

	return false;
}

bool weaponIsOneHanded(struct prop *prop)
{
	if (prop) {
		struct weaponobj *weapon = prop->weapon;
		return weaponHasFlag(weapon->weaponnum, WEAPONFLAG_ONEHANDED);
	}

	return false;
}

/**
 * Returns a value between min and max based on the chr's speedrating property.
 *
 * chr->speedrating is between 0 and 100. The result is scaled between min and
 * max accordingly.
 *
 * This function also applies the PD mode reaction speed, but the PD mode
 * reaction speed is always zero because PD doesn't have it in the settings.
 * It was used in GE but disabled in PD.
 */
f32 chrGetRangedSpeed(struct chrdata *chr, f32 min, f32 max)
{
	f32 speedrating = chr->speedrating;

	speedrating = pdmodeGetEnemyReactionSpeed() * (100.0f - speedrating) + speedrating;

	return (max - min) * speedrating * 0.01f + min;
}

/**
 * Calculates a percentage of how slow the chr is.
 *
 * percentage is expected to be between 0 and 100.
 * chr->speedrating is between 0 and 100.
 *
 * This function takes the difference between the speedrating and the max (100),
 * then multiplies that amount by the given percentage and returns it.
 *
 * For example, if the chr's speedrating is 10 (out of 100) and the given
 * percentage is 50, the result will be 45.
 */
s32 chrGetPercentageOfSlowness(struct chrdata *chr, s32 percentage)
{
	s32 speedrating = chr->speedrating;
	s32 extra = pdmodeGetEnemyReactionSpeed() * (100 - speedrating);

	speedrating = extra + speedrating;

	return (100 - speedrating) * percentage / 100;
}

f32 chrGetRangedArghSpeed(struct chrdata *chr, f32 min, f32 max)
{
	f32 arghrating = chr->arghrating;

	arghrating = pdmodeGetEnemyReactionSpeed() * (100.0f - arghrating) + arghrating;

	return (max - min) * arghrating * 0.01f + min;
}

f32 chrGetAttackEntityRelativeAngle(struct chrdata *chr, s32 attackflags, s32 entityid)
{
	f32 angle;
	struct coord pos;
	RoomNum rooms[8];

	if (attackflags & ATTACKFLAG_AIMFORWARD) {
		return 0;
	}

	if (attackflags & ATTACKFLAG_AIMATDIRECTION) {
		angle = entityid * (M_BADTAU / 65536);
		angle -= chrGetInverseTheta(chr);

		if (angle < 0) {
			angle += M_BADTAU;
		}

		return angle;
	}

	chrGetAttackEntityPos(chr, attackflags, entityid, &pos, rooms);

	return chrGetAngleToPos(chr, &pos);
}

f32 chrGetAttackEntityDistance(struct chrdata *chr, u32 attackflags, s32 entityid)
{
	if (attackflags & ATTACKFLAG_AIMATTARGET) {
		return chrGetDistanceToTarget(chr);
	}

	if (attackflags & ATTACKFLAG_AIMATCHR) {
		return chrGetDistanceToChr(chr, entityid);
	}

	if (attackflags & ATTACKFLAG_AIMATPAD) {
		return chrGetDistanceToPad(chr, entityid);
	}

	return 0;
}

void func0f02e3dc(struct coord *a, struct coord *b,struct coord *c, struct coord *d, struct coord *dst)
{
	f32 value = d->z * (b->x - a->x) - (b->z - a->z) * d->x;

	if (value != 0) {
		f32 tmp = ((b->z - a->z) * (c->x - a->x) + (a->z - c->z) * (b->x - a->x)) / value;
		dst->x = d->x * tmp + c->x;
		dst->y = d->y * tmp + c->y;
		dst->z = d->z * tmp + c->z;
	} else if (d->x == 0 && d->z == 0) {
		dst->x = c->x;
		dst->y = c->y;
		dst->z = c->z;
	} else {
		dst->x = a->x;
		dst->y = a->y;
		dst->z = a->z;
	}
}

void func0f02e4f8(struct coord *arg0, struct coord *arg1, struct coord *dst)
{
	struct coord sp2c;
	struct coord sp20;

#if VERSION >= VERSION_PAL_FINAL
	cdGetEdge(&sp2c, &sp20, 2298, "chr/chraction.c");
#elif VERSION >= VERSION_NTSC_1_0
	cdGetEdge(&sp2c, &sp20, 2298, "chraction.c");
#else
	cdGetEdge(&sp2c, &sp20, 2338, "chraction.c");
#endif

	func0f02e3dc(&sp2c, &sp20, arg0, arg1, dst);
}

f32 func0f02e550(struct prop *prop, f32 arg1, f32 arg2, u32 cdtypes, f32 ymax, f32 ymin)
{
	struct coord sp5c;
	struct coord sp50;
	struct chrdata *chr = prop->chr;
	f32 result;
	struct coord sp3c;
	f32 xdiff;
	f32 zdiff;

	sp5c.x = sinf(arg1);
	sp5c.y = 0;
	sp5c.z = cosf(arg1);

	sp50.x = prop->pos.x + sp5c.f[0] * arg2;
	sp50.y = prop->pos.y;
	sp50.z = prop->pos.z + sp5c.f[2] * arg2;

	chrSetPerimEnabled(chr, false);

	if (cdExamCylMove03(&prop->pos, prop->rooms, &sp50, cdtypes, 1, ymax - prop->pos.y, ymin - prop->pos.y) != CDRESULT_COLLISION) {
		result = arg2;
	} else {
#if VERSION >= VERSION_PAL_FINAL
		cdGetPos(&sp3c, 2377, "chr/chraction.c");
#elif VERSION >= VERSION_NTSC_1_0
		cdGetPos(&sp3c, 2377, "chraction.c");
#else
		cdGetPos(&sp3c, 2417, "chraction.c");
#endif

		xdiff = sp3c.x - prop->pos.x;
		zdiff = sp3c.z - prop->pos.z;

		result = sqrtf(xdiff * xdiff + zdiff * zdiff);
	}

	chrSetPerimEnabled(chr, true);

	return result;
}

f32 func0f02e684(struct prop *prop, f32 arg1, f32 arg2)
{
	f32 ymax;
	f32 ymin;
	f32 radius;

	chrGetBbox(prop, &radius, &ymax, &ymin);

	return func0f02e550(prop, arg1, arg2, CDTYPE_ALL, ymax, ymin);
}

void chrChooseStandAnimation(struct chrdata *chr, f32 mergetime)
{
	struct prop *leftgun = chrGetHeldProp(chr, HAND_LEFT);
	struct prop *rightgun = chrGetHeldProp(chr, HAND_RIGHT);
	s32 race = CHRRACE(chr);
	s32 prevanimnum = modelGetAnimNum(chr->model);

	if (chr->actiontype == ACT_GOPOS) {
		chr->act_gopos.flags |= GOPOSFLAG_WAITING;
	}

	if (chr->aibot) {
		return;
	}

	if (race == RACE_EYESPY) {
		modelSetAnimation(chr->model, ANIM_013E, 0, 0, 0, mergetime);
	} else if (race == RACE_HUMAN) {
		if (prevanimnum == ANIM_SNIPING_GETDOWN
				|| prevanimnum == ANIM_SNIPING_GETUP
				|| prevanimnum == ANIM_SNIPING_ONGROUND) {
			modelSetAnimation(chr->model, ANIM_SNIPING_GETUP, chr->model->anim->flip, -1, chrGetRangedSpeed(chr, 0.5, 0.8), 16);
		} else if ((leftgun && rightgun) || (!leftgun && !rightgun)
				|| weaponIsOneHanded(leftgun)
				|| weaponIsOneHanded(rightgun)) {
			modelSetAnimation(chr->model, ANIM_006A, rngRandom() % 2, 0, 0.25, mergetime);
			modelSetAnimLooping(chr->model, 0, 16);
		} else if (rightgun || leftgun) {
			modelSetAnimation(chr->model, ANIM_TWO_GUN_HOLD, leftgun != NULL, 0, 0.25, mergetime);
			modelSetAnimLooping(chr->model, 0, 16);
			modelSetAnimEndFrame(chr->model, 120);
		}
	} else if (race == RACE_SKEDAR) {
		modelSetAnimation(chr->model, ANIM_00C0, rngRandom() % 2, 0, 0.5, mergetime);
	} else if (race == RACE_DRCAROLL) {
		modelSetAnimation(chr->model, ANIM_013E, 0, 0, 0.5, mergetime);
	} else if (race == RACE_ROBOT) {
		modelSetAnimation(chr->model, ANIM_0237, 0, 0, 0.5, mergetime);
	}
}

void func0f02e9a0(struct chrdata *chr, f32 mergetime)
{
	f32 limit = 127;
	f32 fsleep;

	chrStopFiring(chr);
	chr->actiontype = ACT_STAND;
	chr->act_stand.prestand = false;
	chr->act_stand.flags = 0;
	chr->act_stand.entityid = 0;
	chr->act_stand.reaim = 0;
	chr->act_stand.turning = TURNSTATE_OFF;
	chr->act_stand.checkfacingwall = false;
	chr->act_stand.wallcount = rngRandom() % 120 + 180; // 180 to 299
	chr->act_stand.mergetime = mergetime;
	chr->act_stand.playwalkanim = false;

	fsleep = mergetime;

	if (chr->model->anim->playspeed != PALUPF(1.0f)) {
		fsleep *= PALUPF(1.0f) / chr->model->anim->playspeed;
	}

	if (fsleep > limit) {
		fsleep = limit;
	}

	chr->sleep = fsleep;

	if (modelIsAnimMerging(chr->model) && !chr->aibot) {
		chr->hidden |= CHRHFLAG_NEEDANIM;
	} else {
		chrChooseStandAnimation(chr, mergetime);
		chr->hidden &= ~CHRHFLAG_NEEDANIM;
	}
}

void chrStand(struct chrdata *chr)
{
	s32 race = CHRRACE(chr);
	f32 result;

	if (race != RACE_EYESPY) {
		chrStopFiring(chr);

		if (race == RACE_HUMAN && chr->actiontype == ACT_KNEEL) {
			chrStopFiring(chr);

			chr->actiontype = ACT_STAND;
			chr->act_stand.prestand = true;
			chr->act_stand.flags = 0;
			chr->act_stand.entityid = 0;
			chr->act_stand.reaim = 0;
			chr->act_stand.turning = TURNSTATE_OFF;
			chr->act_stand.checkfacingwall = false;
			chr->act_stand.wallcount = rngRandom() % 120 + 180;
			chr->sleep = 0;
			chr->act_stand.playwalkanim = false;

			if (chr->aibot == NULL) {
				if (modelGetAnimNum(chr->model) == ANIM_KNEEL_SHOOT_RIGHT_HAND) {
					result = chrGetRangedSpeed(chr, 0.5, 0.8);
					modelSetAnimation(chr->model, ANIM_KNEEL_SHOOT_RIGHT_HAND,
							chr->model->anim->flip, 109, result, 16);
					modelSetAnimEndFrame(chr->model, 140);
				} else {
					result = chrGetRangedSpeed(chr, 0.5, 0.8);
					modelSetAnimation(chr->model, ANIM_KNEEL_TWO_HANDED_GUN,
							chr->model->anim->flip, 120, result, 16);
					modelSetAnimEndFrame(chr->model, 151);
				}
			}
		} else if (race == RACE_DRCAROLL || race == RACE_ROBOT) {
			chr->actiontype = ACT_STAND;
			chr->act_stand.prestand = true;
			chr->act_stand.flags = 0;
			chr->act_stand.entityid = 0;
			chr->act_stand.reaim = 0;
			chr->act_stand.turning = TURNSTATE_OFF;
			chr->act_stand.checkfacingwall = false;
			chr->act_stand.wallcount = rngRandom() % 120 + 180;
			chr->sleep = 0;
			chr->act_stand.playwalkanim = false;

			func0f02e9a0(chr, 16);
		} else {
			func0f02e9a0(chr, 16);
		}
	}
}

bool chrFaceCover(struct chrdata *chr)
{
	struct cover cover;

	if (!coverUnpack(chr->cover, &cover)) {
		return false;
	}

	chrStand(chr);
	chr->act_stand.reaim = 0;
	chr->act_stand.flags = ATTACKFLAG_AIMATDIRECTION;
	chr->act_stand.turning = TURNSTATE_TURNING;
	//chr->act_stand.entityid = atan2f(-cover.look->x, -cover.look->z) * (0x4000 / BADDEG2RAD(90));
	chr->act_stand.entityid = atan2f(-cover.look->x, -cover.look->z) * 10432.039f;

	return true;
}

void func0f02ed28(struct chrdata *chr, f32 mergetime)
{
	func0f02e9a0(chr, mergetime);

	chr->act_stand.checkfacingwall = true;
}

void chrStop(struct chrdata *chr)
{
	chrStand(chr);

	chr->act_stand.checkfacingwall = true;
}

void chrKneelChooseAnimation(struct chrdata *chr)
{
	struct prop *leftgun = chrGetHeldProp(chr, HAND_LEFT);
	struct prop *rightgun = chrGetHeldProp(chr, HAND_RIGHT);

	if (chr->aibot == NULL) {
		if ((leftgun && rightgun)
				|| (!leftgun && !rightgun)
				|| weaponIsOneHanded(leftgun)
				|| weaponIsOneHanded(rightgun)) {
			bool flip = rngRandom() % 2;
			modelSetAnimation(chr->model, ANIM_KNEEL_SHOOT_RIGHT_HAND, flip, 0, chrGetRangedSpeed(chr, 0.5, 0.8), 16);
			modelSetAnimEndFrame(chr->model, 28);
		} else if (rightgun || leftgun) {
			modelSetAnimation(chr->model, ANIM_KNEEL_TWO_HANDED_GUN, leftgun != NULL, 0, chrGetRangedSpeed(chr, 0.5, 0.8), 16);
			modelSetAnimEndFrame(chr->model, 27);
		}
	}
}

void chrKneel(struct chrdata *chr)
{
	chrStopFiring(chr);
	chr->actiontype = ACT_KNEEL;
	chr->sleep = 0;

	if (modelIsAnimMerging(chr->model)) {
		chr->hidden |= CHRHFLAG_NEEDANIM;
	} else {
		chrKneelChooseAnimation(chr);
		chr->hidden &= ~CHRHFLAG_NEEDANIM;
	}
}

void chrStartAlarmChooseAnimation(struct chrdata *chr)
{
	struct prop *leftgun = chrGetHeldProp(chr, HAND_LEFT);
	struct prop *rightgun = chrGetHeldProp(chr, HAND_RIGHT);
	bool flip = false;

	if (leftgun && !rightgun) {
		flip = true;
	} else if ((leftgun && rightgun) || (!leftgun && !rightgun)) {
		flip = rngRandom() % 2;
	}

	modelSetAnimation(chr->model, ANIM_TALKING_003D, flip, 40, 1, 16);
	modelSetAnimEndFrame(chr->model, 82);
}

void chrStartAlarm(struct chrdata *chr)
{
	chrStopFiring(chr);
	chr->actiontype = ACT_STARTALARM;
	chr->sleep = 0;

	if (modelIsAnimMerging(chr->model)) {
		chr->hidden |= CHRHFLAG_NEEDANIM;
	} else {
		chrStartAlarmChooseAnimation(chr);
		chr->hidden &= ~CHRHFLAG_NEEDANIM;
	}
}

void chrThrowGrenadeChooseAnimation(struct chrdata *chr)
{
	u32 rand = rngRandom();

	if (chr->act_throwgrenade.needsequip) {
		if (rand % 3 == 0) {
			modelSetAnimation(chr->model, ANIM_THROWGRENADE_CROUCHING, chr->act_throwgrenade.hand != 0, 0, chrGetRangedSpeed(chr, 0.5, 1.2), 16);
		} else if (rand % 3 == 1) {
			modelSetAnimation(chr->model, ANIM_THROWGRENADE_NOPIN, chr->act_throwgrenade.hand != 0, 0, chrGetRangedSpeed(chr, 0.5, 1.2), 16);
		} else {
			modelSetAnimation(chr->model, ANIM_THROWGRENADE_STANDING, chr->act_throwgrenade.hand != 0, 0, chrGetRangedSpeed(chr, 0.5, 1.2), 16);
		}
	} else {
		if (rand % 3 == 0) {
			modelSetAnimation(chr->model, ANIM_THROWGRENADE_CROUCHING, chr->act_throwgrenade.hand != 0, 5, chrGetRangedSpeed(chr, 0.5, 1.2), 16);
		} else if (rand % 3 == 1) {
			modelSetAnimation(chr->model, ANIM_THROWGRENADE_NOPIN, chr->act_throwgrenade.hand != 0, 6, chrGetRangedSpeed(chr, 0.5, 1.2), 16);
		} else {
			modelSetAnimation(chr->model, ANIM_THROWGRENADE_STANDING, chr->act_throwgrenade.hand != 0, 84, chrGetRangedSpeed(chr, 0.5, 1.2), 16);
		}
	}

	modelSetAnimEndFrame(chr->model, -1);
}

void chrThrowGrenade(struct chrdata *chr, s32 hand, s32 needsequip)
{
	chrStopFiring(chr);
	chr->actiontype = ACT_THROWGRENADE;
	chr->act_throwgrenade.hand = hand;
	chr->act_throwgrenade.needsequip = needsequip;
	chr->sleep = 0;

	if (modelIsAnimMerging(chr->model)) {
		chr->hidden |= CHRHFLAG_NEEDANIM;
	} else {
		chrThrowGrenadeChooseAnimation(chr);
		chr->hidden &= ~CHRHFLAG_NEEDANIM;
	}
}

void chrSurprisedChooseAnimation(struct chrdata *chr)
{
	if (chr->act_surprised.type == 1) {
		struct prop *leftgun = chrGetHeldProp(chr, HAND_LEFT);
		struct prop *rightgun = chrGetHeldProp(chr, HAND_RIGHT);
		s32 flip = 0;

		if (leftgun != NULL && rightgun == NULL) {
			flip = 1;
		} else if ((leftgun != NULL && rightgun != NULL) || (leftgun == NULL && rightgun == NULL)) {
			flip = rngRandom() & 1;
		}

		modelSetAnimation(chr->model, ANIM_003F, flip, 10, chrGetRangedSpeed(chr, 0.6f, 0.96000003f), 16);
		modelSetAnimEndFrame(chr->model, 52);
	} else if (chr->act_surprised.type == 2) {
		modelSetAnimation(chr->model, ANIM_SURRENDER_002E, rngRandom() & 1, 0, chrGetRangedSpeed(chr, 0.35f, 0.56f), 16);
		modelSetAnimEndFrame(chr->model, 7);
	} else {
		u32 part = rngRandom() % 3;
		modelSetAnimation(chr->model, ANIM_0040, rngRandom() & 1, 17, 0.6f, 16);

		if (part == 0) {
			modelSetAnimEndFrame(chr->model, chrGetRangedSpeed(chr, 38, 8));
		} else if (part == 1) {
			modelSetAnimEndFrame(chr->model, chrGetRangedSpeed(chr, 66, 8));
		} else {
			modelSetAnimEndFrame(chr->model, chrGetRangedSpeed(chr, 96, 8));
		}
	}
}

void chrDoSurprisedOneHand(struct chrdata *chr)
{
	struct prop *prop = chrGetTargetProp(chr);
	f32 angle = chrGetAngleToPos(chr, &prop->pos);

	// BADDEG2RAD(10) doesn't match due to float precision :(
	if (angle < 0.17450514435768f || angle > BADDEG2RAD(350)) {
		chrStopFiring(chr);
		chr->actiontype = ACT_SURPRISED;
		chr->act_surprised.type = 1;
		chr->sleep = 0;

		if (modelIsAnimMerging(chr->model)) {
			chr->hidden |= CHRHFLAG_NEEDANIM;
		} else {
			chrSurprisedChooseAnimation(chr);
			chr->hidden &= ~CHRHFLAG_NEEDANIM;
		}
	} else if (!chrIsStopped(chr)) {
		chrStand(chr);
	}
}

void chrDoSurprisedSurrender(struct chrdata *chr)
{
	chrStopFiring(chr);
	chr->actiontype = ACT_SURPRISED;
	chr->act_surprised.type = 2;
	chr->sleep = 0;

	if (modelIsAnimMerging(chr->model)) {
		chr->hidden |= CHRHFLAG_NEEDANIM;
	} else {
		chrSurprisedChooseAnimation(chr);
		chr->hidden &= ~CHRHFLAG_NEEDANIM;
	}
}

void chrDoSurprisedLookAround(struct chrdata *chr)
{
	chrStopFiring(chr);
	chr->actiontype = ACT_SURPRISED;
	chr->act_surprised.type = 3;
	chr->sleep = 0;

	if (modelIsAnimMerging(chr->model)) {
		chr->hidden |= CHRHFLAG_NEEDANIM;
	} else {
		chrSurprisedChooseAnimation(chr);
		chr->hidden &= ~CHRHFLAG_NEEDANIM;
	}
}

void chrSurrenderChooseAnimation(struct chrdata *chr)
{
	struct prop *leftgun = chrGetHeldProp(chr, HAND_LEFT);
	struct prop *rightgun = chrGetHeldProp(chr, HAND_RIGHT);

	if (rightgun || leftgun) {
		modelSetAnimation(chr->model, ANIM_SURRENDER_002F, rngRandom() & 1, 0, 0.5, 16);
		modelSetAnimLooping(chr->model, 40, 16);

		if (leftgun) {
			objSetDropped(leftgun, DROPTYPE_SURRENDER);
		}

		if (rightgun) {
			objSetDropped(rightgun, DROPTYPE_SURRENDER);
		}

		chr->hidden |= CHRHFLAG_DROPPINGITEM;
	} else {
		modelSetAnimation(chr->model, ANIM_SURRENDER_002E, rngRandom() & 1, 0, 0.5, 16);
		modelSetAnimLooping(chr->model, 30, 16);
	}

	chrDropConcealedItems(chr);
}

void chrSurrender(struct chrdata *chr)
{
	u32 action = ACT_SURRENDER;

	if (chr->actiontype != action) {
		chrStopFiring(chr);
		chr->actiontype = action;
		chr->sleep = action;

		if (modelIsAnimMerging(chr->model)) {
			chr->hidden |= CHRHFLAG_NEEDANIM;
		} else {
			chrSurrenderChooseAnimation(chr);
			chr->hidden &= ~CHRHFLAG_NEEDANIM;
		}
	}
}

void chrSidestepChooseAnimation(struct chrdata *chr)
{
	struct prop *leftgun = chrGetHeldProp(chr, HAND_LEFT);
	struct prop *rightgun = chrGetHeldProp(chr, HAND_RIGHT);
	bool flip = false;
	bool allowflip = false;
	u32 race = CHRRACE(chr);

	if (leftgun && rightgun) {
		flip = rngRandom() % 2;
		allowflip = rngRandom() % 2;
	} else {
		if (weaponIsOneHanded(leftgun) == false
				&& weaponIsOneHanded(rightgun) == false
				&& (leftgun || rightgun)) {
			flip = (leftgun != 0);
			allowflip = rngRandom() % 2;
		}
	}

	if (race == RACE_HUMAN) {
		if (allowflip == false) {
			if (chr->act_sidestep.side) {
				modelSetAnimation(chr->model, ANIM_0068, true, 5, chrGetRangedSpeed(chr, 0.55, 0.88000005), 16);
				modelSetAnimEndFrame(chr->model, 36);
			} else {
				modelSetAnimation(chr->model, ANIM_0068, false, 5, chrGetRangedSpeed(chr, 0.55, 0.88000005), 16);
				modelSetAnimEndFrame(chr->model, 36);
			}
		} else {
			if ((chr->act_sidestep.side && !flip) || (chr->act_sidestep.side == 0 && flip)) {
				modelSetAnimation(chr->model, ANIM_003B, flip, 5, chrGetRangedSpeed(chr, 0.7, 1.12), 16);
				modelSetAnimEndFrame(chr->model, 34);
			} else {
				modelSetAnimation(chr->model, ANIM_003A, flip, 5, chrGetRangedSpeed(chr, 0.7, 1.12), 16);
				modelSetAnimEndFrame(chr->model, 32);
			}
		}
	} else if (race == RACE_SKEDAR) {
		if (chr->act_sidestep.side) {
			modelSetAnimation(chr->model, ANIM_0328, false, 5, chrGetRangedSpeed(chr, 0.55, 0.88000005), 16);
			modelSetAnimEndFrame(chr->model, 27);
		} else {
			modelSetAnimation(chr->model, ANIM_0328, true, 5, chrGetRangedSpeed(chr, 0.55, 0.88000005), 16);
			modelSetAnimEndFrame(chr->model, 27);
		}
	}
}

void chrSidestep(struct chrdata *chr, bool side)
{
	chrStopFiring(chr);
	chr->actiontype = ACT_SIDESTEP;
	chr->act_sidestep.side = side;
	chr->sleep = 0;

	if (modelIsAnimMerging(chr->model)) {
		chr->hidden |= CHRHFLAG_NEEDANIM;
	} else {
		chrSidestepChooseAnimation(chr);
		chr->hidden &= ~CHRHFLAG_NEEDANIM;
	}
}

void chrJumpOutChooseAnimation(struct chrdata *chr)
{
	struct prop *leftgun = chrGetHeldProp(chr, HAND_LEFT);
	struct prop *rightgun = chrGetHeldProp(chr, HAND_RIGHT);
	bool flip = false;

	if (leftgun && !rightgun) {
		flip = true;
	} else if ((leftgun && rightgun) || (!leftgun && !rightgun)
			|| weaponIsOneHanded(leftgun) || weaponIsOneHanded(rightgun)) {
		flip = rngRandom() % 2;
	}

	if ((chr->act_jumpout.side && !flip) || (chr->act_jumpout.side == 0 && flip)) {
		modelSetAnimation(chr->model, ANIM_0068, true, 5, chrGetRangedSpeed(chr, 0.55, 0.88000005), 16);
		modelSetAnimEndFrame(chr->model, 36);
	} else {
		modelSetAnimation(chr->model, ANIM_0068, false, 5, chrGetRangedSpeed(chr, 0.55, 0.88000005), 16);
		modelSetAnimEndFrame(chr->model, 36);
	}
}

void chrJumpOut(struct chrdata *chr, bool side)
{
	chrStopFiring(chr);
	chr->actiontype = ACT_JUMPOUT;
	chr->act_jumpout.side = side;
	chr->sleep = 0;

	if (modelIsAnimMerging(chr->model)) {
		chr->hidden |= CHRHFLAG_NEEDANIM;
	} else {
		chrJumpOutChooseAnimation(chr);
		chr->hidden &= ~CHRHFLAG_NEEDANIM;
	}
}

void chrRunPosChooseAnimation(struct chrdata *chr)
{
	f32 xdiff = chr->prop->pos.x - chr->act_runpos.pos.x;
	f32 ydiff = chr->prop->pos.y - chr->act_runpos.pos.y;
	f32 zdiff = chr->prop->pos.z - chr->act_runpos.pos.z;
	f32 distance = sqrtf(xdiff * xdiff + zdiff * zdiff);
	struct prop *leftgun = chrGetHeldProp(chr, HAND_LEFT);
	struct prop *rightgun = chrGetHeldProp(chr, HAND_RIGHT);
	bool heavy = true;
	bool flip;
	s32 race = CHRRACE(chr);

	if ((leftgun && rightgun) || (!leftgun && !rightgun)) {
		heavy = false;
		flip = rngRandom() % 2;
	} else if (weaponIsOneHanded(leftgun) || weaponIsOneHanded(rightgun)) {
		heavy = false;
		flip = (bool)leftgun != false;
	} else {
		flip = (bool)leftgun != false;
	}

	if (race == RACE_HUMAN) {
		if (heavy) {
			f32 mult = 0.5;
#if PAL
			chr->act_runpos.eta60 = 1.0f / (func0f02dff0(ANIM_RUNNING_TWOHANDGUN) * mult) * distance * 0.83333331346512f;
#else
			chr->act_runpos.eta60 = 1.0f / (func0f02dff0(ANIM_RUNNING_TWOHANDGUN) * mult) * distance;
#endif
			modelSetAnimation(chr->model, ANIM_RUNNING_TWOHANDGUN, flip, 0, mult, 16);
		} else {
			f32 mult = 0.5;
#if PAL
			chr->act_runpos.eta60 = 1.0f / (func0f02dff0(ANIM_RUNNING_ONEHANDGUN) * mult) * distance * 0.83333331346512f;
#else
			chr->act_runpos.eta60 = 1.0f / (func0f02dff0(ANIM_RUNNING_ONEHANDGUN) * mult) * distance;
#endif
			modelSetAnimation(chr->model, ANIM_RUNNING_ONEHANDGUN, flip, 0, mult, 16);
		}
	} else if (race == RACE_SKEDAR) {
		f32 mult = 0.5;
#if PAL
		chr->act_runpos.eta60 = 1.0f / (func0f02dff0(ANIM_SKEDAR_RUNNING) * mult) * distance * 0.83333331346512f;
#else
		chr->act_runpos.eta60 = 1.0f / (func0f02dff0(ANIM_SKEDAR_RUNNING) * mult) * distance;
#endif
		modelSetAnimation(chr->model, ANIM_SKEDAR_RUNNING, flip, 0, mult, 16);
	}
}

void chrRunToPos(struct chrdata *chr, struct coord *pos)
{
	chrStopFiring(chr);
	chr->actiontype = ACT_RUNPOS;
	chr->act_runpos.pos.x = pos->x;
	chr->act_runpos.pos.y = pos->y;
	chr->act_runpos.pos.z = pos->z;
	chr->sleep = 0;
	chr->act_runpos.neardist = 30;
	chr->act_runpos.turnspeed = 0;

	if (modelIsAnimMerging(chr->model)) {
		chr->hidden |= CHRHFLAG_NEEDANIM;
	} else {
		chrRunPosChooseAnimation(chr);
		chr->hidden &= ~CHRHFLAG_NEEDANIM;
	}
}

void chrAttackStand(struct chrdata *chr, u32 attackflags, s32 entityid)
{
	struct prop *leftgun = chrGetHeldProp(chr, HAND_LEFT);
	struct prop *rightgun = chrGetHeldProp(chr, HAND_RIGHT);
	bool flip;
	struct attackanimgroup **animgroup;
	bool firing[] = {false, false};
	s32 race = CHRRACE(chr);

	if (leftgun && rightgun) {
		struct prop *leftgun2 = chrGetHeldUsableProp(chr, HAND_LEFT);
		struct prop *rightgun2 = chrGetHeldUsableProp(chr, HAND_RIGHT);

		if (leftgun2 && rightgun2) {
			flip = rngRandom() % 2;

			if (rngRandom() % 3 == 0) {
				animgroup = g_StandLightAttackAnims[race];
				firing[HAND_LEFT] = flip;
				firing[HAND_RIGHT] = !flip;
			} else {
				animgroup = g_StandDualAttackAnims[race];
				firing[HAND_LEFT] = true;
				firing[HAND_RIGHT] = true;
			}
		} else {
			flip = (bool)rightgun2 == false;
			animgroup = g_StandLightAttackAnims[race];
			firing[HAND_LEFT] = (bool)rightgun2 == false;
			firing[HAND_RIGHT] = !flip;
		}
	} else {
		if (weaponIsOneHanded(leftgun) || weaponIsOneHanded(rightgun)) {
			flip = (bool)leftgun != false;
			animgroup = g_StandLightAttackAnims[race];
			firing[HAND_LEFT] = (bool)leftgun != false;
			firing[HAND_RIGHT] = !flip;
		} else {
			flip = (bool)leftgun != false;
			animgroup = g_StandHeavyAttackAnims[race];
			firing[HAND_LEFT] = (bool)leftgun != false;
			firing[HAND_RIGHT] = !flip;
		}
	}

	chrAttack(chr, animgroup, flip, firing, attackflags, entityid, true);
}

void chrAttackLie(struct chrdata *chr, u32 attackflags, s32 entityid)
{
	u32 stack[2];
	struct prop *gun = chrGetHeldProp(chr, HAND_RIGHT);
	s32 firing[2] = {false, false};

	if (chr);

	if (attackflags & ATTACKFLAG_AIMONLY) {
		firing[1] = false;
		firing[0] = false;
	} else {
		bool tmp = gun == NULL;
		firing[1] = tmp;
		firing[0] = (bool)!tmp;
	}

	chrAttack(chr, &g_LieAttackAnims, gun == NULL, firing, attackflags, entityid, false);
}

void chrAttackKneel(struct chrdata *chr, u32 attackflags, s32 entityid)
{
	struct prop *leftgun = chrGetHeldProp(chr, HAND_LEFT);
	struct prop *rightgun = chrGetHeldProp(chr, HAND_RIGHT);
	s32 flip;
	struct attackanimgroup **animgroup;
	bool firing[2] = {false, false};
	s32 race = CHRRACE(chr);
	struct prop *leftgun2;
	struct prop *rightgun2;

	if (leftgun && rightgun) {
		leftgun2 = chrGetHeldUsableProp(chr, HAND_LEFT);
		rightgun2 = chrGetHeldUsableProp(chr, HAND_RIGHT);

		if (leftgun2 && rightgun2) {
			flip = rngRandom() % 2;

			if (rngRandom() % 3 == 0) {
				animgroup = g_KneelLightAttackAnims[race];
				firing[HAND_LEFT] = flip;
				firing[HAND_RIGHT] = !flip;
			} else {
				animgroup = g_KneelDualAttackAnims[race];
				firing[HAND_LEFT] = true;
				firing[HAND_RIGHT] = true;
			}
		} else {
			flip = (bool)rightgun2 == false;
			animgroup = g_KneelLightAttackAnims[race];
			firing[HAND_LEFT] = (bool)rightgun2 == false;
			firing[HAND_RIGHT] = !flip;
		}
	} else {
		if (weaponIsOneHanded(leftgun) || weaponIsOneHanded(rightgun)) {
			flip = (bool)leftgun != false;
			animgroup = g_KneelLightAttackAnims[race];
			firing[HAND_LEFT] = (bool)leftgun != false;
			firing[HAND_RIGHT] = !flip;
		} else {
			flip = (bool)leftgun != false;
			animgroup = g_KneelHeavyAttackAnims[race];
			firing[HAND_LEFT] = (bool)leftgun != false;
			firing[HAND_RIGHT] = !flip;
		}
	}

	chrAttack(chr, animgroup, flip, firing, attackflags, entityid, false);
}

void chrAttackWalkChooseAnimation(struct chrdata *chr)
{
	if (chr->aibot == NULL) {
		modelSetAnimation(chr->model, chr->act_attackwalk.animcfg->animnum,
				chr->act_attackwalk.flip, chr->act_attackwalk.animcfg->unk10, 0.5, 16);
	}
}

void chrAttackWalk(struct chrdata *chr, bool run)
{
	struct attackanimconfig *animcfg;
	struct prop *leftgun = chrGetHeldProp(chr, HAND_LEFT);
	struct prop *rightgun = chrGetHeldProp(chr, HAND_RIGHT);
	bool flip;
	bool firing[] = {false, false};
	bool everytick[] = {false, false};
	bool singleshot[] = {false, false};
	s32 i;
	struct prop *prop;
	struct weaponobj *weapon;

	if (leftgun && rightgun) {
		struct prop *leftgun2 = chrGetHeldUsableProp(chr, HAND_LEFT);
		struct prop *rightgun2 = chrGetHeldUsableProp(chr, HAND_RIGHT);
		s32 style = 0;

		if (leftgun2 && rightgun2) {
			flip = rngRandom() % 2;
			style = rngRandom() % 3;
		} else {
			flip = (bool)rightgun2 == false;
		}

		if (style == 0) {
			if (run) {
				animcfg = &g_WalkAttackAnims[3];
			} else {
				animcfg = &g_WalkAttackAnims[2];
			}

			if (flip) {
				firing[HAND_LEFT] = true;
			} else {
				firing[HAND_RIGHT] = true;
			}
		} else if (style == 1) {
			if (run) {
				animcfg = &g_WalkAttackAnims[5];
			} else {
				animcfg = &g_WalkAttackAnims[4];
			}

			firing[HAND_LEFT] = firing[HAND_RIGHT] = true;
		} else {
			if (run) {
				animcfg = &g_WalkAttackAnims[7];
			} else {
				animcfg = &g_WalkAttackAnims[6];
			}

			firing[HAND_LEFT] = firing[HAND_RIGHT] = true;
		}
	} else {
		if (weaponIsOneHanded(leftgun) || weaponIsOneHanded(rightgun)) {
			flip = (bool)leftgun != false;

			if (run) {
				animcfg = &g_WalkAttackAnims[3];
			} else {
				animcfg = &g_WalkAttackAnims[2];
			}

			if (flip) {
				firing[HAND_LEFT] = true;
			} else {
				firing[HAND_RIGHT] = true;
			}
		} else {
			flip = (bool)leftgun != false;

			if (run) {
				animcfg = &g_WalkAttackAnims[1];
			} else {
				animcfg = &g_WalkAttackAnims[0];
			}

			if (flip) {
				firing[HAND_LEFT] = true;
			} else {
				firing[HAND_RIGHT] = true;
			}
		}
	}

	for (i = 0; i < 2; i++) {
		if (firing[i]) {
			prop = chrGetHeldProp(chr, i);
			weapon = prop->weapon;

			if (weaponGetNumTicksPerShot(weapon->weaponnum, weapon->gunfunc) < 1) {
				everytick[i] = true;
			}

			if (weapon->weaponnum == WEAPON_ROCKETLAUNCHER
					|| weapon->weaponnum == WEAPON_ROCKETLAUNCHER_34
					|| weapon->weaponnum == WEAPON_SLAYER
					|| weapon->weaponnum == WEAPON_DEVASTATOR
					|| (
						!g_Vars.normmplayerisrunning
						&& weapon->weaponnum == WEAPON_DY357MAGNUM
						&& chr->headnum != HEAD_JONATHAN
						&& chr->headnum != HEAD_CHRIST)
					|| (
						!g_Vars.normmplayerisrunning
						&& weapon->weaponnum == WEAPON_DY357LX)
					|| (
						!g_Vars.normmplayerisrunning
						&& weapon->weaponnum == WEAPON_SHOTGUN)) {
				singleshot[i] = true;
			}
		}
	}

	chr->actiontype = ACT_ATTACKWALK;

	chr->act_attackwalk.frame60count = 0;
#if PAL
	// This is really TICKS(400.0f), but off by one bit :(
	chr->act_attackwalk.frame60max = rngRandom() % (s32)(333.33331298828f * g_AttackWalkDurationScale) + TICKS(120);
#else
	chr->act_attackwalk.frame60max = rngRandom() % (s32)(400 * g_AttackWalkDurationScale) + TICKS(120);
#endif
	chr->act_attackwalk.facedtarget = false;
	chr->act_attackwalk.animcfg = animcfg;
	chr->act_attackwalk.nextshot60 = 0;
	chr->act_attackwalk.nextgun = rngRandom() % 2;
	chr->act_attackwalk.firegun[HAND_LEFT] = firing[HAND_LEFT];
	chr->act_attackwalk.firegun[HAND_RIGHT] = firing[HAND_RIGHT];
	chr->act_attackwalk.everytick[HAND_LEFT] = everytick[HAND_LEFT];
	chr->act_attackwalk.everytick[HAND_RIGHT] = everytick[HAND_RIGHT];
	chr->act_attackwalk.singleshot[HAND_LEFT] = singleshot[HAND_LEFT];
	chr->act_attackwalk.singleshot[HAND_RIGHT] = singleshot[HAND_RIGHT];
	chr->act_attackwalk.turnspeed = 0;
	chr->act_attackwalk.flip = flip;

	chr->sleep = 0;
	chr->chrflags &= ~CHRCFLAG_INJUREDTARGET;

	if (modelIsAnimMerging(chr->model)) {
		chr->hidden |= CHRHFLAG_NEEDANIM;
	} else {
		chrAttackWalkChooseAnimation(chr);

		chr->hidden &= ~CHRHFLAG_NEEDANIM;
	}
}

void chrAttackRollChooseAnimation(struct chrdata *chr)
{
	modelSetAnimation(chr->model, chr->act_attack.animcfg->animnum, chr->act_attack.flip,
			chr->act_attack.animcfg->unk10, chrGetRangedSpeed(chr, 0.5, 0.8), 16);

	if (chr->act_attack.onehanded == false) {
		if (chr->act_attack.dorecoil) {
			if (chr->act_attack.animcfg->unk24 >= 0) {
				modelSetAnimEndFrame(chr->model, chr->act_attack.animcfg->unk24);
			} else {
				modelSetAnimEndFrame(chr->model, chr->act_attack.animcfg->unk1c);
			}
		} else {
			if (chr->act_attack.animcfg->unk20 >= 0) {
				modelSetAnimEndFrame(chr->model, chr->act_attack.animcfg->unk20);
			} else if (chr->act_attack.animcfg->unk14 >= 0) {
				modelSetAnimEndFrame(chr->model, chr->act_attack.animcfg->unk14);
			}
		}
	}
}

void chrAttackRoll(struct chrdata *chr, bool toleft)
{
	struct attackanimconfig *animcfg;
	struct prop *leftgun = chrGetHeldProp(chr, HAND_LEFT);
	struct prop *rightgun = chrGetHeldProp(chr, HAND_RIGHT);
	bool flip;
	bool onehanded = false;
	struct prop *prop;
	struct weaponobj *weapon;
	bool dorecoil = true;
	bool firing[] = {false, false};
	s32 i;
	bool dooneburst = false;
	bool everytick[] = {false, false};
	bool singleshot[] = {false, false};

	if (leftgun && rightgun) {
		struct prop *leftgun2 = chrGetHeldUsableProp(chr, HAND_LEFT);
		struct prop *rightgun2 = chrGetHeldUsableProp(chr, HAND_RIGHT);

		if (leftgun2 && rightgun2) {
			flip = rngRandom() % 2;
			onehanded = true;

			if (rngRandom() % 3 == 0) {
				firing[HAND_LEFT] = flip;
				firing[HAND_RIGHT] = !flip;
			} else {
				firing[HAND_LEFT] = true;
				firing[HAND_RIGHT] = true;
			}
		} else {
			flip = (bool)rightgun2 == false;
			onehanded = true;
			firing[HAND_LEFT] = flip;
			firing[HAND_RIGHT] = !flip;
		}
	} else {
		if (weaponIsOneHanded(leftgun) || weaponIsOneHanded(rightgun)) {
			flip = (bool)leftgun != false;
			onehanded = true;
			firing[HAND_LEFT] = flip;
			firing[HAND_RIGHT] = !flip;
		} else {
			flip = (bool)leftgun != false;
			firing[HAND_LEFT] = flip;
			firing[HAND_RIGHT] = !flip;
		}
	}

	if ((toleft && !flip) || (!toleft && flip)) {
		// Roll to left
		if (rngRandom() % 2) {
			animcfg = &g_RollAttackAnims[0];
		} else {
			animcfg = &g_RollAttackAnims[2];
		}
	} else {
		// Roll to right
		if (rngRandom() % 2) {
			animcfg = &g_RollAttackAnims[1];
		} else {
			animcfg = &g_RollAttackAnims[3];
		}
	}

	if (onehanded) {
		// Move the animation pointer to the one-handed version of the same
		// animation. The one-handed versions are in the array immediately after
		// the four heavy-weapon versions.
		animcfg += 4;
	}

	for (i = 0; i < 2; i++) {
		if (firing[i]) {
			prop = chrGetHeldProp(chr, i);
			weapon = prop->weapon;

			if (weaponGetNumTicksPerShot(weapon->weaponnum, weapon->gunfunc) < 1) {
				everytick[i] = true;

				if (weapon->weaponnum == WEAPON_LASER) {
					dorecoil = false;
				}
			} else {
				dorecoil = false;
				dooneburst = true;
			}

			if (weapon->weaponnum == WEAPON_ROCKETLAUNCHER
					|| weapon->weaponnum == WEAPON_ROCKETLAUNCHER_34
					|| weapon->weaponnum == WEAPON_SLAYER
					|| weapon->weaponnum == WEAPON_DEVASTATOR
					|| (
						!g_Vars.normmplayerisrunning
						&& weapon->weaponnum == WEAPON_DY357MAGNUM
						&& chr->headnum != HEAD_JONATHAN
						&& chr->headnum != HEAD_CHRIST)
					|| (
						!g_Vars.normmplayerisrunning
						&& weapon->weaponnum == WEAPON_DY357LX)
					|| (
						!g_Vars.normmplayerisrunning
						&& weapon->weaponnum == WEAPON_SHOTGUN)) {
				singleshot[i] = true;
			}
		}
	}

	chr->actiontype = ACT_ATTACKROLL;

	chr->act_attack.animcfg = animcfg;
	chr->act_attack.fired = false;
	chr->act_attack.nextgun = rngRandom() % 2;
	chr->act_attack.firegun[HAND_LEFT] = firing[HAND_LEFT];
	chr->act_attack.firegun[HAND_RIGHT] = firing[HAND_RIGHT];
	chr->act_attack.everytick[HAND_LEFT] = everytick[HAND_LEFT];
	chr->act_attack.everytick[HAND_RIGHT] = everytick[HAND_RIGHT];
	chr->act_attack.singleshot[HAND_LEFT] = singleshot[HAND_LEFT];
	chr->act_attack.singleshot[HAND_RIGHT] = singleshot[HAND_RIGHT];
	chr->act_attack.dorecoil = dorecoil;
	chr->act_attack.dooneburst = dooneburst;
	chr->act_attack.onehanded = onehanded;
	chr->act_attack.pausecount = 0;
	chr->act_attack.numshots = 0;
	chr->act_attack.turning = TURNSTATE_TURNING;

	if (singleshot[HAND_LEFT] || singleshot[HAND_RIGHT]) {
		if (singleshot[HAND_LEFT] && singleshot[HAND_RIGHT]) {
			chr->act_attack.maxshots = 2;
		} else {
			chr->act_attack.maxshots = 1;
		}
	} else {
		chr->act_attack.maxshots = (rngRandom() % 4) + 2;

		if (firing[HAND_RIGHT] && firing[HAND_LEFT]) {
			chr->act_attack.maxshots += (rngRandom() % 4) + 2;
		}
	}

	chr->act_attack.flags = ATTACKFLAG_AIMATTARGET;
	chr->act_attack.entityid = 0;
	chr->act_attack.standing = true;
	chr->act_attack.reaim = 0;
	chr->act_attack.lastfire60 = 0;
	chr->act_attack.lastontarget60 = 0;
	chr->act_attack.flip = flip;

	chr->sleep = 0;

	// @bug: CHRCFLAG_INJUREDTARGET is not unset here. This means if the chr
	// does an attack that hits the target, then does a roll attack which
	// misses, their AI list will incorrectly read the roll attack as a hit
	// provided it didn't read and clear the flag between the attacks.
	// It usually (always?) does though, so this isn't really an issue.

	if (modelIsAnimMerging(chr->model)) {
		chr->hidden |= CHRHFLAG_NEEDANIM;
	} else {
		chrAttackRollChooseAnimation(chr);

		chr->hidden &= ~CHRHFLAG_NEEDANIM;
	}
}

void chrStartAnim(struct chrdata *chr, s32 animnum, f32 startframe, f32 endframe, u8 chranimflags, s32 merge, f32 speed)
{
	u32 stack;

	if (chr && chr->model) {
		if (chranimflags & CHRANIMFLAG_REVERSE) {
			speed = -speed;
		}

		if (CHRRACE(chr) != RACE_DRCAROLL) {
			chrStopFiring(chr);
		}

		chr->actiontype = ACT_ANIM;

		chr->act_anim.movewheninvis = (chranimflags & CHRANIMFLAG_MOVEWHENINVIS) != 0;
		chr->act_anim.pauseatend = (chranimflags & CHRANIMFLAG_PAUSEATEND) != 0;
		chr->act_anim.completed = (chranimflags & CHRANIMFLAG_COMPLETED) != 0;
		chr->act_anim.slowupdate = (chranimflags & CHRANIMFLAG_SLOWUPDATE) != 0;
		chr->act_anim.lockpos = (chranimflags & CHRANIMFLAG_LOCKPOS) != 0;
		chr->act_anim.ishitanim = false;
		chr->act_anim.animnum = animnum;
		chr->act_anim.flip = (chranimflags & CHRANIMFLAG_FLIP) != 0;
		chr->act_anim.startframe = startframe;
		chr->act_anim.endframe = endframe;
		chr->act_anim.speed = speed;
		chr->act_anim.blend = merge;

		chr->sleep = chr->act_anim.slowupdate ? merge : 0;

		if (merge > 0 && modelIsAnimMerging(chr->model)) {
			chr->hidden |= CHRHFLAG_NEEDANIM;
		} else {
			modelSetAnimation(chr->model, animnum, (chranimflags & CHRANIMFLAG_FLIP) != 0, startframe, speed, merge);

			if (endframe >= 0) {
				modelSetAnimEndFrame(chr->model, endframe);
			}

			chr->hidden &= ~CHRHFLAG_NEEDANIM;
		}
	}
}

void chrBeginDead(struct chrdata *chr)
{
	if (chr->actiontype != ACT_DEAD) {
		chrStopFiring(chr);

		if (chr->cover != -1) {
			coverSetInUse(chr->cover, false);
			chr->cover = -1;
		}

		chr->actiontype = ACT_DEAD;
		chr->act_dead.fadetimer60 = chr->aibot ? 0 : -1;
		chr->act_dead.fadenow = false;
		chr->act_dead.fadewheninvis = false;
		chr->act_dead.invistimer60 = 0;
		chr->act_dead.notifychrindex = 0;
		chr->sleep = 0;

		if (chr->race == RACE_DRCAROLL) {
			chr->drcarollimage_left = DRCAROLLIMAGE_STATIC;
			chr->drcarollimage_right = DRCAROLLIMAGE_STATIC;
		}
	}
}

void func0f031254(struct chrdata *chr)
{
	struct model *model = chr->model;
	struct attackanimconfig *animcfg = chr->act_attack.animcfg;

	if (chr->act_attack.flags & ATTACKFLAG_AIMONLY) {
		if (animcfg->unk20 >= 0 && animcfg->unk20 < animcfg->unk18) {
			modelSetAnimEndFrame(model, animcfg->unk20);
		} else {
			modelSetAnimEndFrame(model, animcfg->unk18);
		}
	} else if (chr->act_attack.dorecoil) {
		if (animcfg->unk20 >= 0) {
			modelSetAnimEndFrame(model, animcfg->unk20);
		} else {
			modelSetAnimEndFrame(model, animcfg->unk18);
		}
	} else {
		if (animcfg->unk20 >= 0) {
			modelSetAnimEndFrame(model, animcfg->unk20);
		} else if (animcfg->unk14 >= 0) {
			modelSetAnimEndFrame(model, animcfg->unk14);
		} else {
			modelSetAnimEndFrame(model, -1);
		}
	}
}

/**
 * This function implements attack behaviour common to all the attack types,
 * such as stand, kneel and lie.
 */
void chrAttack(struct chrdata *chr, struct attackanimgroup **animgroups, bool flip, bool *firing, u32 attackflags, s32 entityid, bool standing)
{
	struct model *model = chr->model;
	s32 i;
	f32 angle;
	struct attackanimconfig *animcfg;
	struct prop *prop;
	struct weaponobj *weapon;
	s32 groupindex;
	bool dooneburst = false;
	s32 index;
	bool everytick[] = {false, false};
	bool singleshot[] = {false, false};
	bool dorecoil = true;
	s32 race = CHRRACE(chr);
	u8 sniping = false;

	if (race != RACE_DRCAROLL && race != RACE_EYESPY && race != RACE_ROBOT) {
		chr->actiontype = ACT_ATTACK;

		if (&animgroups[0] == &g_LieAttackAnims) {
			sniping = true;

			if (modelGetAnimNum(chr->model) != ANIM_SNIPING_ONGROUND) {
				// Getting up or getting down
				animcfg = var80067d28;
				modelSetAnimation(model, animcfg->animnum, flip, animcfg->unk10, chrGetRangedSpeed(chr, 0.5f, 0.8f), 16);
				modelSetAnimEndFrame(model, 236);
			} else {
				animcfg = var80067d70;
			}
		} else {
			// Non-sniping animations: Choose animation based on angle to target
			angle = chrGetAttackEntityRelativeAngle(chr, attackflags, entityid);

			if (flip) {
				groupindex = (M_BADTAU - angle) * 5.0937690734863f + 0.5f;
			} else {
				groupindex = angle * 5.0937690734863f + 0.5f;
			}

			if (groupindex < 0 || groupindex > 31) {
				groupindex = 0;
			}

			index = rngRandom() % animgroups[groupindex]->len;
			animcfg = &animgroups[groupindex]->animcfg[index];
		}

		for (i = 0; i < 2; i++) {
			if (firing[i]) {
				prop = chrGetHeldProp(chr, i);

				if (prop == NULL) {
					chrChooseStandAnimation(chr, 16);
					return;
				}

				weapon = prop->weapon;

				if (weaponGetNumTicksPerShot(weapon->weaponnum, weapon->gunfunc) < 1) {
					// Note: the only weapon that can enter this branch is the laser
					everytick[i] = true;

					if (weapon->weaponnum == WEAPON_LASER) {
						dorecoil = false;
					}
				} else {
					dorecoil = false;
					dooneburst = true;
				}

				// There's an easter egg here: Any guard with Chris T's head
				// (Foster from the firing range) can fire multiple shots with
				// the magnum.
				if (weapon->weaponnum == WEAPON_ROCKETLAUNCHER
						|| weapon->weaponnum == WEAPON_ROCKETLAUNCHER_34
						|| weapon->weaponnum == WEAPON_SLAYER
						|| weapon->weaponnum == WEAPON_DEVASTATOR
						|| (
							!g_Vars.normmplayerisrunning
							&& weapon->weaponnum == WEAPON_DY357MAGNUM
							&& chr->headnum != HEAD_JONATHAN
							&& chr->headnum != HEAD_CHRIST)
						|| (
							!g_Vars.normmplayerisrunning
							&& weapon->weaponnum == WEAPON_DY357LX)
						|| (
							!g_Vars.normmplayerisrunning
							&& weapon->weaponnum == WEAPON_SHOTGUN)) {
					singleshot[i] = true;
				}
			}
		}

		chr->act_attack.turning = TURNSTATE_TURNING;
		chr->act_attack.animcfg = animcfg;
		chr->act_attack.fired = false;
		chr->act_attack.nextgun = rngRandom() % 2;
		chr->act_attack.firegun[HAND_LEFT] = firing[HAND_LEFT];
		chr->act_attack.firegun[HAND_RIGHT] = firing[HAND_RIGHT];
		chr->act_attack.everytick[HAND_LEFT] = everytick[HAND_LEFT];
		chr->act_attack.everytick[HAND_RIGHT] = everytick[HAND_RIGHT];
		chr->act_attack.singleshot[HAND_LEFT] = singleshot[HAND_LEFT];
		chr->act_attack.singleshot[HAND_RIGHT] = singleshot[HAND_RIGHT];
		chr->act_attack.dorecoil = dorecoil;
		chr->act_attack.dooneburst = dooneburst;
		chr->act_attack.pausecount = 0;
		chr->act_attack.numshots = 0;

		if (singleshot[HAND_LEFT] || singleshot[HAND_RIGHT]) {
			if (singleshot[HAND_LEFT] && singleshot[HAND_RIGHT]) {
				chr->act_attack.maxshots = 2;
			} else {
				chr->act_attack.maxshots = 1;
			}
		} else {
			if (attackflags & ATTACKFLAG_SINGLESHOT) {
				chr->act_attack.maxshots = 1;
			} else {
				chr->act_attack.maxshots = (rngRandom() % 4) + 2;
			}

			// @bug: ATTACKFLAG_SINGLESHOT is not respected here if both guns
			// are firing.
			if (firing[HAND_RIGHT] && firing[HAND_LEFT]) {
				chr->act_attack.maxshots += (rngRandom() % 4) + 2;
			}
		}

		chr->act_attack.flags = attackflags;
		chr->act_attack.entityid = entityid;
		chr->act_attack.standing = standing;
		chr->act_attack.reaim = 0;
		chr->act_attack.lastfire60 = 0;
		chr->act_attack.lastontarget60 = 0;
		chr->act_attack.flip = flip;

		chr->sleep = 0;
		chr->chrflags &= ~CHRCFLAG_INJUREDTARGET;

		if (!sniping && !chr->aibot) {
			if (modelIsAnimMerging(chr->model)) {
				chr->hidden |= CHRHFLAG_NEEDANIM;
			} else {
				modelSetAnimation(model, animcfg->animnum, flip, animcfg->unk10, chrGetRangedSpeed(chr, 0.5f, 0.8f), 16);
				func0f031254(chr);

				chr->hidden &= ~CHRHFLAG_NEEDANIM;
			}
		}
	}
}

void chrAttackAmount(struct chrdata *chr, u32 attackflags, u32 entityid, u32 maxshots)
{
	u32 stack;
	struct prop *prop = chrGetHeldProp(chr, HAND_RIGHT);
	struct attackanimgroup **things = NULL;
	bool firing[] = {false, false};
	u32 race = CHRRACE(chr);

	if (prop) {
		things = weaponIsOneHanded(prop) ? g_StandLightAttackAnims[race] : g_StandHeavyAttackAnims[race];

		firing[1] = false;
		firing[0] = true;
	}

	chrAttack(chr, things, false, firing, attackflags, entityid, false);

	chr->actiontype = ACT_ATTACKAMOUNT;
	chr->act_attack.numshots = 0;
	chr->act_attack.maxshots = maxshots;
	chr->act_attack.dooneburst = false;
}

#if PAL
s32 g_DrCarollDyingTimer = 8;
#else
s32 g_DrCarollDyingTimer = 10;
#endif

u8 var80068080 = 50;

/**
 * Given a perfectly alive chr, make them begin the process of dying or being
 * knocked unconscious.
 *
 * This function handles:
 * - Eyespy destruction
 * - Transitioning to ACT_DIE
 * - Choosing and applying death animations
 * - Updating kill statistics
 * - Dropping items
 */
void chrBeginDeath(struct chrdata *chr, struct coord *dir, f32 relangle, s32 hitpart, struct gset *gset, bool knockout, s32 aplayernum)
{
	bool overridden = false;
	bool instant;
	s32 index = -1;
	s32 animnum;
	u32 stack1;
	struct prop *prop = chr->prop;
	struct model *model = chr->model;
	u32 stack2;
	s32 race = CHRRACE(chr);
	bool wasknockedout = false;
	s32 prevplayernum;
	s32 i;
	s32 buddyplayernum;
	struct eyespy *eyespy;
	s32 objectivenum;
	f32 impactforce1;
	f32 impactforce2;
	f32 impactforce3;

	// If chr was previously knocked out, they are now dead so decrease KO counter
	if (chr->actiontype == ACT_DRUGGEDCOMINGUP
			|| chr->actiontype == ACT_DRUGGEDDROP
			|| chr->actiontype == ACT_DRUGGEDKO) {
#if VERSION >= VERSION_NTSC_1_0
		if (chr->actiontype == ACT_DRUGGEDKO) {
			wasknockedout = true;
		}
#else
		wasknockedout = true;
#endif

		mpstatsDecrementTotalKnockoutCount();
	}

	// Handle eyespy then return early
	if (race == RACE_EYESPY) {
		prevplayernum = g_Vars.currentplayernum;
		buddyplayernum = -1;
		eyespy = chrToEyespy(chr);
		objectivenum = -1;

		// Figure out which playernum has the eyespy that's being destroyed,
		// and the buddy's playernum if applicable. Note that the player count
		// can only be 1 or 2 here.
		for (i = 0; i < PLAYERCOUNT(); i++) {
			if (eyespy == g_Vars.players[i]->eyespy) {
				setCurrentPlayerNum(i);
			} else {
				buddyplayernum = i;
			}
		}

		if (g_Vars.currentplayer->eyespy) {
			// Stop using eyespy if active
			if (g_Vars.currentplayer->eyespy->active) {
				g_Vars.currentplayer->eyespy->active = false;
				g_Vars.currentplayer->devicesactive &= ~DEVICE_EYESPY;
			}

			// Destroy the eyespy
			chr->hidden |= CHRHFLAG_DELETING;

			explosionCreateSimple(g_Vars.currentplayer->eyespy->prop,
					&g_Vars.currentplayer->eyespy->prop->pos,
					g_Vars.currentplayer->eyespy->prop->rooms, EXPLOSIONTYPE_EYESPY, 0);
			invRemoveItemByNum(WEAPON_EYESPY);

			psStopSound(g_Vars.currentplayer->eyespy->prop, PSTYPE_GENERAL, 0xffff);
			g_Vars.currentplayer->eyespy = NULL;
			setCurrentPlayerNum(prevplayernum);

			// For Investigation and G5 Building, set a stage flag to show that
			// the eyespy is destroyed. The scripting in those stages checks for
			// this flag and fails the objective if set.
			switch (stageGetIndex(g_Vars.stagenum)) {
			case STAGEINDEX_INVESTIGATION:
				objectivenum = 0;
				break;
			case STAGEINDEX_G5BUILDING:
				if (lvGetDifficulty() == DIFF_A) {
					objectivenum = 2;
				} else if (lvGetDifficulty() == DIFF_SA) {
					objectivenum = 2;
				} else {
					objectivenum = 2;
				}
				break;
			}

			// But don't set the flag if the coop buddy still has an eyespy
			if (objectivenum >= 0 && buddyplayernum >= 0) {
				setCurrentPlayerNum(buddyplayernum);

				if (g_Vars.currentplayer->eyespy) {
					objectivenum = -1;
				}

				setCurrentPlayerNum(prevplayernum);
			}

			if (objectivenum >= 0 && objectiveCheck(objectivenum) != OBJECTIVE_COMPLETE) {
				g_StageFlags |= STAGEFLAG_EYESPY_DESTROYED;
			}
		}

		setCurrentPlayerNum(prevplayernum);
		return;
	}

	// instant = whether to merge into death animation or switch to it instantly
	instant = chr->actiontype == ACT_ARGH && chr->act_argh.lvframe60 == g_Vars.lvframe60;

	for (i = 0; g_AnimTablesByRace[race][i].hitpart != -1; i++) {
		if (g_AnimTablesByRace[race][i].hitpart == hitpart) {
			index = i;
			break;
		}
	}

	// Set up chr's new action
	chr->blurdrugamount = 0;

	chrStopFiring(chr);
	chrUncloak(chr, true);

#if VERSION >= VERSION_NTSC_1_0
	chr->chrflags &= ~CHRCFLAG_HIDDEN;
#endif

	chr->actiontype = (knockout == true ? ACT_DRUGGEDDROP : ACT_DIE);

	chr->act_die.notifychrindex = 0;
	chr->act_die.timeextra = 0;
	chr->act_die.drcarollimagedelay = TICKS(45);
	chr->act_die.thudframe1 = -1;
	chr->act_die.thudframe2 = -1;

	if (chr->race == RACE_DRCAROLL) {
		chr->drcarollimage_left = (s32)((rngRandom() % 400) * 0.01f) + 1;
		chr->drcarollimage_right = (s32)((rngRandom() % 400) * 0.01f) + 1;
	}

	chr->sleep = 0;

#ifndef PLATFORM_N64
	// Campaign chr death CHOKE POINT: every non-aibot death that plays a death
	// animation funnels through here — gunfire (via the argh path), robots and
	// Dr Caroll from explosions, KO'd chrs finished off. Emit the Lua "kill"
	// event here (not on knockouts — a KO'd chr that's later killed re-enters
	// with knockout == false). The aibot path (chrDie) and the explosion-yeet
	// path (chrDamage) emit at their own sites. The old emit lived ONLY in the
	// explosion branch of chrDamage, so bullet kills never fired the event —
	// which is why kill-based chaos effects (martyrdom) never triggered.
	if (knockout == false) {
		luaEmitKill((s32)chr->chrnum, aplayernum);
	}
#endif

	// Handle robots and Dr Caroll then return early
	if (race == RACE_ROBOT || race == RACE_DRCAROLL) {
		impactforce1 = gsetGetImpactForce(gset) * 0.5f;

		if (impactforce1 <= 0) {
			impactforce1 = 3;
		}

		if (impactforce1 != 0.0f) {
			chr->elapseextra = 0;
			chr->timeextra = impactforce1 * 15;
			chr->extraspeed.x = dir->x * impactforce1;
			chr->extraspeed.y = dir->y * impactforce1;
			chr->extraspeed.z = dir->z * impactforce1;
		}

		if (race == RACE_DRCAROLL) {
			g_DrCarollDyingTimer = 0;

			chr->soundtimer = 0;
			chr->voicebox = VOICEBOX_MALE1;

			modelSetAnimation(chr->model, ANIM_0164, false, 0, 0.5f, 16);
		}

		return;
	}

	// Handle humans and Skedar
	if (race == RACE_HUMAN) {
		animnum = modelGetAnimNum(chr->model);

		// Chrs in lying-down sniping poses don't use standard death animations
		if (animnum == ANIM_SNIPING_GETDOWN
				|| animnum == ANIM_SNIPING_GETUP
				|| animnum == ANIM_SNIPING_ONGROUND) {
			modelSetAnimation(chr->model, ANIM_SNIPING_DIE, false, 0, 0.5f, 16);
		} else {
			// Consider making the chr do an animation where they slump against
			// a wall or object which is behind them.
			if ((relangle < 1.5705462694168f || relangle > 4.7116389274597f)
					&& rngRandom() % 20 == 0
					&& chr->specialdie == SPECIALDIE_NONE) {
				f32 angle1;
				f32 angle2 = chrGetInverseTheta(chr);
				f32 fval1;
				f32 fval2;

				angle1 = angle2 + 3.3155977725983f;
				angle2 += 2.966587305069f;

				if (angle1 >= M_BADTAU) {
					angle1 -= M_BADTAU;
				}

				if (angle2 >= M_BADTAU) {
					angle2 -= M_BADTAU;
				}

				fval1 = func0f02e684(prop, angle1, 150);
				fval2 = func0f02e684(prop, angle2, 150);

				if (fval1 < 150 && fval2 < 150
						&& fval1 - fval2 < 10 && fval1 - fval2 > -10
						&& !wasknockedout) {
					struct animtablerow *row = &g_AnimTableHumanSlumped[rngRandom() % 4];
					u32 stack3;

					chr->act_die.thudframe1 = row->thudframe1;
					chr->act_die.thudframe2 = row->thudframe2;

					modelSetAnimationWithMerge(model, row->animnum, row->flip, 0, row->speed, 16, !instant);

					if (row->endframe >= 0) {
						modelSetAnimEndFrame(model, row->endframe);
					}

					chr->radius = 10;
					chr->chrflags &= ~CHRCFLAG_HAS_SPECIAL_DEATH_ANIMATION;

					overridden = true;
				}
			}

			// Consider making the chr do a fall forward animation.
			// The player must be behind the chr for it to happen.
			if (relangle > 2.3558194637299f && relangle < 3.9263656139374f
					&& rngRandom() % 5 < 2
					&& chr->specialdie == SPECIALDIE_NONE) {
				struct animtablerow *row;

				struct animtablerow rows[] = {
					{ 0x005b, 0, -1, 0.6, 0, 27, -1 },
					{ 0x0255, 0, -1, 0.5, 0, 25, -1 },
				};

				bool flip;

				overridden = true;

				if (hitpart == HITPART_LBICEP || hitpart == HITPART_RBICEP) {
					row = &rows[0];

					if (hitpart == HITPART_LBICEP) {
						flip = true;
					} else {
						flip = false;
					}
				} else {
					row = &rows[1];
					flip = rngRandom() % 2;
				}

				chr->act_die.thudframe1 = row->thudframe1;
				chr->act_die.thudframe2 = row->thudframe2;

				modelSetAnimationWithMerge(model, row->animnum, flip, 0, row->speed, 16, !instant);

				if (row->endframe >= 0) {
					modelSetAnimEndFrame(model, chrGetRangedArghSpeed(chr, row->endframe, 8));
				} else {
					modelSetAnimEndFrame(model, chrGetRangedArghSpeed(chr, animGetNumFrames(row->animnum) - 1, 8));
				}

				chr->chrflags &= ~CHRCFLAG_HAS_SPECIAL_DEATH_ANIMATION;
			}

			if (!overridden && index >= 0) {
				// Handle specialdie animations or choose a random one if not
				// using specialdie
				if (g_AnimTablesByRace[race][index].deathanims
						&& g_AnimTablesByRace[race][index].deathanimcount > 0
						&& !wasknockedout) {
					struct animtablerow *row;

					if (chr->specialdie == SPECIALDIE_NONE) {
						s32 tmp = rngRandom() % g_AnimTablesByRace[race][index].deathanimcount;
						row = &g_AnimTablesByRace[race][index].deathanims[tmp];
					} else if (chr->specialdie == SPECIALDIE_ONCHAIR) {
						row = &g_SpecialDieAnims[chr->specialdie + rngRandom() % 2];

						// chr->myspecial is the tag number of the chr's chair
						if (chr->myspecial >= 0) {
							struct defaultobj *obj = objFindByTagId(chr->myspecial);
							obj->flags3 &= ~OBJFLAG3_PUSHABLE;
							obj->flags |= OBJFLAG_INVINCIBLE;

							// The original source likely didn't have the brackets here,
							// but I'm including them to show the logic that's actually
							// being used. There is no bug here, as obj and obj->prop
							// are always set at this point so these checks are
							// unnecessary.
							if ((obj && obj->prop && obj->modelnum == MODEL_DD_REDARM)
									|| obj->modelnum == MODEL_DD_REDSOFA) {
								row = &g_SpecialDieAnims[chr->specialdie - 1];
							}
						}
					} else {
						row = &g_SpecialDieAnims[chr->specialdie - 1];
					}

					chr->act_die.thudframe1 = row->thudframe1;
					chr->act_die.thudframe2 = row->thudframe2;

					if (chr->specialdie == SPECIALDIE_NONE) {
						modelSetAnimationWithMerge(model, row->animnum, row->flip, 0, row->speed, 16, !instant);
					} else {
						modelSetAnimationWithMerge(model, row->animnum, row->flip, 0, row->speed, 30, !instant);
					}

					if (row->endframe >= 0) {
						modelSetAnimEndFrame(model, row->endframe);
					}

					impactforce2 = gsetGetImpactForce(gset);

					if (impactforce2 <= 0 && (chr->chrflags & CHRCFLAG_DIEWITHFORCE)) {
						impactforce2 = 6;
					}

					if (row->unk10 && impactforce2 > 0) {
						chr->act_die.timeextra = impactforce2 * 15;
						chr->act_die.elapseextra = 0;
						chr->act_die.extraspeed.x = dir->x * impactforce2;
						chr->act_die.extraspeed.y = dir->y * impactforce2;
						chr->act_die.extraspeed.z = dir->z * impactforce2;
					}

					chr->chrflags &= ~CHRCFLAG_HAS_SPECIAL_DEATH_ANIMATION;
				}
			}
		}
	} else if (race == RACE_SKEDAR) {
		struct animtablerow *row;

		if (relangle > 2.3558194637299f && relangle < 3.9263656139374f) {
			// Player is behind the Skedar - use specific set of anims
			row = &g_AnimTablesByRace[race][1 + (rngRandom() % 6)].deathanims[rngRandom() % 3];

			chr->act_die.thudframe1 = row->thudframe1;
			chr->act_die.thudframe2 = row->thudframe2;

			modelSetAnimationWithMerge(model, row->animnum, row->flip, 0, row->speed, 16, !instant);

			if (row->endframe >= 0) {
				modelSetAnimEndFrame(model, row->endframe);
			}
		} else {
			// Normal Skedar death
			if (index >= 0
					&& g_AnimTablesByRace[race][index].deathanims != NULL
					&& g_AnimTablesByRace[race][index].deathanimcount > 0) {
				s32 tmp = rngRandom() % g_AnimTablesByRace[race][index].deathanimcount;
				row = &g_AnimTablesByRace[race][index].deathanims[tmp];
			} else {
				row = &g_AnimTablesByRace[race][0].deathanims[0];
#ifndef PLATFORM_N64
				// Entry 0 of the anim table (hitpart 0) has NULL deathanims, so
				// row is NULL whenever hitpart isn't in the table (index -1, e.g.
				// HITPART_GENERAL). The N64 read low memory harmlessly; the port
				// faults. Fall back to the first entry that has real anims.
				if (row == NULL) {
					for (i = 0; g_AnimTablesByRace[race][i].hitpart != -1; i++) {
						if (g_AnimTablesByRace[race][i].deathanims != NULL) {
							row = &g_AnimTablesByRace[race][i].deathanims[0];
							break;
						}
					}
				}
#endif
			}

			chr->act_die.thudframe1 = row->thudframe1;
			chr->act_die.thudframe2 = row->thudframe2;

			modelSetAnimationWithMerge(model, row->animnum, row->flip, 0, row->speed, 16, !instant);

			if (row->endframe >= 0) {
				modelSetAnimEndFrame(model, row->endframe);
			}

			impactforce3 = gsetGetImpactForce(gset);

			if (impactforce3 <= 0 && (chr->chrflags & CHRCFLAG_DIEWITHFORCE)) {
				impactforce3 = 6;
			}

			if (row->unk10 != 0 && impactforce3 > 0) {
				chr->act_die.timeextra = impactforce3 * 15;
				chr->act_die.elapseextra = 0;
				chr->act_die.extraspeed.x = dir->x * impactforce3;
				chr->act_die.extraspeed.y = dir->y * impactforce3;
				chr->act_die.extraspeed.z = dir->z * impactforce3;
			}
		}
	} else if (race == RACE_DRCAROLL) {
		// empty
	}

	// Handle multiplayer stats and kill count
	if (g_Vars.mplayerisrunning) {
		mpstatsRecordDeath(aplayernum, mpPlayerGetIndex(chr));
	} else if (aplayernum >= 0) {
		s32 prevplayernum = g_Vars.currentplayernum;
		setCurrentPlayerNum(aplayernum);
		mpstatsRecordPlayerKill();
		setCurrentPlayerNum(prevplayernum);
	}

	if (chr->chrflags & CHRCFLAG_KILLCOUNTABLE) {
		mpstatsIncrementTotalKillCount();
	}

	// Drop items
	if ((race == RACE_HUMAN || race == RACE_SKEDAR)
#ifndef PLATFORM_N64
			// Chaos "No drops": skip the whole drop — held weapons stay
			// parented to the corpse and free with it.
			&& !g_ChaosNoDrops
#endif
			) {
#ifndef PLATFORM_N64
		// "Wire owns the lifetime" (PORT_NET_PROP_LIFECYCLE): on a net client,
		// a synced chr's corpse drop must NOT put the LOCAL copies on the
		// floor — the server's authoritative drop arrives via the
		// objSetDropped spawn broadcast and IS the floor item. Both machines
		// dropping their own copy was the ghost-twin family: per-tick
		// ghost-move bandwidth for every floor gun, "prop with syncid N does
		// not exist" warning spam, visual-vs-authoritative divergence, and
		// the weapon-slot pressure feeding the force-recycle crash family.
		// A syncid-0 held twin (Combat Sim bot hands, mirrored locally by the
		// chr-state weapons-held sync) is marked DELETING so it just vanishes;
		// a SYNCED held prop (campaign setup guns) is left parented — the
		// server's spawn broadcast (read-side latest-spawn-wins) replaces it
		// with the world copy. Concealed items are synced setup props covered
		// by the same broadcast, so the local drop is skipped entirely.
		if (g_NetMode == NETMODE_CLIENT && chr->prop && chr->prop->syncid) {
			s32 h;

			for (h = 0; h < 2; h++) {
				struct prop *wp = chr->weapons_held[h];

				if (wp && wp->obj && (wp->obj->flags & OBJFLAG_AIUNDROPPABLE) == 0
						&& wp->syncid == 0) {
					wp->obj->hidden |= OBJHFLAG_DELETING;
					netPropLogEvent(wp, NETPROP_EV_TWIN_SUPPRESS, (u16)h);
				}
			}
		} else {
#endif
		if (chr->weapons_held[0] && (chr->weapons_held[0]->obj->flags & OBJFLAG_AIUNDROPPABLE) == 0) {
			objSetDropped(chr->weapons_held[0], DROPTYPE_DEFAULT);
			chr->hidden |= CHRHFLAG_DROPPINGITEM;
		}

		if (chr->weapons_held[1] && (chr->weapons_held[1]->obj->flags & OBJFLAG_AIUNDROPPABLE) == 0) {
			objSetDropped(chr->weapons_held[1], DROPTYPE_DEFAULT);
			chr->hidden |= CHRHFLAG_DROPPINGITEM;
		}

		chrDropConcealedItems(chr);
#ifndef PLATFORM_N64
		}
#endif
	}
}

void chrBeginArgh(struct chrdata *chr, f32 angle, s32 hitpart)
{
	bool doneanim = false;
	s32 instant;
	s32 index = -1;
	struct model *model = chr->model;
	s32 i;
	s32 race = CHRRACE(chr);
	s32 animnum = modelGetAnimNum(chr->model);

	if (animnum == ANIM_SNIPING_GETDOWN
			|| animnum == ANIM_SNIPING_GETUP
			|| animnum == ANIM_SNIPING_ONGROUND) {
		chrFlinchBody(chr);
		return;
	}

	if (race == RACE_EYESPY || chr->aibot) {
		return;
	}

	if (race == RACE_DRCAROLL) {
		chr->actiontype = ACT_ARGH;
		chr->act_argh.notifychrindex = 0;
		chr->act_argh.lvframe60 = g_Vars.lvframe60;

		chr->sleep = 0;

		modelSetAnimation(chr->model, ANIM_0163, false, 0, 0.5f, 16);

		chr->drcarollimage_left = DRCAROLLIMAGE_X;
		chr->drcarollimage_right = DRCAROLLIMAGE_X;
		return;
	}

	instant = chr->actiontype == ACT_ARGH && chr->act_argh.lvframe60 == g_Vars.lvframe60;

	for (i = 0; g_AnimTablesByRace[race][i].hitpart != -1; i++) {
		if (g_AnimTablesByRace[race][i].hitpart == hitpart) {
			index = i;

			if (chr->hitpart == 0) {
				chr->hitpart = hitpart;
			}

			break;
		}
	}

	// If shot in the butt from behind, 2 in 5 chance of doing a special anim
	if (race == RACE_HUMAN
			&& hitpart == HITPART_PELVIS
			&& angle > 2.3558194637299f
			&& angle < 3.9263656139374f
			&& rngRandom() % 5 < 2) {
		struct animtablerow *row;
		struct animtablerow rows[] = {
			{ 0x013b, 0, -1, 0.5, 0, -1, -1 },
			{ 0x013c, 0, -1, 0.5, 0, -1, -1 },
			{ 0x013f, 0, -1, 0.5, 0, -1, -1 },
			{ 0x0142, 0, -1, 0.5, 0, -1, -1 },
			{ 0x0145, 0, -1, 0.5, 0, -1, -1 },
			{ 0x0148, 0, -1, 0.5, 0, -1, -1 },
			{ 0x0036, 0, -1, 0.5, 0, -1, -1 },
			{ 0x0037, 0, -1, 0.5, 0, -1, -1 },
		};

		chrStopFiring(chr);

		chr->actiontype = ACT_ARGH;
		chr->act_argh.notifychrindex = 0;
		chr->act_argh.lvframe60 = g_Vars.lvframe60;
		chr->sleep = 0;

		row = &rows[rngRandom() % 8];

		modelSetAnimationWithMerge(model, row->animnum, row->flip, 0, row->speed, 16, !instant);

		if (row->endframe >= 0) {
			modelSetAnimEndFrame(model, chrGetRangedArghSpeed(chr, row->endframe, 8));
			doneanim = true;
		} else {
			modelSetAnimEndFrame(model, chrGetRangedArghSpeed(chr, animGetNumFrames(row->animnum) - 1, 8));
			doneanim = true;
		}
	}

	if (!doneanim
			&& index >= 0
			&& g_AnimTablesByRace[race][index].injuryanims
			&& g_AnimTablesByRace[race][index].injuryanimcount > 0) {
		// If shot in a hand that's holding a gun, remap the hit location to the
		// forearm because the hand injury animations assume the hand is empty.
		struct prop *lgun = chrGetHeldProp(chr, HAND_LEFT);
		struct prop *rgun = chrGetHeldProp(chr, HAND_RIGHT);
		s32 rowindex;
		struct animtablerow *row;

		if (race == RACE_HUMAN) {
			if (index == 9 && lgun) { // left hand
				index = 10; // left forearm
			} else if (index == 12 && rgun) { // right hand
				index = 13; // right forearm
			}
		}

		// Select a random animation for this hit location and apply it
		rowindex = rngRandom() % g_AnimTablesByRace[race][index].injuryanimcount;

		row = &g_AnimTablesByRace[race][index].injuryanims[rowindex];

		chrStopFiring(chr);

		chr->actiontype = ACT_ARGH;
		chr->act_argh.notifychrindex = 0;
		chr->act_argh.lvframe60 = g_Vars.lvframe60;
		chr->sleep = 0;

		modelSetAnimationWithMerge(model, row->animnum, row->flip, 0, row->speed, 16, !instant);

		if (row->endframe >= 0) {
			modelSetAnimEndFrame(model, chrGetRangedArghSpeed(chr, row->endframe, 8));
		} else {
			modelSetAnimEndFrame(model, chrGetRangedArghSpeed(chr, animGetNumFrames(row->animnum) - 1, 8));
		}
	}
}

void chrReactToDamage(struct chrdata *chr, struct coord *vector, f32 angle, s32 hitpart, struct gset *gset, s32 aplayernum)
{
	s32 race = CHRRACE(chr);
	bool knockedout = false;
	s32 animnum = modelGetAnimNum(chr->model);

#if VERSION >= VERSION_NTSC_1_0
	if (chr->actiontype == ACT_DRUGGEDKO)
#else
	if (chr->actiontype == ACT_DRUGGEDCOMINGUP
			|| chr->actiontype == ACT_DRUGGEDDROP
			|| chr->actiontype == ACT_DRUGGEDKO)
#endif
	{
		knockedout = true;
	}

	if (race == RACE_EYESPY) {
		f32 strength = gsetGetImpactForce(gset);
		struct eyespy *eyespy = chrToEyespy(chr);

		if (eyespy) {
			if (strength <= 0) {
				strength = 6;
			}

			strength *= 4;

			eyespy->hit = EYESPYHIT_DAMAGE;
			eyespy->vel.x += vector->x * strength;
			eyespy->vel.z += vector->z * strength;
		}
	}

	if (chr->damage >= chr->maxdamage) {
		chrBeginDeath(chr, vector, angle, hitpart, gset, false, aplayernum);
	} else if (animnum == ANIM_SNIPING_GETDOWN
			|| animnum == ANIM_SNIPING_GETUP
			|| animnum == ANIM_SNIPING_ONGROUND) {
		chrFlinchBody(chr);
	} else if (race == RACE_EYESPY) {
		// empty
	} else if (race == RACE_DRCAROLL || race == RACE_ROBOT) {
		f32 strength = gsetGetImpactForce(gset);

		if (race == RACE_DRCAROLL) {
			strength *= 0.5f;
		}

		if (strength <= 0) {
			strength = 6;
		}

		if (strength > 0) {
			chr->elapseextra = 0;
			chr->timeextra = strength * 15;
			chr->extraspeed.x = vector->x * strength;
			chr->extraspeed.y = vector->y * strength;
			chr->extraspeed.z = vector->z * strength;
		}

		if (race == RACE_DRCAROLL) {
			chrBeginArgh(chr, 0, 0);
		}
	} else if (!knockedout) {
		chrBeginArgh(chr, angle, hitpart);
	}
}

/**
 * Launch a chr away from the given pos (for explosions).
 */
void chrYeetFromPos(struct chrdata *chr, struct coord *exppos, f32 force)
{
	struct model *model = chr->model;
	struct prop *prop = chr->prop;
	f32 faceangle;
	f32 latangle;
	u32 stack1;
	s32 angleindex;
	u32 stack2;
	struct yeetanim *row;
	struct coord dist;
	u32 stack3;
	s32 race = CHRRACE(chr);
	f32 speed;
	s32 subindex;
	f32 angletoexplosion;

	if (race != RACE_DRCAROLL && race != RACE_EYESPY && race != RACE_ROBOT) {
		faceangle = chrGetInverseTheta(chr);
		latangle = atan2f(prop->pos.x - exppos->x, prop->pos.z - exppos->z);

		dist.x = prop->pos.x - exppos->x;
		dist.y = prop->pos.y - exppos->y;
		dist.z = prop->pos.z - exppos->z;

		if (dist.f[0] == 0 && dist.f[1] == 0 && dist.f[2] == 0) {
			dist.z = 1;
		}

		speed = 0.625f * force / sqrtf(dist.f[0] * dist.f[0] + dist.f[1] * dist.f[1] + dist.f[2] * dist.f[2]);
		angletoexplosion = latangle - faceangle;

		dist.x *= speed;
		dist.y *= speed;
		dist.z *= speed;

		chr->fallspeed.x = dist.x;
		chr->fallspeed.y = dist.y;
		chr->fallspeed.z = dist.z;

		if (latangle < faceangle) {
			angletoexplosion += M_BADTAU;
		}

		angleindex = angletoexplosion * 1.2734422683716f + 0.5f;

		if (angleindex >= 8) {
			angleindex = 0;
		}

#ifndef PLATFORM_N64
		// Port: extreme face-vs-explosion geometry can drive angletoexplosion negative
		// (latangle - faceangle can fall below -TAU after the wrap), giving a negative
		// angleindex. The >= 8 clamp above never covered that, so g_YeetAnimIndexesByRaceAngle
		// gets read out of bounds, handing back a garbage `indexes` pointer that is then
		// dereferenced -> crash (read at 0xffff...). Clamp the low end too.
		if (angleindex < 0) {
			angleindex = 0;
		}
#endif

		subindex = rngRandom() % g_YeetAnimIndexesByRaceAngle[race][angleindex].count;

		if (race == RACE_HUMAN) {
			row = &g_YeetAnimsHuman[g_YeetAnimIndexesByRaceAngle[race][angleindex].indexes[subindex]];
		} else if (race == RACE_SKEDAR) {
			row = &g_YeetAnimsSkedar[g_YeetAnimIndexesByRaceAngle[race][angleindex].indexes[subindex]];
		}

		chrStopFiring(chr);
		chrUncloak(chr, true);

#if VERSION >= VERSION_NTSC_1_0
		chr->chrflags &= ~CHRCFLAG_HIDDEN;
#endif

		chr->actiontype = ACT_DIE;

		chr->act_die.notifychrindex = 0;
		chr->act_die.thudframe1 = row->thudframe;
		chr->act_die.thudframe2 = -1;
		chr->act_die.timeextra = 0;
		chr->act_die.drcarollimagedelay = TICKS(45);

		if (chr->race == RACE_DRCAROLL) {
			chr->drcarollimage_left = 1 + (s32)((rngRandom() % 400) * 0.01f);
			chr->drcarollimage_right = 1 + (s32)((rngRandom() % 400) * 0.01f);
		}

		chr->sleep = 0;
		modelSetAnimation(model, row->animnum, row->flip, row->startframe, row->speed, 8);

		if (row->endframe >= 0.0f) {
			modelSetAnimEndFrame(model, row->endframe);
		}
	}
}

s32 gsetGetBlurAmount(struct gset *gset)
{
	s32 amount = TICKS(1000);

	if (g_Vars.normmplayerisrunning == false) {
		amount = TICKS(250);
	}

	if (gset->weaponnum == WEAPON_TRANQUILIZER) {
		amount = TICKS(2000);
	}

	if (gset->weaponnum == WEAPON_BOLT) {
		amount = TICKS(5000);
	}

	if (gset->weaponnum == WEAPON_NBOMB) {
		amount = TICKS(100);
	}

	return amount;
}

void chrKnockOut(struct chrdata *chr, f32 angle, s32 hitpart, struct gset *gset)
{
	if (chr->actiontype != ACT_DRUGGEDCOMINGUP
			&& chr->actiontype != ACT_DRUGGEDDROP
			&& chr->actiontype != ACT_DRUGGEDKO) {
#if VERSION >= VERSION_PAL_FINAL
		if (mpstatsGetTotalKnockoutCount() < 2) {
			chr->chrflags |= CHRCFLAG_KEEPCORPSEKO;

			if (mainGetStageNum() == STAGE_VILLA) {
				chr->hidden |= CHRHFLAG_ANTINONINTERACTABLE;
			}
		}
#elif VERSION >= VERSION_NTSC_1_0
		if (mpstatsGetTotalKnockoutCount() < 2) {
			chr->chrflags |= CHRCFLAG_KEEPCORPSEKO;
		}
#endif

		mpstatsIncrementTotalKnockoutCount();

		chr->actiontype = ACT_DRUGGEDCOMINGUP;
		chr->act_druggedcomingup.timer60 = 0;
	}
}

/**
 * Return true if the chr's current animation would be too awkward to transition
 * into an injury animation or if they're already in the PREARGH action state,
 * and set dst to the anim frame number where the chr will become available for
 * transition to an injury animation.
 *
 * The attack roll animation is the only one which is too awkward to transition.
 */
bool chrIsAnimPreventingArgh(struct chrdata *chr, f32 *dst)
{
	bool result = false;
	s32 race = CHRRACE(chr);

	if (race == RACE_DRCAROLL || race == RACE_EYESPY || chr->aibot) {
		return false;
	}

	if (race == RACE_HUMAN) {
		s32 animnum = modelGetAnimNum(chr->model);
		f32 endframe;

		if (animnum == ANIM_SNIPING_GETDOWN
				|| animnum == ANIM_SNIPING_GETUP
				|| animnum == ANIM_SNIPING_ONGROUND) {
			chrFlinchBody(chr);
		} else if (chr->actiontype == ACT_ATTACKROLL
				&& modelGetAnimNum(chr->model) == chr->act_attack.animcfg->animnum) {
			if (chr->act_attack.onehanded) {
				if (chr->act_attack.animcfg == &g_RollAttackAnims[4]
						|| chr->act_attack.animcfg == &g_RollAttackAnims[5]
						|| chr->act_attack.animcfg == &g_RollAttackAnims[6]
						|| chr->act_attack.animcfg == &g_RollAttackAnims[7]) {
					endframe = chr->act_attack.animcfg->unk04 - 8;

					if (chr->act_attack.animcfg->unk14 < chr->act_attack.animcfg->unk04) {
						endframe = chr->act_attack.animcfg->unk14;
					}

					if (endframe > modelGetCurAnimFrame(chr->model)) {
						*dst = endframe;
						result = true;
					}
				}
			} else {
				endframe = chr->act_attack.animcfg->unk04 - 8;

				if (endframe > modelGetCurAnimFrame(chr->model)) {
					*dst = endframe;
					result = true;
				}
			}
		}
	}

	if (chr->actiontype == ACT_PREARGH) {
		result = true;
	}

	return result;
}

void chrChoke(struct chrdata *chr, s32 choketype)
{
	bool male = false;
	s16 soundnum = -1;
	s32 race = CHRRACE(chr);
	s32 playernum;
	s32 allowoverride = false;

	static s32 nextindexdrcaroll = 0;
	static s32 nextindexmaian = 0;
	static s32 nextindexskedar = 0;
	static s32 nextindexshock = 0;
	static s32 nextindexmale = 0;
	static s32 nextindexfemale = 0;

	if (race == RACE_EYESPY || race == RACE_ROBOT) {
		return;
	}

	if (chr->prop->type == PROPTYPE_PLAYER) {
		playernum = playermgrGetPlayerNumByProp(chr->prop);

		if (g_Vars.players[playernum]->isdead) {
			return;
		}
	}

	if (g_HeadsAndBodies[chr->headnum].ismale) {
		male = true;
	}

	if (race == RACE_DRCAROLL) {
		s16 sounds[] = {
			SFX_ARGH_DRCAROLL_0240,
			SFX_ARGH_DRCAROLL_024C,
			SFX_ARGH_DRCAROLL_0250,
			SFX_ARGH_DRCAROLL_0251,
			SFX_ARGH_DRCAROLL_0259,
			SFX_ARGH_DRCAROLL_025A,
		};

		if (g_DrCarollDyingTimer > TICKS(10)) {
			g_DrCarollDyingTimer = 0;

			soundnum = sounds[nextindexdrcaroll];
			nextindexdrcaroll++;

			if (nextindexdrcaroll >= ARRAYCOUNT(sounds)) {
				nextindexdrcaroll = 0;
			}
		}
#ifndef PLATFORM_N64 // PD Plus Mod
	} else if (chr->bodynum == BODY_TESTCHR) { // Dr. Caroll
		s16 sounds[] = {
			SFX_ARGH_DRCAROLL_0240,
			SFX_ARGH_DRCAROLL_024C,
			SFX_ARGH_DRCAROLL_0250,
			SFX_ARGH_DRCAROLL_0251,
			SFX_ARGH_DRCAROLL_0259,
			SFX_ARGH_DRCAROLL_025A,
		};

		soundnum = sounds[nextindexdrcaroll];
		nextindexdrcaroll++;

		if (nextindexdrcaroll >= ARRAYCOUNT(sounds)) {
			nextindexdrcaroll = 0;
		}
	} else if (chr->bodynum == BODY_PRESIDENT_CLONE) { // Skedar
		s16 sounds[] = {
			SFX_SKEDAR_ROAR_052D,
			SFX_SKEDAR_ROAR_052E,
			SFX_SKEDAR_ROAR_052F,
		};

		soundnum = sounds[rngRandom() % 3];
		nextindexskedar++;

		if (nextindexskedar >= ARRAYCOUNT(sounds)) {
			nextindexskedar = 0;
		}
#endif
	} else if (chr->headnum == HEAD_THEKING
			|| chr->headnum == HEAD_ELVIS
			|| chr->headnum == HEAD_MAIAN_S
			|| chr->headnum == HEAD_ELVIS_GOGS) {
		s16 sounds[] = {
			SFX_ARGH_MAIAN_05DF,
			SFX_ARGH_MAIAN_05E0,
			SFX_ARGH_MAIAN_05E1
		};

		soundnum = sounds[rngRandom() % 3];
		nextindexmaian++;

		if (nextindexmaian >= ARRAYCOUNT(sounds)) {
			nextindexmaian = 0;
		}
	} else if (race == RACE_SKEDAR) {
		if (chr->bodynum == BODY_MINISKEDAR) {
			s16 sounds[] = {
				SFX_SKEDAR_ROAR_0536,
				SFX_SKEDAR_ROAR_0537,
				SFX_SKEDAR_ROAR_0538,
				SFX_SKEDAR_ROAR_0539,
				SFX_SKEDAR_ROAR_053A,
			};

			soundnum = sounds[rngRandom() % 5];
			nextindexskedar++;

			if (nextindexskedar >= ARRAYCOUNT(sounds)) {
				nextindexskedar = 0;
			}
		} else {
			s16 sounds[] = {
				SFX_SKEDAR_ROAR_052D,
				SFX_SKEDAR_ROAR_052E,
				SFX_SKEDAR_ROAR_052F,
			};

			soundnum = sounds[rngRandom() % 3];
			nextindexskedar++;

			if (nextindexskedar >= ARRAYCOUNT(sounds)) {
				nextindexskedar = 0;
			}
		}
	} else if (chr->headnum == HEAD_DDSHOCK) {
		s16 sounds[] = {
			SFX_ARGH_MALE_0086,
			SFX_ARGH_MALE_0088,
			SFX_ARGH_MALE_008A,
			SFX_ARGH_MALE_008C,
			SFX_ARGH_MALE_008E,
			SFX_ARGH_MALE_0090,
			SFX_ARGH_MALE_0092,
			SFX_ARGH_MALE_0094,
			SFX_ARGH_MALE_0096,
			SFX_ARGH_MALE_0098,
			SFX_ARGH_MALE_009A,
			SFX_ARGH_MALE_009C,
			SFX_ARGH_MALE_009E,
			SFX_ARGH_MALE_0087,
		};

		soundnum = sounds[nextindexshock];
		nextindexshock++;

		if (nextindexshock >= ARRAYCOUNT(sounds)) {
			nextindexshock = 0;
		}

		allowoverride = true;
	} else if (male) {
		s16 sounds[] = {
			SFX_ARGH_MALE_0086,
			SFX_ARGH_MALE_0087,
			SFX_ARGH_MALE_0088,
			SFX_ARGH_MALE_0089,
			SFX_ARGH_MALE_008A,
			SFX_ARGH_MALE_008B,
			SFX_ARGH_MALE_008C,
			SFX_ARGH_MALE_008D,
			SFX_ARGH_MALE_008E,
			SFX_ARGH_MALE_008F,
			SFX_ARGH_MALE_0090,
			SFX_ARGH_MALE_0091,
			SFX_ARGH_MALE_0092,
			SFX_ARGH_MALE_0093,
			SFX_ARGH_MALE_0094,
			SFX_ARGH_MALE_0095,
			SFX_ARGH_MALE_0096,
			SFX_ARGH_MALE_0097,
			SFX_ARGH_MALE_0098,
			SFX_ARGH_MALE_0099,
			SFX_ARGH_MALE_009A,
			SFX_ARGH_MALE_009B,
			SFX_ARGH_MALE_009C,
			SFX_ARGH_MALE_009D,
			SFX_ARGH_MALE_009E,
		};

		soundnum = sounds[nextindexmale];
		nextindexmale++;

		allowoverride = true;

		if (nextindexmale >= ARRAYCOUNT(sounds)) {
			nextindexmale = 0;
		}
	} else if (chr->headnum == HEAD_DARK_COMBAT
			|| chr->headnum == HEAD_DARK_FROCK
			|| chr->headnum == HEAD_DARKAQUA
			|| chr->headnum == HEAD_DARK_SNOW) {
		s16 sounds[] = {
			SFX_ARGH_JO_02AA,
			SFX_ARGH_JO_02AB,
			SFX_ARGH_JO_02AC,
			SFX_ARGH_JO_02AD,
			SFX_ARGH_JO_02AE,
			SFX_ARGH_JO_02AF,
			SFX_ARGH_JO_02B0,
			SFX_ARGH_JO_02B1,
			SFX_ARGH_JO_02B2,
			SFX_ARGH_JO_02B3,
		};

		soundnum = sounds[rngRandom() % 10];
		allowoverride = true;
	} else {
		s16 sounds[] = {
			SFX_ARGH_FEMALE_000D,
			SFX_ARGH_FEMALE_000E,
			SFX_ARGH_FEMALE_000F,
		};

		soundnum = sounds[nextindexfemale];
		nextindexfemale++;

		if (nextindexfemale >= ARRAYCOUNT(sounds)) {
			nextindexfemale = 0;
		}

		allowoverride = true;
	}

	if (allowoverride) {
		if (choketype == CHOKETYPE_GURGLE) {
			s32 sounds[] = {
				SFX_M1_CHOKING,
				SFX_GURGLE_05B1,
				SFX_GURGLE_05B2,
			};

			if ((rngRandom() % 8) == 0) {
				soundnum = sounds[rngRandom() % 3];
			}

			chr->soundgap = 10;
			chr->soundtimer = 0;
		} else if (choketype == CHOKETYPE_COUGH) {
			if (male) {
				if ((rngRandom() % 2) == 0) {
					soundnum = SFX_COUGH_04AF;
				} else {
					soundnum = SFX_COUGH_04B0;
				}
			} else {
				s32 index = rngRandom() % 4;
				s32 sounds[] = {
					SFX_COUGH_05AB,
					SFX_COUGH_05AC,
					SFX_COUGH_05AD,
					SFX_COUGH_05AE,
				};

				soundnum = sounds[index];
			}

			chr->soundgap = 10;
			chr->soundtimer = 0;
		}
	}

	if (soundnum >= 0) {
		if (chr->prop->type == PROPTYPE_PLAYER) {
			if (g_Vars.players[playernum]->chokehandle == NULL) {
				playerSndStart(var80095200, soundnum, &g_Vars.players[playernum]->chokehandle, playernum, -1, -1, -1);
			}
		} else {
			psStopSound(chr->prop, PSTYPE_CHRTALK, 0);

			if (!psPropHasSoundWithContext(chr->prop, PSTYPE_CHRCHOKE)) {
				psCreate(NULL, chr->prop, soundnum, -1,
						-1, 0, 0, PSTYPE_CHRCHOKE, NULL, -1, NULL, -1, -1, -1, -1);
			}
		}
	}
}

f32 chrGetShield(struct chrdata *chr)
{
#ifndef PLATFORM_N64
	// Crash ledger #18: lvUpdateSoloHandicaps -> playerGetShieldFrac passes
	// currentplayer->prop->chr, which is NULL for a pawn-less net client
	// (Host-Online lobby / spectator / between matches on CI) — read at
	// NULL+0x1cc. Guard here so every caller is covered.
	if (chr == NULL) {
		return 0;
	}
#endif
	return chr->cshield;
}

void chrSetShield(struct chrdata *chr, f32 amount)
{
	if (amount < 0) {
		amount = 0;
	}

	chr->cshield = amount;

	if ((chr->hidden & CHRHFLAG_INFINITESHIELD) && chr->cshield < 1) {
		chr->cshield = 1;
	}

	if (chr->prop->type == PROPTYPE_PLAYER) {
		s32 playernum = playermgrGetPlayerNumByProp(chr->prop);

		if (playernum >= 0) {
			s32 prevplayernum = g_Vars.currentplayernum;
			setCurrentPlayerNum(playernum);
			playerDisplayHealth();
			g_Vars.currentplayerstats->armourcount += amount * 0.125f;
			setCurrentPlayerNum(prevplayernum);
		}
	}
}

bool func0f034080(struct chrdata *chr, struct modelnode *node, struct prop *prop, struct model *model, s32 side, s16 *arg5)
{
	if (chrGetShield(chr) > 0) {
		if (node && (node->type & 0xff) == MODELNODETYPE_BBOX) {
			shieldhitCreate(chr->prop, chrGetShield(chr), prop, node, model, side, arg5);
		}

		return true;
	}

	return false;
}

/**
 * Damage the chr, bypassing any shield.
 *
 * Used for knife poison, nbomb damage, Investigation radioactivity and Escape gas.
 */
void chrDamageByMisc(struct chrdata *chr, f32 damage, struct coord *vector, struct gset *gset, struct prop *prop)
{
#ifndef PLATFORM_N64
	if (g_NetMode == NETMODE_CLIENT) {
		// don't do anything if we're not the authority
		return;
	}
#endif
	chrDamage(chr, damage, vector, gset, prop, HITPART_GENERAL,
			false,     // damageshield
			NULL,      // prop2
			NULL,      // node
			NULL,      // model
			-1,        // side
			NULL,      // arg11
			false,     // explosion
			NULL);     // explosionpos
}

void chrDamageByLaser(struct chrdata *chr, f32 damage, struct coord *vector, struct gset *gset, struct prop *prop)
{
#ifndef PLATFORM_N64
	if (g_NetMode == NETMODE_CLIENT) {
		// don't do anything if we're not the authority
		return;
	}
#endif
	chrDamage(chr, damage, vector, gset, prop, HITPART_GENERAL,
			true,      // damageshield
			chr->prop, // prop2
			NULL,      // node
			NULL,      // model
			-1,        // side
			NULL,      // arg11
			false,     // explosion
			NULL);     // explosionpos
}

void func0f0341dc(struct chrdata *chr, f32 damage, struct coord *vector, struct gset *gset, struct prop *prop, s32 hitpart, struct prop *prop2, struct modelnode *node, struct model *model, s32 side, s16 *arg10)
{
#ifndef PLATFORM_N64
	if (g_NetMode == NETMODE_CLIENT) {
		// Report the hit to the server. It runs chrDamage there and broadcasts
		// SVC_CHR_DAMAGE back to all clients. Only send if the target has a
		// syncid — unsynced props (e.g. local-only debris) have none.
		//
		// ONLY for the LOCAL player's own gunfire. The client also simulates
		// REMOTE players' guns locally (bgunTick runs for remote pawns to drive
		// visuals/positional audio), and that simulation's shots land here too —
		// reporting those echoed every other player's hits to the server as OUR
		// hits: the victim took the damage twice, and whenever the echo landed
		// the killing blow the kill was credited to this client (the "AFK player
		// racking up eliminations" bug — an idle client's machine echoing the
		// host's shots). Same guard netClientReportPropHit has always had.
		if (chr->prop && chr->prop->syncid
				&& g_Vars.currentplayer && !g_Vars.currentplayer->isremote) {
			netbufStartWrite(&g_NetMsgRel);
			netmsgClcHitWrite(&g_NetMsgRel, chr, damage, vector, gset, (s16)hitpart, (s16)side, arg10);
			netSend(g_NetLocalClient, &g_NetMsgRel, true, NETCHAN_CONTROL);
			// Hidden test feature: immediate local hit confirmation (centred
			// marker), so the shooter gets feedback now instead of after the
			// server's SVC_CHR_DAMAGE round-trip. No-op unless /hitmarker is on.
			g_NetHitmarkerExpireTick = g_NetTick + NET_HITMARKER_TICKS;
		}
		return;
	}
#endif
	chrDamage(chr, damage, vector, gset, prop, hitpart,
			true,      // damageshield
			prop2,     // prop2
			node,      // node
			model,     // model
			side,      // side
			arg10,     // arg11
			false,     // explosion
			NULL);     // explosionpos
}

/**
 * Unused, and same as chrDamageByImpact but sets hitpart to HITPART_GENERAL instead of argument.
 */
void func0f034248(struct chrdata *chr, f32 damage, struct coord *vector, struct gset *gset, struct prop *prop)
{
	struct modelnode *node = NULL;
	struct model *model = NULL;
	s32 side = 0;
	s32 hitpart = HITPART_GENERAL;

#ifndef PLATFORM_N64
	if (g_NetMode == NETMODE_CLIENT) {
		// don't do anything if we're not the authority
		return;
	}
#endif

	if (chrGetShield(chr) >= 0 && chr->model) {
		chrCalculateShieldHit(chr, &chr->prop->pos, vector, &node, &hitpart, &model, &side);
	}

	chrDamage(chr, damage, vector, gset, prop, HITPART_GENERAL,
			true,      // damageshield
			chr->prop, // prop2
			node,      // node
			model,     // model
			side,      // side
			NULL,      // arg11
			false,     // explosion
			NULL);     // explosionpos
}

/**
 * Used for punching, but also used by AI commands to make chrs take damage.
 */
void chrDamageByImpact(struct chrdata *chr, f32 damage, struct coord *vector, struct gset *gset, struct prop *prop, s32 hitpart)
{
	struct modelnode *node = NULL;
	struct model *model = NULL;
	s32 side = 0;

#ifndef PLATFORM_N64
	if (g_NetMode == NETMODE_CLIENT) {
		// don't do anything if we're not the authority
		return;
	}
#endif

	if (chrGetShield(chr) >= 0 && chr->model) {
		chrCalculateShieldHit(chr, &chr->prop->pos, vector, &node, &hitpart, &model, &side);
	}

	chrDamage(chr, damage, vector, gset, prop, hitpart,
			true,      // damageshield
			chr->prop, // prop2
			node,      // node
			model,     // model
			side,      // side
			NULL,      // arg11
			false,     // explosion
			NULL);     // explosionpos
}

void chrDamageByExplosion(struct chrdata *chr, f32 damage, struct coord *vector, struct prop *prop, struct coord *explosionpos)
{
#ifndef PLATFORM_N64
	if (g_NetMode == NETMODE_CLIENT) {
		// don't do anything if we're not the authority
		return;
	}
#endif
	chrDamage(chr, damage, vector, NULL, prop, HITPART_GENERAL,
			true,      // damageshield
			chr->prop, // prop2
			NULL,      // node
			NULL,      // model
			-1,        // side
			NULL,      // arg11
			true,      // explosion
			explosionpos);
}

void playerUpdateDamageStats(struct prop *attacker, struct prop *victim, f32 damage)
{
	s32 playernum;

	if (attacker && attacker->type == PROPTYPE_PLAYER) {
		playernum = playermgrGetPlayerNumByProp(attacker);

		if (playernum >= 0) {
			g_Vars.playerstats[playernum].damtransmitted += damage;
		}
	}

	if (victim && victim->type == PROPTYPE_PLAYER) {
		playernum = playermgrGetPlayerNumByProp(victim);

		if (playernum >= 0) {
			g_Vars.playerstats[playernum].damreceived += damage;
		}
	}
}

/**
 * Handle a chr being damaged.
 *
 * Calculates and damage based on many factors and applies it to the chr,
 * killing them if needed.
 *
 * The chr can be a player, aibot or regular chr. The chr can also be of any
 * race (human, Skedar, Dr Caroll, robot or eyespy).
 *
 * chr - the chr being damaged
 * damage - the base amount of damage to deal, prior to scaling factors
 * vector - position of the attacker?
 * gset - gun settings struct
 * aprop - the attacker's prop struct
 * hitpart - "i've been hit" value, ie. the body part (see HITPART constants)
 * damageshield - false if attack should bypass shield if any
 * prop2 - ?
 * node - if shielded, model node (of type bbox) which was hit
 * model - if shielded, model of chr
 * side - if shielded, side of the model node's bounding box which was hit (0-5)
 * arg11 - ?
 * explosion - true if damage is coming from an explosion
 * explosionpos - position of said explosion
 */
#ifndef PLATFORM_N64
bool chaosIsTwin(struct chrdata *chr); // chaos Evil twin — defined below
#endif
void chrDamage(struct chrdata *chr, f32 damage, struct coord *vector, struct gset *gset,
		struct prop *aprop, s32 hitpart, bool damageshield, struct prop *prop2,
		struct modelnode *node, struct model *model, s32 side, s16 *arg11,
		bool explosion, struct coord *explosionpos)
{
#ifndef PLATFORM_N64
	// GoldenEye Style i-frames: any chr that took damage in the last
	// TICKS(18) (~300ms at 60Hz) is invulnerable until the window
	// elapses. Applies to players AND sims/chrs uniformly. Zero is the
	// "never damaged" sentinel so the very first hit always lands.
	// The stamp itself is set at the actual damage-application sites
	// below (the player-damage branch and the sim chr->damage += site)
	// so probe / zero-damage calls (gun/hat hits, hits to a chr that
	// is invincible or already dead) don't start a spurious window.
	//
	// Subtraction is done in u32: a stale stamp from a previous stage
	// (lastdamagetick60 > current lvframe60 because the level counter
	// reset to 0) wraps to a huge unsigned value, which correctly
	// fails the < TICKS(18) check instead of producing a negative
	// signed number that would lock the chr into permanent iframes.
	// i-frame enforcement is server-authoritative. On a client, chrDamage is only
	// ever reached as an authoritative replay of a server-sent SVC_CHR_DAMAGE
	// (the chrDamageBy* originators all early-return under NETMODE_CLIENT), so the
	// client must NOT independently re-evaluate the i-frame window against its own
	// lvframe60 clock — doing so would reject (or admit) hits out of step with the
	// host, since lastdamagetick60 is stamped locally and never synced. The host
	// already gated the hit before broadcasting. See review doc (H-3).
	if (g_NetMode != NETMODE_CLIENT
			&& classicOptionActive(CHEAT_CLASSIC_IFRAMES, MPOPTION_CLASSIC_IFRAMES)
			&& chr->lastdamagetick60 != 0
			&& ((u32)g_Vars.lvframe60 - (u32)chr->lastdamagetick60) < (u32)TICKS(18)) {
		netDiagLogf("dmg_block",
				"chrnum=%d sid=%u stamp=%d age=%u window=%u dmg=%.2f",
				(s32)chr->chrnum,
				chr->prop ? (u32)chr->prop->syncid : 0u,
				chr->lastdamagetick60,
				(u32)g_Vars.lvframe60 - (u32)chr->lastdamagetick60,
				(u32)TICKS(18),
				damage);
		return;
	}
#endif

	bool onehitko = false;
	s32 race = CHRRACE(chr);
	f32 shield;
	bool makedizzy;
	bool ismelee;
	struct prop *vprop = chr->prop;
	f32 headshotdamagescale = 1;
	bool usedshield = false;
	bool showshield = false;
	bool showdamage = false;
	struct gset gset2 = {0};
	f32 explosionforce = damage;
	f32 healthscale = 1;
	f32 armourscale = 1;
	bool isshoot = true;
	bool forceapplydamage = false;
	struct weaponfunc *func;
	f32 amount;
	bool canchoke = true;
	s32 aplayernum = -1;
	s32 choketype = CHOKETYPE_NONE;

#ifndef PLATFORM_N64
	// Chaos "one punch" (docs/PORT_CHAOS.md, pd.one_punch): a player's
	// unarmed strike is lethal through any armour and launches the victim
	// (the explosion-knockback fling). Boosted BEFORE the SVC_CHR_DAMAGE
	// broadcast below so net clients replay the same lethal hit. NPC victims
	// only — other players / co-op partners take normal fist damage.
	if (g_ChaosOnePunch && g_NetMode != NETMODE_CLIENT
			&& gset && gset->weaponnum == WEAPON_UNARMED
			&& aprop && aprop->type == PROPTYPE_PLAYER
			&& vprop && vprop->type == PROPTYPE_CHR
			&& !chrIsDead(chr)) {
		damage = chrGetMaxDamage(chr) + chrGetShield(chr) + 100.0f;
		if (chr->model) {
			chrYeetFromPos(chr, &aprop->pos, 250.0f);
		}
	}

	// Chaos "Space Program" (pd.space_program): the one_punch treatment for GUN
	// shots — any player bullet is lethal through armour and launches the victim
	// with MASSIVE knockback (the fling force is ~3.5x one_punch's). NPC victims
	// only; boosted before the SVC_CHR_DAMAGE broadcast so net clients agree.
	if (g_ChaosSpaceProgram && g_NetMode != NETMODE_CLIENT
			&& gset && gset->weaponnum != WEAPON_UNARMED && gset->weaponnum != WEAPON_NONE
			&& aprop && aprop->type == PROPTYPE_PLAYER
			&& vprop && vprop->type == PROPTYPE_CHR
			&& !chrIsDead(chr)) {
		damage = chrGetMaxDamage(chr) + chrGetShield(chr) + 100.0f;
		if (chr->model) {
			f32 horiz;

			chrYeetFromPos(chr, &aprop->pos, 450.0f);

			// chrYeetFromPos only sets a near-horizontal knockback (attacker and
			// victim stand at similar heights, so dist.y ~ 0). Add an UPWARD kick
			// sized to that horizontal launch so victims rocket skyward as well as
			// back — a "space program" ~45deg arc (+Y is up; gravity turns it into
			// an arc). Self-calibrating off the yeet's own magnitude, so halving
			// the launch force above halves the up-boost proportionally too.
			horiz = sqrtf(chr->fallspeed.x * chr->fallspeed.x
					+ chr->fallspeed.z * chr->fallspeed.z);
			chr->fallspeed.y += horiz * 1.25f;
		}
	}

	// Chaos "Paintball" damage scale (pd.damage_scale): scale ALL chr/player
	// damage. Applied before the SVC_CHR_DAMAGE broadcast so net clients
	// replay the same scaled hit.
	if (g_ChaosDamageScale >= 0.0f && g_ChaosDamageScale != 1.0f) {
		damage *= g_ChaosDamageScale;
	}

	if (g_NetMode == NETMODE_SERVER) {
		netmsgSvcChrDamageWrite(&g_NetMsgRel, chr, damage, vector, gset, aprop, hitpart,
				damageshield, prop2, side, arg11, explosion, explosionpos);
	}
#endif

	if (hitpart == HITPART_HEAD) {
		choketype = CHOKETYPE_GURGLE;
	}

	if (gset) {
		if (gset->weaponnum == WEAPON_COMBATKNIFE) {
			if (gset->weaponfunc == FUNC_2) {
				canchoke = false;
			}

			if (gset->weaponfunc == FUNC_POISON) {
				choketype = CHOKETYPE_COUGH;
			}
		} else if (gset->weaponnum == WEAPON_TRANQUILIZER) {
			if (gset->weaponfunc == FUNC_SECONDARY) {
				choketype = CHOKETYPE_GURGLE;
			}
		}
	}

	// Don't damage if in CI training outside of training session
	if (chr->prop == g_Vars.currentplayer->prop
			&& g_Vars.currentplayer->training == false
			&& mainGetStageNum() == STAGE_CITRAINING) {
		return;
	}

	// Don't damage if attacker was anti and chr is non-interactable by anti
	if (g_Vars.antiplayernum >= 0
			&& aprop
			&& PROP_IS_FOR_ANTI_PLAYER(aprop)
			&& (chr->hidden & CHRHFLAG_ANTINONINTERACTABLE)) {
		return;
	}

	// Don't damage if coop and friendly fire is off (human buddy)
	if (g_Vars.coopplayernum >= 0
			&& g_Vars.coopfriendlyfire == false
			&& aprop
			&& aprop != vprop
			&& aprop->type == PROPTYPE_PLAYER
			&& vprop->type == PROPTYPE_PLAYER) {
		return;
	}

	// Don't damage if coop and friendly fire is off (AI buddy)
	if (g_MissionConfig.iscoop
			&& g_Vars.coopfriendlyfire == false
			&& aprop
			&& aprop != vprop
			&& (aprop->type == PROPTYPE_PLAYER || aprop->type == PROPTYPE_CHR)
			&& chr->team == TEAM_ALLY
			&& aprop->chr->team == TEAM_ALLY) {
		return;
	}

	// Don't allow coop AI to kill or destroy anything
	// which anti wouldn't be able to
	if (g_MissionConfig.iscoop
			&& aprop
			&& aprop != vprop
			&& aprop->type == PROPTYPE_CHR
			&& aprop->chr->team == TEAM_ALLY
			&& (chr->hidden & CHRHFLAG_ANTINONINTERACTABLE)) {
		return;
	}

#ifndef PLATFORM_N64
	// Don't damage if team multiplayer and friendly fire is off
	if (g_Vars.mplayerisrunning
			&& (g_MpSetup.options & MPOPTION_TEAMSENABLED)
			&& !(g_MpSetup.options & MPOPTION_FRIENDLYFIRE)
			&& aprop
			&& aprop != vprop
			&& (aprop->type == PROPTYPE_PLAYER || aprop->type == PROPTYPE_CHR)
			&& chr->team == aprop->chr->team) {
		return;
	}
#endif

	if (gset == NULL) {
		gset = &gset2;
	}

	func = gsetGetWeaponFunction(gset);
	ismelee = func && (func->type & 0xff) == INVENTORYFUNCTYPE_MELEE;
	makedizzy = race != RACE_DRCAROLL && gsetHasFunctionFlags(gset, FUNCFLAG_MAKEDIZZY);

#ifndef PLATFORM_N64
	// Classic "No Blur Effects": no dizzy / blur effects. Clamping
	// `makedizzy` here disables both the player-dizzy block (~4860 — blur
	// accum + FOV bobbing on screen) and the chr-dizzy paths further down,
	// without having to scatter gates at every downstream site.
	if (classicOptionActive(CHEAT_CLASSIC_NOBLUR, MPOPTION_CLASSIC_NOBLUR)) {
		makedizzy = false;
	}
#endif

	if (chr->prop == g_Vars.currentplayer->prop && g_Vars.currentplayer->invincible) {
		return;
	}

#ifndef PLATFORM_N64
	// Respawn Invulnerability (proto 77): 2s of full damage immunity after a
	// respawn — same early-out as the invincibility cheat above, so no chip
	// damage gets through, not just no death. Kill-plane / disconnect kills use
	// playerDieByShooter's `force` path and never reach this handler.
	if (chr->prop == g_Vars.currentplayer->prop && g_Vars.currentplayer->respawnprotect60 > 0) {
		return;
	}
#endif

	if (ismelee) {
		isshoot = false;
	}

	// Set a flag on the victim that makes them switch to their "shot" AI list
	chr->chrflags |= CHRCFLAG_TRIGGERSHOTLIST;

	// Set a flag on the attacker so their AI scripting can tell that they've
	// hit their target
	if (aprop
			&& aprop->type == PROPTYPE_CHR
			&& chrGetTargetProp(aprop->chr) == chr->prop) {
		aprop->chr->chrflags |= CHRCFLAG_INJUREDTARGET;
	}

	// Disarm only hurts the victim in solo missions and if the victim is an NPC
	if (gsetHasFunctionFlags(gset, FUNCFLAG_DISARM)
			&& gset->weaponnum == WEAPON_UNARMED
			&& (vprop->type == PROPTYPE_PLAYER || g_Vars.normmplayerisrunning)) {
		damage = 0;
	}

	// Apply damage scaling based on difficulty settings
	if (g_Vars.mplayerisrunning == false) {
		// Solo
		if (explosion) {
			if (vprop->type == PROPTYPE_PLAYER) {
				damage *= g_ExplosionDamageTxScale;
			}
		} else if (aprop && aprop->type == PROPTYPE_PLAYER) {
			// Player is attacking
			damage *= g_PlayerDamageTxScale;
			headshotdamagescale = 25;
		} else if (aprop && aprop->type == PROPTYPE_CHR && vprop->type == PROPTYPE_PLAYER) {
			// Chr is attacking player
			damage *= g_PlayerDamageRxScale * pdmodeGetEnemyDamage();
		}

		if (vprop->type != PROPTYPE_PLAYER) {
			damage /= pdmodeGetEnemyHealth();
		}

		if (vprop->type == PROPTYPE_PLAYER) {
			healthscale = g_Vars.players[playermgrGetPlayerNumByProp(vprop)]->healthscale;
			armourscale = g_Vars.players[playermgrGetPlayerNumByProp(vprop)]->armourscale;
		}
	} else if (g_Vars.coopplayernum >= 0) {
		// Co-op
		if (explosion) {
			if (vprop->type == PROPTYPE_PLAYER) {
				damage *= g_ExplosionDamageTxScale;
			}
		} else if (aprop && aprop->type == PROPTYPE_PLAYER && vprop->type != PROPTYPE_PLAYER) {
			damage *= g_PlayerDamageTxScale;
			headshotdamagescale = 25;
		} else if (aprop && aprop->type == PROPTYPE_CHR && vprop->type == PROPTYPE_PLAYER) {
			damage *= g_PlayerDamageRxScale * pdmodeGetEnemyDamage();
		}

		if (vprop->type != PROPTYPE_PLAYER) {
			damage /= pdmodeGetEnemyHealth();
		}

		if (vprop->type == PROPTYPE_PLAYER) {
			healthscale = g_Vars.players[playermgrGetPlayerNumByProp(vprop)]->healthscale;
			armourscale = g_Vars.players[playermgrGetPlayerNumByProp(vprop)]->armourscale;
		}
	} else if (g_Vars.antiplayernum >= 0) {
		// Anti
		if (explosion) {
			if (vprop == g_Vars.bond->prop) {
				damage *= g_ExplosionDamageTxScale;
			}
		} else if (aprop && aprop == g_Vars.bond->prop) {
			damage *= g_PlayerDamageTxScale;
			headshotdamagescale = 25;
		} else if (aprop && aprop != g_Vars.bond->prop && vprop == g_Vars.bond->prop) {
			damage *= g_PlayerDamageRxScale * pdmodeGetEnemyDamage();
		}

		if (vprop != g_Vars.bond->prop) {
			damage /= pdmodeGetEnemyHealth();
		}

		if (vprop == g_Vars.bond->prop) {
			healthscale = g_Vars.players[playermgrGetPlayerNumByProp(vprop)]->healthscale;
			armourscale = g_Vars.players[playermgrGetPlayerNumByProp(vprop)]->armourscale;
		}

		// Anti shooting other enemies is lethal
		if (aprop
				&& PROP_IS_FOR_ANTI_PLAYER(aprop)
				&& vprop != g_Vars.bond->prop) {
			damage *= 100;
		}
	} else {
		// Normal multiplayer
		if (vprop->type == PROPTYPE_PLAYER) {
			s32 prevplayernum = g_Vars.currentplayernum;
			setCurrentPlayerNum(playermgrGetPlayerNumByProp(vprop));
			damage *= g_Vars.currentplayerstats->damagescale;
			setCurrentPlayerNum(prevplayernum);
		}
	}

	// Apply rumble
	if (vprop->type == PROPTYPE_PLAYER) {
		s32 prevplayernum = g_Vars.currentplayernum;

#if VERSION >= VERSION_NTSC_1_0
		s32 contpad1;
		s32 contpad2;

		setCurrentPlayerNum(playermgrGetPlayerNumByProp(vprop));

		joyGetContpadNumsForPlayer(g_Vars.currentplayernum, &contpad1, &contpad2);

		if (contpad1 >= 0) {
			pakRumble(contpad1, 0.25f, -1, -1);
		}

		if (contpad2 >= 0) {
			pakRumble(contpad2, 0.25f, -1, -1);
		}
#else
		setCurrentPlayerNum(playermgrGetPlayerNumByProp(vprop));

		pakRumble((s8)g_Vars.currentplayernum, 0.25f, -1, -1);

		s32 controlmode = optionsGetControlMode(g_Vars.currentplayerstats->mpindex);
		if (controlmode >= CONTROLMODE_21 && controlmode < CONTROLMODE_PC) {
			pakRumble((s8)(PLAYERCOUNT() + g_Vars.currentplayernum), 0.25f, -1, -1);
		}
#endif

		setCurrentPlayerNum(prevplayernum);
	}

	// Find the attacker's player number if possible
	// (includes MP aibots, not applicable for solo chrs)
	if (g_Vars.mplayerisrunning) {
		if (aprop && (aprop->type == PROPTYPE_PLAYER || aprop->type == PROPTYPE_CHR)) {
			aplayernum = mpPlayerGetIndex(aprop->chr);
		}
	} else {
		if (aprop && aprop->type == PROPTYPE_PLAYER) {
			aplayernum = playermgrGetPlayerNumByProp(aprop);
		}
	}

#ifndef PLATFORM_N64
	// Diag: every server-side damage event attributed to a HUMAN attacker,
	// with the damage MECHANISM (weapon / explosion flag / attacker prop type
	// / attacker liveness). A kill credited to an idle player will show up
	// here as an hdmg line naming the path that carried their prop as aprop —
	// the missing fact in the "AFK player racking up eliminations" reports.
	if (g_NetMode == NETMODE_SERVER && g_Vars.normmplayerisrunning
			&& aplayernum >= 0 && aplayernum < PLAYERCOUNT()) {
		const s32 apn = (aprop && aprop->type == PROPTYPE_PLAYER)
				? playermgrGetPlayerNumByProp(aprop) : -1;
		const bool apnok = apn >= 0 && apn < PLAYERCOUNT() && g_Vars.players[apn];
		netDiagLogf("hdmg", "a=%d cur=%d v=%d dmg=%.2f wpn=%d exp=%d atype=%d aremote=%d adead=%d",
				aplayernum, (s32)g_Vars.currentplayernum, mpPlayerGetIndex(chr),
				damage, gset ? (s32)gset->weaponnum : -1, explosion ? 1 : 0,
				aprop ? (s32)aprop->type : -1,
				apnok ? (s32)g_Vars.players[apn]->isremote : -1,
				apnok ? (s32)g_Vars.players[apn]->isdead : -1);
	}
#endif

	// If using the shotgun, scale the damage based on distance
	if (aprop && aprop->type == PROPTYPE_CHR && gset->weaponnum == WEAPON_SHOTGUN) {
		f32 xdiff = aprop->pos.x - vprop->pos.x;
		f32 ydiff = aprop->pos.y - vprop->pos.y;
		f32 zdiff = aprop->pos.z - vprop->pos.z;
		f32 sqdist = xdiff * xdiff + ydiff * ydiff + zdiff * zdiff;

		if (sqdist < 200 * 200) {
			damage *= 4.0f + (s32)(rngRandom() % 3); // 4, 5 or 6
		} else if (sqdist < 400 * 400) {
			damage *= 3.0f + (s32)(rngRandom() % 2); // 3 or 4
		} else if (sqdist < 800 * 800) {
			damage *= 2.0f + (s32)(rngRandom() % 2); // 2 or 3
		} else if (sqdist < 1600 * 1600) {
			damage *= 1.0f + (s32)(rngRandom() % 2); // 1 or 2
		}
	}

	// damageshield is an argument to this function,
	// but is forced on if using the Farsight.
	if (gset && gset->weaponnum == WEAPON_FARSIGHT) {
		damageshield = true;
		damage *= 10;
	}

	// Handle shield damage
	if (damageshield) {
		shield = chrGetShield(chr);

		if (chr->aibot && chr->aibot->config->type == BOTTYPE_TURTLE) {
			armourscale = 4;
		}

		if (shield > 0) {
			if (g_Vars.normmplayerisrunning) {
#if VERSION >= VERSION_PAL_FINAL
				// Fixing a @bug?
				damage = damage * mpHandicapToDamageScale(g_PlayerConfigsArray[g_Vars.currentplayerstats->mpindex].handicap);
#else
				damage /= mpHandicapToDamageScale(g_PlayerConfigsArray[g_Vars.currentplayerstats->mpindex].handicap);
#endif
			}

			chr->chrflags |= CHRCFLAG_SHIELDDAMAGED;

			if (prop2 && node && chr->model) {
				func0f034080(chr, node, prop2, model, side, arg11);
			} else {
				shieldhitCreate(chr->prop, chrGetShield(chr), NULL, NULL, NULL, 0, 0);
			}

			if (g_Vars.normmplayerisrunning && (g_MpSetup.options & MPOPTION_ONEHITKILLS)) {
				damage = 0;
				chrSetShield(chr, 0);
			} else if (shield >= damage / armourscale) {
				// Has enough shield to sustain the damage
				shield -= damage / armourscale;
				damage = 0;
				chrSetShield(chr, shield);
			} else {
				// Shield is now gone
				damage = 0;
				chrSetShield(chr, 0);
			}

			showshield = true;
			usedshield = true;
		}
	}

	// Handle hat shots. This is left over from GE, as hats don't exist in PD
	if (damage > 0 && hitpart == HITPART_HAT && chr->weapons_held[2]) {
		s32 type = hatGetType(chr->weapons_held[2]);

		if (type == HATTYPE_CLOTH) {
			// Hat remains on head and damages the chr (eg. Moonraker Elite)
			hitpart = HITPART_HEAD;
		} else if (type != HATTYPE_METAL) {
			// Normal hat
			damage = 0;
			objSetDropped(chr->weapons_held[2], DROPTYPE_HAT);
			chr->hidden |= CHRHFLAG_DROPPINGITEM;
		} else {
			// Metal helmets don't fall off and make a metallic chink noise when shot
			u16 sounds[] = { SFX_HIT_METAL_807B, SFX_HIT_METAL_8079, SFX_HATHIT_807C };
			damage = 0;

			psCreate(NULL, chr->prop, sounds[rngRandom() % 3], -1,
					-1, 0, 0, PSTYPE_NONE, NULL, -1, NULL, -1, -1, -1, -1);
		}
	}

	// Handle incrementing player shot count
	if (aprop && aprop->type == PROPTYPE_PLAYER && !explosion) {
		bool alreadydead = false;
		s32 prevplayernum = g_Vars.currentplayernum;
		setCurrentPlayerNum(playermgrGetPlayerNumByProp(aprop));

		// ACT_DIE is not checked here, so it would appear that shooting
		// a chr as they're dying will increment the shots hit count
		if (chr && chr->actiontype == ACT_DEAD) {
			alreadydead = true;
		}

		if (vprop->type == PROPTYPE_PLAYER && g_Vars.players[playermgrGetPlayerNumByProp(vprop)]->isdead) {
			alreadydead = true;
		}

		if (!alreadydead && hitpart) {
			switch (hitpart) {
			case HITPART_HEAD:
				mpstatsIncrementPlayerShotCount2(gset, SHOTREGION_HEAD);
				break;
			case HITPART_GUN:
				mpstatsIncrementPlayerShotCount2(gset, SHOTREGION_GUN);
				break;
			case HITPART_HAT:
				mpstatsIncrementPlayerShotCount2(gset, SHOTREGION_HAT);
				break;
			case HITPART_PELVIS:
			case HITPART_TORSO:
				mpstatsIncrementPlayerShotCount2(gset, SHOTREGION_BODY);
				break;
			default:
				mpstatsIncrementPlayerShotCount2(gset, SHOTREGION_LIMB);
				break;
			}
		}

		setCurrentPlayerNum(prevplayernum);
	}

	// If the chr is invincible, make them flinch then we're done
	if (chr->chrflags & CHRCFLAG_INVINCIBLE) {
		chrFlinchBody(chr);
		return;
	}

	// If chr is dying or already dead, consider making their head flinch
	// then we're done
	if (chr->actiontype == ACT_DIE || chr->actiontype == ACT_DEAD) {
		if (hitpart == HITPART_HEAD && chr->actiontype == ACT_DIE && race != RACE_SKEDAR && isshoot) {
			struct coord pos;
			pos.x = vprop->pos.x - vector->x;
			pos.y = vprop->pos.y - vector->y;
			pos.z = vprop->pos.z - vector->z;
			chrFlinchHead(chr, chrGetAngleToPos(chr, &pos));
		}

		return;
	}

	// At this point the chr is known to be alive before they were shot
	{
		struct prop *weapon;
		struct coord sp9c;
		f32 angle;

		sp9c.x = vprop->pos.x - vector->x;
		sp9c.y = vprop->pos.y - vector->y;
		sp9c.z = vprop->pos.z - vector->z;
		angle = chrGetAngleToPos(chr, &sp9c);

		// Knife in the back to an unalerted chr is lethal
		if (gset->weaponnum == WEAPON_COMBATKNIFE
				&& gset->weaponfunc == FUNC_PRIMARY
				&& angle > 2.0940616130829f
				&& angle < 4.1881237030029f
				&& (chr->alertness < 100 || chr->lastseetarget60 == 0)) {
			damage *= 1000;
		}

		// Punching and pistol whipping is less effective from the front
		if (gsetHasFunctionFlags(gset, FUNCFLAG_BLUNTIMPACT)) {
			if (angle < 1.0470308065414f || angle > 5.2351541519165f) {
				damage *= 0.4f;
			} else if (angle < 2.0940616130829f || angle > 4.1881237030029f) {
				damage *= 0.7f;
			} else if (chr->alertness < 100) {
				onehitko = true;
			}

			if (chrGetHeldProp(chr, HAND_RIGHT) == NULL
					&& chrGetHeldProp(chr, HAND_LEFT) == NULL
					&& (chr->gunprop == NULL || chr->actiontype == ACT_SURRENDER || chr->actiontype == ACT_SURPRISED)) {
				// Chr is unarmed and has no hope of getting their gun
				onehitko = true;
			}

			forceapplydamage = true;
		}

		if (hitpart == HITPART_GENERAL) {
			// Halve the damage because it's doubled for torso below
			hitpart = HITPART_TORSO;
			damage *= 0.5f;
		} else if (hitpart == HITPART_GENERALHALF) {
			// Likewise, quarter it here so it becomes half below
			hitpart = HITPART_TORSO;
			damage *= 0.25f;
		}

		// Hits to a Skedar's tail are 10x more lethal
		if (race == RACE_SKEDAR && hitpart == HITPART_TAIL) {
			damage *= 10;
		}

		// Apply damage multipliers based on which body parts were hit,
		// and flinch head if shot in the head
		if (hitpart == HITPART_HEAD) {
			if (race == RACE_SKEDAR) {
				damage += damage;
				chrFlinchHead(chr, angle);
			} else {
				damage *= 4;

				if (isshoot && !usedshield) {
					chrFlinchHead(chr, angle);
					damage *= headshotdamagescale;

					if (gset->weaponnum == WEAPON_COMBATKNIFE && gset->weaponfunc != FUNC_POISON) {
						damage += damage;
					}
				}
			}
		} else if (hitpart == HITPART_TORSO) {
			// Double damage for torso hits
			damage += damage;
		} else if (hitpart == HITPART_GUN) {
			// No damage for gun hits
			damage = 0;
			makedizzy = false;
		} else if (hitpart == HITPART_HAT) {
			// No damage for hat hits
			damage = 0;
			makedizzy = false;
		}

		// Handle situations where the player is the one being shot, then return
		if (vprop->type == PROPTYPE_PLAYER) {
			s32 prevplayernum = g_Vars.currentplayernum;
			setCurrentPlayerNum(playermgrGetPlayerNumByProp(vprop));

#ifndef PLATFORM_N64
			// Chaos "No Damage Except Headshots" (pd.headshots_only): zero every
			// non-head hit on the local human. Zeroing damage lets the damage>0
			// guard below fall through; makedizzy=false drops the blur/dizzy accum
			// for the ignored hit. Note kill-plane/forced kills bypass chrDamage
			// entirely, so this doesn't make the player unkillable.
			{
				extern s32 g_ChaosHeadshotsOnly;
				if (g_ChaosHeadshotsOnly && !g_Vars.currentplayer->isremote
						&& hitpart != HITPART_HEAD) {
					damage = 0;
					makedizzy = false;
				}
			}
#endif

			if (g_Vars.normmplayerisrunning) {
				damage /= mpHandicapToDamageScale(g_PlayerConfigsArray[g_Vars.currentplayerstats->mpindex].handicap);
			}

			if (g_Vars.currentplayer->isdead == false && !g_PlayerInvincible) {
				f32 boostscale;

				// Handle player losing gun
				if (gsetHasFunctionFlags(gset, FUNCFLAG_DISARM)) {
					bgunDisarm(aprop);
				}

				// Handle player dizziness
				if (makedizzy && g_Vars.currentplayer->invincible == false) {
					f32 blurscale = 1;
					struct chrdata *achr = NULL;

					if (aprop) {
						achr = aprop->chr;

						if (achr && achr->bodynum == BODY_MINISKEDAR) {
							blurscale = 4;
						}
					}

					if (!achr
							|| !achr->aibot
							|| !gsetHasFunctionFlags(gset, FUNCFLAG_00400000)
							|| chr->blurdrugamount < TICKS(4500)) {
						chr->blurdrugamount += gsetGetBlurAmount(gset) * blurscale;
					}

					chr->blurnumtimesdied = 0;
				}

				// Handle player damage
				if (g_Vars.currentplayer->invincible == false && damage > 0) {
					f32 statsamount = amount = damage * 0.125f;

					if (statsamount > g_Vars.currentplayer->bondhealth) {
						statsamount = g_Vars.currentplayer->bondhealth;
					}

					if (g_Vars.normmplayerisrunning && (g_MpSetup.options & MPOPTION_ONEHITKILLS)) {
						statsamount = g_Vars.currentplayer->bondhealth;
					}

					playerUpdateDamageStats(aprop, vprop, statsamount);
					playerDisplayHealth();

					if (g_Vars.normmplayerisrunning && (g_MpSetup.options & MPOPTION_ONEHITKILLS)) {
						g_Vars.currentplayer->bondhealth = 0;
					}

					g_Vars.currentplayer->bondhealth -= amount / healthscale;
#ifndef PLATFORM_N64
					// Diag: log EVERY server-side player hit — the attacker, whether it
					// resolves to self, and the resulting health. A gunshot kill showing
					// as a suicide means either no pdmg line at all for the victim before
					// death (damage routed elsewhere) or an atk=self line clobbering the
					// real attacker. self=1 means aprop == the victim.
					if (g_NetMode == NETMODE_SERVER && g_Vars.normmplayerisrunning) {
						netDiagLogf("pdmg", "v=%d atk=%d self=%d aprop=%d dmg=%.1f hp=%.2f",
								g_Vars.currentplayernum,
								(aprop && aprop->chr) ? mpPlayerGetIndex(aprop->chr) : -1,
								(aprop == vprop) ? 1 : 0,
								aprop ? (s32)aprop->type : -1,
								amount, g_Vars.currentplayer->bondhealth);
					}
#endif

					chr->lastattacker = (aprop ? aprop->chr : NULL);

#ifndef PLATFORM_N64
					chr->lastattackerstamp60 = aprop ? (s32)g_Vars.lvframe60 : 0;

					// Classic options, split per behaviour:
					//
					// "GoldenEye HUD" stamps the damage flash (rendered
					// inside playerRenderHealthBarGE, so the flash is a
					// GEHUD cosmetic). The flash only re-stamps when the
					// previous one has fully ended (>= 8 frames ago) AND
					// the chr is not still in its i-frame window. The
					// chrDamage top-of-function gate already filters most
					// repeat damage during i-frames, but this guard makes
					// it explicit at the trigger site so chained calls
					// can't restack the flash mid-fade. (With i-frames
					// off, lastdamagetick60 stays 0 and iframe_done is
					// always true.) Must run BEFORE the i-frame stamp
					// below so it reads the previous window.
					if (classicOptionActive(CHEAT_CLASSIC_GEHUD, MPOPTION_CLASSIC_GEHUD)) {
						const u32 prev_flash_age = (u32)g_Vars.lvframe60 - (u32)g_Vars.currentplayer->damageflashstart60;
						const u32 prev_iframe_age = (u32)g_Vars.lvframe60 - (u32)chr->lastdamagetick60;
						const bool flash_done = (prev_flash_age >= 8);
						const bool iframe_done = (chr->lastdamagetick60 == 0) || (prev_iframe_age >= (u32)TICKS(18));
						if (flash_done && iframe_done) {
							g_Vars.currentplayer->damageflashstart60 = (s32)g_Vars.lvframe60;
						}
					}

					// "Damage Invulnerability" starts the i-frame window
					// now that damage was actually applied (bondhealth has
					// decreased). Bump to 1 if lvframe60 happens to be 0
					// so the "never damaged" sentinel isn't re-armed.
					if (classicOptionActive(CHEAT_CLASSIC_IFRAMES, MPOPTION_CLASSIC_IFRAMES)) {
						chr->lastdamagetick60 = g_Vars.lvframe60 ? (s32)g_Vars.lvframe60 : 1;
						netDiagLogf("dmg_player",
								"chrnum=%d sid=%u dmg=%.2f hp=%.3f stamp=%d",
								(s32)chr->chrnum,
								chr->prop ? (u32)chr->prop->syncid : 0u,
								amount,
								g_Vars.currentplayer->bondhealth,
								chr->lastdamagetick60);
					}
#endif

					showdamage = true;

					if (g_Vars.currentplayer->training == false
							&& g_Vars.currentplayer->bondhealth <= 0) {
						playerDieByShooter(aplayernum, false);
						chr->blurnumtimesdied++;
					}

					if (!lvIsPaused() && canchoke) {
						chrChoke(chr, choketype);
					}

					if (g_Vars.currentplayer->haschrbody) {
						chrFlinchBody(chr);
					}
				}

				// Handle player boost
				if (ismelee && gset->weaponnum == WEAPON_REAPER) {
					boostscale = 0.1f;
				} else if (g_Vars.normmplayerisrunning) {
					boostscale = 0.75f;
				} else {
					boostscale = 1;
				}

				g_Vars.currentplayer->bondshotspeed.x += vector->x * boostscale;
				g_Vars.currentplayer->bondshotspeed.z += vector->z * boostscale;

				if (showdamage) {
					playerDisplayDamage();
				}

				if (showshield) {
					playerDisplayShield();
				}

				if (g_Vars.normmplayerisrunning && aprop && aprop->type == PROPTYPE_PLAYER) {
					playerCheckIfShotInBack(prevplayernum, vector->x, vector->z);
				}
			}

#ifndef PLATFORM_N64
			if (g_NetMode == NETMODE_SERVER && g_Vars.currentplayer->client) {
				netmsgSvcPlayerStatsWrite(&g_NetMsgRel, g_Vars.currentplayer->client);
			}
#endif

			setCurrentPlayerNum(prevplayernum);
			return;
		}

		// This check is pointless - a similar check and return exists earlier
		if (chr->actiontype == ACT_DIE || chr->actiontype == ACT_DEAD) {
			return;
		}

		// At this point we know we're dealing with a NPC being shot, and the
		// NPC was alive prior to being shot.

		// Handle aibot/chr losing gun
		if (gsetHasFunctionFlags(gset, FUNCFLAG_DISARM)
				&& ((chr->flags & CHRFLAG0_CANLOSEGUN) || chr->aibot)) {
			if (chr->aibot) {
				botDisarm(chr, aprop);
			} else {
				weapon = chrGetHeldProp(chr, HAND_RIGHT);

				if (weapon) {
					chr->gunprop = weapon;
					objSetDropped(weapon, DROPTYPE_DEFAULT);
					chr->hidden |= CHRHFLAG_DROPPINGITEM;
				}

				weapon = chrGetHeldProp(chr, HAND_LEFT);

				if (weapon) {
					chr->gunprop = weapon;
					objSetDropped(weapon, DROPTYPE_DEFAULT);
					chr->hidden |= CHRHFLAG_DROPPINGITEM;
				}
			}
		}

		// Handle chr damage
		if (chr->damage < chr->maxdamage
				|| (!g_Vars.normmplayerisrunning && chr->actiontype != ACT_PREARGH)) {
			f32 sp80 = 0;

			chr->numarghs++;

			// Handle chr dizziness and psychosis
			if (makedizzy && race != RACE_DRCAROLL && race != RACE_ROBOT) {
				if (gsetHasFunctionFlags(gset, FUNCFLAG_PSYCHOSIS)) {
#ifndef PLATFORM_N64
					// Chaos "Evil twin"/"Clone army" are immune to psychosis: it
					// routes them into GAILIST_INIT_PSYCHOSIS, which rewrites their
					// team to TEAM_NONCOMBAT then TEAM_ALLY and turns them friendly
					// — exactly the "she goes friendly" bug. Keep them hostile.
					if (!chaosIsTwin(chr))
#endif
					chr->hidden |= CHRHFLAG_PSYCHOSISED;
				} else {
					chr->blurdrugamount += gsetGetBlurAmount(gset);
					chr->blurnumtimesdied = 0;

					if (!chr->aibot && chr->blurdrugamount >= TICKS(5000)) {
						onehitko = true;
					}
				}
			}

			// Handle chr boost
			if (chr->aibot) {
				f32 boostscale;

				if (ismelee && gset->weaponnum == WEAPON_REAPER) {
					boostscale = 0.1f;
				} else {
					boostscale = 0.75f;
				}

				chr->aibot->shotspeed.x += vector->x * boostscale;
				chr->aibot->shotspeed.z += vector->z * boostscale;
			}

			if (gset->weaponnum == WEAPON_UNARMED) {
				sp80 = 2;
			}

			if (gset->weaponnum == WEAPON_TRANQUILIZER || gset->weaponnum == WEAPON_PSYCHOSISGUN) {
				forceapplydamage = true;
			}

			// Handle one-hit knockouts
			if (onehitko && chr->aibot == NULL && race == RACE_HUMAN) {
				chrKnockOut(chr, angle, hitpart, gset);
				psStopSound(chr->prop, PSTYPE_CHRTALK, 0);

				if (canchoke) {
					chrChoke(chr, choketype);
				}

				if (gset->weaponnum == WEAPON_UNARMED && chr->actiontype != ACT_DRUGGEDKO) {
					return;
				}

				return;
			}

			// Handle applying damage to NPCs
			// Don't enter this branch if there is no damage to give,
			// or we are making a chr dizzy in solo mode (unless force is set)
			if (damage > 0 && (g_Vars.normmplayerisrunning || !makedizzy || forceapplydamage)) {
				f32 amount = damage;

				if (chr->damage + damage > chr->maxdamage) {
					amount = chr->maxdamage - chr->damage;
				}

				amount *= 0.125f;

				playerUpdateDamageStats(aprop, vprop, amount);

				chr->damage += damage;
				chr->lastattacker = (aprop ? aprop->chr : NULL);
#ifndef PLATFORM_N64
				chr->lastattackerstamp60 = aprop ? (s32)g_Vars.lvframe60 : 0;
#endif
				chr->chrflags |= CHRCFLAG_JUST_INJURED;
#ifndef PLATFORM_N64
				luaEmitDamage((s32)chr->chrnum, aplayernum, (s32)damage);
#endif

#ifndef PLATFORM_N64
				// Classic "Damage Invulnerability": start the i-frame
				// window for this sim/chr now that real damage was
				// applied to chr->damage.
				if (classicOptionActive(CHEAT_CLASSIC_IFRAMES, MPOPTION_CLASSIC_IFRAMES)) {
					chr->lastdamagetick60 = g_Vars.lvframe60 ? (s32)g_Vars.lvframe60 : 1;
					netDiagLogf("dmg_sim",
							"chrnum=%d sid=%u dmg=%.2f chrdmg=%.2f stamp=%d",
							(s32)chr->chrnum,
							chr->prop ? (u32)chr->prop->syncid : 0u,
							damage,
							chr->damage,
							chr->lastdamagetick60);
				}
#endif

				if (chr->aibot) {
					if (g_Vars.normmplayerisrunning && (g_MpSetup.options & MPOPTION_ONEHITKILLS)) {
						chr->damage = chr->maxdamage;
					}

					if (canchoke) {
						chrChoke(chr, choketype);
					}

					chrFlinchBody(chr);

					if (chr->damage >= chr->maxdamage) {
						chrDie(chr, aplayernum);
					}
				} else if (explosion) {
					// Chrs die instantly from explosion damage provided they
					// don't have any armour (the chr has armour if their
					// chr->damage is negative). Note that damage has already
					// been applied to the chr above, so a perfectly healthy chr
					// with no armour will already have a damage value > 0 at
					// this point.
					if (chr->damage > 0) {
						chr->damage = chr->maxdamage;

						if (race == RACE_DRCAROLL || race == RACE_EYESPY || race == RACE_ROBOT) {
							chrBeginDeath(chr, vector, angle, hitpart, gset, false, aplayernum);
						} else {
							chrYeetFromPos(chr, explosionpos, explosionforce);
#ifndef PLATFORM_N64
							// Explosion deaths of humans are yeeted straight to
							// dead and never reach chrBeginDeath (the kill-event
							// choke point) — emit here for this path only.
							luaEmitKill((s32)chr->chrnum,
									(aprop && aprop->type == PROPTYPE_PLAYER)
										? (s32)playermgrGetPlayerNumByProp(aprop) : -1);
#endif
						}

						if (canchoke) {
							chrChoke(chr, choketype);
						}

						if (g_Vars.mplayerisrunning) {
							mpstatsRecordDeath(aplayernum, mpPlayerGetIndex(chr));
						} else if (aprop && aprop->type == PROPTYPE_PLAYER) {
							s32 prevplayernum = g_Vars.currentplayernum;
							setCurrentPlayerNum(playermgrGetPlayerNumByProp(aprop));
							mpstatsRecordPlayerKill();
							setCurrentPlayerNum(prevplayernum);
						}

						if (chr->chrflags & CHRCFLAG_KILLCOUNTABLE) {
							mpstatsIncrementTotalKillCount();
						}

						if (chr->aibot == NULL) {
							// (the Lua "kill" emit that lived here moved to the
							// chrBeginDeath choke point + the yeet branch above —
							// it only covered explosion deaths, so bullet kills
							// never fired the event)
							chrDropConcealedItems(chr);
						}

						if (chr->aibot == NULL) {
							weapon = chr->weapons_held[HAND_RIGHT];

							if (weapon && (weapon->obj->flags & OBJFLAG_AIUNDROPPABLE) == 0) {
								objSetDropped(weapon, DROPTYPE_DEFAULT);
								chr->hidden |= CHRHFLAG_DROPPINGITEM;
							}

							weapon = chr->weapons_held[HAND_LEFT];

							if (weapon && (weapon->obj->flags & OBJFLAG_AIUNDROPPABLE) == 0) {
								objSetDropped(weapon, DROPTYPE_DEFAULT);
								chr->hidden |= CHRHFLAG_DROPPINGITEM;
							}
						}
					}
				} else {
					// Non-explosion damage to solo mode chr
					if (chr->actiontype != ACT_DRUGGEDKO && canchoke) {
						chrChoke(chr, choketype);
					}

					if (makedizzy && chr->damage >= chr->maxdamage) {
						chr->damage = chr->maxdamage - 0.1f;
						chrKnockOut(chr, angle, hitpart, gset);
					}

					// If chr has armour or the weapon doesn't stun
					if (chr->damage < 0 ||
							(gsetHasFunctionFlags(gset, FUNCFLAG_NOSTUN) && chr->damage < chr->maxdamage)) {
						f32 endframe = -1;

						if (!chrIsAnimPreventingArgh(chr, &endframe)) {
							chrFlinchBody(chr);
						}
					} else if (hitpart != HITPART_HAT) {
						// Cancel current animation and prepare for argh
						f32 endframe = -1;

						if (chrIsAnimPreventingArgh(chr, &endframe)) {
							if (endframe >= 0) {
								modelSetAnimEndFrame(chr->model, endframe);
							}

							chr->actiontype = ACT_PREARGH;
							chr->act_preargh.dir.x = vector->x;
							chr->act_preargh.dir.y = vector->y;
							chr->act_preargh.dir.z = vector->z;
							chr->act_preargh.relshotdir = angle;
							chr->act_preargh.hitpart = hitpart;
							chr->act_preargh.aplayernum = aplayernum;
							chr->act_preargh.gset.weaponnum = gset->weaponnum;
							chr->act_preargh.gset.unk0639 = gset->unk0639;
							chr->act_preargh.gset.unk063a = gset->unk063a;
							chr->act_preargh.gset.weaponfunc = gset->weaponfunc;

							chr->sleep = 0;
						} else {
							chrReactToDamage(chr, vector, angle, hitpart, gset, aplayernum);
						}
					}
				}

				if (sp80 > 0) {
					chr->timeextra = sp80 * 15;
					chr->elapseextra = 0;

					chr->extraspeed.x = prop2->pos.x - aprop->pos.x;
					chr->extraspeed.y = prop2->pos.y - aprop->pos.y;
					chr->extraspeed.z = prop2->pos.z - aprop->pos.z;

					guNormalize(&chr->extraspeed.x, &chr->extraspeed.y, &chr->extraspeed.z);

					chr->extraspeed.x *= sp80;
					chr->extraspeed.y *= sp80;
					chr->extraspeed.z *= sp80;
				}
			}
		}
	}
}

void chrDie(struct chrdata *chr, s32 aplayernum)
{
	if (chr->actiontype != ACT_DIE) {
		chrStopFiring(chr);
		chrUncloak(chr, true);
#ifndef PLATFORM_N64
		luaEmitKill((s32)chr->chrnum, aplayernum);
#endif

		chr->actiontype = ACT_DIE;
		chr->act_die.notifychrindex = 0;
		chr->sleep = 0;
		chr->blurnumtimesdied++;
		chr->act_die.thudframe1 = -1;
		chr->act_die.thudframe2 = -1;
		chr->act_die.timeextra = 0;

		chr->ailist = ailistFindById(GAILIST_AIBOT_DEAD);
		chr->aioffset = 0;

		mpstatsRecordDeath(aplayernum, mpPlayerGetIndex(chr));
#ifndef PLATFORM_N64
		// On a net client the server is authoritative for sim deaths and
		// respawns. Calling botinvDropAll here marks the held weapon props
		// OBJHFLAG_DELETING (via botinvSwitchToWeapon) which frees them and
		// clears their syncids. Any in-flight SVC_PROP_MOVE for a projectile
		// the sim just fired still references the weapon's syncid as ownerprop
		// — that lookup then fails ("prop with syncid N does not exist"). Keep
		// the weapons alive; the server's SVC_PROP_MOVE chr-state block will
		// drive the weapon change when botSpawn runs server-side.
		if (g_NetMode != NETMODE_CLIENT) {
#endif
		botinvDropAll(chr, chr->aibot->weaponnum);

#if VERSION >= VERSION_NTSC_1_0
		chr->aibot->hasbriefcase = false;
		chr->aibot->hascase = false;
		chr->aibot->unk04c_04 = false;
		chr->aibot->unk04c_03 = false;
		chr->aibot->hasuplink = false;
#endif
#ifndef PLATFORM_N64
		}
#endif
	}
}

bool func0f03645c(struct chrdata *chr, struct coord *arg1, RoomNum *arg2, struct coord *arg3, struct coord *arg4, s32 arg5)
{
	bool result = false;
	f32 ymax;
	f32 ymin;
	f32 radius;
	RoomNum rooms[8];
	struct prop *prop = chr->prop;

	chrGetBbox(prop, &radius, &ymax, &ymin);
	chrSetPerimEnabled(chr, false);

	if (cdTestCylMove04(arg1, arg2, arg3, rooms, arg5, 1, ymax - prop->pos.y, ymin - prop->pos.y) != CDRESULT_COLLISION) {
		if (cdTestCylMove01(arg3, rooms, arg4, arg5, 1, ymax - prop->pos.y, ymin - prop->pos.y) != CDRESULT_COLLISION) {
			result = true;
		}
	}

	chrSetPerimEnabled(chr, true);

	return result;
}

bool func0f03654c(struct chrdata *chr, struct coord *pos, RoomNum *rooms, struct coord *pos2, RoomNum *rooms2, struct coord *vector, f32 arg6, u32 types)
{
	struct coord tmp;
	f32 a;
	f32 b;
	struct prop *prop;
	bool result = false;
	f32 ymax;
	f32 ymin;
	f32 radius;
	struct coord sp5c;
	struct coord sp50;
	RoomNum sp40[8];
	f32 mult;

	prop = chr->prop;

	chrSetPerimEnabled(chr, false);
	chrGetBbox(prop, &radius, &ymax, &ymin);

	if ((rooms2 && cdTestCylMove02(pos, rooms, pos2, rooms2, types, true, ymax - prop->pos.y, ymin - prop->pos.y))
			|| (rooms2 == NULL && cdTestCylMove01(pos, rooms, pos2, types, 1, ymax - prop->pos.y, ymin - prop->pos.y))) {
		if (vector == NULL) {
			vector = &tmp;

			tmp.x = pos2->x - pos->x;
			tmp.y = 0;
			tmp.z = pos2->z - pos->z;

			if (tmp.f[0] == 0 && tmp.f[2] == 0) {
				// @bug: Needs to call chrSetPerimEnabled(chr, true)
				// before returning
				return true;
			}

			mult = 1.0f / sqrtf(tmp.f[0] * tmp.f[0] + tmp.f[2] * tmp.f[2]);
			tmp.x *= mult;
			tmp.z *= mult;
		}

		a = vector->x * arg6;
		b = vector->z * arg6;

		sp5c.x = pos->x + b;
		sp5c.y = pos->y;
		sp5c.z = pos->z - a;

		sp50.x = pos2->x + b;
		sp50.y = pos2->y;
		sp50.z = pos2->z - a;

		if (cdTestCylMove04(pos, rooms, &sp5c, sp40, types, 1, ymax - prop->pos.y, ymin - prop->pos.y)
				&& cdTestCylMove01(&sp5c, sp40, &sp50, types, 1, ymax - prop->pos.y, ymin - prop->pos.y)) {
			sp5c.x = pos->x - b;
			sp5c.y = pos->y;
			sp5c.z = pos->z + a;

			sp50.x = pos2->x - b;
			sp50.y = pos2->y;
			sp50.z = pos2->z + a;

			if (cdTestCylMove04(pos, rooms, &sp5c, sp40, types, 1, ymax - prop->pos.y, ymin - prop->pos.y)
					&& cdTestCylMove01(&sp5c, sp40, &sp50, types, 1, ymax - prop->pos.y, ymin - prop->pos.y)) {
				result = true;
			}
		}
	}

	chrSetPerimEnabled(chr, true);

	return result;
}

bool propchrHasClearLineToPos(struct prop *prop, struct coord *dstpos, struct coord *vector)
{
	return func0f03654c(prop->chr, &prop->pos, prop->rooms, dstpos, NULL, vector, prop->chr->radius * 1.2f, CDTYPE_ALL);
}

bool propchrHasClearLineInVector(struct prop *prop, struct coord *vector, f32 mult)
{
	struct coord dstpos;

	dstpos.x = vector->x * mult + prop->pos.x;
	dstpos.y = prop->pos.y;
	dstpos.z = vector->z * mult + prop->pos.z;

	return propchrHasClearLineToPos(prop, &dstpos, vector);
}

bool func0f036974(struct prop *prop, struct coord *pos)
{
	return func0f03654c(prop->chr, &prop->pos, prop->rooms, pos, NULL, NULL, prop->chr->radius * 1.2f, CDTYPE_ALL);
}

void chrGetSideVectorToTarget(struct chrdata *chr, bool side, struct coord *vector)
{
	struct prop *prop = chr->prop;
	struct prop *target = chrGetTargetProp(chr);

	vector->x = 0;
	vector->y = 0;
	vector->z = 1;

	if (target) {
		f32 x = target->pos.x - prop->pos.x;
		f32 z = target->pos.z - prop->pos.z;
		f32 distance = sqrtf(x * x + z * z);

		if (distance > 0) {
			x = x / distance;
			z = z / distance;

			if (side) {
				vector->x = z;
				vector->y = 0;
				vector->z = -x;
			} else {
				vector->x = -z;
				vector->y = 0;
				vector->z = x;
			}
		}
	}
}

bool chrCanRollInDirection(struct chrdata *chr, bool side, f32 distance)
{
	struct prop *prop = chr->prop;
	struct coord vector;
	struct coord dstpos;

	chrGetSideVectorToTarget(chr, side, &vector);

	dstpos.x = vector.x * distance + prop->pos.x;
	dstpos.y = prop->pos.y;
	dstpos.z = vector.z * distance + prop->pos.z;

	return propchrHasClearLineToPos(prop, &dstpos, &vector);
}

void chrGetSideVector(struct chrdata *chr, bool side, struct coord *vector)
{
	f32 angle = chrGetInverseTheta(chr);

	if (side) {
		vector->x = cosf(angle);
		vector->y = 0;
		vector->z = -sinf(angle);
	} else {
		vector->x = -cosf(angle);
		vector->y = 0;
		vector->z = sinf(angle);
	}
}

bool chrCanJumpInDirection(struct chrdata *chr, bool side, f32 distance)
{
	struct prop *prop = chr->prop;
	struct coord vector;
	struct coord dstpos;

	chrGetSideVector(chr, side, &vector);

	dstpos.x = vector.x * distance + prop->pos.x;
	dstpos.y = prop->pos.y;
	dstpos.z = vector.z * distance + prop->pos.z;

	return propchrHasClearLineToPos(prop, &dstpos, &vector);
}

bool chrIsRoomOffScreen(struct chrdata *chr, struct coord *waypos, RoomNum *wayrooms)
{
	struct prop *prop = chr->prop;
	RoomNum sp7c[20];
	u32 stack;
	s32 i;
	RoomNum sp64[8];
	bool offscreen = true;
	RoomNum sp50[8];

	if ((chr->hidden & CHRHFLAG_CLOAKED) == 0 || USINGDEVICE(DEVICE_IRSCANNER)) {
		func0f065dfc(&prop->pos, prop->rooms, waypos, sp64, sp7c, 20);

		if (g_Vars.mplayerisrunning) {
			for (i = 0; sp7c[i] != -1; i++) {
				if (g_MpRoomVisibility[sp7c[i]] & MPROOMVIS_ONSCREEN_ALL) {
					offscreen = false;
					break;
				}
			}
		} else {
			for (i = 0; sp7c[i] != -1; i++) {
				if (g_Rooms[sp7c[i]].flags & ROOMFLAG_ONSCREEN) {
					offscreen = false;
					break;
				}
			}
		}
	}

	if (offscreen) {
		for (i = 0; i < PLAYERCOUNT(); i++) {
#ifndef PLATFORM_N64
			// In netplay a slot in g_Vars.players[] can carry a stale prop:
			// netClientReset on disconnect only clears the player->client
			// backlink, not the slot or its ->prop, and playerStartNewLife
			// reassigns prop via propAllocate() without clearing the old.
			// A simple NULL check passes for a freed-but-not-cleared prop
			// pointer, then prop->pos / prop->rooms deref into garbage.
			//
			// Three-layer guard:
			//  (1) NULL slot / NULL prop — fast out.
			//  (2) Orphaned slot — client backlink is NULL after a
			//      mid-stage disconnect (netClientReset clears it);
			//      netPlayersAllocate only re-binds at the next stage
			//      transition. The chr is still in the world but its
			//      prop / chr state may be in a half-cleaned state.
			//      Sim AI iterating this slot via chrIsRoomOffScreen
			//      was the observed 0xc0000005 crash site on every
			//      "client rejoin + punch sim" repro.
			//  (3) Stale prop pointer — pp must live inside
			//      g_Vars.props[] and still be PROPTYPE_PLAYER.
			if (!g_Vars.players[i] || !g_Vars.players[i]->prop) {
				netDiagLogf("chroffsc_skip", "i=%d reason=null pp=%p", i,
						g_Vars.players[i] ? (void *)g_Vars.players[i]->prop : NULL);
				continue;
			}
			if (!g_Vars.players[i]->client) {
				netDiagLogf("chroffsc_skip", "i=%d reason=orphan", i);
				continue;
			}
			struct prop *pp = g_Vars.players[i]->prop;
			if (pp < g_Vars.props || pp >= g_Vars.props + g_Vars.maxprops) {
				netDiagLogf("chroffsc_skip",
						"i=%d reason=oob pp=%p base=%p end=%p",
						i, (void *)pp, (void *)g_Vars.props,
						(void *)(g_Vars.props + g_Vars.maxprops));
				continue;
			}
			if (pp->type != PROPTYPE_PLAYER) {
				netDiagLogf("chroffsc_skip",
						"i=%d reason=type pp=%p type=%d",
						i, (void *)pp, (s32)pp->type);
				continue;
			}
			// Log right before the unprotected dereference so a crash here
			// leaves a final breadcrumb identifying the offending slot.
			// Throttled per-tick to avoid log spam (chrIsRoomOffScreen is
			// called from many AI paths every frame).
			{
				static u32 s_chroffsc_last_tick = 0xffffffffu;
				if (g_NetTick != s_chroffsc_last_tick) {
					s_chroffsc_last_tick = g_NetTick;
					netDiagLogf("chroffsc_iter",
							"chr_sid=%u i=%d pp=%p type=%d pos=(%.1f,%.1f,%.1f)",
							chr->prop ? (unsigned)chr->prop->syncid : 0u,
							i, (void *)pp, (s32)pp->type,
							pp->pos.x, pp->pos.y, pp->pos.z);
				}
			}
#endif
			portal00018148(waypos, &g_Vars.players[i]->prop->pos, wayrooms, sp50, 0, 0);

			if (arrayIntersects(g_Vars.players[i]->prop->rooms, sp50)) {
				offscreen = false;
				break;
			}
		}
	}

	return offscreen;
}

void chrGoPosInitMagic(struct chrdata *chr, struct waydata *waydata, struct coord *padpos, struct coord *chrpos)
{
	f32 xdiff1 = padpos->x - chr->prop->pos.x;
	f32 zdiff1 = padpos->z - chr->prop->pos.z;

	f32 angle = atan2f(xdiff1, zdiff1);

	f32 xdiff2 = padpos->x - chrpos->x;
	f32 zdiff2 = padpos->z - chrpos->z;

	waydata->mode = WAYMODE_MAGIC;

	waydata->magictotal = sqrtf(xdiff1 * xdiff1 + zdiff1 * zdiff1);
	waydata->magicdone = waydata->magictotal - sqrtf(xdiff2 * xdiff2 + zdiff2 * zdiff2);

	chrSetLookAngle(chr, angle);
}

void chrGoPosGetCurWaypointInfoWithFlags(struct chrdata *chr, struct coord *pos, RoomNum *rooms, u32 *flags)
{
	struct waypoint *waypoint = chr->act_gopos.waypoints[chr->act_gopos.curindex];
	struct pad pad;

	if (waypoint) {
		padUnpack(waypoint->padnum, PADFIELD_POS | PADFIELD_ROOM | PADFIELD_FLAGS, &pad);

		pos->x = pad.pos.x;
		pos->y = pad.pos.y;
		pos->z = pad.pos.z;

		rooms[0] = pad.room;
		rooms[1] = -1;

		if (flags) {
			*flags = pad.flags;
		}
	} else {
		pos->x = chr->act_gopos.endpos.x;
		pos->y = chr->act_gopos.endpos.y;
		pos->z = chr->act_gopos.endpos.z;

		rooms[0] = chr->act_gopos.endrooms[0];
		rooms[1] = -1;

		if (flags) {
			*flags = 0;
		}
	}
}

void chrGoPosGetCurWaypointInfo(struct chrdata *chr, struct coord *pos, RoomNum *rooms)
{
	chrGoPosGetCurWaypointInfoWithFlags(chr, pos, rooms, NULL);
}

f32 func0f0370a8(struct chrdata *chr)
{
	f32 result;

	if (chr->aibot) {
		result = botCalculateMaxSpeed(chr);
	} else {
		s16 animnum = modelGetAnimNum(chr->model);
		result = func0f02dff0(animnum) * (chr->model->scale * 9.999999f);
	}

	return result;
}

s32 chrGoPosCalculateBaseTtl(struct chrdata *chr)
{
	f32 xdiff;
	f32 zdiff;
	u32 stack;
	struct coord pos;
	RoomNum rooms[8];
	f32 speed;

	chrGoPosGetCurWaypointInfo(chr, &pos, rooms);

	xdiff = pos.x - chr->prop->pos.x;
	zdiff = pos.z - chr->prop->pos.z;

	if (xdiff < 0) {
		xdiff = -xdiff;
	}

	if (zdiff < 0) {
		zdiff = -zdiff;
	}

	speed = func0f0370a8(chr);

	if (chr->aibot == NULL) {
		speed *= modelGetAbsAnimSpeed(chr->model);
	}

	if (speed < 0.001f) {
		speed = 0.001f;
	}

	return (xdiff + zdiff) / speed;
}

void chrGoPosClearRestartTtl(struct chrdata *chr)
{
	chr->act_gopos.restartttl = 0;
}

void chrGoPosConsiderRestart(struct chrdata *chr)
{
	if (chr->act_gopos.waydata.mode != WAYMODE_MAGIC
			&& chr->liftaction != LIFTACTION_WAITINGONLIFT
			&& chr->liftaction != LIFTACTION_WAITINGFORLIFT) {
		if (chr->act_gopos.restartttl == 0) {
#if PAL
			s32 value = (chrGoPosCalculateBaseTtl(chr) * 100 + 15000) / 60;
#else
			s32 value = chrGoPosCalculateBaseTtl(chr) * 2 + 300;
#endif

			if (value > 0xffff) {
				value = 0xffff;
			}

			chr->act_gopos.restartttl = value;
		} else if (chr->act_gopos.restartttl <= (u16)g_Vars.lvupdate60) {
			if (chr->aibot) {
				botCheckFetch(chr);
			} else {
				chrGoToRoomPos(chr, &chr->act_gopos.endpos, chr->act_gopos.endrooms, chr->act_gopos.flags);
			}
		} else {
			chr->act_gopos.restartttl -= (u16)g_Vars.lvupdate60;
		}
	}
}

void chrGoPosInitExpensive(struct chrdata *chr)
{
	struct coord pos;
	RoomNum rooms[8];

	chrGoPosGetCurWaypointInfo(chr, &pos, rooms);

	chr->act_gopos.waydata.mode = WAYMODE_INIT;
	chr->act_gopos.waydata.iter = 0;
	chr->act_gopos.waydata.gotaimpos = false;
	chr->act_gopos.waydata.aimpos.x = pos.x;
	chr->act_gopos.waydata.aimpos.y = pos.y;
	chr->act_gopos.waydata.aimpos.z = pos.z;

	chrGoPosClearRestartTtl(chr);
}

/**
 * Advance the chr's current waypoint index to the next one in the route.
 *
 * The waypoints array allows 6 waypoints and it's important that they have a
 * couple loaded in front of their current one. So if the index is moving too
 * far into the array, new pathfinding will be done and the array and index will
 * be reset.
 */
void chrGoPosAdvanceWaypoint(struct chrdata *chr)
{
	if (chr->act_gopos.curindex < 3) {
		chr->act_gopos.curindex++;
	} else {
		struct waypoint *from = chr->act_gopos.waypoints[chr->act_gopos.curindex];
		chr->act_gopos.curindex = 1;

		navSetSeed(CHRNAVSEED(chr), CHRNAVSEED(chr));
		navFindRoute(from, chr->act_gopos.target, chr->act_gopos.waypoints, MAX_CHRWAYPOINTS);
		navSetSeed(0, 0);
	}

	chrGoPosInitExpensive(chr);
}

/**
 * Determines which step index the chr will be at given their current index, the
 * number of steps to take and in which direction (forward or back).
 *
 * Returns the step index and populates *forward with true or false depending on
 * whether the chr will be traversing the path in the forward direction at that
 * point.
 */
s32 chrPatrolCalculateStep(struct chrdata *chr, bool *forward, s32 numsteps)
{
	s32 nextstep = chr->act_patrol.nextstep;
	bool isforward = *forward;

	if (numsteps < 0) {
		isforward = !isforward;
		numsteps = -numsteps;
	}

	while (numsteps > 0) {
		numsteps--;

		if (isforward) {
			nextstep++;

			if (chr->act_patrol.path->pads[nextstep] < 0) {
				// Reached the end of the list
				if (chr->act_patrol.path->flags & PATHFLAG_CIRCULAR) {
					nextstep = 0;
				} else {
					isforward = false;
					nextstep -= 2;

					if (nextstep < 0) {
						nextstep = 0;
					}
				}
			}
		} else {
			nextstep--;

			if (nextstep < 0) {
				// Reached the start of the list
				if (chr->act_patrol.path->flags & PATHFLAG_CIRCULAR) {
					nextstep = chr->act_patrol.path->len - 1;
				} else {
					isforward = true;
					nextstep = 1;

					if (chr->act_patrol.path->len - 1 <= 0) {
						nextstep = chr->act_patrol.path->len - 1;
					}
				}
			}
		}
	}

	*forward = isforward;

	return nextstep;
}

/**
 * Determines which pad number the chr will be at given their current index and
 * the number of steps to take.
 */
s16 chrPatrolCalculatePadNum(struct chrdata *chr, s32 numsteps)
{
	s32 *padnumptr;
	bool forward = chr->act_patrol.forward;
	s32 step = chrPatrolCalculateStep(chr, &forward, numsteps);
	padnumptr = &chr->act_patrol.path->pads[step];

	return *padnumptr;
}

void chrPatrolGetCurWaypointInfoWithFlags(struct chrdata *chr, struct coord *pos, RoomNum *rooms, u32 *flags)
{
	s32 padnum = chrPatrolCalculatePadNum(chr, 0);
	struct pad pad;

	padUnpack(padnum, PADFIELD_POS | PADFIELD_ROOM | PADFIELD_FLAGS, &pad);

	pos->x = pad.pos.x;
	pos->y = pad.pos.y;
	pos->z = pad.pos.z;

	rooms[0] = pad.room;
	rooms[1] = -1;

	if (flags) {
		*flags = pad.flags;
	}
}

void chrPatrolGetCurWaypointInfo(struct chrdata *chr, struct coord *pos, RoomNum *rooms)
{
	chrPatrolGetCurWaypointInfoWithFlags(chr, pos, rooms, NULL);
}

void func0f037580(struct chrdata *chr)
{
	RoomNum rooms[8];

	chr->act_patrol.waydata.mode = WAYMODE_INIT;
	chr->act_patrol.waydata.iter = 0;
	chr->act_patrol.waydata.gotaimpos = false;

	chrPatrolGetCurWaypointInfo(chr, &chr->act_patrol.waydata.aimpos, rooms);
}

void func0f0375b0(struct chrdata *chr)
{
	s32 nextstep = chrPatrolCalculateStep(chr, &chr->act_patrol.forward, 1);

	chr->act_patrol.nextstep = nextstep;
	chr->patrolnextstep = nextstep;

	func0f037580(chr);
}

void chrNavTickMagic(struct chrdata *chr, struct waydata *waydata, f32 speed, struct coord *arg3, RoomNum *rooms)
{
	RoomNum sp118[8];
	f32 ymax;
	f32 ymin;
	f32 radius;
	f32 ground;
	u16 floorcol;
	u8 floortype;
	RoomNum floorroom;
	struct coord spf4;
	struct prop *prop = chr->prop;
	union modelrwdata *rwdata;
	struct waypoint *waypoint;
	struct coord spdc;
	RoomNum spcc[8];
	u32 stack[4];
	struct pad pad;
	struct coord sp5c;
	RoomNum sp4c[8];

	chr->invalidmove = 0;
	chr->lastmoveok60 = g_Vars.lvframe60;

	waydata->magicdone += speed * modelGetAbsAnimSpeed(chr->model) * g_Vars.lvupdate60freal;

	if (waydata->magicdone >= waydata->magictotal) {
		// Reached end of segment
		chrSetPerimEnabled(chr, false);
		roomsCopy(rooms, sp118);
		chr0f021fa8(chr, arg3, sp118);

		ground = cdFindGroundInfoAtCyl(arg3, chr->radius, sp118, &floorcol, &floortype, 0, &floorroom, NULL, NULL);

		spf4.x = arg3->x;
		spf4.y = prop->pos.y - chr->ground + ground;
		spf4.z = arg3->z;

		roomsCopy(rooms, sp118);
		chr0f021fa8(chr, &spf4, sp118);
		chrGetBbox(chr->prop, &radius, &ymax, &ymin);

		if (cdTestVolume(&spf4, chr->radius, sp118, CDTYPE_ALL, CHECKVERTICAL_YES, ymax - prop->pos.y, ymin - prop->pos.y) != CDRESULT_COLLISION) {
			// Reached end of segment with no collision
			prop->pos.x = spf4.x;
			prop->pos.y = spf4.y;
			prop->pos.z = spf4.z;

			chr->ground = ground;
			chr->manground = ground;
			chr->sumground = ground * (PAL ? 8.4175090789795f : 9.999998f);

			chr->floorcol = floorcol;
			chr->floortype = floortype;
			chr->floorroom = floorroom;

			propDeregisterRooms(prop);
			roomsCopy(sp118, prop->rooms);
			propRegisterRooms(prop);

			modelSetRootPosition(chr->model, &prop->pos);

			rwdata = modelGetNodeRwData(chr->model, chr->model->definition->rootnode);
			rwdata->chrinfo.ground = ground;

			chr->chrflags |= CHRCFLAG_FORCETOGROUND;

			if (chr->actiontype == ACT_PATROL) {
				func0f0375b0(chr);
				chrPatrolGetCurWaypointInfo(chr, &spdc, spcc);
				chrGoPosInitMagic(chr, waydata, &spdc, &prop->pos);
			} else if (chr->actiontype == ACT_GOPOS) {
				if (chr->act_gopos.waypoints[chr->act_gopos.curindex] == NULL) {
					// Reached the end of the route
					if (chr->act_gopos.flags & GOPOSFLAG_FORPATHSTART) {
						chrTryStartPatrol(chr);
					} else {
						if (chr->act_gopos.curindex >= 2) {
							waypoint = chr->act_gopos.waypoints[chr->act_gopos.curindex - 2];
							padUnpack(waypoint->padnum, PADFIELD_POS, &pad);
							chrSetLookAngle(chr, atan2f(prop->pos.x - pad.pos.x, prop->pos.z - pad.pos.z));
						}

						if (CHRRACE(chr) == RACE_HUMAN || CHRRACE(chr) == RACE_SKEDAR) {
							chrStop(chr);
						}
					}
				} else {
					// Advance to next segment, still using magic
					chrGoPosAdvanceWaypoint(chr);
					chrGoPosGetCurWaypointInfo(chr, &sp5c, sp4c);
					chrGoPosInitMagic(chr, waydata, &sp5c, &prop->pos);
				}
			}
		} else {
			// Collision
			waydata->magicdone = waydata->magictotal;

			if (chr->actiontype == ACT_PATROL) {
				chr->act_patrol.waydata.lastvisible60 = g_Vars.lvframe60;
				func0f037580(chr);
			} else {
				chr->act_gopos.waydata.lastvisible60 = g_Vars.lvframe60;
				chrGoPosInitExpensive(chr);
			}
		}

		chrSetPerimEnabled(chr, true);
	}
}

/**
 * Calculate the chr's position when using the magic method of navigating.
 *
 * The magic method is used when the chr is off-screen. It measures the distance
 * between two consecutive pads in the route and simply increments a distance
 * value along that segment on each tick, ignoring collisions. Meanwhile, the
 * chr's prop->pos is left as the original location where this segment started.
 *
 * The calculation is finding the fraction of the distance travelled in this
 * segment, then finding the position between the starting pos and the pad's
 * pos.
 *
 * The pos variable is used for both loading the next pad's position and for
 * returning the new position, which means there's less stack usage.
 */
void chrCalculatePosition(struct chrdata *chr, struct coord *pos)
{
	RoomNum rooms[8];
	f32 frac;

	if (chr->actiontype == ACT_PATROL && chr->act_patrol.waydata.mode == WAYMODE_MAGIC) {
		chrPatrolGetCurWaypointInfo(chr, pos, rooms);

		if (!(chr->act_patrol.waydata.magicdone >= chr->act_patrol.waydata.magictotal)
				&& chr->act_patrol.waydata.magictotal > 0) {
			frac = chr->act_patrol.waydata.magicdone / chr->act_patrol.waydata.magictotal;
			pos->x = (pos->x - chr->prop->pos.x) * frac + chr->prop->pos.x;
			pos->y = (pos->y - chr->prop->pos.y) * frac + chr->prop->pos.y;
			pos->z = (pos->z - chr->prop->pos.z) * frac + chr->prop->pos.z;
		}
	} else if (chr->actiontype == ACT_GOPOS && chr->act_gopos.waydata.mode == WAYMODE_MAGIC) {
		chrGoPosGetCurWaypointInfo(chr, pos, rooms);

		if (!(chr->act_gopos.waydata.magicdone >= chr->act_gopos.waydata.magictotal)
				&& chr->act_gopos.waydata.magictotal > 0) {
			frac = chr->act_gopos.waydata.magicdone / chr->act_gopos.waydata.magictotal;
			pos->x = (pos->x - chr->prop->pos.x) * frac + chr->prop->pos.x;
			pos->y = (pos->y - chr->prop->pos.y) * frac + chr->prop->pos.y;
			pos->z = (pos->z - chr->prop->pos.z) * frac + chr->prop->pos.z;
		}
	} else {
		pos->x = chr->prop->pos.x;
		pos->y = chr->prop->pos.y;
		pos->z = chr->prop->pos.z;
	}
}

void chrGoPosChooseAnimation(struct chrdata *chr)
{
	s32 gospeed = chr->act_gopos.flags & GOPOSMASK_SPEED;
	s32 male = g_HeadsAndBodies[chr->bodynum].ismale;
	struct prop *leftgun = chrGetHeldProp(chr, HAND_LEFT);
	struct prop *rightgun = chrGetHeldProp(chr, HAND_RIGHT);
	s32 flip = false;
	s32 heavy;
	s32 race = CHRRACE(chr);
	s32 anim = -1;
	u32 stack;
	f32 speed = 0.5;
	f32 sp60 = 16;
	f32 animspeed = -1;
	f32 startframe = 16;

	if (chr->actiontype == ACT_GOPOS) {
		chr->act_gopos.flags &= ~GOPOSFLAG_WAITING;
	}

	if (race == RACE_EYESPY || chr->aibot) {
		return;
	}

	if (race == RACE_HUMAN || race == RACE_SKEDAR) {
		if ((leftgun && rightgun) || (!leftgun && !rightgun)) {
			heavy = false;
			flip = rngRandom() % 2;
		} else {
			if (weaponIsOneHanded(leftgun) || weaponIsOneHanded(rightgun)) {
				heavy = false;
				flip = (bool)leftgun != false;
			} else {
				heavy = true;
				flip = (bool)leftgun != false;
			}
		}

		if (race == RACE_SKEDAR) {
			if (gospeed == GOPOSFLAG_RUN) {
				anim = ANIM_SKEDAR_RUNNING;
			} else if (gospeed == GOPOSFLAG_JOG) {
				anim = ANIM_0393;
			} else if (gospeed == GOPOSFLAG_WALK) {
				anim = ANIM_0392;
			}
		} else {
			if (heavy) {
				if (gospeed == GOPOSFLAG_RUN) {
					// Human, heavy weapon, running
					if (chr->hitpart == HITPART_LFOOT
							|| chr->hitpart == HITPART_LSHIN
							|| chr->hitpart == HITPART_LTHIGH
							|| chr->hitpart == HITPART_RFOOT
							|| chr->hitpart == HITPART_RSHIN
							|| chr->hitpart == HITPART_RTHIGH) {
						anim = ANIM_020A;
						speed = 0.4;
					} else if (chr->hitpart == HITPART_LHAND
							|| chr->hitpart == HITPART_LFOREARM
							|| chr->hitpart == HITPART_LBICEP
							|| chr->hitpart == HITPART_RHAND
							|| chr->hitpart == HITPART_RFOREARM
							|| chr->hitpart == HITPART_RBICEP) {
						anim = ANIM_020D;
						speed = 0.4;
					} else {
						anim = ANIM_0029;
						speed = 0.25;
					}

					if (chr->chrflags & CHRCFLAG_RUNFASTER) {
						animspeed = 0.65;
						startframe = 48;
					} else {
						animspeed = 0.5;
						startframe = 48;
					}
				} else if (gospeed == GOPOSFLAG_JOG) {
					// Human, heavy weapon, jogging
					if (chr->hitpart == HITPART_LFOOT
							|| chr->hitpart == HITPART_LSHIN
							|| chr->hitpart == HITPART_LTHIGH
							|| chr->hitpart == HITPART_RFOOT
							|| chr->hitpart == HITPART_RSHIN
							|| chr->hitpart == HITPART_RTHIGH) {
						anim = ANIM_01F9;
					} else if (chr->hitpart == HITPART_LHAND
							|| chr->hitpart == HITPART_LFOREARM
							|| chr->hitpart == HITPART_LBICEP
							|| chr->hitpart == HITPART_RHAND
							|| chr->hitpart == HITPART_RFOREARM
							|| chr->hitpart == HITPART_RBICEP) {
						anim = ANIM_01F8;
					} else {
						anim = ANIM_RUNNING_TWOHANDGUN;
					}
				} else {
					// Human, heavy weapon, walking
					if (chr->hitpart == HITPART_LFOOT
							|| chr->hitpart == HITPART_LSHIN
							|| chr->hitpart == HITPART_LTHIGH
							|| chr->hitpart == HITPART_RFOOT
							|| chr->hitpart == HITPART_RSHIN
							|| chr->hitpart == HITPART_RTHIGH) {
						anim = ANIM_01F9;
					} else if (chr->hitpart == HITPART_LHAND
							|| chr->hitpart == HITPART_LFOREARM
							|| chr->hitpart == HITPART_LBICEP
							|| chr->hitpart == HITPART_RHAND
							|| chr->hitpart == HITPART_RFOREARM
							|| chr->hitpart == HITPART_RBICEP) {
						anim = ANIM_01F8;
					} else {
						if (rngRandom() % 2) {
							anim = ANIM_0018;
						} else {
							anim = ANIM_0028;
						}
					}
				}
			} else {
				if (gospeed == GOPOSFLAG_RUN) {
					// Human, light weapon, running
					if (chr->hitpart == HITPART_LFOOT
							|| chr->hitpart == HITPART_LSHIN
							|| chr->hitpart == HITPART_LTHIGH) {
						anim = ANIM_020A;
						flip = true;
						speed = 0.4;
					} else if (chr->hitpart == HITPART_RFOOT
							|| chr->hitpart == HITPART_RSHIN
							|| chr->hitpart == HITPART_RTHIGH) {
						anim = ANIM_020A;
						speed = 0.4;
						flip = false;
					} else if (chr->hitpart == HITPART_LHAND
							|| chr->hitpart == HITPART_LFOREARM
							|| chr->hitpart == HITPART_LBICEP) {
						anim = ANIM_020D;
						speed = 0.4;
						flip = true;
					} else if (chr->hitpart == HITPART_RHAND
							|| chr->hitpart == HITPART_RFOREARM
							|| chr->hitpart == HITPART_RBICEP) {
						anim = ANIM_020D;
						speed = 0.4;
						flip = false;
					} else if (male) {
						if (rngRandom() % 4 == 0) {
							speed = 0.25;
							anim = ANIM_001E;
						} else {
							speed = 0.25;
							anim = ANIM_005A;
							sp60 = 24;
						}
					} else {
						if (rngRandom() % 2) {
							anim = ANIM_005E;
						} else {
							anim = ANIM_005A;
						}

						speed = 0.25;
					}

					if (chr->chrflags & CHRCFLAG_RUNFASTER) {
						animspeed = 0.65;
						startframe = 48;
					} else {
						animspeed = 0.5;
						startframe = 48;
					}
				} else if (gospeed == GOPOSFLAG_JOG) {
					// Human, light weapon, jogging
					if (chr->hitpart == HITPART_LFOOT
							|| chr->hitpart == HITPART_LSHIN
							|| chr->hitpart == HITPART_LTHIGH) {
						anim = ANIM_01F9;
						flip = false;
					} else if (chr->hitpart == HITPART_RFOOT
							|| chr->hitpart == HITPART_RSHIN
							|| chr->hitpart == HITPART_RTHIGH) {
						anim = ANIM_01F9;
						flip = true;
					} else if (chr->hitpart == HITPART_LHAND
							|| chr->hitpart == HITPART_LFOREARM
							|| chr->hitpart == HITPART_LBICEP) {
						anim = ANIM_01F8;
						flip = false;
					} else if (chr->hitpart == HITPART_RHAND
							|| chr->hitpart == HITPART_RFOREARM
							|| chr->hitpart == HITPART_RBICEP) {
						anim = ANIM_01F8;
						flip = true;
					} else if (stageGetIndex(g_Vars.stagenum) == STAGEINDEX_CHICAGO) {
						anim = ANIM_005F;
					} else if (male) {
						if (rngRandom() % 2) {
							anim = ANIM_001D;
						} else {
							anim = ANIM_RUNNING_ONEHANDGUN;
						}
					} else {
						if (chr->myaction != MA_PANIC) {
							if (rngRandom() % 2) {
								anim = ANIM_005D;
							} else {
								anim = ANIM_0073;
							}
						} else {
							anim = ANIM_021D;
						}
					}
				} else {
					// Human, light weapon, walking
					s32 anims[] = {ANIM_006B, ANIM_001B, ANIM_0016};

					if (chr->hitpart == HITPART_LFOOT
							|| chr->hitpart == HITPART_LSHIN
							|| chr->hitpart == HITPART_LTHIGH) {
						anim = ANIM_01F9;
						flip = false;
					} else if (chr->hitpart == HITPART_RFOOT
							|| chr->hitpart == HITPART_RSHIN
							|| chr->hitpart == HITPART_RTHIGH) {
						anim = ANIM_01F9;
						flip = true;
					} else if (chr->hitpart == HITPART_LHAND
							|| chr->hitpart == HITPART_LFOREARM
							|| chr->hitpart == HITPART_LBICEP) {
						anim = ANIM_01F8;
						flip = false;
					} else if (chr->hitpart == HITPART_RHAND
							|| chr->hitpart == HITPART_RFOREARM
							|| chr->hitpart == HITPART_RBICEP) {
						anim = ANIM_01F8;
						flip = true;
					} else if (male) {
						anim = anims[rngRandom() % 3];
					} else {
						if (rngRandom() % 2) {
							anim = ANIM_005C;
						} else {
							anim = ANIM_0072;
						}
					}
				}
			}
		}
	} else if (race == RACE_DRCAROLL) {
		if (gospeed == GOPOSFLAG_RUN) {
			anim = ANIM_0160;
		} else if (gospeed == GOPOSFLAG_WALK) {
			anim = ANIM_015F;
		} else {
			anim = ANIM_015F;
		}
	} else if (race == RACE_ROBOT) {
		anim = ANIM_0238;
	}

	if (anim >= 0) {
		modelSetAnimation(chr->model, anim, flip, 0, speed, sp60);

		if (animspeed > 0) {
			modelSetAnimSpeed(chr->model, animspeed, startframe);
		}
	}
}

bool chrGoToRoomPos(struct chrdata *chr, struct coord *pos, RoomNum *room, u32 goposflags)
{
	struct prop *prop = chr->prop;
	struct waypoint *nextwaypoint;
	struct waypoint *lastwaypoint;
	struct waypoint *waypoints[MAX_CHRWAYPOINTS];
	s32 i;
	struct coord curwppos;
	RoomNum curwprooms[8];
	s32 isgopos = chr->actiontype == ACT_GOPOS
		&& (chr->act_gopos.flags & GOPOSMASK_SPEED) == (goposflags & 0xff & GOPOSMASK_SPEED)
		&& !chrGoPosIsWaiting(chr);
	s32 ismagic = isgopos && chr->act_gopos.waydata.mode == WAYMODE_MAGIC;
	struct coord prevpos;
	s32 numwaypoints = 0;

	for (i = 0; chr->prop->rooms[i] != -1; i++) {
		chr->oldrooms[i] = chr->prop->rooms[i];
	}

	chr->oldrooms[i] = -1;

	if (isgopos && ismagic && chr->act_gopos.waypoints[chr->act_gopos.curindex]) {
		nextwaypoint = chr->act_gopos.waypoints[chr->act_gopos.curindex];
	} else {
		nextwaypoint = waypointFindClosestToPos(&prop->pos, prop->rooms);
	}

	lastwaypoint = waypointFindClosestToPos(pos, room);

	if (nextwaypoint && lastwaypoint) {
		navSetSeed(CHRNAVSEED(chr), CHRNAVSEED(chr));
		numwaypoints = navFindRoute(nextwaypoint, lastwaypoint, waypoints, MAX_CHRWAYPOINTS);
		navSetSeed(0, 0);
	}

	if (numwaypoints > 1) {
		if (isgopos && ismagic) {
			chrCalculatePosition(chr, &prevpos);
		} else {
			prevpos.x = prop->pos.x;
			prevpos.y = prop->pos.y;
			prevpos.z = prop->pos.z;
		}

		chrStopFiring(chr);

		chr->actiontype = ACT_GOPOS;
		chr->act_gopos.endpos.x = pos->x;
		chr->act_gopos.endpos.y = pos->y;
		chr->act_gopos.endpos.z = pos->z;
		roomsCopy(room, chr->act_gopos.endrooms);

		chr->act_gopos.target = lastwaypoint;
		chr->act_gopos.curindex = 0;
		chr->act_gopos.flags = goposflags | GOPOSFLAG_INIT;
		chr->act_gopos.turnspeed = 0;
		chr->unk32c_21 = 0;
		chr->act_gopos.waydata.age = rngRandom() % 100;
		chr->act_gopos.waydata.gotaimposobj = 0;

		if (!isgopos) {
			chr->act_gopos.waydata.lastvisible60 = -1;
		}

		for (i = 0; i < MAX_CHRWAYPOINTS; i++) {
			chr->act_gopos.waypoints[i] = waypoints[i];
		}

		chrGoPosInitExpensive(chr);
#if VERSION >= VERSION_NTSC_1_0
		chr->goposforce = -1;
#endif
		chr->sleep = 0;
		chr->liftaction = 0;
		chr->act_gopos.flags &= ~(GOPOSFLAG_DUCK | GOPOSFLAG_CROUCH | GOPOSFLAG_WAITING);
		chrGoPosGetCurWaypointInfo(chr, &curwppos, curwprooms);

		if ((!isgopos || ismagic)
				&& g_Vars.normmplayerisrunning == false
				&& (prop->flags & (PROPFLAG_ONANYSCREENPREVTICK | PROPFLAG_ONANYSCREENTHISTICK | PROPFLAG_ONTHISSCREENTHISTICK)) == 0
				&& chrIsRoomOffScreen(chr, &curwppos, curwprooms)
				&& chr->inlift == false) {
			chrGoPosInitMagic(chr, &chr->act_gopos.waydata, &curwppos, &prevpos);
		}

		if (chr->act_gopos.waydata.mode != WAYMODE_MAGIC
				&& modelIsAnimMerging(chr->model) && !chr->aibot) {
			chr->hidden |= CHRHFLAG_NEEDANIM;
			return true;
		} else {
			if (!isgopos) {
				chrGoPosChooseAnimation(chr);
			}

			chr->hidden &= ~CHRHFLAG_NEEDANIM;
			return true;
		}
	}

	return false;
}

struct path *pathFindById(u32 path_id)
{
	s32 i = 0;

	for (i = 0; g_StageSetup.paths[i].pads; i++) {
		if (path_id == g_StageSetup.paths[i].id) {
			return &g_StageSetup.paths[i];
		}
	}

	return NULL;
}

void chrPatrolChooseAnimation(struct chrdata *chr)
{
	struct prop *leftprop = chrGetHeldProp(chr, HAND_LEFT);
	struct prop *rightprop = chrGetHeldProp(chr, HAND_RIGHT);
	s32 flip;
	bool heavy;
	s32 race = CHRRACE(chr);
	s32 ismale = g_HeadsAndBodies[chr->bodynum].ismale;
	f32 speed;

	if (race == RACE_EYESPY) {
		// empty
	} else if (race == RACE_HUMAN || race == RACE_SKEDAR) {
		if ((leftprop && rightprop) || (!leftprop && !rightprop)) {
			// No weapon, or double weapons
			heavy = false;
			flip = rngRandom() % 2;
		} else {
			// Single weapon
			if (weaponIsOneHanded(leftprop) || weaponIsOneHanded(rightprop)) {
				heavy = false;
				flip = ((bool)leftprop != false);
			} else {
				heavy = true;
				flip = ((bool)leftprop != false);
			}
		}

		if (race == RACE_SKEDAR) {
			modelSetAnimation(chr->model, ANIM_0392, flip, 0, 0.25f, 16);
		} else {
			speed = 0.5f * func0f02dff0(ANIM_0028) / func0f02dff0(ANIM_006B);

			if (heavy) {
				modelSetAnimation(chr->model, rngRandom() % 2 ? ANIM_0018 : ANIM_0028, flip, 0, speed, 16);
			} else if (ismale) {
				s32 anims[] = { ANIM_006B, ANIM_001B, ANIM_0016 };
				modelSetAnimation(chr->model, anims[rngRandom() % 3], flip, 0, speed, 16);
			} else {
				modelSetAnimation(chr->model, rngRandom() % 2 ? ANIM_005C : ANIM_0072, flip, 0, speed, 16);
			}
		}
	} else if (race == RACE_DRCAROLL) {
		modelSetAnimation(chr->model, ANIM_015F, false, 0, 0.5f, 16);
	} else if (race == RACE_ROBOT) {
		modelSetAnimation(chr->model, ANIM_0238, false, 0, 0.5f, 16);
	}
}

void chrStartPatrol(struct chrdata *chr, struct path *path)
{
	s32 i;
	f32 dist;
	f32 xdiff;
	f32 zdiff;
	s32 *padnumptr;
	struct pad pad;
	struct coord nextpos;
	RoomNum nextrooms[8];
	RoomNum rooms[8];
	f32 ymax;
	f32 ymin;
	f32 radius;
	f32 bestdistance = 0;
	s32 nextstep = -1;
	struct prop *prop = chr->prop;
	RoomNum sp60[2];

	if (CHRRACE(chr) != RACE_EYESPY) {
		// Do some kind of collision test with the pad to resume from...
		// maybe a line of sight check?
		if (chr->patrolnextstep >= 0 && chr->patrolnextstep < path->len) {
			padnumptr = &path->pads[chr->patrolnextstep];
			padUnpack(*padnumptr, PADFIELD_POS | PADFIELD_ROOM, &pad);

			rooms[0] = pad.room;
			rooms[1] = -1;

			chrGetBbox(prop, &radius, &ymax, &ymin);

			chrSetPerimEnabled(chr, false);

			if (cdTestCylMove04(&prop->pos, prop->rooms, &pad.pos, rooms, CDTYPE_BG, 1,
						ymax - prop->pos.y, ymin - prop->pos.y) != CDRESULT_COLLISION) {
				nextstep = chr->patrolnextstep;
			}

			chrSetPerimEnabled(chr, true);
		}

		// If the pad to resume from is not in sight, find the closest pad
		// to the chr's current position and start from there.
		if (nextstep < 0) {
			for (i = 0; path->pads[i] >= 0; i++) {
				padnumptr = &path->pads[i];
				padUnpack(*padnumptr, PADFIELD_POS, &pad);

				xdiff = pad.pos.x - prop->pos.x;
				zdiff = pad.pos.z - prop->pos.z;
				dist = xdiff * xdiff + zdiff * zdiff;

				if (nextstep < 0 || dist < bestdistance) {
					bestdistance = dist;
					nextstep = i;
				}
			}
		}

		padnumptr = &path->pads[nextstep];
		padUnpack(*padnumptr, PADFIELD_POS | PADFIELD_ROOM, &pad);

		rooms[0] = pad.room;
		rooms[1] = -1;

		// If chr has line of sight to the pad then begin the patrol,
		// otherwise use gopos to get to the starting pad
		if (func0f03654c(chr, &prop->pos, prop->rooms, &pad.pos, rooms, NULL,
					chr->radius * 1.2f, CDTYPE_PATHBLOCKER | CDTYPE_BG) != CDRESULT_COLLISION) {
			chrStopFiring(chr);

			chr->actiontype = ACT_PATROL;
			chr->act_patrol.path = path;
			chr->act_patrol.nextstep = nextstep;
			chr->act_patrol.forward = true;

			chr->act_patrol.waydata.age = rngRandom() % 100;
			chr->act_patrol.waydata.gotaimposobj = 0;
			chr->act_patrol.waydata.lastvisible60 = -1;

			chr->act_patrol.turnspeed = 0;

			func0f037580(chr);

			chr->sleep = 0;
			chr->liftaction = LIFTACTION_NOTUSINGLIFT;
			chr->patrolnextstep = chr->act_patrol.nextstep;

			chrPatrolGetCurWaypointInfo(chr, &nextpos, nextrooms);

			if (!g_Vars.normmplayerisrunning
					&& (chr->prop->flags & (PROPFLAG_ONTHISSCREENTHISTICK | PROPFLAG_ONANYSCREENTHISTICK | PROPFLAG_ONANYSCREENPREVTICK)) == 0
					&& chrIsRoomOffScreen(chr, &nextpos, nextrooms)
					&& !chr->inlift) {
				chrGoPosInitMagic(chr, &chr->act_patrol.waydata, &nextpos, &prop->pos);
			}

			// @bug: This should be act_patrol rather than act_gopos.
			// It's actually reading act_patrol.waydata.aimposobj.y which is a
			// float, so it's taking its binary representation and comparing it
			// with integer 6.
			if (chr->act_gopos.waydata.mode != WAYMODE_MAGIC && modelIsAnimMerging(chr->model)) {
				chr->hidden |= CHRHFLAG_NEEDANIM;
			} else {
				chrPatrolChooseAnimation(chr);
				chr->hidden &= ~CHRHFLAG_NEEDANIM;
			}
		} else {
			sp60[0] = pad.room;
			sp60[1] = -1;

			chrGoToRoomPos(chr, &pad.pos, sp60, GOPOSFLAG_FORPATHSTART);
		}
	}
}

void chrRecordLastVisibleTargetTime(struct chrdata *chr)
{
	chr->lastvisibletarget60 = g_Vars.lvframe60;
}

bool chrHasLosToEntity(struct chrdata *chr, struct coord *chrpos, RoomNum *chrrooms, bool allowextraheight, u32 attackflags, u32 entityid)
{
	bool result = false;
	struct coord targetpos;
	RoomNum targetrooms[8];
	struct prop *targetprop;
	struct chrdata *targetchr;
	u32 types;
	struct prop *weaponprop;

	if (attackflags & ATTACKFLAG_AIMFORWARD) {
		result = true;
	} else {
		types = CDTYPE_DOORSWITHOUTFLAG | CDTYPE_ALL;
		weaponprop = chrGetHeldProp(chr, HAND_RIGHT);

		if (weaponprop == NULL) {
			weaponprop = chrGetHeldProp(chr, HAND_LEFT);
		}

		if (weaponprop) {
			struct weaponobj *weapon = weaponprop->weapon;

			if (weapon->weaponnum == WEAPON_ROCKETLAUNCHER
					|| weapon->weaponnum == WEAPON_SLAYER
					|| weapon->weaponnum == WEAPON_ROCKETLAUNCHER_34) {
				types = CDTYPE_DOORSWITHOUTFLAG | CDTYPE_OBJSIMMUNETOEXPLOSIONS | CDTYPE_ALL;
			} else {
				types = CDTYPE_DOORSWITHOUTFLAG | CDTYPE_OBJSIMMUNETOGUNFIRE | CDTYPE_ALL;
			}
		}

		chrGetAttackEntityPos(chr, attackflags, entityid, &targetpos, targetrooms);
		chrSetPerimEnabled(chr, false);

		if ((attackflags & ATTACKFLAG_AIMATTARGET)) {
			targetprop = chrGetTargetProp(chr);

			if (targetprop->type != PROPTYPE_PLAYER || g_Vars.bondvisible) {
				propSetPerimEnabled(targetprop, false);

				if (allowextraheight && (chr->chrflags & CHRCFLAG_LOSEXTRAHEIGHT)) {
					struct coord frompos;
					RoomNum fromrooms[8];

					frompos.x = chrpos->x;
					frompos.y = chrpos->y + 70;
					frompos.z = chrpos->z;

					func0f065dd8(chrpos, chrrooms, &frompos, fromrooms);

					if (cdTestLos05(&frompos, fromrooms, &targetpos, targetrooms, types, GEOFLAG_BLOCK_SHOOT)) {
						chrRecordLastVisibleTargetTime(chr);
						result = true;
					}
				} else {
					if (cdTestLos05(chrpos, chrrooms, &targetpos, targetrooms, types, GEOFLAG_BLOCK_SHOOT)) {
						chrRecordLastVisibleTargetTime(chr);
						result = true;
					}
				}

				propSetPerimEnabled(targetprop, true);
			}
		} else if (attackflags & ATTACKFLAG_AIMATCHR) {
			targetchr = chrFindById(chr, entityid);

			if (!targetchr || !targetchr->prop) {
				targetchr = chr;
			}

			chrSetPerimEnabled(targetchr, false);

			if (cdTestLos05(chrpos, chrrooms, &targetpos, targetrooms, types, GEOFLAG_BLOCK_SHOOT)) {
				result = true;
			}

			chrSetPerimEnabled(targetchr, true);
		} else if (attackflags & ATTACKFLAG_AIMATPAD) {
			if (cdTestLos05(chrpos, chrrooms, &targetpos, targetrooms, types, GEOFLAG_BLOCK_SHOOT)) {
				result = true;
			}
		}

		chrSetPerimEnabled(chr, true);
	}

	return result;
}

bool chrHasLosToAttackTarget(struct chrdata *chr, struct coord *pos, RoomNum *rooms, bool allowextraheight)
{
	u32 attackflags = ATTACKFLAG_AIMATTARGET;
	u32 entityid = 0;

	if (chr->actiontype == ACT_ATTACK) {
		attackflags = chr->act_attack.flags;
		entityid = chr->act_attack.entityid;
	}

	return chrHasLosToEntity(chr, pos, rooms, allowextraheight, attackflags, entityid);
}

bool chrHasLosToChr(struct chrdata *chr, struct chrdata *target, RoomNum *room)
{
	bool cansee = false;
	u32 stack;
	RoomNum sp88[] = {-1, 0, 0, 0, 0, 0, 0, 0};

	if (!botIsTargetInvisible(chr, target)) {
		struct prop *prop = chr->prop;
		struct coord pos;
		RoomNum rooms[8];

		pos.x = prop->pos.x;
		pos.y = chr->ground + chr->height - 20;
		pos.z = prop->pos.z;

		chrSetPerimEnabled(chr, false);
		chrSetPerimEnabled(target, false);

		func0f065e74(&prop->pos, prop->rooms, &pos, rooms);

		if (cdTestLos07(&pos, rooms, &target->prop->pos, target->prop->rooms, sp88,
					CDTYPE_OBJS | CDTYPE_DOORS | CDTYPE_PATHBLOCKER | CDTYPE_BG | CDTYPE_AIOPAQUE,
					GEOFLAG_BLOCK_SIGHT)) {
			cansee = true;
		}

		chrSetPerimEnabled(chr, true);
		chrSetPerimEnabled(target, true);
	}

	if (room) {
		*room = sp88[0];
	}

	return cansee;
}

bool chrHasLosToTarget(struct chrdata *chr)
{
	bool cansee;
	struct prop *prop;

#if VERSION >= VERSION_JPN_FINAL
	cansee = false;
	prop = chrGetTargetProp(chr);

	if (prop && prop->chr) {
		cansee = chrHasLosToChr(chr, prop->chr, NULL);

		if (cansee) {
			chrRecordLastVisibleTargetTime(chr);
		}
	}
#else
	prop = chrGetTargetProp(chr);
	cansee = chrHasLosToChr(chr, prop->chr, NULL);

	if (cansee) {
		chrRecordLastVisibleTargetTime(chr);
	}
#endif

	return cansee;
}

bool chrHasLosToPos(struct chrdata *chr, struct coord *pos, RoomNum *rooms)
{
	struct prop *prop = chr->prop;
	bool result = false;
	struct coord eyepos;
	RoomNum chrrooms[8];

	eyepos.x = prop->pos.x;
	eyepos.y = chr->ground + chr->height - 20;
	eyepos.z = prop->pos.z;

	chrSetPerimEnabled(chr, false);
	func0f065e74(&prop->pos, prop->rooms, &eyepos, chrrooms);

	if (cdTestLos05(&eyepos, chrrooms, pos, rooms,
				CDTYPE_OBJS | CDTYPE_DOORS | CDTYPE_PATHBLOCKER | CDTYPE_BG | CDTYPE_AIOPAQUE,
				GEOFLAG_BLOCK_SIGHT)) {
		result = true;
	}

	chrSetPerimEnabled(chr, true);

	return result;
}

/**
 * Wasteful because this function calculates angles then does nothing with them.
 * chrHasLosToPos is called with the same arguments regardless.
 */
bool chrHasLosToPosWasteful(struct chrdata *chr, struct coord *pos, RoomNum *rooms)
{
	f32 facingangle = chrGetInverseTheta(chr);
	f32 posangle = atan2f(pos->x - chr->prop->pos.x, pos->z - chr->prop->pos.z);
	f32 diffangle = posangle - facingangle;

	if (posangle < facingangle) {
		diffangle += M_BADTAU;
	}

	if ((diffangle < 1.7450513839722f || diffangle > 4.5371336936951f)
			&& chrHasFlag(chr, CHRFLAG1_NOOP_00200000, BANK_1) == false) {
		return chrHasLosToPos(chr, pos, rooms);
	}

	return chrHasLosToPos(chr, pos, rooms);
}

bool chrHasLosToProp(struct chrdata *chr, struct prop *prop)
{
	bool result;

	propSetPerimEnabled(prop, false);
	result = chrHasLosToPosWasteful(chr, &prop->pos, prop->rooms);
	propSetPerimEnabled(prop, true);

	return result;
}

void chrRecordLastSeeTargetTime(struct chrdata *chr)
{
	chr->lastseetarget60 = g_Vars.lvframe60;
}

void chrRecordLastHearTargetTime(struct chrdata *chr)
{
	chr->hidden |= CHRHFLAG_IS_HEARING_TARGET;
	chr->lastheartarget60 = g_Vars.lvframe60;
}

bool chrIsStopped(struct chrdata *chr)
{
	s16 anim = modelGetAnimNum(chr->model);

	if (anim == ANIM_SNIPING_GETDOWN || anim == ANIM_SNIPING_GETUP) {
		return false;
	}

	if (anim == ANIM_SNIPING_ONGROUND && chr->act_attack.numshots >= chr->act_attack.maxshots) {
		chrStopFiring(chr);
		return true;
	}

	if (chr->actiontype == ACT_ROBOTATTACK && chr->act_robotattack.finished) {
		return true;
	}

	if (chr->actiontype == ACT_ATTACKAMOUNT && chr->act_attack.numshots >= chr->act_attack.maxshots) {
		return true;
	}

	if (chr->actiontype == ACT_STAND
			&& !chr->act_stand.prestand
			&& !chr->act_stand.reaim
			&& chr->act_stand.turning != 1) {
		return true;
	}

	if (chr->actiontype == ACT_ANIM) {
		if (chr->act_anim.completed
				|| (modelGetAnimSpeed(chr->model) >= 0 && modelGetCurAnimFrame(chr->model) >= modelGetAnimEndFrame(chr->model))
				|| (modelGetAnimSpeed(chr->model) < 0 && modelGetCurAnimFrame(chr->model) <= 0)) {
			return true;
		}
	} else if (chr->actiontype == ACT_PATROL) {
		return true;
	}

	return false;
}

bool chrCheckCanSeeTarget(struct chrdata *chr)
{
	struct prop *prop = chr->prop;
	struct prop *target = chrGetTargetProp(chr);
	f32 sqdistance;
	f32 invtheta = chrGetInverseTheta(chr);

	f32 x = target->pos.x - prop->pos.x;
	f32 y = target->pos.y - prop->pos.y;
	f32 z = target->pos.z - prop->pos.z;

	f32 angletotarget = atan2f(x, z);
	f32 angle = angletotarget - invtheta;
	bool result = false;

	if (angletotarget < invtheta) {
		angle += M_BADTAU;
	}

	sqdistance = x * x + y * y + z * z;

	if ((sqdistance < chr->visionrange * chr->visionrange * 10000.0f && (angle < 1.9195564985275f || angle > 4.3626284599304f))
			|| (sqdistance < 40000.0f && (angle < 1.9195564985275f || angle > 4.3626284599304f))) {
		result = false;

		if (sqdistance < envGetSquaredFogMax()) {
			f32 tmp;
			s32 iVar8 = (sqrtf(sqdistance) * 0.0018749999580905f);
			s32 tmp2;

			if (angle > 0.7852731347084f && angle < 5.4969120025635f) {
				tmp = angle;

				if (tmp > M_PI) {
					tmp = M_BADTAU - angle;
				}

				tmp -= 0.7852731347084f;
				tmp2 = tmp * 3.8203268051147f;
				iVar8 *= 1 + tmp2;
			}

			iVar8 = chrGetPercentageOfSlowness(chr, iVar8) + 1;
			result = rngRandom() % iVar8 == 0;
		}
	}

	if (result) {
		result = chrHasLosToTarget(chr);
	}

	if (result) {
		chrRecordLastSeeTargetTime(chr);
	}

	return result;
}

bool chrIsReadyForOrders(struct chrdata *chr)
{
	if (chr->onladder) {
		return false;
	}

	switch ((s32) chr->actiontype) {
#if VERSION >= VERSION_NTSC_1_0
	case ACT_DIE:
	case ACT_DEAD:
	case ACT_PREARGH:
	case ACT_DRUGGEDDROP:
	case ACT_DRUGGEDKO:
	case ACT_DRUGGEDCOMINGUP:
		return false;
	case ACT_ARGH:
		if ((chr->chrflags & CHRCFLAG_CANCHANGEACTDURINGARGH) == 0) {
			return false;
		}
		break;
#else
	case ACT_DIE:
	case ACT_DEAD:
	case ACT_PREARGH:
	case ACT_DRUGGEDDROP:
	case ACT_DRUGGEDKO:
	case ACT_DRUGGEDCOMINGUP:
	case ACT_ARGH:
	case 0x200:
		return false;
#endif
	case ACT_ROBOTATTACK:
		if (!chr->act_robotattack.finished) {
			return false;
		}
		break;
	}

	return true;
}

bool chrIsDead(struct chrdata *chr)
{
	if (!chr || chr->actiontype == ACT_DIE || chr->actiontype == ACT_DEAD) {
		return true;
	}

	if (chr->prop && chr->prop->type == PROPTYPE_PLAYER) {
		u32 playernum = playermgrGetPlayerNumByProp(chr->prop);

		if (g_Vars.players[playernum]->isdead) {
			return true;
		}
	}

	return false;
}

bool chrTrySidestep(struct chrdata *chr)
{
	u8 race = CHRRACE(chr);

	if ((race == RACE_HUMAN || race == RACE_SKEDAR)
			&& chrIsReadyForOrders(chr)) {
		struct prop *prop = chr->prop;
		struct prop *target = chrGetTargetProp(chr);
		f32 a = chrGetInverseTheta(chr);
		f32 b = atan2f(target->pos.x - prop->pos.x, target->pos.z - prop->pos.z);
		f32 angle = b - a;
		u32 stack[2];

		if (b < a) {
			angle += M_BADTAU;
		}

		if (angle < 0.7852731347084f || angle > 5.4969120025635f
				|| (angle > 2.3558194637299f && angle < 3.9263656139374f)) {
			bool side = (rngRandom() % 2) == 0;

			if (chrCanJumpInDirection(chr, side, 100)) {
				chrSidestep(chr, side);
				return true;
			}

			if (chrCanJumpInDirection(chr, !side, 100)) {
				chrSidestep(chr, !side);
				return true;
			}
		}
	}

	return false;
}

bool chrTryJumpOut(struct chrdata *chr)
{
	if (CHRRACE(chr) == RACE_HUMAN && chrIsReadyForOrders(chr)) {
		struct prop *prop = chr->prop;
		struct prop *target = chrGetTargetProp(chr);

		f32 a = chrGetInverseTheta(chr);
		f32 b = atan2f(target->pos.x - prop->pos.x, target->pos.z - prop->pos.z);
		f32 angle = b - a;
		u32 stack[2];

		if (b < a) {
			angle += M_BADTAU;
		}

		// This commented code is what the floats represent, but mismatches due
		// to float precision:
		//if (angle < BADDEG2RAD(45) || angle > BADDEG2RAD(315)
		//		|| (angle > BADDEG2RAD(135) && angle < BADDEG2RAD(225))) {
		if (angle < 0.7852731347084f || angle > 5.4969120025635f
				|| (angle > 2.3558194637299f && angle < BADDEG2RAD(225))) {
			bool side = (rngRandom() % 2) == 0;

			if (chrCanJumpInDirection(chr, side, 200)) {
				chrJumpOut(chr, side);
				return true;
			}

			if (chrCanJumpInDirection(chr, !side, 200)) {
				chrJumpOut(chr, !side);
				return true;
			}
		}
	}

	return false;
}

bool chrTryRunSideways(struct chrdata *chr)
{
	u32 race = CHRRACE(chr);

	if ((race == RACE_HUMAN || race == RACE_SKEDAR)
			&& chrIsReadyForOrders(chr)
			&& g_Vars.lvframe60 - chr->lastwalk60 > TICKS(180)) {
		struct prop *prop = chr->prop;
		f32 distance = 200.0f + RANDOMFRAC() * 200.0f;
		struct coord vector;
		struct coord dstpos;

		chrGetSideVectorToTarget(chr, rngRandom() % 2 == 0, &vector);

		dstpos.x = vector.x * distance + prop->pos.x;
		dstpos.y = prop->pos.y;
		dstpos.z = vector.z * distance + prop->pos.z;

		if (propchrHasClearLineToPos(prop, &dstpos, &vector)) {
			chrRunToPos(chr, &dstpos);
			return true;
		}

		vector.x = -vector.x;
		vector.z = -vector.z;

		dstpos.x = vector.x * distance + prop->pos.x;
		dstpos.y = prop->pos.y;
		dstpos.z = vector.z * distance + prop->pos.z;

		if (propchrHasClearLineToPos(prop, &dstpos, &vector)) {
			chrRunToPos(chr, &dstpos);
			return true;
		}
	}

	return false;
}

bool chrTryAttackWalk(struct chrdata *chr)
{
	u32 race = CHRRACE(chr);

	if (race == RACE_DRCAROLL || race == RACE_EYESPY) {
		return false;
	}

	if (chrIsReadyForOrders(chr)) {
		struct prop *prop = chr->prop;

		if (chrHasLosToAttackTarget(chr, &prop->pos, prop->rooms, false)
				&& (chrGetHeldUsableProp(chr, 0) || chrGetHeldUsableProp(chr, 1))
				&& g_Vars.lvframe60 - chr->lastwalk60 > TICKS(120)) {
			struct prop *target = chrGetTargetProp(chr);
			f32 x = target->pos.x - prop->pos.x;
			f32 y = target->pos.y - prop->pos.y;
			f32 z = target->pos.z - prop->pos.z;

			if (race == RACE_HUMAN && x * x + y * y + z * z >= 1000 * 1000) {
				chrAttackWalk(chr, false);
				return true;
			}
		}
	}

	return false;
}

bool chrTryAttackRun(struct chrdata *chr)
{
	u32 race = CHRRACE(chr);

	if (race == RACE_DRCAROLL || race == RACE_EYESPY) {
		return false;
	}

	if (chrIsReadyForOrders(chr)) {
		struct prop *prop = chr->prop;

		if (chrHasLosToAttackTarget(chr, &prop->pos, prop->rooms, false)
				&& (chrGetHeldUsableProp(chr, 0) || chrGetHeldUsableProp(chr, 1))
				&& g_Vars.lvframe60 - chr->lastwalk60 > TICKS(180)) {
			struct prop *target = chrGetTargetProp(chr);
			f32 x = target->pos.x - prop->pos.x;
			f32 y = target->pos.y - prop->pos.y;
			f32 z = target->pos.z - prop->pos.z;

			if (race == RACE_HUMAN && x * x + y * y + z * z >= 1000 * 1000) {
				chrAttackWalk(chr, true);
				return true;
			}
		}
	}

	return false;
}

bool chrTryAttackRoll(struct chrdata *chr)
{
	if (CHRRACE(chr) == RACE_HUMAN && chrIsReadyForOrders(chr)) {
		struct prop *prop = chr->prop;

		if (chrHasLosToAttackTarget(chr, &prop->pos, prop->rooms, false) &&
				(chrGetHeldUsableProp(chr, 0) || chrGetHeldUsableProp(chr, 1))) {
			struct prop *target = chrGetTargetProp(chr);
			f32 x = target->pos.x - prop->pos.x;
			f32 y = target->pos.y - prop->pos.y;
			f32 z = target->pos.z - prop->pos.z;
			f32 sqdistance = x * x + y * y + z * z;

			if (sqdistance >= 200 * 200) {
				bool toleft = (rngRandom() % 2) == 0;

				if (chrCanRollInDirection(chr, toleft, 200)) {
					chrAttackRoll(chr, toleft);
					return true;
				}

				if (chrCanRollInDirection(chr, !toleft, 200)) {
					chrAttackRoll(chr, !toleft);
					return true;
				}
			}
		}
	}

	return false;
}

bool chrTryAttackAmount(struct chrdata *chr, u32 arg1, u32 arg2, u8 lower, u8 upper)
{
	u8 race = CHRRACE(chr);

	if ((race == RACE_HUMAN || race == RACE_SKEDAR)
			&& chrIsReadyForOrders(chr)
			&& chr->weapons_held[0]) {
		s32 quantity;
		f32 percentage;
		struct weaponobj *weapon = chr->weapons_held[0]->weapon;
		struct inventory_ammo *ammo;

		quantity = 1;

		if (upper > 100) {
			upper = 100;
		}

		if (lower < upper) {
			percentage = ((rngRandom() % (upper - lower)) + (u32)lower) * 0.01f;
		} else {
			percentage = 0;
		}

		ammo = weaponGetAmmoByFunction(weapon->weaponnum, 0);

		if (ammo) {
			quantity = ammo->clipsize * percentage;
		}

		if (quantity < 1) {
			quantity = 1;
		}

		chr->aimendrshoulder = 0;
		chr->aimendlshoulder = 0;
		chr->aimendback = 0;
		chr->aimendsideback = 0;
		chr->aimendcount = 10;

		chrAttackAmount(chr, arg1, arg2, quantity);

		return true;
	}

	return false;
}

bool chrTryAttackStand(struct chrdata *chr, u32 attackflags, s32 entityid)
{
	s32 race = CHRRACE(chr);

	if (race == RACE_DRCAROLL || race == RACE_EYESPY) {
		return false;
	}

	if (chrIsReadyForOrders(chr)) {
		if (race == RACE_ROBOT) {
			robotAttack(chr);
			return true;
		}

		if (race == RACE_HUMAN || race == RACE_SKEDAR) {
			if (chrGetHeldUsableProp(chr, 0) ||
					(chrGetHeldUsableProp(chr, 1))) {
				chrAttackStand(chr, attackflags, entityid);
				return true;
			}
		}
	}

	return false;
}

bool chrTryAttackKneel(struct chrdata *chr, u32 attackflags, s32 entityid)
{
	s32 race = CHRRACE(chr);

	if (race == RACE_HUMAN || race == RACE_SKEDAR) {
		if (chrIsReadyForOrders(chr) && (chrGetHeldUsableProp(chr, 0) || chrGetHeldUsableProp(chr, 1))) {
			chrAttackKneel(chr, attackflags, entityid);
			return true;
		}
	}

	return false;
}

bool chrTryAttackLie(struct chrdata *chr, u32 attackflags, s32 entityid)
{
	s32 race = CHRRACE(chr);

	if (race == RACE_HUMAN || race == RACE_SKEDAR) {
		if (chrIsReadyForOrders(chr) && (chrGetHeldUsableProp(chr, 0) || chrGetHeldUsableProp(chr, 1))) {
			chrAttackLie(chr, attackflags, entityid);
			return true;
		}
	}

	return false;
}

bool chrTryModifyAttack(struct chrdata *chr, u32 attackflags, s32 entityid)
{
	s32 race = CHRRACE(chr);

	if (race == RACE_DRCAROLL || race == RACE_EYESPY) {
		return false;
	}

	if (chr->actiontype == ACT_ATTACK
			&& (chr->act_attack.flags & (ATTACKFLAG_AIMONLY | ATTACKFLAG_DONTTURN))) {
		chr->act_attack.flags = attackflags;
		chr->act_attack.entityid = entityid;
#ifndef PLATFORM_N64
		// infinitely shooting guard fix from 1964GEPD
		struct anim *anim = chr->model->anim;
		if (anim && anim->framea == anim->frameb) {
				anim->frameb++;
		}
#endif
		func0f031254(chr);
		return true;
	}

	return false;
}

bool chrFaceEntity(struct chrdata *chr, u32 attackflags, s32 entityid)
{
	if (chrIsReadyForOrders(chr)) {
		if (chr->actiontype != ACT_STAND) {
			chrStand(chr);
		}

		if (attackflags != chr->act_stand.flags || entityid != chr->act_stand.entityid) {
			chr->act_stand.flags = attackflags;
			chr->act_stand.entityid = entityid;
			chr->act_stand.reaim = 0;
			chr->act_stand.checkfacingwall = false;

			if (attackflags == ATTACKFLAG_AIMATTARGET && entityid == 1) {
				chr->act_stand.playwalkanim = true;
				chr->act_stand.entityid = 0;
			}
		}

		return true;
	}

	return false;
}

bool chrGoToPad(struct chrdata *chr, s32 padnum, u32 goposflags)
{
	if (padnum >= 0
			&& chrIsReadyForOrders(chr)
#if VERSION >= VERSION_NTSC_1_0
			&& (g_NumChrsSeenPlayerRecently2 <= 8
				|| (chr->hidden & CHRHFLAG_BASICGUARD) == 0
				|| (chr->flags & CHRFLAG0_ACTIVATEALARM))
#else
			&& g_NumChrsSeenPlayerRecently2 <= 9
#endif
				) {
		padnum = chrResolvePadId(chr, padnum);

#if VERSION >= VERSION_NTSC_1_0
		if (padnum >= 0)
#endif
		{
			RoomNum rooms[2];
			struct pad pad;

			padUnpack(padnum, PADFIELD_ROOM | PADFIELD_POS, &pad);

			rooms[0] = pad.room;
			rooms[1] = -1;

			if (chrGoToRoomPos(chr, &pad.pos, rooms, goposflags)) {
				return true;
			}
		}
	}

	return false;
}

bool chrSetPath(struct chrdata *chr, u32 path_id)
{
	chr->path = path_id;
	return true;
}

bool chrTryStartPatrol(struct chrdata *chr)
{
	struct path *path = pathFindById(chr->path);

	if (path && chrIsReadyForOrders(chr)) {
		chrStartPatrol(chr, path);
		return true;
	}

	return false;
}

bool chrTrySurrender(struct chrdata *chr)
{
	if (CHRRACE(chr) == RACE_HUMAN && chrIsReadyForOrders(chr)) {
		chrSurrender(chr);
		return true;
	}

	return false;
}

bool chrFadeOut(struct chrdata *chr)
{
	chrBeginDead(chr);
	chrFadeCorpse(chr);

	return true;
}

bool chrGoToTarget(struct chrdata *chr, u32 goposflags)
{
	if (chrIsReadyForOrders(chr)) {
		if (
#if VERSION >= VERSION_NTSC_1_0
				g_NumChrsSeenPlayerRecently2 <= 8
				|| (chr->hidden & CHRHFLAG_BASICGUARD) == 0
				|| (chr->flags & CHRFLAG0_ACTIVATEALARM)
#else
				g_NumChrsSeenPlayerRecently2 <= 9
#endif
				) {
			struct prop *prop = chrGetTargetProp(chr);

			if (chrGoToRoomPos(chr, &prop->pos, prop->rooms, goposflags)) {
				return true;
			}
		}
	}

	return false;
}

bool chrGoToChr(struct chrdata *chr, u32 dst_chrnum, u32 goposflags)
{
	if (chrIsReadyForOrders(chr)) {
		if (
#if VERSION >= VERSION_NTSC_1_0
				g_NumChrsSeenPlayerRecently2 <= 8
				|| (chr->hidden & CHRHFLAG_BASICGUARD) == 0
				|| (chr->flags & CHRFLAG0_ACTIVATEALARM)
#else
				g_NumChrsSeenPlayerRecently2 <= 9
#endif
				) {
			struct chrdata *dstchr = chrFindById(chr, dst_chrnum);

			if (dstchr && dstchr->prop && chrGoToRoomPos(chr, &dstchr->prop->pos, dstchr->prop->rooms, goposflags)) {
				return true;
			}
		}
	}

	return false;
}

bool chrGoToProp(struct chrdata *chr, struct prop *prop, u32 goposflags)
{
	if (chrIsReadyForOrders(chr) && prop) {
		if (chrGoToRoomPos(chr, &prop->pos, prop->rooms, goposflags)) {
			return true;
		}
	}

	return false;
}

bool chrGoToPos(struct chrdata *chr, struct coord *pos, u32 goposflags)
{
	RoomNum inrooms[21];
	RoomNum aboverooms[21];

	if (chrIsReadyForOrders(chr)) {
#if VERSION >= VERSION_NTSC_1_0
		if (g_NumChrsSeenPlayerRecently2 < 9
				|| (chr->hidden & CHRHFLAG_BASICGUARD) == 0
				|| (chr->flags & CHRCFLAG_NEVERSLEEP))
#else
		if (g_NumChrsSeenPlayerRecently2 < 10)
#endif
		{
			RoomNum *rooms = NULL;

			bgFindRoomsByPos(pos, inrooms, aboverooms, 20, NULL);

			if (inrooms[0] != -1) {
				rooms = inrooms;
			} else if (aboverooms[0] != -1) {
				rooms = aboverooms;
			}

			if (rooms != NULL && chrGoToRoomPos(chr, pos, rooms, goposflags)) {
				return true;
			}
		}
	}

	return false;
}

s32 func0f03aca0(struct chrdata *chr, f32 arg1, u8 arg2)
{
	f32 somefloat;

	if (!arg2) {
		arg1 -= chrGetDistanceToCoord(chr, &chr->runfrompos);
	}

	if (arg1 < 0) {
		chrAssignCoverByCriteria(g_Vars.chrdata,
				COVERCRITERIA_FURTHEREST
				| COVERCRITERIA_DISTTOTARGET
				| COVERCRITERIA_ONLYNEIGHBOURINGROOMS
				| COVERCRITERIA_ROOMSFROMME, 0);
		return chrGoToCover(chr, GOPOSFLAG_RUN);
	}

	somefloat = arg1 - 2000;

	if (somefloat < 2000) {
		somefloat = 2000;
	}

	if (!chrAssignCoverAwayFromDanger(chr, somefloat, arg1 + 10000)) {
		chrAssignCoverByCriteria(g_Vars.chrdata,
				COVERCRITERIA_FURTHEREST
				| COVERCRITERIA_DISTTOTARGET
				| COVERCRITERIA_ONLYNEIGHBOURINGROOMS
				| COVERCRITERIA_ROOMSFROMME, 0);
	}

	return chrGoToCover(chr, GOPOSFLAG_RUN);
}

bool chrTryStop(struct chrdata *chr)
{
	if (CHRRACE(chr) == RACE_EYESPY) {
		func0f02e9a0(chr, 0);
		return true;
	}

	if (chrIsReadyForOrders(chr)) {
		chrStop(chr);
		return true;
	}

	return false;
}

bool chrTrySurprisedOneHand(struct chrdata *chr)
{
	if (CHRRACE(chr) == RACE_HUMAN && chrIsReadyForOrders(chr)) {
		chrDoSurprisedOneHand(chr);
		return true;
	}

	return false;
}

bool chrTrySurprisedSurrender(struct chrdata *chr)
{
	if (CHRRACE(chr) == RACE_HUMAN && chrIsReadyForOrders(chr)) {
		chrDoSurprisedSurrender(chr);
		return true;
	}

	return false;
}

bool chrTrySurprisedLookAround(struct chrdata *chr)
{
	if (CHRRACE(chr) == RACE_HUMAN && chrIsReadyForOrders(chr)) {
		chrDoSurprisedLookAround(chr);
		return true;
	}

	return false;
}

bool chrTryKneel(struct chrdata *chr)
{
	if (CHRRACE(chr) == RACE_HUMAN && chrIsReadyForOrders(chr)) {
		chrKneel(chr);
		return true;
	}

	return false;
}

bool chrTryStartAnim(struct chrdata *chr, s32 animfnum, f32 startframe, f32 endframe, u8 chranimflags, s32 merge, f32 speed)
{
	if (chrIsReadyForOrders(chr)) {
		chrStartAnim(chr, animfnum, startframe, endframe, chranimflags, merge, speed);
		return true;
	}

	return false;
}

bool chrTryRunFromTarget(struct chrdata *chr)
{
	struct prop *target = chrGetTargetProp(chr);
	struct prop *prop = chr->prop;
	f32 ymax;
	f32 ymin;
	f32 radius;
	struct coord dst;
	RoomNum rooms[8];
	struct coord diff;
	f32 distance;

	if (chrIsReadyForOrders(chr)) {
		if (!target) {
			return false;
		}

		diff.x = target->pos.x - prop->pos.x;
		diff.z = target->pos.z - prop->pos.z;
		diff.y = 0;

		distance = sqrtf(diff.f[0] * diff.f[0] + diff.f[2] * diff.f[2]);

		// Scale diff into range -1 to +1
		diff.x *= (1 / distance);
		diff.z *= (1 / distance);

		// Set dst to 1000 units in the opposite direction
		dst.x = prop->pos.x - diff.x * 1000;
		dst.z = prop->pos.z - diff.z * 1000;
		dst.y = prop->pos.y;

		propGetBbox(prop, &radius, &ymax, &ymin);

		// If dst runs into a wall, set it to closest valid spot
		if (cdExamCylMove03(&prop->pos, prop->rooms, &dst,
					CDTYPE_OBJS | CDTYPE_DOORS | CDTYPE_PATHBLOCKER | CDTYPE_BG,
					1, ymax - prop->pos.y, ymin - prop->pos.y) == CDRESULT_COLLISION) {
#if VERSION >= VERSION_JPN_FINAL
			cdGetPos(&dst, 8796, "chr/chraction.c");
#elif VERSION >= VERSION_PAL_FINAL
			cdGetPos(&dst, 8793, "chr/chraction.c");
#elif VERSION >= VERSION_NTSC_1_0
			cdGetPos(&dst, 8788, "chraction.c");
#else
			cdGetPos(&dst, 8782, "chraction.c");
#endif
		}

		// Adjust dst to be two chr widths closer to avoid collision with wall
		dst.x = dst.x + (diff.x * radius + diff.x * radius);
		dst.z = dst.z + (diff.z * radius + diff.z * radius);

		if (func0f036974(prop, &dst)) {
			u32 speed = GOPOSFLAG_RUN;

			if (CHRRACE(chr) == RACE_HUMAN) {
				f32 dist = chrGetDistanceToCoord(chr, &dst);

				if (dist > 100) {
					speed = GOPOSFLAG_RUN;
				} else if (dist < 50) {
					speed = GOPOSFLAG_WALK;
				} else {
					speed = GOPOSFLAG_JOG;
				}
			}

			func0f065e74(&prop->pos, prop->rooms, &dst, rooms);
			chrGoToRoomPos(chr, &dst, rooms, speed);

			return true;
		}
	}

	return false;
}

/**
 * Attempt to make the chr find a suitable prop in their current room to use as
 * cover and run to it. However, a bug prevents this from working.
 */
bool chrGoToCoverProp(struct chrdata *chr)
{
	struct prop *targetprop = chrGetTargetProp(chr);
	struct prop *chrprop = chr->prop;
	s16 i;
	s16 propnums[258];
	struct defaultobj *obj;
	s16 numprops;
	s16 startindex;

	if (targetprop == NULL) {
		return false;
	}

	if (chrIsReadyForOrders(chr)) {
		roomGetProps(chrprop->rooms, propnums, 256);

		for (numprops = 0; propnums[numprops] >= 0; numprops++);

		if (numprops > 0) {
			startindex = rngRandom() % numprops;
		}

		for (i = 0; i < numprops; i++) {
			struct prop *prop = &g_Vars.props[propnums[(i + startindex) % numprops]];
			obj = prop->obj;

			if (prop->type == PROPTYPE_OBJ
					&& (obj->hidden2 & OBJH2FLAG_DESTROYED) == 0
					&& (obj->hidden & OBJHFLAG_00008000) == 0
					&& (obj->hidden & OBJHFLAG_OCCUPIEDCHAIR) == 0) {
				f32 targetdist = propGetDistanceToProp(targetprop, prop);
				f32 chrdist = propGetDistanceToProp(chrprop, prop);

				// @bug: This condition cannot pass
				// (should be chrdist < targetdist + targetdist)
				if (chrdist < 300 && chrdist > targetdist + targetdist && targetdist > 800) {
					f32 propymax;
					f32 propymin;
					f32 propradius;
					f32 chrymax;
					f32 chrymin;
					f32 chrradius;
					f32 propheight;
					f32 chrheight;
					struct coord dstpos;
					RoomNum dstrooms[8];

					propGetBbox(prop, &propradius, &propymax, &propymin);
					propGetBbox(chrprop, &chrradius, &chrymax, &chrymin);

					propheight = propymax - propymin;
					chrheight = chrymax - chrymin;

					if (propheight > chrheight * 0.4f && propheight < chrheight * 0.9f) {
						propSetPerimEnabled(prop, false);

						if (cdTestLos04(&chrprop->pos, chrprop->rooms, &prop->pos, CDTYPE_DOORS | CDTYPE_BG)) {
							propSetPerimEnabled(prop, true);

							dstpos.x = prop->pos.x - (targetprop->pos.x - prop->pos.x) / targetdist * (propradius * 1.25f + chrradius);
							dstpos.z = prop->pos.z - (targetprop->pos.z - prop->pos.z) / targetdist * (propradius * 1.25f + chrradius);
							dstpos.y = prop->pos.y;

							if (func0f036974(chrprop, &dstpos)) {
								u32 speed = GOPOSFLAG_JOG;

								if (CHRRACE(chr) == RACE_HUMAN) {
									if (chrdist > 1500) {
										speed = GOPOSFLAG_RUN;
									} else if (chrdist < 100) {
										speed = GOPOSFLAG_WALK;
									}
								}

								func0f065e74(&chrprop->pos, chrprop->rooms, &dstpos, dstrooms);
								chrGoToRoomPos(chr, &dstpos, dstrooms, speed);

								chr->proppreset1 = prop - g_Vars.props;
								obj->hidden |= OBJHFLAG_OCCUPIEDCHAIR;

								return true;
							}
						}
					}

					propSetPerimEnabled(prop, true);
				}
			}
		}
	}

	return false;
}

bool chrTryStartAlarm(struct chrdata *chr, s32 pad_id)
{
	if (CHRRACE(chr) == RACE_HUMAN && chrIsReadyForOrders(chr)) {
		pad_id = chrResolvePadId(chr, pad_id);

#if VERSION >= VERSION_NTSC_1_0
		if (pad_id >= 0)
#endif
		{
			struct defaultobj *obj = objFindByPadNum(pad_id);

			if (obj && objIsHealthy(obj)) {
				chrStartAlarm(chr);
				return true;
			}
		}
	}

	return false;
}

bool chrConsiderGrenadeThrow(struct chrdata *chr, u32 attackflags, u32 entityid)
{
	bool done = false;
	bool wantthrow;

#ifndef PLATFORM_N64
	// Chaos "Frag Out" (pd.frag_out): any human that could open fire lobs a
	// grenade instead — skip the grenadeprob roll and drop the min engagement
	// range (still keep a small standoff so they don't nuke themselves at
	// point-blank). The block below hands them a grenade if they lack one.
	if (g_ChaosFragOut) {
		wantthrow = CHRRACE(chr) == RACE_HUMAN
				&& chrGetDistanceToTarget(chr) > 100
				&& chrIsReadyForOrders(chr);
	} else
#endif
	{
		wantthrow = CHRRACE(chr) == RACE_HUMAN
				&& chr->grenadeprob > (rngRandom() % 255)
				&& chrGetDistanceToTarget(chr) > 200
				&& chrIsReadyForOrders(chr);
	}

	if (wantthrow) {
		struct prop *target = chrGetTargetProp(chr);
		struct coord pos;

		if (target) {
			pos.x = target->pos.x;
			pos.y = target->pos.y;
			pos.z = target->pos.z;
		}

		if (target && cdTestLos04(&chr->prop->pos, chr->prop->rooms, &pos,
					CDTYPE_OBJS | CDTYPE_DOORS | CDTYPE_PATHBLOCKER | CDTYPE_BG)) {
			struct prop *leftprop = chrGetHeldProp(chr, HAND_LEFT);
			struct prop *rightprop = chrGetHeldProp(chr, HAND_RIGHT);
			struct weaponobj *weapon;

#if PIRACYCHECKS
			{
				u32 checksum = 0;
				s32 *i = (s32 *)&bgReset;
				s32 *end = (s32 *)&bgBuildTables;
				u32 stackpadding[1];

				while (i < end) {
					checksum += *i;
					checksum += (*i >> 1);
					i++;
				}

				if (checksum != CHECKSUM_PLACEHOLDER) {
					// Make player explode continuously
					g_Vars.currentplayer->bondexploding = true;
					g_Vars.currentplayer->bondnextexplode = g_Vars.lvframe60 + 120;
					g_Vars.currentplayer->bondcurexplode = 0;
				}
			}
#endif

			// If grenade is equipped in either hand, use it
			if (rightprop) {
				weapon = rightprop->weapon;

				if (weapon->weaponnum == WEAPON_GRENADE || weapon->weaponnum == WEAPON_NBOMB) {
					chrThrowGrenade(chr, 0, false);
					chr->act_throwgrenade.flags = attackflags;
					chr->act_throwgrenade.entityid = entityid;
					done = true;
				}
			}

			if (!done && leftprop) {
				weapon = leftprop->weapon;

				if (weapon->weaponnum == WEAPON_GRENADE || weapon->weaponnum == WEAPON_NBOMB) {
					chrThrowGrenade(chr, 1, false);
					chr->act_throwgrenade.flags = attackflags;
					chr->act_throwgrenade.entityid = entityid;
					done = true;
				}
			}

			// Grenade not equipped, and using a single weapon
			if (!done && (leftprop == NULL || rightprop == NULL)) {
				struct prop *prop;
				u32 flags = 0;
				u32 stackpadding2[2];

				if (rightprop) {
					flags = OBJFLAG_WEAPON_LEFTHANDED;
				}

				if (stageGetIndex(g_Vars.stagenum) == STAGEINDEX_MBR) {
					prop = chrGiveWeapon(chr, MODEL_CHRGRENADE, WEAPON_NBOMB, flags);
				} else {
					prop = chrGiveWeapon(chr, MODEL_CHRGRENADE, WEAPON_GRENADE, flags);
				}

				if (prop) {
					weapon = prop->weapon;
					weapon->base.hidden |= OBJHFLAG_GONE;
					chrThrowGrenade(chr, rightprop == NULL ? 0 : 1, true);
					chr->act_throwgrenade.flags = attackflags;
					chr->act_throwgrenade.entityid = entityid;
					done = true;
				}
			}
		}
	}

	return done;
}

#ifndef PLATFORM_N64
// Lua bridge: spawn a weapon/item world object at the given chr's location, by
// chrnum. Reuses chrDropItem -- the same engine-blessed path the drop_item AI
// command and chrDie's own loot-drop use -- so object init, model load, and
// floor placement are handled correctly (no manual prop-pos / room math). This
// is the mutating counterpart to the read-only pd.* queries; it is server-side
// only (AI/world mutation must not run on a net client) and a no-op if the
// chrnum is unknown or the weapon has no world model. Returns 1 on success.
//
// Intended for the kill event (spawn a marker where an enemy died); chrDie
// itself drops the chr's weapons at this same point, so spawning here is safe.
s32 chraiLuaSpawnAtChr(s32 chrnum, s32 weaponnum)
{
	struct chrdata *chr;

	if (g_NetMode == NETMODE_CLIENT) {
		return 0; // world mutation is server-authoritative
	}

	chr = (chrnum < 0) ? NULL : chrFindByLiteralId(chrnum);
	if (chr == NULL || chr->prop == NULL) {
		return 0;
	}

	// Drop a weapon as an INDEPENDENT floor pickup at the chr's feet. Do NOT
	// use chrDropItem here: it reparents the item to the chr and defers the
	// actual drop to the chr's death/drop tick, so on a LIVING NPC the weapon
	// stays attached and never lands on the ground (Fire Sale spawned nothing at
	// their feet). chraiLuaSpawnAtPos creates a free-standing, floor-snapped
	// pickup at the given position — exactly what Fire Sale wants.
	return chraiLuaSpawnAtPos(chrnum, weaponnum,
			chr->prop->pos.x, chr->prop->pos.y, chr->prop->pos.z);
}

// Lua bridge: spawn a weapon/item world object at an arbitrary position, with
// rooms seeded from a reference chr (refchrnum, default-resolved by the caller).
// Backs pd.spawn(). The object is created anchored to the reference chr (so the
// engine's MP-index / creation paths stay valid), then repositioned to the
// target and floor-snapped using the same primitives the engine uses for normal
// object placement: cdFindFloorRoomYColourFlagsAtPos walks the portal graph from
// the reference chr's (known-valid) rooms to find the real floor room + height
// at the target, then func0f06a580 sets pos + re-registers rooms. Placement is
// reliable when the target is reachable through portals from the reference chr;
// if no floor is found we fall back to the raw pos with the seed rooms (the
// object may float rather than crash). Server-side only.
s32 chraiLuaSpawnAtPos(s32 refchrnum, s32 weaponnum, f32 x, f32 y, f32 z)
{
	struct chrdata *refchr;
	struct weaponobj *weapon;
	s32 modelnum;
	struct coord pos;
	Mtxf mtx;
	RoomNum seedrooms[8];
	RoomNum floorroom;
	f32 floory;
	struct modelrodata_bbox *bbox;

	if (g_NetMode == NETMODE_CLIENT) {
		return 0; // world mutation is server-authoritative
	}

	// refchrnum < 0 means "use the local player's chr" -- the common case for
	// spawning near the player, and a guaranteed-valid seed-rooms source.
	if (refchrnum < 0) {
		refchr = (g_Vars.currentplayer && g_Vars.currentplayer->prop)
				? g_Vars.currentplayer->prop->chr : NULL;
	} else {
		refchr = chrFindByLiteralId(refchrnum);
	}
	if (refchr == NULL || refchr->prop == NULL) {
		return 0; // need a valid reference chr for creation + seed rooms
	}

	modelnum = playermgrGetModelOfWeapon(weaponnum);
	if (modelnum < 0) {
		return 0;
	}

	weapon = weaponCreateProjectileFromWeaponNum(modelnum, (u8)weaponnum, refchr);
	if (weapon == NULL || weapon->base.prop == NULL || weapon->base.model == NULL) {
		return 0;
	}

	modelSetScale(weapon->base.model, weapon->base.model->scale);
	weapon->timer240 = TICKS(720);

	pos.x = x;
	pos.y = y;
	pos.z = z;
	mtx4LoadIdentity(&mtx);
	roomsCopy(refchr->prop->rooms, seedrooms);

	// Floor-snap: find the real floor room + Y at the target (portal-walk from the
	// reference rooms), then place resting on the floor. Mirrors func0f06a650.
	bbox = modelFindBboxRodata(weapon->base.model);
#if VERSION >= VERSION_NTSC_1_0
	floorroom = cdFindFloorRoomYColourFlagsAtPos(&pos, seedrooms, &floory, &weapon->base.floorcol, NULL);
#else
	floorroom = cdFindFloorRoomYColourFlagsAtPos(&pos, seedrooms, &floory, &weapon->base.floorcol);
#endif

	if (floorroom > 0) {
		RoomNum placerooms[2];
		struct coord placepos;
		placepos.x = pos.x;
		placepos.y = floory - objGetRotatedLocalYMinByMtx4(bbox, &mtx);
		placepos.z = pos.z;
		placerooms[0] = floorroom;
		placerooms[1] = -1;
		func0f06a580(&weapon->base, &placepos, &mtx, placerooms);
	} else {
		func0f06a580(&weapon->base, &pos, &mtx, seedrooms);
	}

	objSetDropped(weapon->base.prop, DROPTYPE_DEFAULT);
	return 1;
}

// ---- Archipelago bonus / buff bridges (luaai_ap) ------------------------
// Apply instant boosts to the LOCAL player on receipt of an AP "bonus" item.
// All are server/solo authoritative (no-op on a net client) and no-op when there
// is no live player chr (title/menu). They reuse the engine's own setters so the
// behaviour matches the in-game pickups/cheats they mirror. Backed pd.* wrappers
// live in luaai_api.c.

// Resolve the local player's chr, or NULL if there isn't one right now.
static struct chrdata *apLuaPlayerChr(void)
{
	if (g_NetMode == NETMODE_CLIENT) {
		return NULL; // bonuses apply on the save-owning machine (host/solo)
	}
	if (g_Vars.currentplayer == NULL || g_Vars.currentplayer->prop == NULL) {
		return NULL;
	}
	return g_Vars.currentplayer->prop->chr;
}

// pd.player_heal(): restore the player to full health.
s32 chraiLuaPlayerHeal(f32 amount)
{
	if (apLuaPlayerChr() == NULL) {
		return 0;
	}
	if (amount <= 0.0f) {
		g_Vars.currentplayer->bondhealth = 1.0f; // full heal (default / no arg)
	} else {
		g_Vars.currentplayer->bondhealth += amount; // partial top-up (e.g. 0.5 = half)
		if (g_Vars.currentplayer->bondhealth > 1.0f) {
			g_Vars.currentplayer->bondhealth = 1.0f;
		}
	}
	playerDisplayHealth(); // silent HP change — pop the health bar
	return 1;
}

// pd.player_set_shield(frac): set the player's shield (0..1; >=1 = full).
s32 chraiLuaPlayerSetShield(f32 frac)
{
	if (apLuaPlayerChr() == NULL) {
		return 0;
	}
	if (frac < 0.0f) frac = 0.0f;
	if (frac > 1.0f) frac = 1.0f;
	playerSetShieldFrac(frac);
	playerDisplayHealth(); // silent shield change — pop the health bar
	return 1;
}

// pd.refill_ammo(): top every ammo type to capacity (covers the current weapon).
s32 chraiLuaRefillAmmo(void)
{
	if (apLuaPlayerChr() == NULL) {
		return 0;
	}
	bgunGiveMaxAmmo(true);
	return 1;
}

/**
 * How many magazines the chaos gun-giving effects hand out (user request
 * 2026-07-28: a free gun should be a moment of power, not a licence to stop
 * caring about ammo). Applied at the C grant sites themselves — dual-wield and
 * body-snatch both used to call bgunGiveMaxAmmo directly, so gating it in Lua
 * alone would have left four dual_wield effects still topping up to capacity.
 */
#define CHAOS_GUN_MAGS 2

/**
 * pd.give_mags(n): stock every ammo type with n MAGAZINES instead of filling it
 * to capacity, so the gun-giving chaos effects hand you a usable weapon rather
 * than an inexhaustible one.
 *
 * A magazine is a property of the WEAPON FUNCTION, not the ammo type — several
 * weapons share an ammo type with different clip sizes — so there is no single
 * "one magazine" figure to read off the ammo table. Pass 1 walks every weapon
 * and both its functions and records the LARGEST clip size seen per ammo type;
 * pass 2 writes n of those back. Largest rather than smallest deliberately:
 * n mags should be a sensible amount for whatever you are actually holding, and
 * undershooting on a big-magazine gun (Cyclone, Reaper) would leave the effect
 * feeling broken.
 *
 * Everything goes through the public bgun*ForWeapon accessors rather than the
 * ammo table itself, which is file-local to bondgun.c. Ammo types no weapon
 * uses are left alone — zeroing them would strip gadgets and objective items
 * that ride the same array.
 */
s32 chraiLuaGiveMags(s32 mags)
{
	s32 clip[AMMOTYPE_ECM_MINE + 1];
	s32 i;
	s32 f;

	if (apLuaPlayerChr() == NULL) {
		return 0;
	}

	if (mags < 1) {
		mags = 1;
	} else if (mags > 99) {
		mags = 99;
	}

	for (i = 0; i < ARRAYCOUNT(clip); i++) {
		clip[i] = 0;
	}

	for (i = WEAPON_UNARMED; i <= WEAPON_SUICIDEPILL; i++) {
		for (f = 0; f < 2; f++) {
			struct inventory_ammo *ammo = weaponGetAmmoByFunction(i, f);

			if (ammo && ammo->type < ARRAYCOUNT(clip) && ammo->clipsize > clip[ammo->type]) {
				clip[ammo->type] = ammo->clipsize;
			}
		}
	}

	for (i = WEAPON_UNARMED; i <= WEAPON_SUICIDEPILL; i++) {
		for (f = 0; f < 2; f++) {
			struct inventory_ammo *ammo = weaponGetAmmoByFunction(i, f);

			if (ammo && ammo->type < ARRAYCOUNT(clip) && clip[ammo->type] > 0) {
				s32 cap = bgunGetAmmoCapacityForWeapon(i, f);
				s32 qty = clip[ammo->type] * mags;

				if (cap > 0 && qty > cap) {
					qty = cap;
				}

				bgunSetAmmoQtyForWeapon(i, f, qty);
			}
		}
	}

	return 1;
}

// pd.give_ammo(ammotype, qty): grant ammo (auto-gives the matching weapon).
s32 chraiLuaGiveAmmo(s32 ammotype, s32 qty)
{
	if (apLuaPlayerChr() == NULL) {
		return 0;
	}
	ammoHandlePickup(ammotype, qty, true, true);
	return 1;
}

// pd.give_weapon(weaponnum): add a weapon to the player's inventory.
s32 chraiLuaGiveWeaponToPlayer(s32 weaponnum)
{
	if (apLuaPlayerChr() == NULL) {
		return 0;
	}
	invGiveSingleWeapon(weaponnum);
	return 1;
}

// pd.device_on(weaponnum): activate a device (e.g. WEAPON_CLOAKINGDEVICE).
s32 chraiLuaDeviceOn(s32 weaponnum)
{
	if (apLuaPlayerChr() == NULL) {
		return 0;
	}
	currentPlayerSetDeviceActive(weaponnum, true);
	return 1;
}

// pd.invincible(on): toggle player invincibility (Lua manages any timer).
s32 chraiLuaSetInvincible(s32 on)
{
	if (apLuaPlayerChr() == NULL) {
		return 0;
	}
	g_Vars.currentplayer->invincible = on ? 1 : 0;
	return 1;
}

// --------------------------------------------------------------------------
// Chaos-mode primitives (docs/PORT_CHAOS.md). Same contract as the AP helpers
// above: operate on the current local player via the apLuaPlayerChr() guard,
// return 0/false when no pawn exists (menus, dedicated lobby). All are
// invoked from Lua only (scripts/chaos.lua and anything a chat bridge feeds
// through the external event queue).
// --------------------------------------------------------------------------

// pd.lvupdate(): game ticks elapsed this frame (g_Vars.lvupdate60) — exactly
// 0 while the game is paused, scaled during slo-mo/boost. Chaos uses it to
// advance effect timers on GAME time, so pausing can't run out a bad effect.
s32 chraiLuaLvUpdate(void)
{
	return g_Vars.lvupdate60;
}

// pd.device_off(weaponnum): deactivate a device (the pd.device_on inverse —
// currentPlayerSetDeviceActive with active=false clears the devicesactive
// bit, which is all device_on ever set). Backs the timed gadget effects'
// stop functions (X-ray specs / night vision / cloak wearing off).
s32 chraiLuaDeviceOff(s32 weaponnum)
{
	if (apLuaPlayerChr() == NULL) {
		return 0;
	}
	currentPlayerSetDeviceActive(weaponnum, false);
	return 1;
}

// pd.device_active(weaponnum): query whether a device is currently switched
// ON (DEVICESTATE_ACTIVE — wearing the eyewear, not merely owning it). Backs
// the Lights out afterglow: darkness persists until the player unequips.
s32 chraiLuaDeviceActive(s32 weaponnum)
{
	if (apLuaPlayerChr() == NULL) {
		return 0;
	}
	return currentPlayerGetDeviceState(weaponnum) == DEVICESTATE_ACTIVE;
}

// pd.take_weapon(weaponnum): remove a weapon from the player's inventory and
// cycle off it if held (the aiChrDropWeapon player branch, minus the world
// drop — chaos takes the gun, it doesn't gift it to the floor).
s32 chraiLuaTakeWeapon(s32 weaponnum)
{
	if (apLuaPlayerChr() == NULL) {
		return 0;
	}
	invRemoveItemByNum(weaponnum);
	if (bgunGetWeaponNum(HAND_RIGHT) == weaponnum) {
		bgunCycleBack();
	}
	return 1;
}

// pd.weapon_held() -> weaponnum of the right hand (-1 with no pawn).
s32 chraiLuaWeaponHeld(void)
{
	if (apLuaPlayerChr() == NULL) {
		return -1;
	}
	return bgunGetWeaponNum(HAND_RIGHT);
}

// pd.switch_weapon(weaponnum): force-equip a weapon the player owns.
s32 chraiLuaSwitchWeapon(s32 weaponnum)
{
	if (apLuaPlayerChr() == NULL) {
		return 0;
	}
	bgunEquipWeapon2(HAND_RIGHT, weaponnum);
	return 1;
}

// pd.fade(r,g,b,a,time60): start a screen fade on the local player's viewport
// (the cutscene fade machinery — playerSetFadeColour + a full-fraction fade
// over time60 ticks). Chaos uses it for blink/flashbang-style effects.
s32 chraiLuaScreenFade(s32 r, s32 g, s32 b, s32 a, f32 time60)
{
	f32 frac;

	if (apLuaPlayerChr() == NULL) {
		return 0;
	}
	// Flash instantly to (r,g,b) at intensity `a` (0-255), then fade back to the
	// game over time60 frames — one clean flash. The colour fraction is 0..1, so
	// `a` must be normalised: passing it raw (e.g. 200) set frac far over 1 (an
	// overflowing full-white strobe) and the animation then targeted 1, i.e. it
	// faded TO white and stuck there ("flashes white and never comes back").
	if (a < 0) {
		a = 0;
	} else if (a > 255) {
		a = 255;
	}
	frac = a * (1.0f / 255.0f);

	playerSetFadeColour(r, g, b, frac); // instant flash to the colour + HOLD

	// time60 > 0: fade back out to the game over time60 frames.
	// time60 <= 0: leave it held at the colour — the caller drives the fade-out
	// later with a second pd.fade (e.g. Snap/Blink hold white 1s then fade 2s).
	if (time60 > 0) {
		playerSetFadeFrac(time60, 0);
	}
	return 1;
}

// pd.chr_yeet(chrnum, force): fling a chr away from the local player with the
// explosion-knockback machinery (chrYeetFromPos). Purely kinetic — no damage.
s32 chraiLuaYeetChr(s32 chrnum, f32 force)
{
	struct chrdata *chr = chrFindByLiteralId(chrnum);

	if (apLuaPlayerChr() == NULL || chr == NULL || chr->prop == NULL || chr->model == NULL) {
		return 0;
	}
	chrYeetFromPos(chr, &g_Vars.currentplayer->prop->pos, force);
	return 1;
}

// pd.explosion(chrnum, type): detonate an explosion of the given type at a
// chr's feet, attributed to the local player. Position + rooms come from the
// live prop so the visual/damage register in the right room.
s32 chraiLuaExplodeAtChr(s32 chrnum, s32 type)
{
	struct chrdata *chr = chrFindByLiteralId(chrnum);

	if (apLuaPlayerChr() == NULL || chr == NULL || chr->prop == NULL) {
		return 0;
	}
	return explosionCreateSimple(NULL, &chr->prop->pos, chr->prop->rooms,
			(s16)type, g_Vars.bondplayernum) ? 1 : 0;
}

// pd.sound(sfxnum): play a one-shot sound locally (announcer stingers etc).
s32 chraiLuaPlaySound(s32 sfxnum)
{
	sndStart(var80095200, (s16)sfxnum, NULL, -1, -1, -1, -1, -1);
	return 1;
}

// pd.metronome_click(): a short click for the "Beat game" metronome, played at
// HALF the in-game music volume. optionsGetMusicVolume is the music-slider level
// (0..0x5000); map it onto the SFX full-volume scale (0..0x7fff = AL_VOL_FULL) and
// halve, so at max music the click is half of full SFX (clearly audible) and it
// scales down with the player's music setting.
s32 chraiLuaMetronomeClick(void)
{
	extern u16 optionsGetMusicVolume(void);
	s32 vol = (s32)optionsGetMusicVolume() * 0x7fff / 0x5000 / 2;

	sndStart(var80095200, (s16)SFX_MENU_FOCUS, NULL, vol, -1, -1, -1, -1);
	return 1;
}

// pd.explosion_at(x, y, z [, type]): detonate at an arbitrary position,
// attributed to the local player. Rooms are portal-walked from the player's
// (known-valid) rooms to the real floor room at the target — the same
// placement recipe as chraiLuaSpawnAtPos. Backs the Live Grenade / Martyrdom
// delayed-boom pattern: Lua records a position, spawns a grenade pickup
// there (pd.spawn), then calls this when the fuse runs out.
s32 chraiLuaExplodeAtPos(f32 x, f32 y, f32 z, s32 type)
{
	struct coord pos;
	RoomNum rooms[8];
	f32 floory;
	u16 floorcol;
	s32 floorroom;

	if (apLuaPlayerChr() == NULL || g_NetMode == NETMODE_CLIENT) {
		return 0;
	}

	pos.x = x;
	pos.y = y;
	pos.z = z;

	roomsCopy(g_Vars.currentplayer->prop->rooms, rooms);
#if VERSION >= VERSION_NTSC_1_0
	floorroom = cdFindFloorRoomYColourFlagsAtPos(&pos, rooms, &floory, &floorcol, NULL);
#else
	floorroom = cdFindFloorRoomYColourFlagsAtPos(&pos, rooms, &floory, &floorcol);
#endif
	if (floorroom > 0) {
		rooms[0] = floorroom;
		rooms[1] = -1;
	}

	return explosionCreateSimple(NULL, &pos, rooms, (s16)type, g_Vars.bondplayernum) ? 1 : 0;
}

// pd.grenade(x, y, z [, chrnum]): drop a LIVE, armed grenade at a position —
// the real engine thrown-grenade path (bgunCreateThrownProjectile2): it lands,
// arms, and the engine detonates it on the grenade's own fuse. Plays the
// pin-pull ping (SFX_05C1 — the sound the throw animation would have played)
// plus the engine's own SFX_THROW. Attributed to the local player. Backs Live
// Grenade / Martyrdom (no Lua-side explosion timing needed — it's real).
//
// chrnum >= 0 seeds the floor-room search from that chr's rooms: martyrdom
// drops at corpses far from the player, where the player's rooms resolve no
// floor and the grenade lands in the wrong room set (never detonating where
// it should). Defaults to the player's rooms for at-your-feet drops.
s32 chraiLuaSpawnGrenade(f32 x, f32 y, f32 z, s32 chrnum)
{
	struct coord pos;
	struct coord vel;
	RoomNum rooms[8];
	f32 floory;
	u16 floorcol;
	s32 floorroom;
	Mtxf mtx;
	struct gset gset;
	struct defaultobj *obj;
	struct chrdata *seedchr = (chrnum >= 0) ? chrFindByLiteralId(chrnum) : NULL;

	if (apLuaPlayerChr() == NULL || g_NetMode == NETMODE_CLIENT) {
		return 0;
	}

	pos.x = x;
	pos.y = y;
	pos.z = z;

	if (seedchr && seedchr->prop) {
		roomsCopy(seedchr->prop->rooms, rooms);
	} else {
		roomsCopy(g_Vars.currentplayer->prop->rooms, rooms);
	}
#if VERSION >= VERSION_NTSC_1_0
	floorroom = cdFindFloorRoomYColourFlagsAtPos(&pos, rooms, &floory, &floorcol, NULL);
#else
	floorroom = cdFindFloorRoomYColourFlagsAtPos(&pos, rooms, &floory, &floorcol);
#endif
	if (floorroom > 0) {
		pos.y = floory + 10.0f; // just above the floor so it settles, not clips
		rooms[0] = floorroom;
		rooms[1] = -1;
	}

	gset.weaponnum = WEAPON_GRENADE;
	gset.weaponfunc = FUNC_PRIMARY; // invfunc_grenade_throw
	gset.unk0639 = 0;
	gset.unk063a = 0;

	vel.x = 0.0f;
	vel.y = -1.0f; // drop straight down at the target
	vel.z = 0.0f;
	mtx4LoadIdentity(&mtx);

	obj = bgunCreateThrownProjectile2(g_Vars.currentplayer->prop->chr, &gset,
			&pos, rooms, &mtx, &vel);

	if (obj != NULL) {
		// the pin-pull ping the throw animation would have played
		// (gunscript_playsound(6, SFX_05C1) in invanim_grenade_throw). Played
		// flat from the player's perspective via sndStart — the gunscripts
		// route their sounds the same way, and the psCreate-on-the-projectile
		// attempt was inaudible in testing (fresh projectile props don't
		// register with propsnd this early).
		sndStart(var80095200, SFX_05C1, NULL, -1, -1, -1, -1, -1);
	}

	return obj ? 1 : 0;
}

// pd.door_traps(on): booby-trapped doors — any door that starts opening
// detonates (the doorSetMode hook in propobj.c). g_ChaosDoorOpenCount is
// bumped there unconditionally so pd.door_opens() can be a task sensor
// (EULA/CAPTCHA "open a door" requirements) even with traps off.
extern s32 g_ChaosDoorTraps;
extern u32 g_ChaosDoorOpenCount;
s32 chraiLuaDoorTraps(s32 on)
{
	if (apLuaPlayerChr() == NULL) {
		return 0;
	}
	g_ChaosDoorTraps = on ? 1 : 0;
	return 1;
}

u32 chraiLuaDoorOpens(void)
{
	return g_ChaosDoorOpenCount;
}

// pd.env_colours(skyr,skyg,skyb, cloudr,cloudg,cloudb): override the current
// stage environment's sky + cloud colours in place (Brandon's mod random
// bright skies). Restore = pd.env() — chraiLuaEnv(-1) re-applies the stage's
// authored environment wholesale, colours included.
s32 chraiLuaEnvColours(s32 sr, s32 sg, s32 sb, s32 cr, s32 cg, s32 cb)
{
	struct environment *env = envGetCurrent();

	if (apLuaPlayerChr() == NULL || env == NULL) {
		return 0;
	}
	env->sky_r = (u8)(sr < 0 ? 0 : sr > 255 ? 255 : sr);
	env->sky_g = (u8)(sg < 0 ? 0 : sg > 255 ? 255 : sg);
	env->sky_b = (u8)(sb < 0 ? 0 : sb > 255 ? 255 : sb);
	env->clouds_r = (cr < 0 ? 0 : cr > 255 ? 255 : cr) * (1.0f / 255.0f);
	env->clouds_g = (cg < 0 ? 0 : cg > 255 ? 255 : cg) * (1.0f / 255.0f);
	env->clouds_b = (cb < 0 ? 0 : cb > 255 ? 255 : cb) * (1.0f / 255.0f);
	return 1;
}

// pd.player_yaw(): the player's look yaw in degrees (0..360) — the EULA
// "spin around" task sensor.
f32 chraiLuaPlayerYaw(void)
{
	if (apLuaPlayerChr() == NULL) {
		return 0;
	}
	return g_Vars.currentplayer->vv_theta;
}

// pd.player_slip(push [, pitch_deg]): Banana peel — drop the player to full
// squat, shove them forward along their facing, and optionally snap the view
// pitch (vv_verta, +up, clamps at 90; pass > 180 / omit in Lua to leave the
// pitch alone — the effect glides it separately via pd.player_pitch). The
// shove rides bondshotspeed (the explosion-knockback vector bwalk integrates
// with collision + decay); world forward = (-vv_sintheta, 0, vv_costheta) —
// the bond2 look-vector convention (bondmove.c ~3138).
s32 chraiLuaPlayerSlip(f32 push, f32 pitchdeg)
{
	struct player *pl;

	if (apLuaPlayerChr() == NULL) {
		return 0;
	}
	pl = g_Vars.currentplayer;
	pl->crouchpos = CROUCHPOS_SQUAT;
	if (pitchdeg <= 180.0f) {
		if (pitchdeg > 90.0f) {
			pitchdeg = 90.0f;
		} else if (pitchdeg < -90.0f) {
			pitchdeg = -90.0f;
		}
		pl->vv_verta = pitchdeg;
	}
	pl->bondshotspeed.x += -pl->vv_sintheta * push;
	pl->bondshotspeed.z += pl->vv_costheta * push;
	return 1;
}

// pd.player_pitch([deg]): get (no arg) or set the player's view pitch in
// degrees (vv_verta, +up, clamped to the engine's +/-90).
f32 chraiLuaPlayerPitchGet(void)
{
	if (apLuaPlayerChr() == NULL) {
		return 0;
	}
	return g_Vars.currentplayer->vv_verta;
}

s32 chraiLuaPlayerPitchSet(f32 deg)
{
	if (apLuaPlayerChr() == NULL) {
		return 0;
	}
	if (deg > 90.0f) {
		deg = 90.0f;
	} else if (deg < -90.0f) {
		deg = -90.0f;
	}
	g_Vars.currentplayer->vv_verta = deg;
	return 1;
}

// pd.player_push(mag): shove the player along their facing (positive =
// forward, negative = backward). The banana-peel bondshotspeed knockback
// without the squat/pitch — Heavy Recoil fires it backward per shot.
s32 chraiLuaPlayerPush(f32 mag)
{
	struct player *pl;

	if (apLuaPlayerChr() == NULL) {
		return 0;
	}
	pl = g_Vars.currentplayer;
	pl->bondshotspeed.x += -pl->vv_sintheta * mag;
	pl->bondshotspeed.z += pl->vv_costheta * mag;
	return 1;
}

// pd.one_bullet(on): "One Bullet Mags" — clip capacity 1. The bake normally
// only runs at equip, so bgunChaosRebakeClipSizes applies it to the gun in
// hand immediately (excess loaded rounds refunded to reserve).
s32 chraiLuaOneBullet(s32 on)
{
	extern s32 g_ChaosOneBulletMags;

	g_ChaosOneBulletMags = on ? 1 : 0;
	if (apLuaPlayerChr() != NULL) {
		bgunChaosRebakeClipSizes();
	}
	return 1;
}

// pd.invert_look(on): flip vertical look — mouse+gyro dy and the pad right
// stick (input.c).
s32 chraiLuaInvertLook(s32 on)
{
	extern s32 g_ChaosInvertLook;

	g_ChaosInvertLook = on ? 1 : 0;
	return 1;
}

// pd.input_delay(frames): "Stadia Mode" — every pad read (buttons, kbm keys,
// sticks) is served N frames late (input.c ring). Mouse look stays live
// (honest limit — that getter runs multiple times per frame). 0 = off.
s32 chraiLuaInputDelay(s32 frames)
{
	extern void inputSetChaosInputDelay(s32 frames);

	inputSetChaosInputDelay(frames);
	return 1;
}

// pd.uwuify(on): every langGet string is uwuified (lang.c g_ChaosUwuMode;
// %-format specs preserved, long strings pass through).
s32 chraiLuaUwuify(s32 on)
{
	extern s32 g_ChaosUwuMode;

	g_ChaosUwuMode = on ? 1 : 0;
	return 1;
}

// pd.piglatin(on): igpay atinlay — same text pipeline as uwuify, mode 2.
// Turning either off zeroes the shared mode.
s32 chraiLuaPigLatin(s32 on)
{
	extern s32 g_ChaosUwuMode;

	g_ChaosUwuMode = on ? 2 : 0;
	return 1;
}

// pd.forced_march(on): the movement stick is pinned full forward
// (bondmove.c g_ChaosForcedMarch).
s32 chraiLuaForcedMarch(s32 on)
{
	extern s32 g_ChaosForcedMarch;

	g_ChaosForcedMarch = on ? 1 : 0;
	return 1;
}

// pd.forced_fire(on): "Itchy Trigger Finger" — the trigger is held down for
// you (bondmove.c movedata choke).
s32 chraiLuaForcedFire(s32 on)
{
	extern s32 g_ChaosForcedFire;

	g_ChaosForcedFire = on ? 1 : 0;
	return 1;
}

// pd.rapid_fire(on): "Trigger Happy" — while you hold fire, semi-autos fire as
// fast as automatics (bondmove.c g_ChaosRapidFire).
s32 chraiLuaRapidFire(s32 on)
{
	extern s32 g_ChaosRapidFire;

	g_ChaosRapidFire = on ? 1 : 0;
	return 1;
}

// pd.forced_crouch(on): "Permacrouch" — the stance is pinned to a crouch
// (bondmove.c g_ChaosForcedCrouch).
s32 chraiLuaForcedCrouch(s32 on)
{
	extern s32 g_ChaosForcedCrouch;

	g_ChaosForcedCrouch = on ? 1 : 0;
	return 1;
}

// pd.no_reload(on): "Reload Denied" — every reload transition is refused
// (bondgun.c g_ChaosNoReload).
s32 chraiLuaNoReload(s32 on)
{
	extern s32 g_ChaosNoReload;

	g_ChaosNoReload = on ? 1 : 0;
	return 1;
}

// pd.spread(mult): "Chaos Weapon Spread" — scale every weapon's shot spread +
// crosshair bloom (bondgun.c g_ChaosSpreadMult). 1 = normal; clamped sane.
s32 chraiLuaSpread(f32 mult)
{
	extern f32 g_ChaosSpreadMult;

	if (mult < 0.0f) mult = 0.0f;
	if (mult > 20.0f) mult = 20.0f;
	g_ChaosSpreadMult = mult;
	return 1;
}

// pd.chr_armor(chrnum, amount): "Armor Guard" — add body armor to an NPC by
// driving chr->damage negative via chrAddHealth (health beyond maxdamage is
// armor; a negative damage value is the engine's no-flinch body-armor state,
// see add_health_or_armor in commands.h). NPCs only; server-authoritative.
s32 chraiLuaChrArmor(s32 chrnum, f32 amount)
{
	struct chrdata *chr;

	if (g_NetMode == NETMODE_CLIENT) {
		return 0;
	}
	chr = (chrnum < 0) ? NULL : chrFindByLiteralId(chrnum);
	if (chr == NULL) {
		return 0;
	}
	if (chr->prop && chr->prop->type == PROPTYPE_PLAYER) {
		return 0; // never armor the player through this path
	}
	chrAddHealth(chr, amount);
	return 1;
}

// pd.headshots_only(on): "No Damage Except Headshots" — chrDamage zeroes every
// non-head hit on the local human (bondmove.c g_ChaosHeadshotsOnly).
s32 chraiLuaHeadshotsOnly(s32 on)
{
	extern s32 g_ChaosHeadshotsOnly;

	g_ChaosHeadshotsOnly = on ? 1 : 0;
	return 1;
}

// pd.trapdoor(): "Trapdoor" — open a hole under the local player for ~2s so the
// floor vanishes and they fall to their death (bondwalk.c g_ChaosTrapdoorTicks).
s32 chraiLuaTrapdoor(void)
{
	extern s32 g_ChaosTrapdoorTicks;

	if (g_NetMode == NETMODE_CLIENT) {
		return 0;
	}
	g_ChaosTrapdoorTicks = 120; // ~2s of no floor -> fall past the death plane
	return 1;
}

// pd.ice_floor(mult): "Ice Floor" — scale the walk accel/decel (bondwalk.c
// g_ChaosIceAccel). <1 = slow to start, slow to stop (slippery). 1 = normal.
s32 chraiLuaIceFloor(f32 mult)
{
	extern f32 g_ChaosIceAccel;

	if (mult < 0.02f) mult = 0.02f;
	if (mult > 4.0f) mult = 4.0f;
	g_ChaosIceAccel = mult;
	return 1;
}

// pd.player_movespeed(): the local player's current normalised move speed
// (0..~1), max of the forward/strafe components. Lets Ice Floor fire the banana
// peel when the player is sliding at top speed.
f32 chraiLuaPlayerMoveSpeed(void)
{
	f32 f, s;

	if (apLuaPlayerChr() == NULL) {
		return 0.0f;
	}
	f = g_Vars.currentplayer->speedforwards;
	s = g_Vars.currentplayer->speedstrafe;
	if (f < 0.0f) f = -f;
	if (s < 0.0f) s = -s;
	return (f > s) ? f : s;
}

// pd.drop_weapon(weaponnum): drop one of the player's weapons as a collectable
// floor pickup (weaponCreateForPlayerDrop = the engine's blessed objDrop +
// netSyncPropSpawn path) AND remove it from inventory so it isn't duplicated.
// Used by "Sonic Mode" to toss the arsenal so it can be re-collected.
s32 chraiLuaDropWeapon(s32 weaponnum)
{
	if (apLuaPlayerChr() == NULL || g_NetMode == NETMODE_CLIENT) {
		return 0;
	}
	weaponCreateForPlayerDrop(weaponnum);
	invRemoveItemByNum(weaponnum);
	if (bgunGetWeaponNum(HAND_RIGHT) == weaponnum) {
		bgunCycleBack();
	}
	return 1;
}

// pd.haunt(force): "Paranormal Activity" prop-throw — hurl up to a few pushable
// props that have a clear line of sight to the player AT the player (with a
// slight upward arc). Only the blessed objApplyMomentum mutation is used (it
// self-broadcasts SVC_PROP_MOVE and never touches the prop list), so it can't
// corrupt activeprops. Server-authoritative.
s32 chraiLuaHaunt(f32 force)
{
	struct prop *prop;
	struct prop *plprop;
	s32 thrown = 0;

	if (apLuaPlayerChr() == NULL || g_NetMode == NETMODE_CLIENT) {
		return 0;
	}
	plprop = g_Vars.currentplayer->prop;
	if (plprop == NULL) {
		return 0;
	}

	for (prop = g_Vars.activeprops; prop && thrown < 3; prop = prop->next) {
		if ((prop->type == PROPTYPE_OBJ || prop->type == PROPTYPE_WEAPON) && prop->obj) {
			struct defaultobj *obj = prop->obj;
			struct coord speed;
			f32 dx, dy, dz, dist;

			if ((obj->hidden & OBJHFLAG_MOUNTED) || (obj->hidden & OBJHFLAG_GRABBED)
					|| !(obj->flags3 & OBJFLAG3_PUSHABLE)) {
				continue;
			}
			dx = plprop->pos.x - prop->pos.x;
			dy = plprop->pos.y - prop->pos.y;
			dz = plprop->pos.z - prop->pos.z;
			dist = sqrtf(dx * dx + dy * dy + dz * dz);
			if (dist < 1.0f || dist > 2000.0f) {
				continue;
			}
			if (!cdTestLos05(&prop->pos, prop->rooms, &plprop->pos, plprop->rooms,
					CDTYPE_BG, GEOFLAG_BLOCK_SIGHT)) {
				continue;
			}
			speed.x = dx / dist * force * 0.05f;
			speed.y = dy / dist * force * 0.05f + force * 0.02f; // arc up a touch
			speed.z = dz / dist * force * 0.05f;
			objApplyMomentum(obj, &speed, 0.0f, false, true);
			thrown++;
		}
	}
	return thrown;
}

// pd.hud_off(on): "No HUD" — skip every HUD element render (the seven
// hudvd-wrapped sites in player.c/lv.c). Chaos overlays still draw.
s32 chraiLuaHudOff(s32 on)
{
	extern s32 g_ChaosHudOff;

	g_ChaosHudOff = on ? 1 : 0;
	return 1;
}

// pd.gun_fov(deg): "WAYTOODANK Viewmodel" — override the viewmodel Gun FOV
// (bondgun.c g_ChaosGunFovOverride); 0 restores the player's configured FOV.
s32 chraiLuaGunFov(f32 deg)
{
	extern f32 g_ChaosGunFovOverride;

	if (deg != 0.0f) {
		if (deg < 5.0f) {
			deg = 5.0f;
		} else if (deg > 165.0f) {
			deg = 165.0f;
		}
	}
	g_ChaosGunFovOverride = deg;
	return 1;
}

// pd.chr_freeze_one(chrnum | -1): "Weeping Skedar" — statue exactly one chr
// (chr.c anim gate + chrTickShoot fire gate); -1 unfreezes.
s32 chraiLuaChrFreezeOne(s32 chrnum)
{
	extern s32 g_ChaosFreezeChrnum;

	g_ChaosFreezeChrnum = chrnum;
	return 1;
}

// pd.buttsbot(on): text mode 3 — random-but-stable words become "butt".
// Shares the text-mode global with uwuify/piglatin (off zeroes it).
s32 chraiLuaButtsbot(s32 on)
{
	extern s32 g_ChaosUwuMode;

	g_ChaosUwuMode = on ? 3 : 0;
	return 1;
}

// pd.ipod_ad(on [, r, g, b]): "iPod Ad" silhouette — walls the given bright
// colour (default vivid green), chrs black, objects/weapons white, white
// wireframe edges (the fills are per-prop G_FLATFILL brackets in propRender +
// player.c; the enable + wall colour sync in bgTickPortals).
s32 chraiLuaIpodAd(s32 on, s32 r, s32 g, s32 b)
{
	extern s32 g_ChaosIpodAd;
	extern u8 g_ChaosIpodWall[3];

	g_ChaosIpodAd = on ? 1 : 0;
	if (on) {
		g_ChaosIpodWall[0] = (u8)(r < 0 ? 0 : r > 255 ? 255 : r);
		g_ChaosIpodWall[1] = (u8)(g < 0 ? 0 : g > 255 ? 255 : g);
		g_ChaosIpodWall[2] = (u8)(b < 0 ? 0 : b > 255 ? 255 : b);
	}
	return 1;
}

// pd.player_add_yaw(deg): rotate the local player's view yaw by deg degrees
// (Speen — spins the actual player: view, aim and movement heading). Additive
// with normal look input; wrapped 0..360 the same way bondwalk's rotate path
// does.
s32 chraiLuaPlayerAddYaw(f32 deg)
{
	f32 angle;

	if (apLuaPlayerChr() == NULL) {
		return 0;
	}
	angle = g_Vars.currentplayer->vv_theta + deg;
	while (angle < 0) {
		angle += 360;
	}
	while (angle >= 360) {
		angle -= 360;
	}
	g_Vars.currentplayer->vv_theta = angle;
	return 1;
}

// pd.player_crouch(): 0 stand / 1 duck / 2 squat — the "crouch for N
// seconds" task sensor. NOTE the engine's CROUCHPOS_* run the OTHER way
// (SQUAT=0, DUCK=1, STAND=2), so remap to the documented low-is-standing
// order — a raw return made the crouch task tick while STANDING.
s32 chraiLuaPlayerCrouch(void)
{
	if (apLuaPlayerChr() == NULL) {
		return 0;
	}
	return CROUCHPOS_STAND - g_Vars.currentplayer->crouchpos;
}

// pd.has_weapon(weaponnum): is the weapon in the player's inventory.
s32 chraiLuaHasWeapon(s32 weaponnum)
{
	if (apLuaPlayerChr() == NULL) {
		return 0;
	}
	return invHasSingleWeaponIncAllGuns(weaponnum) ? 1 : 0;
}

// pd.alarm(on): raise/clear the stage alarm (klaxon + every alarm-conditional
// AI script). Server-side only — on a netplay server the SVC_ALARM mirror
// (proto 85) carries the transition to clients.
s32 chraiLuaSetAlarm(s32 on)
{
	if (apLuaPlayerChr() == NULL || g_NetMode == NETMODE_CLIENT) {
		return 0;
	}
	if (on) {
		alarmActivate();
	} else {
		alarmDeactivate();
	}
	return 1;
}

// pd.boost(secs): grant `secs` seconds of the Combat Boost / Speed Pill
// (bgunAddBoost owns the want-flag, activation sting, and the lv.c zoom-blur
// ramp; bgunTickBoost decays the time and shuts it off — no cleanup needed).
// secs <= 0 cancels an active boost immediately.
s32 chraiLuaBoost(f32 secs)
{
	if (apLuaPlayerChr() == NULL) {
		return 0;
	}
	if (secs > 0.0f) {
		bgunAddBoost((s32)(secs * TICKS(60)));
	} else {
		g_Vars.speedpilltime = 0;
		g_Vars.speedpillwant = false;
	}
	return 1;
}

// pd.player_set_health(frac): set the player's health directly (0..1 of the
// bar). Floored just above zero — chaos scares, it doesn't execute; kills go
// through real damage paths so death bookkeeping stays consistent.
s32 chraiLuaPlayerSetHealth(f32 frac)
{
	if (apLuaPlayerChr() == NULL) {
		return 0;
	}
	if (frac < 0.01f) frac = 0.01f;
	if (frac > 1.0f) frac = 1.0f;
	g_Vars.currentplayer->bondhealth = frac;
	playerDisplayHealth(); // silent HP change — pop the health bar
	return 1;
}

// pd.show_health(): pop the health bar WITHOUT changing anything — lets an
// effect present the current value first, then animate to a new one on a
// later player_set_health (whose own playerDisplayHealth call re-enters as
// HEALTHSHOWMODE_UPDATING while the bar is still open).
s32 chraiLuaShowHealth(void)
{
	if (apLuaPlayerChr() == NULL) {
		return 0;
	}
	playerDisplayHealth();
	return 1;
}

// pd.dizzy(amount): apply the tranquiliser screen-sway to the local player
// (chr->blurdrugamount — the same accumulator tranq/psychosis rounds feed;
// decays naturally). Capped below the TICKS(5000) knockout band the drugged
// paths key on. amount is in blur units, ~2000-4000 is a solid wobble.
s32 chraiLuaDizzy(s32 amount)
{
	struct chrdata *chr = apLuaPlayerChr();

	if (chr == NULL) {
		return 0;
	}
	if (amount < 0) amount = 0;
	if (amount > 4000) amount = 4000;
	if (chr->blurdrugamount < TICKS(amount)) {
		chr->blurdrugamount = TICKS(amount);
	}
	return 1;
}

// pd.chr_cloak(chrnum, on): toggle a chr's cloak (CHRHFLAG_CLOAKED — the same
// bit the cloaking device sets; render + AI treat the chr as cloaked, IR
// scanner still reveals them).
s32 chraiLuaChrCloak(s32 chrnum, s32 on)
{
	struct chrdata *chr = chrFindByLiteralId(chrnum);

	if (apLuaPlayerChr() == NULL || chr == NULL || chr->prop == NULL) {
		return 0;
	}
	if (on) {
		chr->hidden |= CHRHFLAG_CLOAKED;
	} else {
		chr->hidden &= ~CHRHFLAG_CLOAKED;
	}
	return 1;
}

// pd.strip_ammo(): zero every ammo pool (the inverse of pd.refill_ammo).
// Weapons stay in the inventory — the chaos is the click, not the loss.
s32 chraiLuaStripAmmo(void)
{
	s32 type;

	if (apLuaPlayerChr() == NULL) {
		return 0;
	}
	for (type = 1; type <= AMMOTYPE_ECM_MINE; type++) {
		bgunSetAmmoQuantity(type, 0);
	}
	return 1;
}

// pd.set_ammo(ammotype, qty): set ONE ammo pool to an exact quantity,
// leaving every other pool alone (russian roulette wants exactly one
// Magnum round without confiscating the rest of the arsenal).
s32 chraiLuaSetAmmo(s32 ammotype, s32 qty)
{
	if (apLuaPlayerChr() == NULL) {
		return 0;
	}
	if (ammotype < 1 || ammotype > AMMOTYPE_ECM_MINE) {
		return 0;
	}
	if (qty < 0) {
		qty = 0;
	}
	bgunSetAmmoQuantity(ammotype, qty);
	return 1;
}

// pd.teleport_to_chr(chrnum): snap the local player to a chr's position.
// Server-side only (a client's move would just be force-corrected straight
// back). When the player carries a body model (Combat Sim / anti-reality),
// reuse chrSetPos — the same primitive the netcode uses to force-correct
// player pawns, so bondwalk state / camera / rooms all follow.
//
// In the solo campaign the first-person player has NO chr model (chr->model
// == NULL), and both chrSetPos and chrMoveToPos unconditionally dereference
// it (modelSetRootPosition / model->definition) — calling them there crashes.
// For that case relocate the prop directly: pos + room registration + the
// bondwalk view-height (vv_*) fields, which is all a model-less player needs.
s32 chraiLuaTeleportToChr(s32 chrnum)
{
	struct chrdata *pl = apLuaPlayerChr();
	struct chrdata *chr = (chrnum < 0) ? NULL : chrFindByLiteralId(chrnum);
	struct prop *plprop;
	struct coord target;
	RoomNum rooms[8];
	f32 dx, dz, len, dist, angle;
	f32 ground;

	if (pl == NULL || pl->prop == NULL || g_NetMode == NETMODE_CLIENT) {
		return 0;
	}
	if (chr == NULL || chr->prop == NULL || chr->prop == pl->prop) {
		return 0;
	}

	// Ideal landing spot: beside the target, not inside it — offset
	// horizontally by the two radii plus a margin, along the direction the
	// player is coming from (so they arrive on their own side, roughly facing
	// the target). Degenerate overlap falls back to a fixed direction.
	target = chr->prop->pos;
	dx = pl->prop->pos.x - chr->prop->pos.x;
	dz = pl->prop->pos.z - chr->prop->pos.z;
	len = sqrtf(dx * dx + dz * dz);
	dist = pl->radius + chr->radius + 30.0f;
	if (len > 0.001f) {
		target.x += dx / len * dist;
		target.z += dz / len * dist;
		angle = atan2f(dx, dz);
	} else {
		target.x += dist;
		angle = 0;
	}

	// Validate the spot against walls AND physics objects (tables, crates...):
	// chrAdjustPosForSpawn tests CDTYPE_ALL at the ideal point and, if it
	// collides, nudges through a ring of 8 directions looking for a clear one.
	// If nothing is safe it returns false — fall back to the target's own
	// position/rooms so the player at least lands in the same room as the NPC
	// rather than embedded in an object.
	roomsCopy(chr->prop->rooms, rooms);
#if VERSION >= VERSION_NTSC_1_0
	if (!chrAdjustPosForSpawn(pl->radius, &target, rooms, angle, true, false, false)) {
#else
	if (!chrAdjustPosForSpawn(pl->radius, &target, rooms, angle, true, false)) {
#endif
		target = chr->prop->pos;
		roomsCopy(chr->prop->rooms, rooms);
	}

	if (pl->model != NULL) {
		return chrSetPos(pl, &target, rooms, chrGetRotY(pl), true) ? 1 : 0;
	}

	// Model-less first-person player: move the prop by hand.
	plprop = pl->prop;
	propSetPerimEnabled(plprop, false);

	plprop->pos = target;

	ground = cdFindGroundInfoAtCyl(&plprop->pos, pl->radius, rooms,
			&pl->floorcol, &pl->floortype, NULL, &pl->floorroom, NULL, NULL);
	pl->ground = ground;
	pl->manground = ground;
	pl->sumground = ground * (PAL ? 8.4175090789795f : 9.999998f);

	propDeregisterRooms(plprop);
	roomsCopy(rooms, plprop->rooms);
	chr0f0220ac(pl);

	if (plprop->type == PROPTYPE_PLAYER) {
		struct player *player = g_Vars.players[playermgrGetPlayerNumByProp(plprop)];
		player->vv_manground = ground;
		player->vv_ground = ground;
		player->vv_verta = 0;
		player->unk1c64 = 1;
	}

	propSetPerimEnabled(plprop, true);
	return 1;
}

// pd.explosions_around(on): the Air Force One crash sequence — surround the
// local player with staggered random explosions (playerSurroundWithExplosions
// starts the bondexploding loop that playerTickExplode drives; off just
// clears the flag). Damage respects pd.invincible — the chr damage handler
// early-outs on player->invincible, but the explosions still spawn, so the
// self-destruct effect looks lethal without being lethal.
s32 chraiLuaPlayerExplosions(s32 on)
{
	if (apLuaPlayerChr() == NULL) {
		return 0;
	}
	if (on) {
		playerSurroundWithExplosions(0);
	} else {
		g_Vars.currentplayer->bondexploding = false;
	}
	return 1;
}

// pd.nbomb(): detonate an N-Bomb storm on the local player (nbombCreateStorm,
// the same call the thrown N-Bomb's impact makes). Owner is the player, so
// kills it causes are credited to them; the player is inside the storm and
// takes the full disorientation ride.
s32 chraiLuaNbomb(void)
{
	if (apLuaPlayerChr() == NULL || g_NetMode == NETMODE_CLIENT) {
		return 0;
	}
	nbombCreateStorm(&g_Vars.currentplayer->prop->pos, g_Vars.currentplayer->prop);
	return 1;
}

// pd.gust(force): shove the whole map in one random compass direction — every
// living chr (chrYeetFromPos from a virtual point behind them, so they all
// fly the same way), every pushable object (the explosion-knockback gate:
// !MOUNTED && !GRABBED && OBJFLAG3_PUSHABLE -> objApplyMomentum), and the
// local player (bondshotspeed, the shot-knockback velocity bondwalk decays).
s32 chraiLuaGust(f32 force)
{
	struct prop *prop;
	struct coord dir;
	f32 angle;

	if (apLuaPlayerChr() == NULL || g_NetMode == NETMODE_CLIENT) {
		return 0;
	}

	angle = RANDOMFRAC() * M_BADTAU;
	dir.x = sinf(angle);
	dir.y = 0;
	dir.z = cosf(angle);

	for (prop = g_Vars.activeprops; prop; prop = prop->next) {
		if (prop->type == PROPTYPE_CHR && prop->chr && prop->chr->model && !chrIsDead(prop->chr)) {
			struct coord from;
			from.x = prop->pos.x - dir.x * 100.0f;
			from.y = prop->pos.y;
			from.z = prop->pos.z - dir.z * 100.0f;
			chrYeetFromPos(prop->chr, &from, force);
		} else if ((prop->type == PROPTYPE_OBJ || prop->type == PROPTYPE_WEAPON) && prop->obj) {
			struct defaultobj *obj = prop->obj;

			if ((obj->hidden & OBJHFLAG_MOUNTED) == 0
					&& (obj->hidden & OBJHFLAG_GRABBED) == 0
					&& (obj->flags3 & OBJFLAG3_PUSHABLE)) {
				struct coord speed;
				speed.x = dir.x * force * 0.05f;
				speed.y = 0;
				speed.z = dir.z * force * 0.05f;
				objApplyMomentum(obj, &speed, 0.0f, true, true);
			}
		}
	}

	g_Vars.currentplayer->bondshotspeed.x += dir.x * force * 0.2f;
	g_Vars.currentplayer->bondshotspeed.z += dir.z * force * 0.2f;
	return 1;
}

// pd.dual_wield(weaponnum [, funcnum]): give and equip the weapon in BOTH
// hands (the playerSpawnAnti dual-wield recipe: single + double inventory
// entries, then equip each hand) with CHAOS_GUN_MAGS magazines. funcnum 0/1 forces
// fire function on both hands (1 = secondary, e.g. the Cyclone's Magazine
// Discharge); the player can still cycle functions manually afterwards.
s32 chraiLuaDualWield(s32 weaponnum, s32 funcnum)
{
	struct player *pl = g_Vars.currentplayer;

	if (apLuaPlayerChr() == NULL) {
		return 0;
	}
	if (weaponFindById(weaponnum) == NULL) {
		return 0;
	}

	invGiveSingleWeapon(weaponnum);
	invGiveDoubleWeapon(weaponnum, weaponnum);
	bgunEquipWeapon2(HAND_RIGHT, weaponnum);
	bgunEquipWeapon2(HAND_LEFT, weaponnum);
	chraiLuaGiveMags(CHAOS_GUN_MAGS);

	if (funcnum == FUNC_PRIMARY || funcnum == FUNC_SECONDARY) {
		pl->hands[HAND_RIGHT].gset.weaponfunc = funcnum;
		pl->hands[HAND_LEFT].gset.weaponfunc = funcnum;
	}
	return 1;
}

// pd.gun_lock(on): "Cyclone Frenzy" lock — while on, bmoveProcessInput forces
// the SECONDARY fire function on both hands, holds the trigger (auto-fire), and
// blocks weapon switching (cycle offsets + the amOpen weapon menu). Local player
// only; cleared in lvInit so a mid-effect stage change can't leave it stuck.
extern s32 g_ChaosGunLock;
s32 chraiLuaGunLock(s32 on)
{
	g_ChaosGunLock = on ? 1 : 0;
	return 1;
}

// pd.rubber_objects(on): "Rubber Objects" — items dropped into the world while
// this is on (corpse drops, disarms, the player's own drop, thrown grenades)
// bounce like rubber instead of settling after the vanilla 6 bounces. The mark
// is stamped in objSetDropped and read in projectileTick, both in propobj.c, so
// props already lying on the floor are untouched. Turning it off settles
// everything on its next contact; also cleared in lvResetChaosPerStage.
extern s32 g_ChaosRubberObjects;
s32 chraiLuaRubberObjects(s32 on)
{
	g_ChaosRubberObjects = on ? 1 : 0;
	return 1;
}

// pd.yassify(on): "Yassify" — non-uniform per-joint body shaping (cinched
// waist, broader shoulders, bigger head) applied in chrHandleJointPositioned
// (chr.c). Humans only, render-side only — no collision or hit boxes move, so
// it's purely cosmetic. Per-joint multipliers are tunable live with /yassify;
// cleared in lvResetChaosPerStage.
extern s32 g_ChaosYassify;
s32 chraiLuaYassify(s32 on)
{
	g_ChaosYassify = on ? 1 : 0;
	return 1;
}

// pd.mag_dump(on): "Mag Dump" — a single trigger press empties the whole clip.
// Automatic weapons get the trigger held; semi-autos get it rapidly pulsed. The
// per-tick driving + auto/semi detection lives in bmoveProcessInput. Local player
// only; both g_ChaosMagDump and the g_ChaosMagDumpArmed latch clear in lvInit.
extern s32 g_ChaosMagDump;
extern s32 g_ChaosMagDumpArmed;
s32 chraiLuaMagDump(s32 on)
{
	g_ChaosMagDump = on ? 1 : 0;
	if (!g_ChaosMagDump) {
		g_ChaosMagDumpArmed = 0;
	}
	return 1;
}

// pd.knife_lock(on): "Knife fight" lock — block weapon switching (cycle offsets
// + the amOpen weapon menu) so only the equipped knife can be used, but leave
// firing/functions normal. Local player only; cleared in lvInit.
extern s32 g_ChaosKnifeLock;
s32 chraiLuaKnifeLock(s32 on)
{
	g_ChaosKnifeLock = on ? 1 : 0;
	return 1;
}

// pd.cloak_lock(on): make the player's cloak unbreakable — firing doesn't
// drop it and it needs no cloak ammo (chr.c chrUncloakTemporarily +
// chrUpdateCloak gates). Backs "Now you see me..." paired with
// pd.device_on(WEAPON_CLOAKINGDEVICE).
s32 chraiLuaCloakLock(s32 on)
{
	extern s32 g_ChaosCloakLock;
	g_ChaosCloakLock = on ? 1 : 0;
	return 1;
}

// pd.song(slot [, frac]) / pd.song(): play an unlocked Combat Sim music track
// over the stage music (musicStartTrackAsMenu — the credits-roll mechanism;
// the stage music pauses underneath and resumes when the menu track ends).
// slot is wrapped into the unlocked-track range; no arg / negative stops the
// song. frac > 0 starts the song that far into its length (the seqPlay seek
// latch, snd.c seqSeekToFrac) — only armed when the start will actually be
// queued (not suppressed, not the already-playing menu track), so a stale
// latch can't hijack a later pause-menu track start.
s32 chraiLuaPlaySong(s32 slot, f32 frac)
{
	extern s32 g_MusicSuppressed;
	extern s32 g_MenuTrack;
	s32 numtracks;
	s32 tracknum;

	if (slot < 0) {
		musicEndMenu();
		return 1;
	}
	numtracks = mpGetNumUnlockedTracks();
	if (numtracks <= 0) {
		return 0;
	}
	tracknum = mpGetTrackMusicNum(slot % numtracks);
	if (frac > 0.0f && !g_MusicSuppressed && tracknum != g_MenuTrack) {
		if (frac > 0.95f) {
			frac = 0.95f;
		}
		seqSetNextSeek(tracknum, frac);
	}
	musicStartTrackAsMenu(tracknum);
	return 1;
}

// pd.stage_music(on): stop (on=false) or restart (on=true) the CURRENT stage's
// music. Unlike pd.song, which layers a menu track over the paused stage music,
// this genuinely silences the level track — for the Silo Countdown effect, which
// plays its own external track (pd.play_file) with the mission music killed
// underneath. Restart re-derives primary + ambient from the live stage number.
s32 chraiLuaStageMusic(s32 on)
{
	extern s32 g_MusicSuppressed;
	if (on) {
		// Clear the suppress latch BEFORE restarting so the start paths aren't
		// blocked by their own guard.
		g_MusicSuppressed = 0;
		musicSetStageAndStartMusic(g_Vars.stagenum);
	} else {
		// Latch music off, then stop what's playing. The latch blocks every
		// restart path (musicEndMenu on pause-menu close, NRG combat re-trigger,
		// ambient) for the whole effect, so the game music can't creep back.
		g_MusicSuppressed = 1;
		musicStop();
	}
	return 1;
}

// pd.music_bpm(): tempo of the current sequenced music track in beats/min, or 0
// if no sequenced track is playing (e.g. the menu, or a pd.play_file MP3, which
// carries no tempo). Backs the "beat game" chaos effect.
f32 chraiLuaMusicBpm(void)
{
	extern s32 sndGetMusicBeat(f32 *bpm, f32 *phase);
	f32 bpm = 0.0f;
	sndGetMusicBeat(&bpm, NULL);
	return bpm;
}

// pd.music_beat(): position within the current musical beat as a fraction
// [0, 1) (0 = on the beat), or -1 if no sequenced track is playing.
f32 chraiLuaMusicBeat(void)
{
	extern s32 sndGetMusicBeat(f32 *bpm, f32 *phase);
	f32 phase = -1.0f;
	sndGetMusicBeat(NULL, &phase);
	return phase;
}

// pd.aim_chr(): the chrnum the local player is currently aiming at (the
// autoaim/crosshair target via propFindAimingAt), or -1 if none. Lets the beat
// game reward an on-beat shot by dealing bonus damage to that chr.
s32 chraiLuaAimChr(void)
{
	struct prop *prop;

	if (apLuaPlayerChr() == NULL) {
		return -1;
	}
	prop = propFindAimingAt(HAND_RIGHT, false, FINDPROPCONTEXT_QUERY);
	if (prop && prop->type == PROPTYPE_CHR && prop->chr) {
		return (s32)prop->chr->chrnum;
	}
	return -1;
}

// Chaos Evil-twin/clone registry: chrnums of live twins, used to make them
// psychosis-immune (a twin damaged by the player must stay hostile, not flip to
// the psychosised "friendly" AI list — see chaosIsTwin's use in chraiLuaChrDamage).
s16 g_ChaosTwinChrnums[8];

// True if chr is a chaos evil-twin/clone (used to make them psychosis-immune).
bool chaosIsTwin(struct chrdata *chr)
{
	s32 i;

	if (chr == NULL) {
		return false;
	}
	for (i = 0; i < (s32)ARRAYCOUNT(g_ChaosTwinChrnums); i++) {
		if (g_ChaosTwinChrnums[i] == chr->chrnum) {
			return true;
		}
	}
	return false;
}

// pd.spawn_body(bodynum, weaponnum, dx, dz): spawn a HOSTILE chr of the given
// body at the player's position plus a horizontal offset, facing the player,
// already alerted. The chraiLuaSpawnAlly recipe with the allegiance inverted;
// weaponnum -1 spawns unarmed (melee bodies like the mini Skedar claw).
s32 chraiLuaSpawnBody(s32 bodynum, s32 weaponnum, f32 dx, f32 dz, s32 sunglasses)
{
	struct prop *prop;
	struct chrdata *chr;
	struct coord pos;
	RoomNum spawnrooms[8];
	f32 floory;
	u16 floorcol;
	s32 floorroom;
	u32 spawnflags = SPAWNFLAG_ALLOWONSCREEN;
	s32 headnum = -1; // >= 0 overrides bodyChooseHead (a twin uses Jo's own head)

	// pd.spawn_body(..., sunglasses=true): the head model gets its shades
	// variant (SPAWNFLAG_FORCESUNGLASSES — the setup-file guard mechanism).
	if (sunglasses) {
		spawnflags |= SPAWNFLAG_FORCESUNGLASSES;
	}

	if (apLuaPlayerChr() == NULL || g_NetMode == NETMODE_CLIENT) {
		return -1;
	}

	// bodynum -1 = "a copy of the player" (the evil-twin effect): use the
	// player's own body AND head so she actually looks like Jo. (The head was a
	// red herring earlier — the real "goes friendly" bug was the psychosis flag.)
	if (bodynum < 0) {
		bodynum = g_Vars.currentplayer->prop->chr->bodynum;
		headnum = g_Vars.currentplayer->prop->chr->headnum;
	}

	// Force-load the body's model file if it isn't resident on this stage.
	// The spawn path only loads on demand and bails silently if the file
	// isn't in memory — which is why the full-size Skedar (BODY_SKEDAR /
	// FILE_CSKEDAR) never appeared on non-Skedar stages. bodyLoad is a no-op
	// if already loaded; a genuinely absent file still bails downstream.
	if (bodynum >= 0 && bodynum < 152) {
		bodyLoad(bodynum);
	}

	pos.x = g_Vars.currentplayer->prop->pos.x + dx;
	pos.y = g_Vars.currentplayer->prop->pos.y;
	pos.z = g_Vars.currentplayer->prop->pos.z + dz;

	// Floor-snap the spawn: portal-walk from the player's rooms to the real
	// floor room + height at the (possibly far, e.g. 1000-unit) target, so she
	// lands on the ground in the correct room instead of floating or clipping.
	roomsCopy(g_Vars.currentplayer->prop->rooms, spawnrooms);
#if VERSION >= VERSION_NTSC_1_0
	floorroom = cdFindFloorRoomYColourFlagsAtPos(&pos, spawnrooms, &floory, &floorcol, NULL);
#else
	floorroom = cdFindFloorRoomYColourFlagsAtPos(&pos, spawnrooms, &floory, &floorcol);
#endif
	if (floorroom > 0) {
		pos.y = floory;
		spawnrooms[0] = floorroom;
		spawnrooms[1] = -1;
	}

	// GAILIST_ALERTED is a real combat list (chase + shoot). The twin is kept
	// locked onto the PLAYER — and hostile — every frame by chaosTwinsHuntPlayer
	// (propsTick), which overrides the AI/team drift that otherwise let her
	// re-pick the nearest guard or turn into a passive blue-reticle buddy.
	// Pass headnum through raw: chrSpawnAtCoord picks a head for headed
	// bodies and (port fix) leaves built-in-head bodies (skedar etc.)
	// headless instead of wedging a human head on — that made
	// pd.spawn_body(SKEDARKING/MINISKEDAR) fail outright.
	prop = chrSpawnAtCoord(bodynum, headnum, &pos,
			spawnrooms,
			atan2f(-dx, -dz), // face inward toward the player
			ailistFindById(GAILIST_ALERTED),
			spawnflags);

	if (prop == NULL || prop->chr == NULL) {
		return -1;
	}

	chr = prop->chr;
	chr->flags |= CHRFLAG0_SKIPSAFETYCHECKS;
	// Put her on the GUARDS' side so the PLAYER is her only enemy. Then her own
	// AI acquires the player through normal sight/detection and fights properly
	// (forcing chr->target instead just made her replay the "spotted you"
	// reaction forever without firing — the combat AI rejects a target it did
	// not acquire itself). Copy the team of an existing chr that is hostile to
	// the player: that team makes the player an enemy AND the guards allies.
	// Skip TEAM_ALLY (0x10) — that team is the follow-Bond buddy. If nothing
	// suitable is found, fall back to a hostile team the player doesn't share.
	{
		struct chrdata *plchr = g_Vars.currentplayer->prop->chr;
		u8 twinteam = 0;
		s32 numslots = chrsGetNumSlots();
		s32 si;

		for (si = 0; si < numslots; si++) {
			s32 cn = (s32)g_ChrSlots[si].chrnum;
			struct chrdata *other = (cn < 0) ? NULL : chrFindByLiteralId(cn);

			if (other != NULL && other != plchr && other != chr && other->prop != NULL
					&& other->prop->type == PROPTYPE_CHR && other->team != 0
					&& other->team != TEAM_ALLY && !chrIsDead(other)
					&& chrCompareTeams(other, plchr, COMPARE_ENEMIES)) {
				twinteam = other->team;
				break;
			}
		}

		if (twinteam == 0) {
			u8 plteam = plchr->team;
			twinteam = TEAM_ENEMY;
			if (twinteam & plteam) twinteam = TEAM_01;
			if (twinteam & plteam) twinteam = TEAM_04;
			if (twinteam & plteam) twinteam = TEAM_20;
		}
		chr->team = twinteam;
	}
	chr->voicebox = VOICEBOX_FEMALE; // Jo twin
	chr->squadron = SQUADRON_01;
	// NB: do NOT set CHRHFLAG_DETECTED here — that bit (0x80000000) means
	// "detected" only on a PLAYER; on an AI chr the very same bit is
	// CHRHFLAG_PSYCHOSISED, so setting it made the twin psychotic at spawn
	// (GAILIST_ALERTED -> GAILIST_INIT_PSYCHOSIS, which rewrites her team to
	// TEAM_NONCOMBAT then TEAM_ALLY = friendly). This was THE "goes friendly" bug.
	chr->teamscandist = 50;
	chr->accuracyrating = 100;
	chr->speedrating = 100;
	chrAddHealth(chr, 20);
	chrSetMaxDamage(chr, 4);
	chr->chrflags |= CHRCFLAG_NEVERSLEEP;

	if (weaponnum >= 0) {
		s32 modelnum = playermgrGetModelOfWeapon(weaponnum);
		if (modelnum >= 0) {
			chrGiveWeapon(chr, modelnum, weaponnum, 0);
		}
	}

	// Point her at the player and alert her so she engages immediately; with the
	// player as her only enemy (guard team above) she stays on him and fires.
	chr->target = propGetIndexByChrId(chr, CHR_BOND);
	chr->alertness = 100;
	chr->chrflags |= CHRCFLAG_TRIGGERSHOTLIST;

	// Register the twin so chaosIsTwin() keeps her psychosis-immune when the
	// player damages her (otherwise she flips to the friendly psychosised AI).
	{
		s32 ti;
		for (ti = 0; ti < (s32)ARRAYCOUNT(g_ChaosTwinChrnums); ti++) {
			if (g_ChaosTwinChrnums[ti] < 0) {
				g_ChaosTwinChrnums[ti] = chr->chrnum;
				break;
			}
		}
	}

	return chr->chrnum;
}

// Body-snatch "home" — the player's position before the snatch, so the timed
// effect can teleport them back when it ends.
static struct coord g_ChaosSnatchHome;
static RoomNum g_ChaosSnatchHomeRooms[8];
// Non-static so lvResetChaosPerStage() can clear it on stage load (lvInit runs
// only once at boot in this port) — a mid-snatch stage change
// tears down the Lua state without calling stop(), and a stale "active" flag
// would keep guards from ever firing (and break real disguise missions).
s32 g_ChaosSnatchActive = 0;

// Warp the local player to pos/rooms. The solo first-person player has no chr
// model, so move the prop by hand (the chraiLuaTeleportToChr model-less path);
// a body-model player (Combat Sim) uses chrSetPos.
static void chaosPlayerWarp(struct coord *pos, RoomNum *rooms)
{
	struct chrdata *pl = g_Vars.currentplayer->prop->chr;
	struct prop *plprop = g_Vars.currentplayer->prop;
	RoomNum tmp[8];
	f32 ground;

	if (pl == NULL) {
		return;
	}

	roomsCopy(rooms, tmp);

	if (pl->model != NULL) {
		chrSetPos(pl, pos, tmp, chrGetRotY(pl), true);
		return;
	}

	propSetPerimEnabled(plprop, false);
	plprop->pos = *pos;
	ground = cdFindGroundInfoAtCyl(&plprop->pos, pl->radius, tmp,
			&pl->floorcol, &pl->floortype, NULL, &pl->floorroom, NULL, NULL);
	pl->ground = ground;
	pl->manground = ground;
	pl->sumground = ground * (PAL ? 8.4175090789795f : 9.999998f);
	propDeregisterRooms(plprop);
	roomsCopy(tmp, plprop->rooms);
	chr0f0220ac(pl);
	if (plprop->type == PROPTYPE_PLAYER) {
		struct player *player = g_Vars.players[playermgrGetPlayerNumByProp(plprop)];
		player->vv_manground = ground;
		player->vv_ground = ground;
		player->vv_verta = 0;
		player->unk1c64 = 1;
	}
	propSetPerimEnabled(plprop, true);
}

// pd.body_snatch(chrnum): "lite" Counter-Op takeover for the solo campaign.
// The full playerSpawnAnti takeover needs the player to have a third-person chr
// body model, which the first-person solo player lacks and can't build cleanly
// mid-mission (the gunmem body-swap is cutscene-only machinery and stalls in
// active gameplay). So instead of becoming the guard's model we take its PLACE:
// remember where the player was (for pd.body_unsnatch), give the player the
// guard's weapon, warp onto it, mark the player disguised, and remove the guard.
// The chaos.lua effect also calms every chr so nobody aggros, and pd.body_unsnatch
// (called when the timer ends) un-disguises and teleports the player home. Solo
// only. Returns 1 on success, 0 if the target can't be snatched.
s32 chraiLuaBodySnatch(s32 chrnum)
{
	struct chrdata *chr;
	struct chrdata *pl;
	struct prop *gunprop;
	s32 weaponnum;

	if (apLuaPlayerChr() == NULL || g_NetMode != NETMODE_NONE || g_Vars.normmplayerisrunning) {
		return 0;
	}
	chr = (chrnum < 0) ? NULL : chrFindByLiteralId(chrnum);
	if (chr == NULL || chr->prop == NULL || chr->prop->type != PROPTYPE_CHR
			|| chr->model == NULL || chrIsDead(chr)
			|| chr->prop == g_Vars.currentplayer->prop) {
		return 0;
	}

	pl = g_Vars.currentplayer->prop->chr;

	// Remember the pre-snatch position so the timed effect can return here.
	g_ChaosSnatchHome = g_Vars.currentplayer->prop->pos;
	roomsCopy(g_Vars.currentplayer->prop->rooms, g_ChaosSnatchHomeRooms);
	g_ChaosSnatchActive = 1;

	// Take the guard's weapon.
	gunprop = chrGetHeldProp(chr, HAND_RIGHT);
	weaponnum = (gunprop && gunprop->weapon) ? gunprop->weapon->weaponnum : WEAPON_UNARMED;
	if (weaponnum > WEAPON_UNARMED) {
		invGiveSingleWeapon(weaponnum);
		bgunEquipWeapon2(HAND_RIGHT, weaponnum);
		chraiLuaGiveMags(CHAOS_GUN_MAGS);
	}

	// Warp onto the guard.
	chaosPlayerWarp(&chr->prop->pos, chr->prop->rooms);

	// Disguise: the target-selection search (chraicommands.c) skips chrs with
	// CHRHFLAG_DISGUISED, so guards won't acquire the player. Also set the
	// struct-player flag the outfit code reads. (chaos.lua calms everyone too.)
	if (pl != NULL) {
		pl->hidden |= CHRHFLAG_DISGUISED;
	}
	g_Vars.currentplayer->disguised = true;

	// Remove the snatched guard — the playerSpawnAnti host-teardown sequence.
	chrRemove(chr->prop, true);
	propDeregisterRooms(chr->prop);
	propDelist(chr->prop);
	propDisable(chr->prop);
	propFree(chr->prop);
	return 1;
}

// pd.body_unsnatch(): end the snatch — drop the disguise so guards behave
// normally again, and teleport the player back to where they were. Called from
// the body_snatch effect's stop() when the timer ends.
s32 chraiLuaBodyUnsnatch(void)
{
	struct chrdata *pl;

	if (apLuaPlayerChr() == NULL) {
		return 0;
	}
	pl = g_Vars.currentplayer->prop->chr;
	if (pl != NULL) {
		pl->hidden &= ~CHRHFLAG_DISGUISED;
	}
	g_Vars.currentplayer->disguised = false;

	if (g_ChaosSnatchActive) {
		chaosPlayerWarp(&g_ChaosSnatchHome, g_ChaosSnatchHomeRooms);
		g_ChaosSnatchActive = 0;
	}
	return 1;
}

// pd.mark_home() / pd.warp_home(): record the local player's current position
// (chaos.lua calls it once on stage entry = the mission start pad) and later
// teleport back to it via the model-less-safe chaosPlayerWarp. The valid flag
// is non-static and cleared in lvReset so a stale home from a previous stage
// can never be warped to.
static struct coord g_ChaosLuaHome;
static RoomNum g_ChaosLuaHomeRooms[8];
s32 g_ChaosLuaHomeValid = 0;

s32 chraiLuaMarkHome(void)
{
	if (g_Vars.currentplayer == NULL || g_Vars.currentplayer->prop == NULL) {
		return 0;
	}
	g_ChaosLuaHome = g_Vars.currentplayer->prop->pos;
	roomsCopy(g_Vars.currentplayer->prop->rooms, g_ChaosLuaHomeRooms);
	g_ChaosLuaHomeValid = 1;
	return 1;
}

s32 chraiLuaWarpHome(void)
{
	if (!g_ChaosLuaHomeValid
			|| g_Vars.currentplayer == NULL || g_Vars.currentplayer->prop == NULL) {
		return 0;
	}
	chaosPlayerWarp(&g_ChaosLuaHome, g_ChaosLuaHomeRooms);
	return 1;
}

// pd.env(stagenum): apply another stage's sky/fog/cloud environment (Brandon's
// mod). pd.env() / stagenum -1 restores the current stage's own environment.
s32 chraiLuaEnv(s32 stagenum)
{
	envChooseAndApply(stagenum >= 0 ? stagenum : chraiLuaGetStageNum(), false);
	return 1;
}

// pd.fog(fogmin, fogmax, r, g, b): overlay a custom fog on the current stage
// (env.c envChaosFog — works on no-fog stages too). fogmin/fogmax are
// per-mille of the z-range (stock stages sit around 950..1050; lower = the
// wall starts closer). pd.fog() restores via envChooseAndApply.
#ifndef PLATFORM_N64
extern void envChaosFog(s32 stagenum, s32 fogmin, s32 fogmax, u8 r, u8 g, u8 b);
#endif
s32 chraiLuaFog(s32 fogmin, s32 fogmax, s32 r, s32 g, s32 b)
{
#ifndef PLATFORM_N64
	envChaosFog(chraiLuaGetStageNum(), fogmin, fogmax, (u8)r, (u8)g, (u8)b);
	return 1;
#else
	return 0;
#endif
}

// pd.blood_colour(r, g, b): every chr bleeds this colour (chr.c
// chrGetBloodColour override — sparks, hit splats and floor drips all derive
// from it). pd.blood_colour() restores per-body colours.
extern u32 g_ChaosBloodColour;
s32 chraiLuaBloodColour(s32 r, s32 g, s32 b, s32 on)
{
	if (!on) {
		g_ChaosBloodColour = 0;
	} else {
		// low bit doubles as the "set" flag so pure black still registers
		g_ChaosBloodColour = ((u32)(r & 0xff) << 24) | ((u32)(g & 0xff) << 16)
				| ((u32)(b & 0xff) << 8) | 1;
	}
	return 1;
}

// pd.max_blood(on): every hit splatters (no 1-in-3 dry hits, triple spark
// spray) and wounded chrs drip at the maximum rate (splat.c / chr.c).
extern s32 g_ChaosMaxBlood;
s32 chraiLuaMaxBlood(s32 on)
{
	g_ChaosMaxBlood = on ? 1 : 0;
	return 1;
}

// pd.items_shuffle(): every weapon pickup lying on the ground trades places
// with another (propobj.c chaosItemsShuffle). Returns how many moved.
#ifndef PLATFORM_N64
extern s32 chaosItemsShuffle(void);
#endif
s32 chraiLuaItemsShuffle(void)
{
#ifndef PLATFORM_N64
	return chaosItemsShuffle();
#else
	return 0;
#endif
}

// pd.chr_wireframe(on): hostile chrs render as polygon outlines (prop.c
// G_CHRWIREFRAME_EXT bracket around chrRender; both fast3d backends).
extern s32 g_ChaosWireframeChrs;
s32 chraiLuaChrWireframe(s32 on)
{
	g_ChaosWireframeChrs = on ? 1 : 0;
	return 1;
}

// pd.double_shots(on): every fire event takes twice the shots (bondgun.c
// shotstotake doubling — with dual-wield that's the "Quad handed" bit).
extern s32 g_ChaosDoubleShots;
s32 chraiLuaDoubleShots(s32 on)
{
	g_ChaosDoubleShots = on ? 1 : 0;
	return 1;
}

// pd.quad_top(on): render a second pair of viewmodel guns hanging upside-down
// from the top of the screen (bgunRender 180-degree-rotated projection pass).
extern s32 g_ChaosQuadTopGuns;
s32 chraiLuaQuadTop(s32 on)
{
	g_ChaosQuadTopGuns = on ? 1 : 0;
	// clip capacity bakes at equip — re-run it now so the 2x applies to the
	// gun already in hand (same fix as one_bullet)
	if (apLuaPlayerChr() != NULL) {
		bgunChaosRebakeClipSizes();
	}
	return 1;
}

// pd.buttons() / pd.buttons_pressed(): the local player's RAW pad buttons
// (held / newly-pressed this frame). Reads the joy layer directly, so it sees
// buttons even while pd.button_block hides them from gameplay — the popup
// framework (quiz/EULA) blocks FIRE from shooting but still reads the answer.
u32 chraiLuaButtons(s32 pressed)
{
	s32 contpad;

	if (g_Vars.currentplayer == NULL || g_Vars.currentplayerstats == NULL) {
		return 0;
	}
	contpad = optionsGetContpadNum1(g_Vars.currentplayerstats->mpindex);
	if (pressed) {
		return joyGetButtonsPressedThisFrame(contpad, 0xffffffff);
	}
	return joyGetButtons(contpad, 0xffffffff);
}

// pd.chr_target(chrnum, victimchrnum): point a chr's combat AI at another chr
// (the aiSetTargetChr recipe: target index + the trigger-shot flag + full
// alertness). Backs the "Civil war" infighting effect.
s32 chraiLuaChrTarget(s32 chrnum, s32 victimchrnum)
{
	struct chrdata *chr = chrFindByLiteralId(chrnum);
	struct chrdata *victim = chrFindByLiteralId(victimchrnum);

	if (g_NetMode == NETMODE_CLIENT || chr == NULL || victim == NULL
			|| victim->prop == NULL || chr == victim || chrIsDead(chr) || chrIsDead(victim)) {
		return 0;
	}
	chr->target = propGetIndexByChrId(chr, victim->chrnum);
	chr->alertness = 100;
	chr->chrflags |= CHRCFLAG_TRIGGERSHOTLIST;
	return 1;
}

// pd.chr_calm(chrnum): the neuralyzer — drop a chr's alertness to zero, clear
// its target and the trigger-shot flag. The chr doesn't rewind to its patrol
// script, but it stops hunting until re-provoked.
s32 chraiLuaChrCalm(s32 chrnum)
{
	struct chrdata *chr = chrFindByLiteralId(chrnum);

	if (g_NetMode == NETMODE_CLIENT || chr == NULL || chrIsDead(chr)) {
		return 0;
	}
	chr->alertness = 0;
	chr->target = -1;
	chr->chrflags &= ~CHRCFLAG_TRIGGERSHOTLIST;
	return 1;
}

// pd.civil_war(on): "Civil war" — turn the NPCs on each other for the effect's
// duration. Just forcing a target isn't enough: guards are the same team, so the
// AI drops a same-team target and re-locks onto the player. So we split them into
// mutually-hostile teams (distinct single team bits, cycled through the 8) — the
// AI's own nearest-enemy search then fires on other guards — and additionally
// point each one at its nearest living neighbour with full alertness + the
// trigger-shot flag. The Lua effect re-asserts this every ~0.5s so they keep
// hunting as they move and die. on=false restores the original teams and calms
// everyone. Solo/server only. First activation snapshots teams; re-activations
// (on=true while already active) just re-target.
#define CHAOS_CIVILWAR_MAX 256
static s16 g_ChaosCivilWarChr[CHAOS_CIVILWAR_MAX];
static u8  g_ChaosCivilWarTeam[CHAOS_CIVILWAR_MAX];
// Non-static so lvResetChaosPerStage() can clear it on stage load (lvInit runs
// only once at boot in this port) — a mid-effect stage change
// tears down the Lua state without calling stop(), and a stale count would let a
// later restore write teams onto the wrong (recycled-chrnum) chrs.
s32 g_ChaosCivilWarCount = 0;

s32 chraiLuaCivilWar(s32 on)
{
	struct chrdata *pl = apLuaPlayerChr();
	s32 n, i;

	if (pl == NULL || g_NetMode == NETMODE_CLIENT) {
		return 0;
	}

	if (!on) {
		for (i = 0; i < g_ChaosCivilWarCount; i++) {
			struct chrdata *chr = chrFindByLiteralId(g_ChaosCivilWarChr[i]);
			if (chr != NULL) {
				chr->team = g_ChaosCivilWarTeam[i];
				chr->chrflags &= ~CHRCFLAG_TRIGGERSHOTLIST;
			}
		}
		g_ChaosCivilWarCount = 0;
		return 1;
	}

	// Turn ONLY hostile combat guards on each other. Two hard rules learned the
	// hard way: (1) never touch non-combatants / mission NPCs — forcing team +
	// alert onto them breaks their scripted action block irreversibly and
	// softlocks the mission; so skip TEAM_NONCOMBAT and anything that isn't an
	// enemy of the player. (2) Split the guards into distinct team bits so each
	// registers the others as enemies, but ONLY bits 0-6 — bit 7 (0x80) IS
	// TEAM_NONCOMBAT and would turn a guard into a passive non-combatant. Then
	// kick CHRCFLAG_TRIGGERSHOTLIST (guards have no mission script to break) so
	// idle ones scan; their own AI acquires + engages the nearest enemy guard.
	n = chraiLuaGetChrSlotCount();

	for (i = 0; i < n && g_ChaosCivilWarCount < CHAOS_CIVILWAR_MAX; i++) {
		s32 cn = chraiLuaGetChrNumBySlot(i);
		struct chrdata *chr = (cn < 0) ? NULL : chrFindByLiteralId(cn);
		s32 k;
		bool known = false;

		if (chr == NULL || chr->prop == NULL || chr->prop->type != PROPTYPE_CHR
				|| chr->model == NULL || chrIsDead(chr) || chr == pl) {
			continue;
		}
		// Combat guards only — never non-combatants / allies / mission NPCs.
		if ((chr->team & TEAM_NONCOMBAT) || !chrCompareTeams(pl, chr, COMPARE_ENEMIES)) {
			continue;
		}
		for (k = 0; k < g_ChaosCivilWarCount; k++) {
			if (g_ChaosCivilWarChr[k] == chr->chrnum) {
				known = true;
				break;
			}
		}
		if (!known) {
			g_ChaosCivilWarChr[g_ChaosCivilWarCount] = chr->chrnum;
			g_ChaosCivilWarTeam[g_ChaosCivilWarCount] = chr->team;
			chr->team = (u8)(1 << (g_ChaosCivilWarCount % 7)); // bits 0-6 only
			g_ChaosCivilWarCount++;
		}

		chr->chrflags |= CHRCFLAG_TRIGGERSHOTLIST;
	}

	return g_ChaosCivilWarCount > 0;
}

// pd.doors_all(open): request every door on the stage to open (1) or close
// (0) — doorsRequestMode, the same call the AI door commands use. Closing is
// transient (walking up re-triggers them); opening everything at once is the
// tactical chaos.
s32 chraiLuaDoorsAll(s32 open)
{
	struct prop *prop;
	s32 n = 0;

	if (apLuaPlayerChr() == NULL || g_NetMode == NETMODE_CLIENT) {
		return 0;
	}
	for (prop = g_Vars.activeprops; prop; prop = prop->next) {
		if (prop->type == PROPTYPE_DOOR && prop->door) {
			doorsRequestMode(prop->door, open ? DOORMODE_OPENING : DOORMODE_CLOSING);
			n++;
		}
	}
	return n;
}

// pd.doors_lock(on): "Lockdown" — actually lock every door shut, not just the
// transient close of pd.doors_all. Each locked door gets a fake key flag the
// player can never hold (CHAOS_DOOR_LOCKBIT), so doorIsUnlocked returns false
// for both interaction and auto-open (propobj.c: keyflags==0 gates opening).
// We only OR/clear our own bit, so a door's real mission keyflags are preserved.
// on = lock (set the bit + request close); off = unlock (clear the bit).
#define CHAOS_DOOR_LOCKBIT 0x80000000u
s32 chraiLuaDoorsLock(s32 on)
{
	struct prop *prop;
	s32 n = 0;

	if (apLuaPlayerChr() == NULL || g_NetMode == NETMODE_CLIENT) {
		return 0;
	}
	for (prop = g_Vars.activeprops; prop; prop = prop->next) {
		if (prop->type == PROPTYPE_DOOR && prop->door) {
			if (on) {
				prop->door->keyflags |= CHAOS_DOOR_LOCKBIT;
				doorsRequestMode(prop->door, DOORMODE_CLOSING);
			} else {
				prop->door->keyflags &= ~CHAOS_DOOR_LOCKBIT;
			}
			n++;
		}
	}
	return n;
}

// pd.chr_summon(chrnum, dx, dz): teleport a chr to the player's position plus
// a horizontal offset — chrMoveToPos, the ground-validated primitive the
// Counter-Op spawn uses (rooms come from the player, so cross-map summons
// register correctly). Fails cleanly if the spot doesn't validate.
s32 chraiLuaChrSummon(s32 chrnum, f32 dx, f32 dz)
{
	struct chrdata *chr = chrFindByLiteralId(chrnum);
	struct coord pos;

	if (apLuaPlayerChr() == NULL || g_NetMode == NETMODE_CLIENT
			|| chr == NULL || chr->prop == NULL || chrIsDead(chr)
			|| chr->prop == g_Vars.currentplayer->prop) {
		return 0;
	}
	pos.x = g_Vars.currentplayer->prop->pos.x + dx;
	pos.y = g_Vars.currentplayer->prop->pos.y;
	pos.z = g_Vars.currentplayer->prop->pos.z + dz;
	return chrMoveToPos(chr, &pos, g_Vars.currentplayer->prop->rooms,
			atan2f(-dx, -dz), false) ? 1 : 0;
}

// pd.one_punch(on): a player's unarmed strikes become lethal-through-armour
// and launch the victim flying (the chrDamage boost near the top of this
// file). Pair with CHEAT_FISTS + forced-unarmed for the full Saitama.
s32 chraiLuaOnePunch(s32 on)
{
	if (apLuaPlayerChr() == NULL || g_NetMode == NETMODE_CLIENT) {
		return 0;
	}
	g_ChaosOnePunch = on ? 1 : 0;
	return 1;
}

// pd.space_program(on): every player bullet becomes a one-hit-kill launcher
// (chrDamage reads g_ChaosSpaceProgram). Like one_punch, but for guns.
s32 chraiLuaSpaceProgram(s32 on)
{
	if (apLuaPlayerChr() == NULL || g_NetMode == NETMODE_CLIENT) {
		return 0;
	}
	g_ChaosSpaceProgram = on ? 1 : 0;
	return 1;
}

// pd.frag_out(on): human enemies throw grenades whenever they would fire
// (chrConsiderGrenadeThrow reads g_ChaosFragOut). No player pawn needed to clear
// it, so an /chaos off from the hub still turns it back off.
s32 chraiLuaFragOut(s32 on)
{
	g_ChaosFragOut = on ? 1 : 0;
	return 1;
}

// Chaos FOV multiplier (playermgr.c, playermgrSetFovY).
extern f32 g_ChaosFovMult;

// pd.fov_scale(mult): stretch the vertical FOV — >1 fisheye, <1 tunnel
// vision. Same self-restoring setter-hook pattern as pd.aspect_scale.
s32 chraiLuaFovScale(f32 mult)
{
	if (apLuaPlayerChr() == NULL) {
		return 0;
	}
	if (mult <= 0.0f) mult = 1.0f;
	if (mult < 0.4f) mult = 0.4f;
	if (mult > 2.2f) mult = 2.2f;
	g_ChaosFovMult = mult;
	return 1;
}

// Chaos aspect multiplier (playermgr.c, playermgrSetAspectRatio).
extern f32 g_ChaosAspectMult;

// pd.aspect_scale(mult): stretch the projection aspect — 2.0 = extra wide
// (2:1-style CinemaScope), 0.5 = extra tall. 1.0 (or no arg) restores;
// playerTick re-derives the natural aspect every tick so restore is instant.
s32 chraiLuaAspectScale(f32 mult)
{
	if (apLuaPlayerChr() == NULL) {
		return 0;
	}
	if (mult <= 0.0f) mult = 1.0f;
	if (mult < 0.25f) mult = 0.25f;
	if (mult > 4.0f) mult = 4.0f;
	g_ChaosAspectMult = mult;
	return 1;
}

// Chaos SFX shuffle (src/lib/snd.c, sndStart) — s32, no bool-width gotcha.
extern s32 g_ChaosSfxShuffle;

// pd.sfx_replace(from, to): ONE sound id plays as another (snd.c sndStart);
// no args (both -1) = off. Mediguns swaps the weapon-pickup jingle for the
// keycard blip.
s32 chraiLuaSfxReplace(s32 from, s32 to)
{
	extern s32 g_ChaosSfxReplaceFrom;
	extern s32 g_ChaosSfxReplaceTo;

	g_ChaosSfxReplaceFrom = from;
	g_ChaosSfxReplaceTo = to;
	return 1;
}

// pd.sfx_shuffle(on): every one-shot sound effect plays as a random other
// sound (remapped inside sndStart, always to a valid sound-table id). Turning it
// OFF also hard-stops every playing sample sound — a one-shot remapped to a
// looping sound would otherwise loop forever (needing a game restart).
s32 chraiLuaSfxShuffle(s32 on)
{
	extern void sndStopAll(void);

	g_ChaosSfxShuffle = on ? 1 : 0;
	if (!on) {
		sndStopAll();
	}
	return 1;
}

// Chaos instrument shuffle (src/lib/naudio/n_csplayer.c). Defined there as a
// C `u8` — declare 1-byte here, NOT game `bool` (= s32), the same width
// gotcha as g_SndTonalInversion (see bg.c).
extern unsigned char g_ChaosInstrumentShuffle;

// pd.instrument_shuffle(on): every MIDI program change picks a random
// instrument from the loaded bank. Applies when a track (re)starts — pair
// with pd.song() to hear it immediately.
s32 chraiLuaInstrumentShuffle(s32 on)
{
	g_ChaosInstrumentShuffle = on ? 1 : 0;
	return 1;
}

// pd.spawn_bike(): spawn a personal HALF-SIZE hoverbike at the player's feet
// (extrascale 128 — the collision cylinder radius scales with it via the
// propobj.c geo fix, so it genuinely fits where a full bike wouldn't).
// One static instance: retriggering repositions the existing bike back to
// the player instead of allocating another. Solo/offline only — runtime
// objects have no syncid, so netplay clients would never see it.
static struct hoverbikeobj g_ChaosBike;
static s32 g_ChaosBikeSpawned = 0;

s32 chraiLuaSpawnBike(void)
{
	static const struct hoverbikeobj zerobike; // BSS zero template for reinit
	struct hoverbikeobj *bike = &g_ChaosBike;
	struct defaultobj *obj = &bike->base;
	struct coord pos;
	Mtxf mtx;
	RoomNum seedrooms[8];
	f32 floory;
	struct modelrodata_bbox *bbox;

	if (apLuaPlayerChr() == NULL || g_NetMode != NETMODE_NONE) {
		return 0;
	}

	pos.x = g_Vars.currentplayer->prop->pos.x + g_Vars.currentplayer->bond2.unk00.x * 80.0f;
	pos.y = g_Vars.currentplayer->prop->pos.y;
	pos.z = g_Vars.currentplayer->prop->pos.z + g_Vars.currentplayer->bond2.unk00.z * 80.0f;
	// Materialise above the player's feet (user-tuned 2026-07-28: 85 waist →
	// 160 head → settled at 120): both placement paths below rest the bike's
	// bbox min at pos.y, so lifting here spawns it mid-air and OBJFLAG_FALL
	// settles it onto the floor instead of it popping up embedded at ground
	// level.
	pos.y += 120.0f;
	mtx4LoadIdentity(&mtx);
	roomsCopy(g_Vars.currentplayer->prop->rooms, seedrooms);

	// Already spawned this stage (prop still points back at us — a stage
	// unload recycles the prop pool, which breaks this backlink): just
	// summon the existing bike back to the player.
	if (g_ChaosBikeSpawned && obj->prop && obj->prop->obj == obj
			&& obj->prop->type == PROPTYPE_OBJ && obj->model) {
		bbox = modelFindBboxRodata(obj->model);
		// Sets obj->floorcol (floor shading) via the out-param.
		cdFindFloorRoomYColourFlagsAtPos(&pos, seedrooms, &floory, &obj->floorcol, NULL);
		// Rest on the player's own floor (pos.y) in the player's rooms — see the
		// fresh-spawn path for why the snapped floor is unreliable.
		{
			struct coord placepos;
			placepos.x = pos.x;
			placepos.y = pos.y - objGetRotatedLocalYMinByMtx4(bbox, &mtx) * obj->model->scale;
			placepos.z = pos.z;
			func0f06a580(obj, &placepos, &mtx, seedrooms);
		}
		return 1;
	}

	// Fresh spawn: build the template (the setup.c OBJTYPE_HOVERBIKE recipe,
	// minus the pad — we place manually like chraiLuaSpawnAtPos).
	*bike = zerobike;
	obj->extrascale = 128; // HALF SIZE
	obj->type = OBJTYPE_HOVERBIKE;
	obj->modelnum = MODEL_HOVBIKE;
	obj->pad = -1;
	obj->flags = OBJFLAG_FALL;
	obj->flags3 = OBJFLAG3_GEOCYL; // bikes use the cylinder geo
	obj->realrot[0][0] = 1;
	obj->realrot[1][1] = 1;
	obj->realrot[2][2] = 1;
	obj->maxdamage = 1000;
	obj->shadecol[0] = obj->shadecol[1] = obj->shadecol[2] = 0xff;
	obj->nextcol[0] = obj->nextcol[1] = obj->nextcol[2] = 0xff;
	obj->floorcol = 0x0fff;

	if (!setupLoadModeldef(MODEL_HOVBIKE)) {
		return 0;
	}
	if (objInitWithModelDef(obj, g_ModelStates[MODEL_HOVBIKE].modeldef) == NULL || obj->model == NULL) {
		return 0;
	}
	modelSetScale(obj->model, obj->model->scale * (obj->extrascale * (1.0f / 256.0f)));
	setupCreateHov(obj, &bike->hov);

	bbox = modelFindBboxRodata(obj->model);
	// Sets obj->floorcol (floor shading) via the out-param.
	cdFindFloorRoomYColourFlagsAtPos(&pos, seedrooms, &floory, &obj->floorcol, NULL);
	// Rest the bike on the PLAYER's own floor level (pos.y) in the player's rooms
	// — NOT the snapped floor. A floor-snap at the offset can portal-walk to a
	// much lower floor over a ledge and drop the bike out of sight (WAR put it
	// ~130u below the player, on an elevated deck). Lift by the scaled bbox min so
	// it rests on the surface; OBJFLAG_FALL settles any small mismatch.
	{
		struct coord placepos;
		placepos.x = pos.x;
		placepos.y = pos.y - objGetRotatedLocalYMinByMtx4(bbox, &mtx) * obj->model->scale;
		placepos.z = pos.z;
		func0f06a580(obj, &placepos, &mtx, seedrooms);
	}

	// Register the bike's prop into the active/rendered prop list. objInit only
	// allocates the prop; the setup.c object recipe calls propActivate+propEnable
	// after placement (setup.c:1199) — without them the bike exists but never
	// ticks or renders (spawns "nothing"). This was the missing step.
	propActivate(obj->prop);
	propEnable(obj->prop);

	g_ChaosBikeSpawned = 1;
	return 1;
}

// Chaos "Sentries Out": free-standing hostile laptop sentry guns. Storage is our
// own pool (the engine's g_ThrownLaptops is per-player and slot-limited), so we
// can drop up to CHAOS_MAX_SENTRIES of them. Reset per stage in chraiLuaResetSentries.
#define CHAOS_MAX_SENTRIES 8
static struct autogunobj g_ChaosSentries[CHAOS_MAX_SENTRIES];
static s32 g_ChaosSentryCount = 0;

void chraiLuaResetSentries(void)
{
	// A stage unload recycles the prop/model pools, so the objects are already
	// gone — just drop our count so the next stage can spawn a fresh batch.
	g_ChaosSentryCount = 0;
}

// pd.spawn_sentry(dx, dz) -> bool. Deploy a laptop sentry gun (MODEL_CHRAUTOGUN,
// OBJTYPE_AUTOGUN) at the player's position plus a horizontal offset, floor-
// snapped, hostile to the PLAYER (targetteam = the player's team, which is how
// the autogun's target scan selects who to shoot). The laptop-sentry field
// values (aim range/speed, full rotation) mirror laptopDeploy; storage is our
// own pool so several can coexist. Solo only; returns 1 on success.
s32 chraiLuaSpawnSentry(f32 dx, f32 dz)
{
	struct chrdata *plchr = apLuaPlayerChr();
	struct autogunobj *gun;
	struct defaultobj *obj;
	struct coord pos;
	Mtxf mtx;
	RoomNum seedrooms[8];
	f32 floory;
	u16 floorcol;
	s32 floorroom;
	struct modelrodata_bbox *bbox;
	struct coord mountnormal;

	if (plchr == NULL || g_NetMode != NETMODE_NONE) {
		return 0;
	}
	if (g_ChaosSentryCount >= CHAOS_MAX_SENTRIES) {
		return 0;
	}

	gun = &g_ChaosSentries[g_ChaosSentryCount];
	obj = &gun->base;

	roomsCopy(g_Vars.currentplayer->prop->rooms, seedrooms);
	mtx4LoadIdentity(&mtx);
	mountnormal.x = 0.0f;
	mountnormal.y = 1.0f;
	mountnormal.z = 0.0f;

	// Cast a ray from the player toward this ring direction (with a random
	// vertical tilt, so some rays find the FLOOR/CEILING and others WALLS) and
	// mount the sentry flush on the first surface hit, oriented to its normal —
	// like a deployed laptop sentry. If the ray hits nothing (open direction),
	// fall back to floor-snapping at the ring offset.
	{
		struct coord raystart, rayend, dir, n;
		f32 m, vy;

		m = sqrtf(dx * dx + dz * dz);
		if (m < 1.0f) {
			m = 1.0f;
		}
		dir.x = dx / m;
		dir.z = dz / m;
		vy = ((s32)(rngRandom() % 2001) - 1000) * 0.0012f; // ~[-1.2, 1.2] vertical tilt
		dir.y = vy;
		m = sqrtf(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
		dir.x /= m;
		dir.y /= m;
		dir.z /= m;

		raystart.x = g_Vars.currentplayer->prop->pos.x;
		raystart.y = g_Vars.currentplayer->prop->pos.y + 100.0f;
		raystart.z = g_Vars.currentplayer->prop->pos.z;
		rayend.x = raystart.x + dir.x * 2500.0f;
		rayend.y = raystart.y + dir.y * 2500.0f;
		rayend.z = raystart.z + dir.z * 2500.0f;

		if (cdExamLos08(&raystart, seedrooms, &rayend, CDTYPE_BG, GEOFLAG_BLOCK_SHOOT) == CDRESULT_COLLISION) {
			struct coord right, fwd, ref;

			cdGetPos(&pos, __LINE__, "chraction.c"); // surface hit point
			cdGetObstacleNormal(&n);

			m = sqrtf(n.x * n.x + n.y * n.y + n.z * n.z);
			if (m < 0.0001f) {
				n.x = 0.0f; n.y = 1.0f; n.z = 0.0f;
			} else {
				n.x /= m; n.y /= m; n.z /= m;
			}
			// Point the normal back toward the room (opposite the ray) so the
			// sentry mounts ON the surface, not buried in it.
			if (n.x * dir.x + n.y * dir.y + n.z * dir.z > 0.0f) {
				n.x = -n.x; n.y = -n.y; n.z = -n.z;
			}

			// Orthonormal basis with local Y (the autogun's up/mount axis) = normal,
			// so its base sits against the surface and it points into the room.
			if (fabsf(n.y) < 0.99f) {
				ref.x = 0.0f; ref.y = 1.0f; ref.z = 0.0f;
			} else {
				ref.x = 1.0f; ref.y = 0.0f; ref.z = 0.0f;
			}
			right.x = ref.y * n.z - ref.z * n.y;
			right.y = ref.z * n.x - ref.x * n.z;
			right.z = ref.x * n.y - ref.y * n.x;
			m = sqrtf(right.x * right.x + right.y * right.y + right.z * right.z);
			right.x /= m; right.y /= m; right.z /= m;
			fwd.x = right.y * n.z - right.z * n.y;
			fwd.y = right.z * n.x - right.x * n.z;
			fwd.z = right.x * n.y - right.y * n.x;

			mtx.m[0][0] = right.x; mtx.m[0][1] = right.y; mtx.m[0][2] = right.z;
			mtx.m[1][0] = n.x;     mtx.m[1][1] = n.y;     mtx.m[1][2] = n.z;
			mtx.m[2][0] = fwd.x;   mtx.m[2][1] = fwd.y;   mtx.m[2][2] = fwd.z;
			mountnormal = n;
		} else {
			// No surface hit: floor-snap at the ring offset (original behaviour).
			pos.x = g_Vars.currentplayer->prop->pos.x + dx;
			pos.y = g_Vars.currentplayer->prop->pos.y;
			pos.z = g_Vars.currentplayer->prop->pos.z + dz;
#if VERSION >= VERSION_NTSC_1_0
			floorroom = cdFindFloorRoomYColourFlagsAtPos(&pos, seedrooms, &floory, &floorcol, NULL);
#else
			floorroom = cdFindFloorRoomYColourFlagsAtPos(&pos, seedrooms, &floory, &floorcol);
#endif
			if (floorroom > 0) {
				pos.y = floory;
				seedrooms[0] = floorroom;
				seedrooms[1] = -1;
			}
		}
	}

	// Build the autogun object template (the laptopDeploy defaultobj recipe).
	{
		static const struct autogunobj zerogun; // BSS zero template
		*gun = zerogun;
	}
	obj->extrascale = 256;
	obj->type = OBJTYPE_AUTOGUN;
	obj->modelnum = MODEL_CHRAUTOGUN;
	obj->pad = -1;
	obj->flags = 0;
	// Orient the object to the mount surface (identity = upright on a floor).
	obj->realrot[0][0] = mtx.m[0][0]; obj->realrot[0][1] = mtx.m[0][1]; obj->realrot[0][2] = mtx.m[0][2];
	obj->realrot[1][0] = mtx.m[1][0]; obj->realrot[1][1] = mtx.m[1][1]; obj->realrot[1][2] = mtx.m[1][2];
	obj->realrot[2][0] = mtx.m[2][0]; obj->realrot[2][1] = mtx.m[2][1]; obj->realrot[2][2] = mtx.m[2][2];
	obj->maxdamage = 1000;
	obj->shadecol[0] = obj->shadecol[1] = obj->shadecol[2] = 0xff;
	obj->nextcol[0] = obj->nextcol[1] = obj->nextcol[2] = 0xff;
	obj->floorcol = 0x0fff;

	// setupLoadModeldef returns true only when it actually LOADS the modeldef and
	// false when it's already resident — NOT a success/failure flag (all canonical
	// callers ignore it). Treating false as failure meant only the FIRST sentry
	// spawned; the rest found the model already loaded and bailed. Load (if needed)
	// then check the modeldef is actually available.
	setupLoadModeldef(MODEL_CHRAUTOGUN);
	if (g_ModelStates[MODEL_CHRAUTOGUN].modeldef == NULL) {
		return 0;
	}
	if (objInitWithModelDef(obj, g_ModelStates[MODEL_CHRAUTOGUN].modeldef) == NULL || obj->model == NULL) {
		return 0;
	}

	// Autogun runtime state (setupCreateAutogun + laptopDeploy values). The
	// beam is the aiming laser; MEMPOOL_STAGE is freed with the stage.
	gun->targetpad = -1;
	gun->aimdist = 5000.0f;
	gun->maxspeed = PALUPF(0.0697f);
	gun->ymaxleft = 12.56f;
	gun->ymaxright = -12.56f;
	gun->firecount = 0;
	gun->lastseebond60 = -1;
	gun->lastaimbond60 = -1;
	gun->allowsoundframe = -1;
	gun->yrot = gun->yspeed = gun->yzero = 0;
	gun->xrot = gun->xspeed = gun->xzero = 0;
	gun->barrelspeed = gun->barrelrot = 0;
	gun->firing = false;
	gun->shotbondsum = 0;
	gun->target = NULL;
	gun->nextchrtest = 0;
	gun->ammoquantity = 255;
	// Hostile to the player. The autogun target scan (autogunTick) only runs when
	// targetteam != 0 (else the gun sits idle), and matches victims by team bits
	// when MP teams are on — so prefer the player's own team, but fall back to
	// "all teams" (0xffff) when the player's team is 0 (offline Combat Sim with
	// teams off), which keeps the scan alive and, with teams off, targets everyone.
	gun->targetteam = plchr->team ? plchr->team : 0xffff;
	gun->beam = mempAlloc(ALIGN16(sizeof(struct beam)), MEMPOOL_STAGE);
	if (gun->beam) {
		gun->beam->age = -1;
	}

	// Seat the base flush on the mount surface: push the origin out along the
	// surface normal by the model's base offset (bbox->ymin, the local mount axis).
	// For a floor (normal = +Y) this is exactly the old floor lift; for a wall or
	// ceiling it lifts along that surface's normal instead of world-down.
	bbox = modelFindBboxRodata(obj->model);
	{
		struct coord placepos;
		f32 lift = bbox->ymin * obj->model->scale;
		placepos.x = pos.x - lift * mountnormal.x;
		placepos.y = pos.y - lift * mountnormal.y;
		placepos.z = pos.z - lift * mountnormal.z;
		// Bake model->scale into the rotation before func0f06a580 copies it into
		// realrot — the autogun BASE renders straight from realrot with NO
		// model->scale (objInitMatrices), while the turret DOES get it
		// (autogunInitMatrices), so without this the base is huge next to a
		// normal turret. This mirrors the real laptop throw (bgun0f09ebcc).
		mtx00015f04(obj->model->scale, &mtx);
		func0f06a580(obj, &placepos, &mtx, seedrooms);
	}
	if (obj->prop) {
		obj->prop->forcetick = true; // tick + fire even while off-screen
		// No owning player. In MP the autogun scan SKIPS its owner player
		// (objGetOwnerPlayerNum), which defaults to 0 = the human player (slot 0),
		// so without this the sentry never targets you. -2 makes it report -1
		// (owned by nobody), so every player/bot is a valid target.
		obj->prop->ownerplayernum = -2;
		propActivate(obj->prop);
		propEnable(obj->prop);
	}

	g_ChaosSentryCount++;
	return 1;
}

// pd.spawn_chopper(): a hostile dD hovercopter appears near the player and
// opens fire (the "helicopter helicopter" chaos effect). The spawn_bike
// recipe adapted to OBJTYPE_CHOPPER: runtime template + objInitWithModelDef +
// manual placement, then setup.c's chopper field block with an IDLE ailist
// (choppers run chraiExecute every tick, and a NULL list is not survivable),
// the player as target, and CHOPPERMODE_COMBAT. No patrol path: a path-less
// chopper holds position and engages when its target is visible
// (chopperTickMove's "stay put" branch). EXPLORATORY — the mission choppers
// are ailist-driven, so combat behaviour without a script is best-effort.
static struct chopperobj g_ChaosChoppers[2]; // 0 = dD hovercopter, 1 = A51 interceptor
static s32 g_ChaosChopperSpawned[2] = {0, 0};

s32 chraiLuaSpawnChopper(s32 kind, s32 extrascale)
{
	static const struct chopperobj zerochopper;
	struct chopperobj *chopper;
	struct defaultobj *obj;
	struct coord pos;
	Mtxf mtx;
	RoomNum seedrooms[8];
	f32 floory;
	u16 floorcol;
	s32 modelnum;

	if (apLuaPlayerChr() == NULL || g_NetMode != NETMODE_NONE) {
		return 0;
	}

	kind = (kind == 1) ? 1 : 0;
	modelnum = kind ? MODEL_A51INTERCEPTOR : MODEL_DD_HOVERCOPTER;
	// The interceptor's MODELDEF is natively ~0.1 scale (the chopper gunfire
	// code corrects its gun position by 0.1/model->scale), so extrascale 256
	// = authored size — scaling it further down made it near-invisible. The
	// dD copter is 1:1 at 256 (chaos runs it quarter size). Clamp so a bad
	// script value can't produce a zero-scale model.
	if (extrascale < 8) {
		extrascale = kind ? 1024 : 64;
	} else if (extrascale > 2048) {
		extrascale = 2048;
	}
	chopper = &g_ChaosChoppers[kind];
	obj = &chopper->base;

	// ~350 units ahead of the player, hovering 200 above their floor
	pos.x = g_Vars.currentplayer->prop->pos.x + g_Vars.currentplayer->bond2.unk00.x * 350.0f;
	pos.y = g_Vars.currentplayer->prop->pos.y + 200.0f;
	pos.z = g_Vars.currentplayer->prop->pos.z + g_Vars.currentplayer->bond2.unk00.z * 350.0f;
	mtx4LoadIdentity(&mtx);
	roomsCopy(g_Vars.currentplayer->prop->rooms, seedrooms);

	// Already spawned this stage (prop backlink still valid): summon it back
	// to the player and re-aggro.
	if (g_ChaosChopperSpawned[kind] && obj->prop && obj->prop->obj == obj
			&& obj->prop->type == PROPTYPE_OBJ && obj->model
			&& chopper->attackmode != CHOPPERMODE_FALL
			&& chopper->attackmode != CHOPPERMODE_DEAD) {
		func0f06a580(obj, &pos, &mtx, seedrooms);
		chopper->target = g_Vars.currentplayer->prop - g_Vars.props;
		chopper->attackmode = CHOPPERMODE_COMBAT;
		chopper->patroltimer60 = TICKS(240);
		return 1;
	}

	*chopper = zerochopper;
	obj->extrascale = (u16)extrascale;
	obj->type = OBJTYPE_CHOPPER;
	obj->modelnum = (s16)modelnum;
	obj->pad = -1;
	obj->flags = OBJFLAG_CHOPPER_INIT;
	obj->realrot[0][0] = 1;
	obj->realrot[1][1] = 1;
	obj->realrot[2][2] = 1;
	obj->maxdamage = 1000;
	obj->shadecol[0] = obj->shadecol[1] = obj->shadecol[2] = 0xff;
	obj->nextcol[0] = obj->nextcol[1] = obj->nextcol[2] = 0xff;
	obj->floorcol = 0x0fff;

	if (!setupLoadModeldef(modelnum)) {
		return 0;
	}
	if (objInitWithModelDef(obj, g_ModelStates[modelnum].modeldef) == NULL
			|| obj->model == NULL || obj->prop == NULL) {
		return 0;
	}
	modelSetScale(obj->model, obj->model->scale * (obj->extrascale * (1.0f / 256.0f)));

	// keep obj->floorcol fresh for the shade path (out-param)
	cdFindFloorRoomYColourFlagsAtPos(&pos, seedrooms, &floory, &floorcol, NULL);
	obj->floorcol = floorcol;

	func0f06a580(obj, &pos, &mtx, seedrooms);

	// setup.c's OBJTYPE_CHOPPER init block, minus the setup-file ailist
	chopper->turnrot60 = 0;
	chopper->roty = 0;
	chopper->rotx = 0;
	chopper->gunroty = 0;
	chopper->gunrotx = 0;
	chopper->barrelrot = 0;
	chopper->barrelrotspeed = 0;
	chopper->ailist = ailistFindById(GAILIST_IDLE);
	chopper->aioffset = 0;
	chopper->aireturnlist = -1;
	chopper->path = NULL;
	chopper->nextstep = 0;
	chopper->targetvisible = false;
	chopper->vz = 0;
	chopper->vy = 0;
	chopper->vx = 0;
	chopper->otz = 0;
	chopper->oty = 0;
	chopper->otx = 0;
	chopper->power = 0;
	chopper->bob = 0;
	chopper->bobstrength = 0.05f;
	chopper->timer60 = 0;
	chopper->cw = 0;
	chopper->weaponsarmed = true;
	chopper->fireslotthing = mempAlloc(sizeof(struct fireslotthing), MEMPOOL_STAGE);
	chopper->fireslotthing->beam = mempAlloc(ALIGN16(sizeof(struct beam)), MEMPOOL_STAGE);
	chopper->fireslotthing->beam->age = -1;
	chopper->fireslotthing->unk08 = -1;
	chopper->fireslotthing->unk00 = 0;
	chopper->fireslotthing->unk01 = 0;
	chopper->fireslotthing->unk0c = 0.85f;
	chopper->fireslotthing->unk10 = 0.2f;
	chopper->fireslotthing->unk14 = 0;
	chopper->dead = false;

	// hunt the player
	chopper->target = g_Vars.currentplayer->prop - g_Vars.props;
	chopper->attackmode = CHOPPERMODE_COMBAT;
	chopper->patroltimer60 = TICKS(240);

	obj->prop->forcetick = true;
	propActivate(obj->prop);
	propEnable(obj->prop);

	g_ChaosChopperSpawned[kind] = 1;
	return 1;
}

// propobj.c's chopper tick asks whether a chopper is one of ours (the chaos
// spawns run GAILIST_IDLE, so the tick drives their see-target/attack loop).
s32 chaosChopperIsChaos(struct chopperobj *chopper)
{
	return chopper == &g_ChaosChoppers[0] || chopper == &g_ChaosChoppers[1];
}

// Which chaos chopper is this? 0 = dD hovercopter, 1 = A51 interceptor, -1 =
// not one of ours. Slot 1 stalks the player (chopperTickCombat); slot 0 keeps
// the original hold-position behaviour.
s32 chaosChopperKind(struct chopperobj *chopper)
{
	if (chopper == &g_ChaosChoppers[0]) {
		return 0;
	}

	if (chopper == &g_ChaosChoppers[1]) {
		return 1;
	}

	return -1;
}

// Chaos Gormless master switch (bondmove.c, bmoveProcessInput).
extern s32 g_ChaosGormless;

// pd.gormless(on): EVERYTHING backwards — movement (both sticks, dpad,
// keyboard steps), look (stick + mouse), and the fire/aim buttons swapped,
// all at the bmoveProcessInput chokepoints.
s32 chraiLuaGormless(s32 on)
{
	if (apLuaPlayerChr() == NULL) {
		return 0;
	}
	g_ChaosGormless = on ? 1 : 0;
	return 1;
}

// Chaos backfire master switch (bondgun.c, bgunCalculatePlayerShotSpread).
extern s32 g_ChaosBackfire;

// pd.backfire(on): the local player's shots (hitscan traces, fired
// projectiles, tracers) leave 180 degrees behind them; the crosshair and gun
// render stay where they are. Vertical aim is preserved.
s32 chraiLuaBackfire(s32 on)
{
	if (apLuaPlayerChr() == NULL) {
		return 0;
	}
	g_ChaosBackfire = on ? 1 : 0;
	return 1;
}

// Chaos ammo swap master switch (game_0b0fd0.c, the gset function getters).
extern s32 g_ChaosAmmoSwapWeapon;

// pd.ammo_swap(weaponnum): every gun the player holds fires this weapon's
// primary rounds (rockets, Devastator grenades, DY357 bullets...). The target
// must have a SHOOT-type primary — melee/throw/device functions need hand
// anim states a gun can't provide. pd.ammo_swap() with no arg turns it off.
s32 chraiLuaAmmoSwap(s32 weaponnum)
{
	struct weaponfunc *func;

	if (apLuaPlayerChr() == NULL) {
		return 0;
	}
	if (weaponnum < 0) {
		g_ChaosAmmoSwapWeapon = -1;
		return 1;
	}
	func = weaponGetFunctionById(weaponnum, FUNC_PRIMARY);
	if (func == NULL || (func->type & 0xff) != INVENTORYFUNCTYPE_SHOOT) {
		return 0;
	}
	g_ChaosAmmoSwapWeapon = weaponnum;
	return 1;
}

// Renderer chaos globals (port/fast3d/gfx_pc.cpp). Both are C++ `int` — the
// same width as game-side s32, so the 1-byte `bool` bridging gotcha the
// wireframe/mirror flags have (see bg.c) does not apply here.
extern s32 gfx_flattex_mode;
extern s32 gfx_force_grayscale;
// Chaos room tint (dlights.c): global room-lighting colour multiplier.
extern f32 g_ChaosRoomTintFrac[3];
extern s32 g_ChaosRoomTintOn;

// pd.flattex(mode): 0 = normal textures, 1 = all-white (pure vertex shading),
// 2 = every texture flooded with its own average colour. Alpha is preserved
// so HUD text stays readable. Applied renderer-side at the next frame
// boundary via a texture-cache reimport; purely cosmetic, net/save-safe.
s32 chraiLuaFlatTex(s32 mode)
{
	if (mode < 0) mode = 0;
	if (mode > 2) mode = 2;
	gfx_flattex_mode = mode;
	return 1;
}

// pd.grayscale(on): force the renderer's grayscale shader path (film noir).
s32 chraiLuaGrayscale(s32 on)
{
	gfx_force_grayscale = on ? 1 : 0;
	return 1;
}

// Renderer chaos global (gfx_pc.cpp, C++ int == s32 like the two above).
extern s32 gfx_shiny_mode;

// pd.shiny(mode): 0 = off, 1 = every 3D surface gets fake-chrome screen-space
// UVs ("everything is reflective"), 2 = the same plus a gold tint via the
// grayscale shader (Midas mode). HUD texrects and menu geometry are exempt
// renderer-side; bg.c gates dlcache off while active. Cosmetic only,
// net/save-safe.
s32 chraiLuaShiny(s32 mode)
{
	if (mode < 0) mode = 0;
	if (mode > 2) mode = 2;
	gfx_shiny_mode = mode;
	return 1;
}

// pd.chr_give_weapon(chrnum, weaponnum): make a chr wield the given weapon.
// NPCs only — a player's inventory is managed through give_weapon/take_weapon.
//
// Two different actor kinds need two different mechanisms:
//
//  - Simulants (chr->aibot): a bot re-picks its weapon from its own inventory
//    every tick (bot.c changeguntimer60 path), so overwriting weapons_held is
//    reverted almost immediately. Instead add the weapon to the bot inventory
//    with a full ammo reserve and drive the engine's own switch routine, which
//    sets aibot->weaponnum and schedules the hand-model equip. The bot keeps it
//    until its AI later decides to change guns.
//
//  - Campaign guards (non-bot): chrTickShoot fires straight from the prop in
//    weapons_held[hand], so replacing the held weapon models is enough. The old
//    hand props are marked DELETING so they vanish rather than drop.
s32 chraiLuaChrGiveWeapon(s32 chrnum, s32 weaponnum)
{
	struct chrdata *chr = chrFindByLiteralId(chrnum);
	s32 h;

	if (apLuaPlayerChr() == NULL || chr == NULL || chr->prop == NULL || chr->model == NULL) {
		return 0;
	}
	if (chr->prop->type != PROPTYPE_CHR) {
		return 0;
	}

	if (chr->aibot) {
		if (weaponnum != WEAPON_UNARMED && weaponnum != WEAPON_NONE) {
			botinvGiveSingleWeapon(chr, weaponnum);
			botactGiveAmmoByWeapon(chr->aibot, weaponnum, FUNC_PRIMARY, 0x7fff);
			botactGiveAmmoByWeapon(chr->aibot, weaponnum, FUNC_SECONDARY, 0x7fff);
		}
		return botinvSwitchToWeapon(chr, weaponnum, FUNC_PRIMARY) ? 1 : 0;
	}

	for (h = 0; h < 2; h++) {
		struct prop *wp = chr->weapons_held[h];

		if (wp && wp->obj) {
			wp->obj->hidden |= OBJHFLAG_DELETING;
			chr->weapons_held[h] = NULL;
		}
	}

	// Unarmed / no weapon has no hand model (model -1) — the hands are already
	// cleared above, so leave the guard empty-handed. This is the restore path
	// for a chr that was originally unarmed.
	{
		s32 model = playermgrGetModelOfWeapon(weaponnum);

		if (model < 0) {
			return 1;
		}

		return chrGiveWeapon(chr, model, weaponnum, 0) != NULL;
	}
}

// pd.chr_weapon(chrnum) -> weaponnum. The chr's currently-wielded weapon:
// aibot->weaponnum for simulants, else the right/left held weapon (WEAPON_UNARMED
// if empty-handed). Returns -1 for an invalid chr. Lets Lua snapshot an NPC's
// gun before pd.chr_give_weapon and restore it when a timed effect ends.
s32 chraiLuaChrWeapon(s32 chrnum)
{
	struct chrdata *chr = chrFindByLiteralId(chrnum);
	struct prop *wp;

	if (chr == NULL || chr->prop == NULL) {
		return -1;
	}
	if (chr->aibot) {
		return chr->aibot->weaponnum;
	}

	wp = chrGetHeldProp(chr, HAND_RIGHT);
	if (wp && wp->weapon) {
		return wp->weapon->weaponnum;
	}

	wp = chrGetHeldProp(chr, HAND_LEFT);
	if (wp && wp->weapon) {
		return wp->weapon->weaponnum;
	}

	return WEAPON_UNARMED;
}

// pd.player_health(): current health fraction (0..1), the same scale
// player_set_health writes.
f32 chraiLuaPlayerHealth(void)
{
	if (apLuaPlayerChr() == NULL) {
		return 0.0f;
	}
	return g_Vars.currentplayer->bondhealth;
}

// pd.player_shield(): the local player's current shield fraction (0..1).
f32 chraiLuaPlayerShield(void)
{
	if (apLuaPlayerChr() == NULL) {
		return 0.0f;
	}
	return playerGetShieldFrac();
}

// pd.player_reloading(): true while either hand is in the reload state.
s32 chraiLuaPlayerReloading(void)
{
	if (apLuaPlayerChr() == NULL) {
		return 0;
	}
	return (g_Vars.currentplayer->hands[HAND_RIGHT].state == HANDSTATE_RELOAD
			|| g_Vars.currentplayer->hands[HAND_LEFT].state == HANDSTATE_RELOAD) ? 1 : 0;
}

// pd.player_activate(): true this frame if the use/activate button is held
// (opening a door / interacting). Mirrors lv.c's JO_ACTION_ACTIVATE test.
s32 chraiLuaPlayerActivate(void)
{
	if (apLuaPlayerChr() == NULL) {
		return 0;
	}
	return (g_Vars.currentplayer->bondactivateorreload & JO_ACTION_ACTIVATE) ? 1 : 0;
}

// pd.player_damage(amount): hurt the local player through the real damage
// path (shield first, damage flash/sound, death) — ~1.0 is roughly one
// gunshot. Attacker is NULL (environment), so a death reads as a suicide.
s32 chraiLuaPlayerDamage(f32 amount)
{
	struct chrdata *pchr = apLuaPlayerChr();
	struct coord vec = {0, 0, 1};

	if (pchr == NULL || amount <= 0.0f) {
		return 0;
	}
	chrDamageByMisc(pchr, amount, &vec, NULL, NULL);
	return 1;
}

// pd.weapon_jam(mode): 1 = trigger pulls dry-fire (bondgun.c reroute);
// 2 = "jam v2": pulls sometimes dry-fire, and a shot that DOES fire jams the
// rest of the magazine (clip drains to 0 — reload to clear). 0 = off.
extern s32 g_ChaosWeaponJam;
s32 chraiLuaWeaponJam(s32 mode)
{
	g_ChaosWeaponJam = mode;
	return 1;
}

// pd.force_secondary(on): pin both hands to the secondary weapon function
// (bondmove.c per-tick re-assert, the gun_lock pattern minus auto-fire and
// the switch block).
extern s32 g_ChaosForceSecondary;
s32 chraiLuaForceSecondary(s32 on)
{
	g_ChaosForceSecondary = on ? 1 : 0;
	return 1;
}

// pd.button_block(mask): named pad buttons vanish from gameplay input
// (bondmove.c c1buttons strip; menus read the joy layer directly and are
// unaffected). mask 0 = off.
extern u32 g_ChaosButtonMask;
s32 chraiLuaButtonMask(u32 mask)
{
	g_ChaosButtonMask = mask;
	return 1;
}

// pd.ammo_cost(mult): each shot spends mult rounds from the clip
// (bondgun.c post-decrement top-up). 1 = normal.
extern s32 g_ChaosAmmoCost;
s32 chraiLuaAmmoCost(s32 mult)
{
	g_ChaosAmmoCost = mult < 1 ? 1 : mult;
	return 1;
}

// pd.temu_mag(on): reloads pay full price but only partially refill the clip
// (bondgun.c reload site reads g_ChaosTemuMag).
extern s32 g_ChaosTemuMag;
s32 chraiLuaTemuMag(s32 on)
{
	g_ChaosTemuMag = on ? 1 : 0;
	return 1;
}

// pd.autoaim(on): force aim assist on regardless of the player option
// (options.c optionsGetAutoAim override).
extern s32 g_ChaosAutoAim;
s32 chraiLuaAutoAim(s32 on)
{
	g_ChaosAutoAim = on ? 1 : 0;
	return 1;
}

// pd.deadzone(frac): override every analog-stick deadzone to frac (0..1) of
// full deflection when larger than the user's own setting; 0 = off.
// (port/src/input.c inputAxisScale.)
#ifndef PLATFORM_N64
extern void inputSetChaosDeadzone(s32 dz);
#endif
s32 chraiLuaDeadzone(f32 frac)
{
#ifndef PLATFORM_N64
	if (frac < 0) {
		frac = 0;
	}
	if (frac > 0.95f) {
		frac = 0.95f;
	}
	inputSetChaosDeadzone((s32)(frac * 32768.0f));
	return 1;
#else
	return 0;
#endif
}

// pd.nitro(on): every destroyed object explodes like the Crash Site ship
// (propobj.c objCheckDestroyed exptype override).
extern s32 g_ChaosNitro;
s32 chraiLuaNitro(s32 on)
{
	g_ChaosNitro = on ? 1 : 0;
	return 1;
}

// pd.objective_force(index, state): 0 = off (real status), 1 = force
// INCOMPLETE, 2 = force COMPLETE. index -1 + state 0 clears all. Solo only
// (objectives.c objectiveCheck override — display AND the all-complete check
// both route through it, so a held-down objective blocks mission end until
// released).
extern u8 g_ChaosObjectiveForce[MAX_OBJECTIVES];
s32 chraiLuaObjectiveForce(s32 index, s32 state)
{
	s32 i;

	if (index < 0) {
		for (i = 0; i < MAX_OBJECTIVES; i++) {
			g_ChaosObjectiveForce[i] = 0;
		}
		return 1;
	}
	if (index >= MAX_OBJECTIVES) {
		return 0;
	}
	g_ChaosObjectiveForce[index] = (u8)(state < 0 ? 0 : (state > 2 ? 2 : state));
	return 1;
}

// pd.objective_status(index): the objective's REAL status (override bypassed):
// 0 incomplete / 1 complete / 2 failed, or -1 if the index isn't a live
// objective on this stage+difficulty.
s32 chraiLuaObjectiveStatus(s32 index)
{
	u8 saved;
	s32 status;

	if (index < 0 || index >= MAX_OBJECTIVES || index >= objectiveGetCount()) {
		return -1;
	}
	if (!(objectiveGetDifficultyBits(index) & (1 << lvGetDifficulty()))) {
		return -1;
	}
	saved = g_ChaosObjectiveForce[index];
	g_ChaosObjectiveForce[index] = 0;
	status = objectiveCheck(index);
	g_ChaosObjectiveForce[index] = saved;
	return status;
}

// pd.pinball(on): fired physics projectiles become grenade-secondary
// Proximity Pinballs (bondgun.c launch conversion).
extern s32 g_ChaosPinball;
s32 chraiLuaPinball(s32 on)
{
	g_ChaosPinball = on ? 1 : 0;
	return 1;
}

// pd.player_freeze(on): root the local player in place (bondmove.c).
extern s32 g_ChaosPlayerFreeze;
s32 chraiLuaPlayerFreeze(s32 on)
{
	g_ChaosPlayerFreeze = on ? 1 : 0;
	return 1;
}

// pd.chr_freeze(on): statue every non-player chr (chr.c anim gate +
// chrTickShoot fire gate).
s32 chraiLuaChrFreeze(s32 on)
{
	extern s32 g_ChaosChrFreeze;

	g_ChaosChrFreeze = on ? 1 : 0;
	return 1;
}

// pd.no_drops(on): dead chrs keep their weapons (chrBeginDeath gate).
s32 chraiLuaNoDrops(s32 on)
{
	g_ChaosNoDrops = on ? 1 : 0;
	return 1;
}

// pd.paintball(on): force paintball visuals for everyone (wallhit.c).
extern s32 g_ChaosPaintball;
s32 chraiLuaPaintball(s32 on)
{
	g_ChaosPaintball = on ? 1 : 0;
	return 1;
}

// pd.damage_scale(frac): scale all chr/player damage (1.0 = off).
s32 chraiLuaDamageScale(f32 frac)
{
	if (frac < 0.0f) frac = 0.0f;
	if (frac > 10.0f) frac = 10.0f;
	g_ChaosDamageScale = frac;
	return 1;
}

// pd.zoom_scale(mult): multiply every weapon's aim-zoom FOV (game_0b0fd0.c);
// > 1 zooms OUT. 1.0 = off.
extern f32 g_ChaosZoomMult;
s32 chraiLuaZoomScale(f32 mult)
{
	if (mult < 0.1f) mult = 0.1f;
	if (mult > 16.0f) mult = 16.0f;
	g_ChaosZoomMult = mult;
	return 1;
}

// pd.gun_sound(weaponnum): every gun's fire sound becomes this weapon's
// primary shoot sound (resolved once here; gsetGetSingleShootSound applies
// it at every consumer). pd.gun_sound() turns it off.
extern s32 g_ChaosGunSfxOverride;
s32 chraiLuaGunSound(s32 weaponnum)
{
	struct gset gset = {0};

	if (weaponnum <= 0) {
		g_ChaosGunSfxOverride = 0;
		return 1;
	}

	gset.weaponnum = weaponnum;
	gset.weaponfunc = FUNC_PRIMARY;
	g_ChaosGunSfxOverride = 0; // resolve against the REAL table, not the override
	g_ChaosGunSfxOverride = (s32)gsetGetSingleShootSound(&gset);
	return g_ChaosGunSfxOverride != 0;
}

// pd.mute(on) / pd.play_file(path): port audio layer (port/src/audio.c).
extern void audioSetMuted(s32 on);
extern s32 audioPlayExternal(const char *path, s32 loop, s32 followMusic);
extern void audioStopExternal(void);
s32 chraiLuaMute(s32 on)
{
	audioSetMuted(on ? 1 : 0);
	return 1;
}

s32 chraiLuaPlayFile(const char *path, s32 loop, s32 followMusic)
{
	return audioPlayExternal(path, loop, followMusic);
}

// pd.ext_volume([pct]): volume of ALL external one-shots as a percentage OF
// the music slider (the slider stays the ceiling). pct < 0 = just read.
// Persists as Audio.ExtVolume in pd.ini.
s32 chraiLuaExtVolume(s32 pct)
{
	extern void audioSetExtVolume(s32 pct);
	extern s32 audioGetExtVolume(void);

	if (pct >= 0) {
		audioSetExtVolume(pct);
	}
	return audioGetExtVolume();
}

// pd.stop_file(): stop the external sound started by pd.play_file (e.g. the
// phone ringtone when the call is answered).
void chraiLuaStopFile(void)
{
	audioStopExternal();
}

// pd.weapon_rename(weaponnum, name): relabel a weapon everywhere its name is
// displayed (HUD, inventory, pickup toast, weapon wheel — full name AND short
// name funnel through langGet). While renamed, the pause-menu inventory also
// hides the weapon's 3D model (func0f105948 checks g_ChaosRenamedWeapon) so a
// spinning Psychosis Gun doesn't undercut the "Nokia 3315" bit. Pass nil/empty
// to restore everything.
s32 chraiLuaWeaponRename(s32 weaponnum, const char *name)
{
	extern s32 g_ChaosLangOverrideId;
	extern s32 g_ChaosLangOverrideId2;
	extern s32 g_ChaosRenamedWeapon;
	extern char g_ChaosLangOverrideStr[64];

	if (name == NULL || name[0] == '\0') {
		g_ChaosLangOverrideId = -1;
		g_ChaosLangOverrideId2 = -1;
		g_ChaosRenamedWeapon = -1;
		return 1;
	}
	g_ChaosLangOverrideId = (s32)bgunGetNameId(weaponnum);
	g_ChaosLangOverrideId2 = (s32)bgunGetShortNameId(weaponnum);
	g_ChaosRenamedWeapon = weaponnum;
	{
		s32 i;
		for (i = 0; i < (s32)sizeof(g_ChaosLangOverrideStr) - 1 && name[i]; i++) {
			g_ChaosLangOverrideStr[i] = name[i];
		}
		g_ChaosLangOverrideStr[i] = '\0';
	}
	return 1;
}

// pd.chr_speed(mult): scale every non-player chr's anim playback (movement +
// attack cadence ride along). 1 = off. chr.c consumes it in chr0f0220ec.
s32 chraiLuaChrSpeed(f32 mult)
{
	extern f32 g_ChaosChrSpeedMult;

	if (mult < 0.1f) mult = 0.1f;
	if (mult > 8.0f) mult = 8.0f;
	g_ChaosChrSpeedMult = mult;
	return 1;
}

// pd.player_speed(mult): scale the LOCAL player's real walk + strafe speed
// ("Gotta go fast"). 1 = normal. Consumed in bwalkApplyMoveData (bondwalk.c) —
// a genuine movement multiplier, unlike the Combat Boost (bullet-time + 1.25x).
s32 chraiLuaPlayerSpeed(f32 mult)
{
	extern f32 g_ChaosPlayerSpeed;

	if (mult < 0.1f) mult = 0.1f;
	if (mult > 5.0f) mult = 5.0f;
	g_ChaosPlayerSpeed = mult;
	return 1;
}

// pd.chr_damage(chrnum, amount): hurt any chr (or player pawn) through the
// real damage path — shield, flinch, death, kill credit as environment.
s32 chraiLuaChrDamage(s32 chrnum, f32 amount)
{
	struct chrdata *chr = chrFindByLiteralId(chrnum);
	struct coord vec = {0, 0, 1};

	// Skip chrs that are already dead or mid-death: re-running the damage/death
	// path on a corpse re-processes the kill and can corrupt the prop list
	// (the ->next-cycle hang family). Reproduced by Airstrike (kills chrs) then
	// The Snap (damages random chrs) hard-locking the game. Covers all
	// pd.chr_damage callers (snap, plague, ...).
	//
	// NEVER damage a PLAYER pawn: pd.all_chrs() includes the local player, so
	// The Snap/Plague could roll the player and kill Jo. When these effects are
	// fired FROM the Lua Director menu, the player-death path re-enters
	// menuPopDialog/menuClose mid-menu-tick and hangs the game (WAR crash). The
	// player is damaged only through the proper player paths, never here.
	if (apLuaPlayerChr() == NULL || chr == NULL || chr->prop == NULL
			|| chr->prop->type == PROPTYPE_PLAYER
			|| amount <= 0.0f || chrIsDead(chr)) {
		return 0;
	}
	chrDamageByMisc(chr, amount, &vec, NULL, NULL);
	return 1;
}

// pd.chr_scale(chrnum, mult): multiply a chr's model scale (visual size).
// Cosmetic — collision/eye height keep the original values, which is fine
// for a chaos gag. Callers undo with the inverse multiplier.
s32 chraiLuaChrScale(s32 chrnum, f32 mult)
{
	struct chrdata *chr = chrFindByLiteralId(chrnum);

	if (apLuaPlayerChr() == NULL || chr == NULL || chr->prop == NULL || chr->model == NULL) {
		return 0;
	}
	if (mult < 0.05f) mult = 0.05f;
	if (mult > 8.0f) mult = 8.0f;
	// Cosmetic uniform resize done ENTIRELY in the render matrix
	// (modelUpdateChrNodeMtx scales the composed root->world matrix by groundmult,
	// pivoting X/Z on the chr's vertical axis and Y on the GROUND so the feet stay
	// planted). This replaces the old model->scale + animscale approach, whose
	// pelvis-pivot skeleton spread floated/sank the feet. Collision/eye height keep
	// their original values. Accumulate so a caller's inverse-mult undo divides it
	// back toward 1.0; clamp the running value. chrInit resets it on a recycled slot.
	chr->groundmult *= mult;
	if (chr->groundmult < 0.05f) chr->groundmult = 0.05f;
	if (chr->groundmult > 8.0f) chr->groundmult = 8.0f;
	return 1;
}

// pd.shake(ticks): kick the explosion screen-shake for N ticks (~60/s).
extern s32 g_ExplosionShakeTotalTimer;
extern s32 g_ExplosionShakeIntensityTimer;
s32 chraiLuaShake(s32 ticks)
{
	if (ticks < 1) ticks = 1;
	if (ticks > 120) ticks = 120;
	g_ExplosionShakeTotalTimer = ticks;
	g_ExplosionShakeIntensityTimer = ticks;
	return 1;
}

// pd.screen_tint(r,g,b) / pd.screen_tint(): full-screen luminance tint via
// the grayscale shader path (the Midas gold mechanism with a custom colour).
extern s32 gfx_screen_tint;
s32 chraiLuaScreenTint(s32 r, s32 g, s32 b, s32 on)
{
	if (!on) {
		gfx_screen_tint = 0;
		return 1;
	}
	r = r < 0 ? 0 : r > 255 ? 255 : r;
	g = g < 0 ? 0 : g > 255 ? 255 : g;
	b = b < 0 ? 0 : b > 255 ? 255 : b;
	gfx_screen_tint = (r << 16) | (g << 8) | b;
	if (gfx_screen_tint == 0) {
		gfx_screen_tint = 1; // black tint, still distinct from "off"
	}
	return 1;
}

// pd.upside_down(on): "Australia mode" — rotate the whole finished frame 180
// via the retro post filter (renderer 1-byte bool; the wireframe gotcha), and
// reverse the controls game-side (g_ChaosControlReverse, bondmove.c). The old
// clip-space Y-flip (gfx_upsidedown_mode) left the HUD upright and inverted
// only vertical aim; the post rotation flips world + HUD together and, with the
// controls reversed, aim tracks the rotated view.
extern unsigned char gfx_rotate180_mode;
extern s32 g_ChaosControlReverse;
s32 chraiLuaUpsideDown(s32 on)
{
	gfx_rotate180_mode = on ? 1 : 0;
	g_ChaosControlReverse = on ? 1 : 0;
	return 1;
}

// pd.screen_roll(deg): "Speen" — rotate the 3D view about the screen centre
// by an absolute angle in DEGREES (0 = off/upright). The renderer applies an
// aspect-corrected clip-space rotation in gfx_sp_vertex (gfx_screen_roll,
// C++ float == f32); HUD texrects stay upright; bg.c gates dlcache off while
// non-zero. The caller animates by re-setting the angle each tick.
extern f32 gfx_screen_roll;
s32 chraiLuaScreenRoll(f32 deg)
{
	gfx_screen_roll = deg * (3.14159265f / 180.0f);
	return 1;
}

// pd.vertex_wobble(amp, freq, phase): "Jelly" — true on-the-fly vertex
// deformation. amp (world units) drives the eye-space displacement (0/absent =
// off); freq (radians per world unit) sets the ripple wavelength; phase is the
// animation angle, advanced by the caller each tick. Consumed in gfx_sp_vertex;
// gates dlcache off (bg.c) while amp != 0.
extern f32 gfx_vtx_wobble_amp;
extern f32 gfx_vtx_wobble_freq;
extern f32 gfx_vtx_wobble_phase;
extern f32 gfx_vtx_wobble_sag;
extern f32 gfx_vtx_wobble_desync;
extern f32 gfx_vtx_wobble_nearfade;
s32 chraiLuaVertexWobble(f32 amp, f32 freq, f32 phase, f32 sag, f32 desync, f32 nearfade)
{
	if (amp < 0.0f) {
		amp = 0.0f;
	}
	if (amp > 200.0f) {
		amp = 200.0f; // sanity clamp so a stray value can't fold the scene
	}
	if (sag < 0.0f) {
		sag = 0.0f;
	}
	if (sag > 200.0f) {
		sag = 200.0f;
	}
	if (desync < 0.0f) {
		desync = 0.0f;
	}
	if (desync > 4.0f) {
		desync = 4.0f; // beyond this the phase spread just looks like noise
	}
	gfx_vtx_wobble_amp = amp;
	gfx_vtx_wobble_freq = freq;
	gfx_vtx_wobble_phase = phase;
	gfx_vtx_wobble_sag = sag;
	if (nearfade < 0.0f) {
		nearfade = 0.0f; // 0 = off: uniform wobble at every distance
	}
	if (nearfade > 4000.0f) {
		nearfade = 4000.0f; // past this nothing in a normal room wobbles at all
	}
	gfx_vtx_wobble_desync = desync;
	gfx_vtx_wobble_nearfade = nearfade;
	return 1;
}

// pd.hall_of_mirrors(on): skip the framebuffer colour clear so the frame smears
// (Doom HOM / acid-trip trails). Consumed in gfx_pc.cpp; cleared in lvReset.
extern s32 gfx_hom_mode;
s32 chraiLuaHallOfMirrors(s32 on)
{
	gfx_hom_mode = on ? 1 : 0;
	return 1;
}

// pd.beyblade(on): "Bayblade!" — every non-player chr's model yaw spins at
// ~2 rev/s (absolute frame-derived stomp in chr0f0220ec, chr.c, so AI facing
// writes can't unwind it). Purely visual: AI, movement and aim keep running.
s32 chraiLuaBeyblade(s32 on)
{
	extern s32 g_ChaosBeyblade;

	g_ChaosBeyblade = on ? 1 : 0;
	return 1;
}

// pd.double_vision(on): "One too many" — blend a 180-flipped ghost of the
// finished frame over the normal frame (retro post filter; renderer 1-byte
// bool). Drunk double-vision: the world stays put and a flipped copy of the
// whole frame is overlaid at 50%. Pairs with pd.dizzy for the sway.
extern unsigned char gfx_doublevision_mode;
s32 chraiLuaDoubleVision(s32 on)
{
	gfx_doublevision_mode = on ? 1 : 0;
	return 1;
}

// pd.pixelate(w, h, colours): chunk the rendered frame down to a w x h pixel
// grid via the retro post filter (gfx_retro.cpp on GL, the gfx_sdlgpu.cpp
// retro section on Vulkan/D3D12), optionally applying a colour mode (2..64 =
// N-level greyscale, 256 = RGB 3-3-2, 1000 = invert, 1001 = Game Boy greens,
// 1002 = thermal palette). w = 0 with a colour set = colour mode at full
// resolution; everything <= 0 = off.
extern s32 gfx_retro_pixel_w;
extern s32 gfx_retro_pixel_h;
extern s32 gfx_retro_colors;
s32 chraiLuaPixelate(s32 w, s32 h, s32 colors)
{
	if (w > 0 && h > 0) {
		gfx_retro_pixel_w = w < 8 ? 8 : w > 1024 ? 1024 : w;
		gfx_retro_pixel_h = h < 8 ? 8 : h > 1024 ? 1024 : h;
	} else {
		gfx_retro_pixel_w = 0;
		gfx_retro_pixel_h = 0;
	}
	gfx_retro_colors = colors < 0 ? 0 : colors;
	return 1;
}

// pd.screen_fx(bits, on): set/clear retro post-filter effect bits (1 =
// scanlines, 2 = RGB grille, 4 = CRT curvature, 8 = vignette, 16 = VHS,
// 32 = underwater wobble, 1024 = side-by-side eyes). Bits compose, so
// simultaneous effects stack.
extern s32 gfx_retro_fx;
s32 chraiLuaScreenFx(s32 bits, s32 on)
{
	bits &= 0x43f;
	if (on) {
		gfx_retro_fx |= bits;
	} else {
		gfx_retro_fx &= ~bits;
	}
	return 1;
}

// pd.pirate(side): "Pirate" eyepatch — black out one half of the finished frame
// (post-process retro-fx bits 0x800 = left, 0x1000 = right), so the HUD in that
// half goes dark too. side 1 = left, 2 = right, anything else = off. Only one
// side is ever set at a time.
s32 chraiLuaPirate(s32 side)
{
	gfx_retro_fx &= ~(0x800 | 0x1000); // clear both pirate bits first
	if (side == 1) {
		gfx_retro_fx |= 0x800;  // black the LEFT half
	} else if (side == 2) {
		gfx_retro_fx |= 0x1000; // black the RIGHT half
	}
	return 1;
}

// pd.lens(k): fisheye lens warp on the rendered frame — centre magnified,
// corners pinned. 0 = off; negative = pincushion (clamped shy of the pole).
extern f32 gfx_retro_warp;
s32 chraiLuaLens(f32 k)
{
	gfx_retro_warp = k < -0.8f ? -0.8f : k > 4.0f ? 4.0f : k;
	return 1;
}

// pd.audio_crush(step, bits): sample-and-hold + bit-depth crush on the device
// stream (audio.c push point, the pd.mute mechanism). 1, 16 (or no args) = off.
extern void audioSetCrush(s32 step, s32 bits);
s32 chraiLuaAudioCrush(s32 step, s32 bits)
{
	audioSetCrush(step, bits);
	return 1;
}

// pd.audio_radio(on): AM-radio voicing (bandpass + overdrive) on all audio.
extern void audioSetRadio(s32 on);
s32 chraiLuaAudioRadio(s32 on)
{
	audioSetRadio(on ? 1 : 0);
	return 1;
}

// pd.audio_reverb(wet): cathedral reverb wash, wet 0..1; 0/no args = off.
extern void audioSetReverb(f32 wet);
s32 chraiLuaAudioReverb(f32 wet)
{
	audioSetReverb(wet);
	return 1;
}

// pd.audio_reverse(on): everything plays backwards in ~0.74s granules.
extern void audioSetReverse(s32 on);
s32 chraiLuaAudioReverse(s32 on)
{
	audioSetReverse(on ? 1 : 0);
	return 1;
}

// pd.audio_pitch(rate): pitch shift at constant tempo; 1 (or no args) = off.
extern void audioSetPitch(f32 rate);
s32 chraiLuaAudioPitch(f32 rate)
{
	audioSetPitch(rate);
	return 1;
}

// pd.weather(type, intensity): 0 off / 1 rain / 2 snow, on any stage.
s32 chraiLuaWeather(s32 type, s32 intensity)
{
	if (apLuaPlayerChr() == NULL) {
		return 0;
	}
	return weatherChaosSet(type, intensity);
}

// pd.gas(on): the Investigation nerve gas anywhere — green env wash (fog
// stages), coughing, positional hiss, damage every ~3.75s (gasTick).
s32 chraiLuaGas(s32 on)
{
	if (apLuaPlayerChr() == NULL) {
		return 0;
	}
	gasChaosSet(on ? 1 : 0);
	return 1;
}

// pd.t_pose(on): every skeletal model renders in its bind pose (anim.c joint
// rotations read as zero). Root motion still applies — T-posers glide.
s32 chraiLuaTPose(s32 on)
{
	extern s32 g_ChaosTPose;

	g_ChaosTPose = on ? 1 : 0;
	return 1;
}

// pd.chr_ko(chrnum): knock a chr out via the tranquiliser's sanctioned
// knockout path (chrBeginDeath with knockout=true -> ACT_DRUGGEDDROP).
// The chr collapses and drops its weapon. Engine knockouts are PERMANENT —
// chrTickDruggedKo only fades/reaps the body, and a reaped chr reads as
// eliminated to mission scripts (aiIfChrDead passes on !chr), failing
// protect objectives. So CHRCFLAG_KEEPCORPSEKO is set to park the body
// un-reaped until chraiLuaChrWake recovers it. The KO counter is NOT
// incremented (nap KOs must not trip aiIfNumKnockedOutChrs branches or
// exhaust the first-two-KOs KEEPCORPSEKO budget in chrKnockOut); the
// matching decrement-below-zero hazard is clamped in mpstats.c.
// NPCs only; dead/already-KO'd chrs skip.
s32 chraiLuaChrKo(s32 chrnum)
{
	struct chrdata *chr = chrFindByLiteralId(chrnum);
	struct coord dir = {0, 0, 1};
	struct gset gset = {0};

	if (apLuaPlayerChr() == NULL || chr == NULL || chr->prop == NULL || chr->model == NULL) {
		return 0;
	}
	if (chr->prop->type != PROPTYPE_CHR || chr->aibot) {
		return 0;
	}
	if (chrIsDead(chr) || chr->actiontype == ACT_DIE || chr->actiontype == ACT_DEAD
			|| chr->actiontype == ACT_DRUGGEDDROP || chr->actiontype == ACT_DRUGGEDKO
			|| chr->actiontype == ACT_DRUGGEDCOMINGUP) {
		return 0;
	}

	gset.weaponnum = WEAPON_TRANQUILIZER;
	gset.weaponfunc = FUNC_SECONDARY;
	// HITPART_TORSO, not HITPART_GENERAL: GENERAL (200) isn't in
	// g_AnimTablesByRace, so chrBeginDeath's Skedar branch fell through to
	// table entry 0 whose deathanims is NULL -> NULL row deref (crash at
	// chraction.c row->thudframe1). TORSO exists in every race's table.
	chrBeginDeath(chr, &dir, 0.0f, HITPART_TORSO, &gset, true, -1);
	chr->chrflags |= CHRCFLAG_KEEPCORPSEKO;
	return 1;
}

// pd.chr_wake(chrnum): recover a chr from the tranquiliser knockout chain.
// Uses the engine's own knockdown recovery (the ANIM_DEATH_STOMACH_LONG
// path in chrTickArgh): a 26-tick blend back to standing via
// func0f02ed28, after which normal AI resumes. Any drugged stage
// (coming-up / falling / on the floor) can be woken. The chr comes up
// unarmed — the drop scattered their weapons.
s32 chraiLuaChrWake(s32 chrnum)
{
	struct chrdata *chr = chrFindByLiteralId(chrnum);

	if (apLuaPlayerChr() == NULL || chr == NULL || chr->prop == NULL || chr->model == NULL) {
		return 0;
	}
	if (chr->prop->type != PROPTYPE_CHR || chr->aibot) {
		return 0;
	}
	if (chr->actiontype != ACT_DRUGGEDCOMINGUP && chr->actiontype != ACT_DRUGGEDDROP
			&& chr->actiontype != ACT_DRUGGEDKO) {
		return 0;
	}
	if (chr->hidden & CHRHFLAG_DELETING) {
		return 0;
	}

	chr->chrflags &= ~CHRCFLAG_KEEPCORPSEKO;
	chr->fadealpha = 255;
	chrRecordLastSeeTargetTime(chr);
	func0f02ed28(chr, 26);
	return 1;
}

// pd.room_tint(r,g,b) / pd.room_tint(): tint every room's lighting by an RGB
// multiplier (0..255 per channel = 0..1x) — the KotH hill-highlight effect
// applied stage-wide. Dirties all rooms so the reshade re-runs; rooms
// recompute as they come on screen.
s32 chraiLuaRoomTint(s32 r, s32 g, s32 b, s32 on)
{
	s32 i;

	if (apLuaPlayerChr() == NULL) {
		return 0;
	}
	g_ChaosRoomTintFrac[0] = (r < 0 ? 0 : r > 255 ? 255 : r) * (1.0f / 255.0f);
	g_ChaosRoomTintFrac[1] = (g < 0 ? 0 : g > 255 ? 255 : g) * (1.0f / 255.0f);
	g_ChaosRoomTintFrac[2] = (b < 0 ? 0 : b > 255 ? 255 : b) * (1.0f / 255.0f);
	g_ChaosRoomTintOn = on ? 1 : 0;

	for (i = 1; i < g_Vars.roomcount; i++) {
		g_Rooms[i].flags |= ROOMFLAG_BRIGHTNESS_DIRTY_TEMP;
	}
	return 1;
}

// pd.spawn_ally(): spawn a friendly "Perfect Buddy" that fights alongside the
// player. Mirrors the campaign buddy spawn (player.c) -- TEAM_ALLY + a buddy
// ailist; solo allegiance is the bitwise chrCompareTeams test, so it targets
// enemies and won't shoot the player. Returns the new chrnum, or -1.
s32 chraiLuaSpawnAlly(void)
{
	struct prop *prop;
	struct chrdata *chr;

	if (apLuaPlayerChr() == NULL) {
		return -1;
	}

	prop = chrSpawnAtCoord(BODY_DARK_COMBAT, HEAD_VD,
			&g_Vars.currentplayer->prop->pos,
			g_Vars.currentplayer->prop->rooms,
			BADDEG2RAD(g_Vars.currentplayer->vv_theta / 2),
			ailistFindById(GAILIST_INIT_DEFAULT_BUDDY),
			SPAWNFLAG_ALLOWONSCREEN);

	if (prop == NULL || prop->chr == NULL) {
		return -1;
	}

	chr = prop->chr;
	chr->flags |= CHRFLAG0_SKIPSAFETYCHECKS;
	chr->team = TEAM_ALLY;
	chr->squadron = SQUADRON_01;
	chr->hidden |= CHRHFLAG_DETECTED;
	chr->voicebox = VOICEBOX_FEMALE;
	chr->teamscandist = 50;
	chr->accuracyrating = 100;
	chr->speedrating = 100;
	chrAddHealth(chr, 20);
	chrSetMaxDamage(chr, 4);
	chr->chrflags |= CHRCFLAG_NEVERSLEEP;
	chrGiveWeapon(chr, MODEL_CHRFALCON2, WEAPON_FALCON2, 0);

	return chr->chrnum;
}

// pd.spawn_ally_clone([healthfrac]) -> chrnum or -1. The spawn_ally recipe, but
// the buddy wears the PLAYER's own body AND head — a friendly Jo clone that
// fights on TEAM_ALLY beside you — and her health pool is scaled by healthfrac
// (default 0.5 = a fragile half-HP clone). Backs the "Me and my son" chaos
// effect. Server/solo-side (a net client never spawns AI).
s32 chraiLuaSpawnAllyClone(f32 healthfrac, f32 yscale)
{
	struct prop *prop;
	struct chrdata *chr;
	struct chrdata *plchr = apLuaPlayerChr();
	s32 bodynum;
	s32 headnum;
	f32 maxdmg;

	if (plchr == NULL || g_NetMode == NETMODE_CLIENT) {
		return -1;
	}

	if (healthfrac <= 0.0f) {
		healthfrac = 0.5f;
	}
	if (healthfrac > 1.0f) {
		healthfrac = 1.0f;
	}

	// Use the player's own body/head so the clone actually looks like Jo. Force
	// the body's model file resident first (the spawn path only loads on demand
	// and bails silently otherwise) — mirrors chraiLuaSpawnBody.
	bodynum = plchr->bodynum;
	headnum = plchr->headnum;
	if (bodynum >= 0 && bodynum < 152) {
		bodyLoad(bodynum);
	}

	prop = chrSpawnAtCoord(bodynum, headnum,
			&g_Vars.currentplayer->prop->pos,
			g_Vars.currentplayer->prop->rooms,
			BADDEG2RAD(g_Vars.currentplayer->vv_theta / 2),
			ailistFindById(GAILIST_INIT_DEFAULT_BUDDY),
			SPAWNFLAG_ALLOWONSCREEN);

	if (prop == NULL || prop->chr == NULL) {
		return -1;
	}

	chr = prop->chr;
	chr->flags |= CHRFLAG0_SKIPSAFETYCHECKS;
	chr->team = TEAM_ALLY;
	chr->squadron = SQUADRON_01;
	chr->hidden |= CHRHFLAG_DETECTED;
	chr->voicebox = VOICEBOX_FEMALE;
	chr->teamscandist = 50;
	chr->accuracyrating = 100;
	chr->speedrating = 100;
	// A standard buddy is maxdamage 4 + 20 armour (~24 effective). Scale the
	// whole pool by healthfrac so the clone dies that much sooner. chrAddHealth
	// subtracts from chr->damage (negative damage = armour buffer).
	maxdmg = 4.0f * healthfrac;
	if (maxdmg < 1.0f) {
		maxdmg = 1.0f;
	}
	chrSetMaxDamage(chr, maxdmg);
	chrAddHealth(chr, 20.0f * healthfrac);
	chr->chrflags |= CHRCFLAG_NEVERSLEEP;
	chrGiveWeapon(chr, MODEL_CHRFALCON2, WEAPON_FALCON2, 0);

	// Apply the vertical squash HERE, directly on the chr we just spawned, rather
	// than leaving it to a follow-up pd.chr_yscale(chrnum, ...) — that has to
	// re-find the chr by the returned chrnum, and the squat "Me and my son" clone
	// was rendering full height because that separate set wasn't landing. Same
	// clamp/semantics as chraiLuaChrYscale; <= 0 or 1.0 leaves the default.
	if (yscale > 0.0f && yscale != 1.0f) {
		if (yscale > 4.0f) {
			yscale = 4.0f;
		}
		chr->yscale = yscale;
	}

	return chr->chrnum;
}

// pd.chr_yscale(chrnum, mult): non-uniform VERTICAL squash/stretch. Unlike
// chr_scale (uniform), this scales only the model's local up axis, so mult 0.4
// makes a chr 40% as tall while keeping full width/depth (a squat, wide look).
// Stored on chr->yscale and applied every frame in modelUpdateChrNodeMtx.
// mult <= 0 resets to 1.0 (no squash); the render side additionally caps it at
// 4.0. Purely visual (hitbox/AI unchanged). Returns 1 on success, else 0.
s32 chraiLuaChrYscale(s32 chrnum, f32 mult)
{
	struct chrdata *chr = (chrnum < 0) ? NULL : chrFindByLiteralId(chrnum);

	if (apLuaPlayerChr() == NULL || chr == NULL) {
		return 0;
	}
	if (mult <= 0.0f) {
		mult = 1.0f;
	}
	if (mult > 4.0f) {
		mult = 4.0f;
	}
	chr->yscale = mult;
	return 1;
}

// ---- Toolkit framework bridges (all-actor iteration + per-chr mutators) ----
// These back pd.all_chrs / pd.chr_anim / pd.chr_set_shield / pd.chr_alert. They
// are thin wrappers over existing engine setters so the toolkit is easy to grow:
// adding a new mass-effect primitive = one wrapper here + one pd.* in luaai_api.c.
// All are server-side (world/AI mutation must not run on a net client).

s32 chraiLuaGetChrSlotCount(void)
{
	return chrsGetNumSlots();
}

s32 chraiLuaGetChrNumBySlot(s32 slot)
{
	if (slot < 0 || slot >= chrsGetNumSlots()) {
		return -1;
	}
	return (s32)g_ChrSlots[slot].chrnum; // < 0 for an empty slot; caller skips it
}

s32 chraiLuaChrAnim(s32 chrnum, s32 animnum, f32 speed)
{
	struct chrdata *chr;

	if (g_NetMode == NETMODE_CLIENT) {
		return 0;
	}
	chr = (chrnum < 0) ? NULL : chrFindByLiteralId(chrnum);
	if (chr == NULL || chr->model == NULL) {
		return 0;
	}
	// flip 0, start frame 0, given speed, short merge for a smooth cut-in.
	modelSetAnimation(chr->model, (s16)animnum, 0, 0, speed, 16);
	return 1;
}

s32 chraiLuaChrSetShield(s32 chrnum, f32 value)
{
	struct chrdata *chr;

	if (g_NetMode == NETMODE_CLIENT) {
		return 0;
	}
	chr = (chrnum < 0) ? NULL : chrFindByLiteralId(chrnum);
	if (chr == NULL) {
		return 0;
	}
	// NPCs only — the player's shield is a separate system (pd.player_set_shield).
	// Without this, "Shielded enemies" (all_chrs loop) also shielded Jo.
	if (chr->prop && chr->prop->type == PROPTYPE_PLAYER) {
		return 0;
	}
	chrSetShield(chr, value);
	return 1;
}

s32 chraiLuaChrAlert(s32 chrnum)
{
	struct chrdata *chr;
	struct chrdata *pl;

	if (g_NetMode == NETMODE_CLIENT) {
		return 0;
	}
	chr = (chrnum < 0) ? NULL : chrFindByLiteralId(chrnum);
	if (chr == NULL || chrIsDead(chr)) {
		return 0;
	}
	// Same flag the damage path sets to make a chr switch to its shot/alert
	// list — but the flag alone reproduces only PART of being shot: chrDamage
	// also raises alertness and identifies the attacker. Without those the
	// shot list ran with no target and settled straight back to idle (the
	// PANIC! visible-no-op bug, fixed 2026-07-29).
	chr->chrflags |= CHRCFLAG_TRIGGERSHOTLIST;
	chr->alertness = 100;

	pl = apLuaPlayerChr();
	if (pl != NULL && chr != pl
			&& (chr->team & TEAM_NONCOMBAT) == 0
			&& chrCompareTeams(pl, chr, COMPARE_ENEMIES)) {
		// Hostile combatants learn WHO to hunt; civilians/allies just panic
		// (never force targets onto non-combatants — the civil-war lesson).
		chr->target = propGetIndexByChrId(chr, pl->chrnum);
	}
	return 1;
}

// Swap a live actor's body model at runtime ("turn everyone into X") -- the most
// invasive toolkit primitive. Rebuilds the chr's model from a new bodynum and
// re-establishes the chr<->model links by reusing the engine's own spawn wiring
// (chr0f020b14) + teardown (free vertices/model, detach held weapons). Captures
// and re-gives held weapons to the new skeleton. Solo/missions only (refused in
// Combat Sim: bodynum is only synced at stage start, so a runtime swap wouldn't
// replicate); player props refused; server-side. Returns 1 on success, else 0.
s32 chraiLuaChrSetBody(s32 chrnum, s32 bodynum, s32 headnum)
{
	struct chrdata *chr;
	struct model *oldmodel;
	struct model *newmodel;
	f32 faceangle;
	s32 h;
	s32 heldweapon[2];

	if (g_NetMode == NETMODE_CLIENT) {
		return 0;
	}
	if (g_Vars.normmplayerisrunning) {
		return 0; // Combat Sim: not supported (no runtime bodynum sync)
	}
	chr = (chrnum < 0) ? NULL : chrFindByLiteralId(chrnum);
	if (chr == NULL || chr->prop == NULL || chr->model == NULL) {
		return 0;
	}
	if (chr->prop->type == PROPTYPE_PLAYER) {
		return 0; // never swap the player's own body model
	}
	if (chrIsDead(chr) || chr->actiontype == ACT_DIE) {
		return 0; // mid-death model state is fragile
	}

	for (h = 0; h < 2; h++) {
		heldweapon[h] = (chr->weapons_held[h] && chr->weapons_held[h]->obj
				&& chr->weapons_held[h]->obj->type == OBJTYPE_WEAPON
				&& chr->weapons_held[h]->weapon)
			? (s32)chr->weapons_held[h]->weapon->weaponnum : -1;
	}

	if (headnum < 0) {
		headnum = bodyChooseHead(bodynum);
	}

	newmodel = bodyAllocateModel(bodynum, headnum, 0);
	if (newmodel == NULL) {
		return 0;
	}

	oldmodel = chr->model;
	faceangle = chrGetRotY(chr);

	for (h = 0; h < 3; h++) {
		if (chr->weapons_held[h]) {
			if (chr->weapons_held[h]->obj) {
				chr->weapons_held[h]->obj->hidden |= OBJHFLAG_DELETING;
			}
			chr->weapons_held[h] = NULL;
		}
	}

	chr0f020b14(chr->prop, newmodel, &chr->prop->pos, chr->prop->rooms, faceangle, chr->ailist);

	modelFreeVertices(VTXSTORETYPE_CHRVTX, oldmodel);
	modelmgrFreeModel(oldmodel);

	chr->bodynum = bodynum;
	chr->headnum = headnum;

	for (h = 0; h < 2; h++) {
		if (heldweapon[h] >= 0) {
			s32 modelnum = playermgrGetModelOfWeapon(heldweapon[h]);
			if (modelnum >= 0) {
				u32 flags = (h == 1) ? OBJFLAG_WEAPON_LEFTHANDED : 0;
				chrGiveWeapon(chr, modelnum, heldweapon[h], flags);
			}
		}
	}

	return 1;
}

// Move a chr prop to a position without physics (used by the possession freecam
// to keep the visible cube under the fly pose). Mirrors how eyespy moves its
// prop: chr0f021fa8 + modelSetRootPosition. Server-side. Returns 1 on success.
s32 chraiLuaSetChrPos(s32 chrnum, f32 x, f32 y, f32 z)
{
	struct chrdata *chr;
	struct coord pos;

	if (g_NetMode == NETMODE_CLIENT) {
		return 0;
	}
	chr = (chrnum < 0) ? NULL : chrFindByLiteralId(chrnum);
	if (chr == NULL || chr->prop == NULL) {
		return 0;
	}
	pos.x = x;
	pos.y = y;
	pos.z = z;
	chr0f021fa8(chr, &pos, chr->prop->rooms);
	if (chr->model) {
		modelSetRootPosition(chr->model, &pos);
	}
	return 1;
}

// Spawn a "cube" entity (BODY_EYESPY model -- a small controllable device) at
// the player's location and possess it (free-fly). Solo/missions only. Returns
// the spawned chrnum, or -1 on failure. Backs pd.possess_spawn().
s32 chraiLuaPossessSpawn(s32 bodynum)
{
	struct player *pl;
	struct model *model;
	struct prop *prop;
	RoomNum rooms[8];
	struct coord pos;

	if (g_NetMode == NETMODE_CLIENT || g_Vars.normmplayerisrunning) {
		return -1;
	}
	pl = g_Vars.players ? g_Vars.players[0] : NULL;
	if (pl == NULL || pl->prop == NULL) {
		return -1;
	}

	if (bodynum < 0) {
		bodynum = BODY_EYESPY;
	}

	model = bodyAllocateModel(bodynum, bodyChooseHead(bodynum), 0);
	if (model == NULL) {
		return -1;
	}

	pos = pl->prop->pos;
	pos.y += 60.f; // float a touch above the player
	roomsCopy(pl->prop->rooms, rooms);

	prop = chrAllocate(model, &pos, rooms, 0.0f, NULL);
	if (prop == NULL || prop->chr == NULL) {
		return -1;
	}
	propActivate(prop);
	propEnable(prop);
	prop->chr->chrflags |= CHRCFLAG_INVINCIBLE;

	if (!luaPossessBegin((s32)prop->chr->chrnum)) {
		return -1;
	}
	return (s32)prop->chr->chrnum;
}

// Stop possessing and return control to the player body. Frees the cube prop.
void chraiLuaUnpossess(void)
{
	s32 chrnum = luaPossessGetChrNum();
	luaPossessEnd();
	if (chrnum >= 0) {
		struct chrdata *chr = chrFindByLiteralId(chrnum);
		if (chr && chr->prop) {
			chr->hidden |= CHRHFLAG_DELETING; // remove the cube
		}
	}
}
#endif

bool chrDropItem(struct chrdata *chr, u32 modelnum, u32 weaponnum)
{
	struct weaponobj *weapon;
	u8 race = CHRRACE(chr);

	if (race == RACE_DRCAROLL || race == RACE_ROBOT) {
		return false;
	}

	weapon = weaponCreateProjectileFromWeaponNum(modelnum, (u8)weaponnum, chr);

	if (weapon && weapon->base.prop) {
		modelSetScale(weapon->base.model, weapon->base.model->scale);
		propReparent(weapon->base.prop, chr->prop);
		weapon->timer240 = TICKS(720);
		objSetDropped(weapon->base.prop, DROPTYPE_DEFAULT);
		chr->hidden |= CHRHFLAG_DROPPINGITEM;

		return true;
	}

	return false;
}

void chrPunchInflictDamage(struct chrdata *chr, s32 damage, s32 range, u8 reverse)
{
	struct prop *targetprop = chrGetTargetProp(chr);
	struct gset gset = {WEAPON_UNARMED, 0, 0, FUNC_PRIMARY};
	struct coord vector;

	if (chr->aibot) {
		gset.weaponnum = chr->aibot->weaponnum;
		gset.weaponfunc = chr->aibot->gunfunc;
	}

	if (chrIsTargetInFov(chr, 20, reverse)
			&& chrGetDistanceToTarget(chr) < range
			&& cdTestLos04(&chr->prop->pos, chr->prop->rooms, &targetprop->pos,
				CDTYPE_OBJS | CDTYPE_DOORS | CDTYPE_PATHBLOCKER | CDTYPE_BG)) {
		vector.x = targetprop->pos.x - chr->prop->pos.x;
		vector.y = 0;
		vector.z = targetprop->pos.z - chr->prop->pos.z;

		guNormalize(&vector.x, &vector.y, &vector.z);

		bgunPlayPropHitSound(&gset, targetprop, -1);

		if (targetprop->type == PROPTYPE_PLAYER || targetprop->type == PROPTYPE_CHR) {
			chrDamageByImpact(targetprop->chr, gsetGetDamage(&gset) * damage, &vector, &gset, chr->prop, 200);
		}
	}

	weaponPlayWhooshSound(gset.weaponnum, chr->prop);
}

struct punchanim {
	s32 animnum;
	u32 damage;
	s16 hitframe;
	f32 endframe;
};

struct punchanim g_HumanPunchAnims[] = {
	{ 0x027c, 5, 20, 60 },
	{ 0x027d, 5, 20, 31 },
	{ 0x027e, 5, 20, 48 },
	{ 0x027f, 5, 20, 69 },
	{ 0x0212, 5, 20, 64 },
	{ 0x0213, 5, 20, 52 },
	{ 0x0214, 5, 20, 51 },
	{ 0x020e, 5, 20, 53 },
	{ 0x020f, 5, 20, 89 },
	{ 0x0210, 5, 20, 71 },
	{ 0x0215, 5, 20, 62 },
	{ 0x0211, 5, 20, 72 },
};

struct punchanim g_SkedarPunchAnims[] = {
	{ 0x034c, 15, 25, 100 },
	{ 0x034d, 15, 25, -1  },
	{ 0x0395, 15, 25, -1  },
	{ 0x0346, 15, 25, -1  },
	{ 0x0347, 15, 25, -1  },
	{ 0x034f, 15, 25, -1  },
};

/**
 * Make the chr try to punch or kick.
 *
 * The function is only ever called with reverse = 0. If non-zero, it would
 * cause Skedar to kick behind them.
 *
 * Note that the final human anim can't be used because the modulus value is too
 * short by one.
 */
bool chrTryPunch(struct chrdata *chr, u8 reverse)
{
	struct punchanim *anims = NULL;
	s32 race = CHRRACE(chr);
	s32 animindex;
	u32 chranimflags = (rngRandom() % 256 > 128) ? CHRANIMFLAG_FLIP : 0;
	s32 chrhitradius;
	s32 playerhitradius;
	f32 startframe;
	bool ok;

	if (race == RACE_HUMAN) {
		anims = g_HumanPunchAnims;
		startframe = 10;
		chrhitradius = 120;
		playerhitradius = 120;
		animindex = rngRandom() % 11;
	} else if (race == RACE_SKEDAR) {
		anims = g_SkedarPunchAnims;
		chrhitradius = 200;
#if VERSION >= VERSION_NTSC_1_0
		playerhitradius = 200;
#else
		playerhitradius = 350;
#endif
		startframe = 20;

		if (reverse) {
			// Skedar kick behind
			animindex = 5;
		} else if (!chr->weapons_held[HAND_RIGHT] && !chr->weapons_held[HAND_LEFT]) {
			// Unarmed: Only use indexes 0 or 1
			animindex = rngRandom() % 2;
		} else {
			// Allow indexes 0-4, but if 3 or 4 then flip the anim based on
			// which hand is holding the gun
			animindex = rngRandom() % 5;

			if (animindex >= 3) {
				if (!chr->weapons_held[HAND_RIGHT] || !chr->weapons_held[HAND_LEFT]) {
					if (chr->weapons_held[HAND_RIGHT]) {
						chranimflags = 0;
					} else if (chr->weapons_held[HAND_LEFT]) {
						chranimflags = CHRANIMFLAG_FLIP;
					}
				}
			}
		}
	}

	if (chrHasFlag(chr, CHRFLAG1_ADJUSTPUNCHSPEED, BANK_1)) {
		if (chrHasFlag(chr, CHRFLAG0_CHUCKNORRIS, BANK_0)) {
			// Fast punch
			ok = chrTryStartAnim(chr, anims[animindex].animnum, startframe, anims[animindex].endframe, chranimflags, 16, 1.5f);
		} else {
			// Slow punch
			ok = chrTryStartAnim(chr, anims[animindex].animnum, startframe, anims[animindex].endframe - 25, chranimflags, 16, 0.5f);
		}

		chr->dodgerating = 0;
	} else {
		// Normal punch
		ok = chrTryStartAnim(chr, anims[animindex].animnum, startframe, anims[animindex].endframe, chranimflags, 16, 0.85f);
		chr->dodgerating = 0;
	}

	if (ok) {
		struct prop *targetprop = chrGetTargetProp(chr);

#ifndef PLATFORM_N64
		{
			// Chaos "Pacifists": an NPC punch/kick counts like a gun
			// discharge (weaponnum UNARMED marks it as melee).
			extern void luaEmitChrFire(s32 chrnum, s32 weaponnum);
			luaEmitChrFire(chr->chrnum, WEAPON_UNARMED);
		}
#endif

		if (targetprop->type == PROPTYPE_EYESPY || targetprop->type == PROPTYPE_PLAYER) {
			chr->act_anim.hitradius = playerhitradius;
		} else {
			chr->act_anim.hitradius = chrhitradius;
		}

		chr->act_anim.ishitanim = true;
		chr->act_anim.hitframe = anims[animindex].hitframe;

		if (g_Vars.normmplayerisrunning) {
			chr->act_anim.hitdamage = 1;
		} else if (chrHasFlag(chr, CHRFLAG1_ADJUSTPUNCHSPEED, BANK_1) && chrHasFlag(chr, CHRFLAG0_CHUCKNORRIS, BANK_0)) {
			chr->act_anim.hitdamage = (u16)anims[animindex].damage * (f32)chr->morale + (u16)anims[animindex].damage * (f32)chr->morale;
		} else if (chrHasFlag(chr, CHRFLAG1_PUNCHHARDER, BANK_1)) {
			chr->act_anim.hitdamage = (u16)anims[animindex].damage * 6;
		} else {
			chr->act_anim.hitdamage = anims[animindex].damage;
		}

		chr->act_anim.reverse = reverse;
		chr->chrflags &= ~CHRCFLAG_INJUREDTARGET;

		return true;
	}

	return false;
}

void func0f03c03c(void)
{
	// empty
}

/**
 * Verifies that the given chr struct is actually an eyespy and returns the
 * eyespy struct.
 *
 * Eyespys have their own chr struct, even though they aren't a chr. Iterating
 * the player list is required because the only pointer to an eyespy is via the
 * player struct.
 */
struct eyespy *chrToEyespy(struct chrdata *chr)
{
	if (chr && chr->prop) {
		if (CHRRACE(chr) == RACE_EYESPY) {
			s32 playercount = PLAYERCOUNT();
			s32 i;

			for (i = 0; i < playercount; i++) {
				if (g_Vars.players[i]->eyespy && chr->prop == g_Vars.players[i]->eyespy->prop) {
					return g_Vars.players[i]->eyespy;
				}
			}

			return NULL;
		}
	}

	return NULL;
}

void chrTickStand(struct chrdata *chr)
{
	s32 race;
	s32 i;
	s32 j;
	s32 stack2;
	struct prop *leftgun;
	struct prop *rightgun;
	u32 stack[2];
	f32 angle;
	f32 sp74[8];
	f32 sp70;
	f32 sp6c;
	s32 tmp;
	s32 index;
	s32 sp44[8];

	if (chr->hidden & CHRHFLAG_NEEDANIM) {
		if (modelIsAnimMerging(chr->model)) {
			return;
		}

		chrChooseStandAnimation(chr, chr->act_stand.mergetime);
		chr->hidden &= ~CHRHFLAG_NEEDANIM;
	}

	if (modelGetAnimNum(chr->model) == ANIM_SNIPING_GETUP) {
		if (modelGetCurAnimFrame(chr->model) >= modelGetAnimEndFrame(chr->model)) {
			chrChooseStandAnimation(chr, 8);
			chr->act_stand.prestand = 0;
		}

		return;
	}

	if (chr->sleep > 0) {
		return;
	}

	race = CHRRACE(chr);

	if (race == RACE_EYESPY) {
		return;
	}

	if (chr->act_stand.prestand) {
		if (modelGetCurAnimFrame(chr->model) >= modelGetAnimEndFrame(chr->model)) {
			chrChooseStandAnimation(chr, 8);
			chr->act_stand.prestand = 0;
		}

		chr->sleep = 0;
		return;
	}

	if (!chr->aibot && (race == RACE_HUMAN || race == RACE_SKEDAR) && chr->act_stand.flags > 0) {
		if (chr->act_stand.reaim) {
			chr->act_stand.turning = chrTurn(chr, chr->act_stand.turning, modelGetNumAnimFrames(chr->model) - 1, 1, 0);

			if (chr->act_stand.turning != TURNSTATE_TURNING) {
				chrChooseStandAnimation(chr, 8);
				chr->act_stand.reaim = false;

				if (chr->act_stand.flags & ATTACKFLAG_AIMATDIRECTION) {
					chr->act_stand.flags = 0;
				}
			}
		} else {
			f32 relangle = chrGetAttackEntityRelativeAngle(chr, chr->act_stand.flags, chr->act_stand.entityid);

			if ((relangle > 0.34901028871536f && relangle < 5.9331746101379f)
					|| (relangle > 0.17450514435768f && relangle < 6.1076798439026f && !chr->act_stand.playwalkanim)) {
				leftgun = chrGetHeldProp(chr, HAND_LEFT);
				rightgun = chrGetHeldProp(chr, HAND_RIGHT);

				chr->act_stand.reaim = true;
				chr->act_stand.turning = TURNSTATE_TURNING;

				if (race == RACE_HUMAN) {
					if ((leftgun && rightgun)
							|| (!leftgun && !rightgun)
							|| weaponIsOneHanded(leftgun)
							|| weaponIsOneHanded(rightgun)) {
						modelSetAnimation(chr->model, ANIM_006B, rngRandom() % 2, 0, 0.5f, 16);
						modelSetAnimEndFrame(chr->model, animGetNumFrames(ANIM_006B) - 1);
					} else {
						if (rightgun || leftgun) {
							modelSetAnimation(chr->model, ANIM_0028, leftgun != NULL, 0, 0.5f, 16);
							modelSetAnimEndFrame(chr->model, animGetNumFrames(ANIM_0028) - 1);
						}
					}
				} else if (race == RACE_SKEDAR) {
					modelSetAnimation(chr->model, ANIM_0392, rngRandom() % 2, 0, 0.5f, 16);
					modelSetAnimEndFrame(chr->model, animGetNumFrames(ANIM_0392) - 1);
				}
			} else if (chr->act_stand.flags & ATTACKFLAG_AIMATDIRECTION) {
				chr->act_stand.flags = 0;
			}
		}

		chr->sleep = 0;
		return;
	}

	if (chr->aibot) {
		return;
	}

	if (chr->prop->flags & PROPFLAG_ONANYSCREENPREVTICK) {
		chr->sleep = 0;
	} else {
		chr->sleep = 14 + (rngRandom() % 5);
	}

	if (chr->act_stand.checkfacingwall == false) {
		return;
	}

	if (chr->chrflags & CHRCFLAG_CANFACEWALL) {
		chr->act_stand.checkfacingwall = false;
		return;
	}

	chr->act_stand.wallcount -= chr->sleep;

	if (chr->act_stand.wallcount >= 0) {
		return;
	}

	sp6c = sp70 = chrGetInverseTheta(chr);

	for (i = 0; i < ARRAYCOUNT(sp74); i++) {
		sp6c += 0.7852731347084f;

		if (sp6c >= M_BADTAU) {
			sp6c -= M_BADTAU;
		}

		sp74[i] = func0f02e550(chr->prop, sp6c, 1000, CDTYPE_BG, 0, 1);
	}

	for (i = 0; i < ARRAYCOUNT(sp44); i++) {
		sp44[i] = i;
	}

	for (i = 0; i < ARRAYCOUNT(sp44) - 1; i++) {
		index = i;

		for (j = index + 1; j < ARRAYCOUNT(sp44); j++) {
			if (sp74[sp44[j]] < sp74[sp44[index]]) {
				index = j;
			}
		}

		j = sp44[i];
		sp44[i] = sp44[index];
		sp44[index] = j;
	}

	index = -1;

	if (sp74[0] < 490) {
		if (sp74[sp44[4]] < 200) {
			index = 7;
		} else if (sp44[0] == 0 || sp44[1] == 0 || sp44[2] == 0) {
			if ((sp44[3] == 4 || sp44[4] == 4) && (rngRandom() % 3) == 0) {
				if (sp44[3] == 4) {
					index = 3;
				} else {
					index = 4;
				}
			} else {
				index = 5 + rngRandom() % 3;
			}
		} else if ((sp44[0] == 1 || sp44[0] == 7) && sp44[5] && sp44[6] && sp44[7]) {
			index = 5 + rngRandom() % 3;
		}
	}

	if (index >= 0) {
		i = sp44[index];
		angle = sp70 + i * 0.7852731347084f;

		if (angle >= M_BADTAU) {
			angle -= M_BADTAU;
		}

		chrFaceEntity(chr, ATTACKFLAG_AIMATDIRECTION, angle * 10432.0390625f);
	} else {
		chr->act_stand.checkfacingwall = false;
	}
}

void chrTickKneel(struct chrdata *chr)
{
	chr->sleep = 0;

	if ((chr->hidden & CHRHFLAG_NEEDANIM) && !modelIsAnimMerging(chr->model)) {
		chrKneelChooseAnimation(chr);
		chr->hidden &= ~CHRHFLAG_NEEDANIM;
	}
}

void chrTickAnim(struct chrdata *chr)
{
	if (chr->hidden & CHRHFLAG_NEEDANIM) {
		if (modelIsAnimMerging(chr->model)) {
			return;
		}

		modelSetAnimation(chr->model, chr->act_anim.animnum, chr->act_anim.flip,
				chr->act_anim.startframe, chr->act_anim.speed, chr->act_anim.blend);

		if (chr->act_anim.endframe >= 0) {
			modelSetAnimEndFrame(chr->model, chr->act_anim.endframe);
		}

		chr->hidden &= ~CHRHFLAG_NEEDANIM;
	}

	if (!chr->act_anim.pauseatend && modelGetCurAnimFrame(chr->model) >= modelGetAnimEndFrame(chr->model)) {
		chrStand(chr);
	}

	if (chr->act_anim.ishitanim && modelGetCurAnimFrame(chr->model) >= (s32)chr->act_anim.hitframe) {
		chr->act_anim.ishitanim = false;
		chrPunchInflictDamage(chr, chr->act_anim.hitdamage, chr->act_anim.hitradius, chr->act_anim.reverse);
	}

	// Play sneezing sound
	if (CHRRACE(chr) == RACE_HUMAN
			&& modelGetAnimNum(chr->model) == ANIM_SNEEZE
			&& modelGetCurAnimFrame(chr->model) >= 42
			&& (g_Vars.lvframenum % 2) == 0
			&& chrGetDistanceToCurrentPlayer(chr) < 800) {
		psCreate(NULL, chr->prop, SFX_0037, -1,
				-1, 0, 0, PSTYPE_NONE, 0, -1, 0, -1, -1, -1, -1);
	}

	if (chr->sleep <= 0 && chr->act_anim.slowupdate) {
		chr->sleep = 14 + (rngRandom() % 5);
	}

	if (modelGetAnimNum(chr->model) == ANIM_RELOAD_0209) {
		chrSetFiring(chr, HAND_RIGHT, false);
		chrSetFiring(chr, HAND_LEFT, false);
	}
}

void chrTickSurrender(struct chrdata *chr)
{
	if (chr->hidden & CHRHFLAG_NEEDANIM) {
		if (modelIsAnimMerging(chr->model)) {
			return;
		}

		chrSurrenderChooseAnimation(chr);
		chr->hidden &= ~CHRHFLAG_NEEDANIM;
	}

	if (chr->sleep <= 0) {
		if (CHRRACE(chr) == RACE_HUMAN) {
			struct model *model = chr->model;
			chr->sleep = 16;

			if (modelGetAnimNum(model) == ANIM_SURRENDER_002F && modelGetCurAnimFrame(model) >= 80.0f) {
				struct coord coord = {0, 0, 0};
				f32 value = chrGetInverseTheta(chr);
				coord.x = -sinf(value);
				coord.z = -cosf(value);

				if (!propchrHasClearLineInVector(chr->prop, &coord, 20)) {
					modelSetAnimation(chr->model, ANIM_SURRENDER_002E, rngRandom() & 1, 30, 0.5, 16);
					modelSetAnimLooping(chr->model, 30, 16);
				}
			}
		}
	}
}

void chrFadeCorpse(struct chrdata *chr)
{
	if (chr->actiontype == ACT_DEAD || chr->actiontype == ACT_DRUGGEDKO) {
		chr->act_dead.fadenow = true;
	}
}

void chrFadeCorpseWhenOffScreen(struct chrdata *chr)
{
	if (chr->actiontype == ACT_DEAD) {
		chr->act_dead.fadewheninvis = true;
	}
}

void chrTickDead(struct chrdata *chr)
{
	struct aibot *aibot = chr->aibot;

	// If fade is active, handle it
	if (chr->act_dead.fadetimer60 >= 0) {
		chr->act_dead.fadetimer60 += g_Vars.lvupdate60;

		if (chr->act_dead.fadetimer60 >= TICKS(90)) {
			// Fade finished
			chr->fadealpha = 0;

			if (aibot) {
#ifndef PLATFORM_N64
				// Global Lives system: a bot out of lives stays down — leave
				// the (faded-out) corpse in place rather than deleting the
				// chr, so g_MpBotChrPtrs / g_MpAllChrPtrs stay valid.
				if (elimChrCanRespawn(chr))
#endif
				botSpawn(chr, true);
			} else {
				chr->hidden |= CHRHFLAG_DELETING;
			}
		} else {
			// Still fading
			chr->fadealpha = (TICKS(90) - chr->act_dead.fadetimer60) * 255 / TICKS(90);
		}
	} else {
		// If fade has been triggered (this can happen when the corpse is on
		// screen and there's lots of other chrs around)
		if (chr->act_dead.fadenow) {
			chr->act_dead.fadetimer60 = 0;
			chrDropItemsForOwnerReap(chr);
		}

		if (chr->prop->flags & PROPFLAG_ONANYSCREENPREVTICK) {
			// Keep corpse for now
			chr->act_dead.invistimer60 = 0;
		} else {
			chr->act_dead.invistimer60 += g_Vars.lvupdate60;
		}

		if (chr->act_dead.fadewheninvis && chr->act_dead.invistimer60 >= TICKS(120)) {
			// Remove corpse (off-screen)
			if (aibot == NULL) {
				chr->hidden |= CHRHFLAG_DELETING;
			}

			chr->fadealpha = 0;

			chrDropItemsForOwnerReap(chr);
		}
	}

	if (aibot == NULL) {
		chr->ailist = NULL;
	}
}

/**
 * This function is called when a chr is injured or killed. It iterates other
 * chrs within 2000 units who can see the chr and updates their chrseeshot or
 * chrseedie properties.
 *
 * The search stops once 7 chrs have been found in range, regardless of whether
 * they can see the chr, presumably to avoid doing too many expensive line of
 * sight checks in one tick. The last iterated chr index is stored and continued
 * from there next time the function is called. The function is called on
 * subsequent ticks while the chr is still in their injured or dying action.
 */
void chrAlertOthersOfInjury(struct chrdata *chr, bool dying)
{
	s32 index = 0;
	s32 numinrange = 0;
	s32 numchrs = chrsGetNumSlots();

	if (g_Vars.antiplayernum >= 0 && PROP_IS_FOR_ANTI_PLAYER(chr->prop)) {
		return;
	}

	if (chr->actiontype == ACT_ARGH) {
		index = chr->act_argh.notifychrindex;
	} else if (chr->actiontype == ACT_DIE || chr->actiontype == ACT_DRUGGEDDROP) {
		index = chr->act_die.notifychrindex;
	} else if (chr->actiontype == ACT_DEAD) {
		index = chr->act_dead.notifychrindex;
	}

#ifdef AVOID_UB
	// notifychrindex can be < 0, so avoid reading g_ChrSlots when that is the case
	for (; index >= 0 && index < numchrs && numinrange < 7; index++) {
#else
	for (; index < numchrs && numinrange < 7; index++) {
#endif
		struct chrdata *loopchr = &g_ChrSlots[index];

		if (loopchr->model && loopchr->prop && (loopchr->prop->flags & PROPFLAG_ENABLED)) {
			f32 xdiff = loopchr->prop->pos.x - chr->prop->pos.x;
			f32 ydiff = loopchr->prop->pos.y - chr->prop->pos.y;
			f32 zdiff = loopchr->prop->pos.z - chr->prop->pos.z;

			if (xdiff * xdiff + ydiff * ydiff + zdiff * zdiff < 4000000.0f) {
				numinrange++;

				if (chrHasLosToPosWasteful(loopchr, &chr->prop->pos, chr->prop->rooms)) {
					if (dying == false) {
						loopchr->chrseeshot = chr->chrnum;
					} else {
						loopchr->chrseedie = chr->chrnum;
					}
				}
			}
		}
	}

	if (chr->actiontype == ACT_ARGH) {
		chr->act_argh.notifychrindex = index;
	} else if (chr->actiontype == ACT_DIE || chr->actiontype == ACT_DRUGGEDDROP) {
		chr->act_die.notifychrindex = index;
	} else if (chr->actiontype == ACT_DEAD) {
		chr->act_dead.notifychrindex = index;
	}
}

void chrTickDie(struct chrdata *chr)
{
	struct model *model = chr->model;
	u32 race = CHRRACE(chr);

	u16 thuds[] = {
		SFX_THUD_808D,
		SFX_THUD_808E,
		SFX_THUD_808F,
		SFX_THUD_8090,
		SFX_THUD_8091,
		SFX_THUD_8092,
		SFX_THUD_8093,
		SFX_THUD_8094,
		SFX_THUD_8095,
		SFX_THUD_8096,
		SFX_THUD_8097,
	};

	u16 specialdiesounds[] = {
#if VERSION >= VERSION_NTSC_1_0
		SFX_M1_NOOO, // "Noooo!"
		SFX_M1_SCREAM, // Death scream
		SFX_M2_NOOO, // "Noooo!"
		SFX_M2_NOOO, // "Noooo!"
		SFX_M1_SCREAM, // Death scream
#else
		SFX_0313,
		SFX_034C,
		SFX_0411,
		SFX_0411,
		SFX_034C,
#endif
		SFX_THUD_8092,
		SFX_THUD_8093,
		SFX_THUD_8094,
		SFX_THUD_8095,
		SFX_THUD_8096,
		SFX_THUD_8097,
	};

	static s32 thudindex = 0;

	if (race == RACE_EYESPY) {
		return;
	}

	if (race == RACE_ROBOT) {
		struct prop *prop = chr->prop;
		psStopSound(prop, PSTYPE_GENERAL, 0xffff);
		explosionCreateSimple(prop, &prop->pos, prop->rooms, EXPLOSIONTYPE_8, g_Vars.currentplayernum);
		chr->hidden |= CHRHFLAG_DELETING;
		return;
	}

	if (race == RACE_DRCAROLL) {
		struct prop *prop = chr->prop;

		if (g_DrCarollDyingTimer > TICKS(120) && chr->voicebox) {
			// Play speech
			u16 phrases[] = {
				SFX_DRCAROLL_SYSTEMS_FAILURE,
				SFX_DRCAROLL_YOU_GO_ON,
				SFX_DRCAROLL_I_CANT_MAKE_IT,
				SFX_DRCAROLL_IM_DYING,
				SFX_DRCAROLL_GOODBYE,
				SFX_DRCAROLL_YOU_WERE_SUPPOSED,
			};

			psCreate(NULL, chr->prop, phrases[rngRandom() % 5], -1,
					-1, 0, 0, PSTYPE_NONE, 0, -1, 0, -1, -1, -1, -1);
			chr->voicebox = 0;
		}

		// Change images randomly
		if (chr->act_die.drcarollimagedelay > 0) {
			chr->act_die.drcarollimagedelay -= g_Vars.lvupdate60;
		} else {
			chr->act_die.drcarollimagedelay = (rngRandom() % TICKS(1000)) * 0.01f + 5.0f;
			chr->drcarollimage_left = 1 + (s32)((rngRandom() % 400) * 0.01f);
			chr->drcarollimage_right = 1 + (s32)((rngRandom() % 400) * 0.01f);
		}

		if (g_DrCarollDyingTimer > TICKS(310)) {
			// Explode
			psStopSound(prop, PSTYPE_GENERAL, 0xffff);
			explosionCreateSimple(prop, &prop->pos, prop->rooms, EXPLOSIONTYPE_8, g_Vars.currentplayernum);
			chrBeginDead(chr);
		} else if (chr->soundtimer > (s32)var80068080) {
			// Play shield damage sound
			chr->soundtimer = 0;
			var80068080 -= 5;
			psCreate(NULL, prop, SFX_SHIELD_DAMAGE, -1,
					-1, PSFLAG_0400, 0, PSTYPE_NONE, 0, -1, 0, -1, -1, -1, -1);
			sparksCreate(prop->rooms[0], prop, &prop->pos, NULL, 0, SPARKTYPE_ELECTRICAL);
		}

		return;
	}

	// Human or Skedar
	// If due, play thud 1 sound
	if (chr->act_die.thudframe1 >= 0 && modelGetCurAnimFrame(model) >= chr->act_die.thudframe1) {
		if (chr->specialdie == 0) {
			psCreate(NULL, chr->prop, thuds[thudindex], -1,
					-1, 0, 0, PSTYPE_NONE, 0, -1, 0, -1, -1, -1, -1);
		} else if (chr->specialdie != SPECIALDIE_OVERRAILING) {
			psCreate(NULL, chr->prop, specialdiesounds[chr->specialdie - 1], -1,
					-1, 0, 0, PSTYPE_NONE, 0, -1, 0, -1, -1, -1, -1);
		}

		thudindex++;

		if (thudindex > 10) {
			thudindex = 0;
		}

		chr->act_die.thudframe1 = -1;
	}

	// If due, play thud 2 sound
	if (chr->act_die.thudframe2 >= 0 && modelGetCurAnimFrame(model) >= chr->act_die.thudframe2) {
		if (chr->specialdie < 5) {
			psCreate(NULL, chr->prop, SFX_THUD_808E, -1,
					-1, 0, 0, PSTYPE_NONE, 0, -1, 0, -1, -1, -1, -1);
		} else {
			psCreate(NULL, chr->prop, thuds[thudindex], -1,
					-1, 0, 0, PSTYPE_NONE, 0, -1, 0, -1, -1, -1, -1);
		}

		thudindex++;

		if (thudindex > 10) {
			thudindex = 0;
		}

		chr->act_die.thudframe2 = -1;
	}

	// Check for end of death animation and switch to ACT_DEAD
	if (modelGetCurAnimFrame(model) >= modelGetAnimEndFrame(model)) {
		if (CHRRACE(chr) == RACE_HUMAN && modelGetAnimNum(model) == ANIM_DEATH_STOMACH_LONG) {
			modelSetAnimation(model, ANIM_003C, !modelIsFlipped(model), 50, 0.3, animGetNumFrames(ANIM_003C) - 51.0f);
			modelSetAnimSpeed(model, 0.5, animGetNumFrames(ANIM_003C) - 51.0f);
			return;
		}

		chrBeginDead(chr);
	}

	chrAlertOthersOfInjury(chr, true);
}

void chrTickDruggedComingUp(struct chrdata *chr)
{
	u16 thuds[] = {
		SFX_THUD_808D,
		SFX_THUD_808E,
		SFX_THUD_808F,
		SFX_THUD_8090,
		SFX_THUD_8091,
		SFX_THUD_8092,
		SFX_THUD_8093,
		SFX_THUD_8094,
		SFX_THUD_8095,
		SFX_THUD_8096,
		SFX_THUD_8097,
	};

	chr->act_druggedcomingup.timer60 += g_Vars.lvupdate60;

	if (chr->act_druggedcomingup.timer60 > 0) {
		struct animtablerow *row;
		s32 race = CHRRACE(chr);
		struct model *model = chr->model;
		s32 i = 0;
		bool done = false;
		struct prop *weapon;

		chrUncloak(chr, true);

		chr->actiontype = ACT_DRUGGEDDROP;

		while (!done) {
			if (i >= 0
					&& g_AnimTablesByRace[race][i].deathanims != NULL
					&& g_AnimTablesByRace[race][i].deathanimcount > 0) {
				s32 index = rngRandom() % g_AnimTablesByRace[race][i].deathanimcount;
				row = &g_AnimTablesByRace[race][i].deathanims[index];

				chr->act_die.thudframe1 = row->thudframe1;
				chr->act_die.thudframe2 = row->thudframe2;

				modelSetAnimationWithMerge(model, row->animnum, row->flip, 0, row->speed, 16, true);

				if (row->endframe >= 0) {
					modelSetAnimEndFrame(model, row->endframe);
				}

				done = true;
			}

			if (!done) {
				i++;

				if (g_AnimTablesByRace[race][i].hitpart == -1) {
					done = true;
				}
			}
		}

		weapon = chr->weapons_held[HAND_RIGHT];

		if (weapon && (weapon->obj->flags & OBJFLAG_AIUNDROPPABLE) == 0) {
			objSetDropped(weapon, DROPTYPE_DEFAULT);
			chr->hidden |= CHRHFLAG_DROPPINGITEM;
		}

		weapon = chr->weapons_held[HAND_LEFT];

		if (weapon && (weapon->obj->flags & OBJFLAG_AIUNDROPPABLE) == 0) {
			objSetDropped(weapon, DROPTYPE_DEFAULT);
			chr->hidden |= CHRHFLAG_DROPPINGITEM;
		}

		chrDropConcealedItems(chr);
	}
}

void chrTickDruggedDrop(struct chrdata *chr)
{
	struct model *model = chr->model;

	u16 thuds[] = {
		SFX_THUD_808D,
		SFX_THUD_808E,
		SFX_THUD_808F,
		SFX_THUD_8090,
		SFX_THUD_8091,
		SFX_THUD_8092,
		SFX_THUD_8093,
		SFX_THUD_8094,
		SFX_THUD_8095,
		SFX_THUD_8096,
		SFX_THUD_8097,
	};

	static s32 thudindex = 0;

	// If due, play thud 1 sound
	if (chr->act_die.thudframe1 >= 0 && modelGetCurAnimFrame(model) >= chr->act_die.thudframe1) {
		psCreate(NULL, chr->prop, thuds[thudindex], -1,
				-1, 0, 0, PSTYPE_NONE, 0, -1, 0, -1, -1, -1, -1);

		thudindex++;

		if (thudindex > 10) {
			thudindex = 0;
		}

		chr->act_die.thudframe1 = -1;
	}

	// If due, play thud 2 sound
	if (chr->act_die.thudframe2 >= 0 && modelGetCurAnimFrame(model) >= chr->act_die.thudframe2) {
		psCreate(NULL, chr->prop, thuds[thudindex], -1,
				-1, 0, 0, PSTYPE_NONE, 0, -1, 0, -1, -1, -1, -1);

		thudindex++;

		if (thudindex > 10) {
			thudindex = 0;
		}

		chr->act_die.thudframe2 = -1;
	}

	// If falling animation finished, assign ACT_DRUGGEDKO
	if (modelGetCurAnimFrame(model) >= modelGetAnimEndFrame(model)) {
		chr->actiontype = ACT_DRUGGEDKO;
		chr->act_dead.fadetimer60 = chr->aibot ? 0 : -1;
		chr->act_dead.fadenow = false;
		chr->act_dead.fadewheninvis = false;
		chr->act_dead.invistimer60 = 0;
		chr->act_dead.notifychrindex = 0;
		chr->sleep = 0;
	}

	chrAlertOthersOfInjury(chr, true);
}

void chrTickDruggedKo(struct chrdata *chr)
{
	bool reap = false;

	// If fade is active, handle it
	if (chr->act_dead.fadetimer60 >= 0) {
		chr->act_dead.fadetimer60 += g_Vars.lvupdate60;

		if (chr->act_dead.fadetimer60 >= TICKS(90)) {
			reap = true;
		} else {
			chr->fadealpha = (TICKS(90) - chr->act_dead.fadetimer60) * 255 / TICKS(90);
		}
	} else if ((chr->chrflags & CHRCFLAG_KEEPCORPSEKO) == 0) {
		if (chr->act_dead.fadenow) {
			chr->act_dead.fadetimer60 = 0;
		}

		if (chr->prop->flags & PROPFLAG_ONANYSCREENPREVTICK) {
			chr->act_dead.invistimer60 = 0;
		} else {
			chr->act_dead.invistimer60 += g_Vars.lvupdate60;
		}

		if (chr->act_dead.fadewheninvis && chr->act_dead.invistimer60 >= TICKS(120)) {
			reap = true;
		}
	}

	if (reap) {
		chr->fadealpha = 0;
		chr->hidden |= CHRHFLAG_DELETING;
		chrDropItemsForOwnerReap(chr);
	}
}

void chrTickArgh(struct chrdata *chr)
{
	struct model *model = chr->model;

	if (modelGetCurAnimFrame(model) >= modelGetAnimEndFrame(model)) {
		chrRecordLastSeeTargetTime(chr);

		if (CHRRACE(chr) == RACE_HUMAN && modelGetAnimNum(model) == ANIM_DEATH_STOMACH_LONG) {
			func0f02ed28(chr, 26);
		} else {
			if (chr->race == RACE_DRCAROLL) {
				chr->drcarollimage_left = DRCAROLLIMAGE_EYESDEFAULT;
				chr->drcarollimage_right = DRCAROLLIMAGE_EYESDEFAULT;
			}

			chrStop(chr);
		}
	}

	chrAlertOthersOfInjury(chr, false);
}

void chrTickPreArgh(struct chrdata *chr)
{
	struct model *model = chr->model;

	if (modelGetCurAnimFrame(model) >= modelGetAnimEndFrame(model)) {
		struct coord dir;
		dir.x = chr->act_preargh.dir.x;
		dir.y = chr->act_preargh.dir.y;
		dir.z = chr->act_preargh.dir.z;

		chrReactToDamage(chr, &dir,
				chr->act_preargh.relshotdir,
				chr->act_preargh.hitpart,
				&chr->act_preargh.gset,
				chr->act_preargh.aplayernum);
	}
}

void chrTickSidestep(struct chrdata *chr)
{
	struct model *model = chr->model;

	if (chr->hidden & CHRHFLAG_NEEDANIM) {
		if (modelIsAnimMerging(chr->model)) {
			return;
		}

		chrSidestepChooseAnimation(chr);
		chr->hidden &= ~CHRHFLAG_NEEDANIM;
	}

	if (modelGetCurAnimFrame(model) >= modelGetAnimEndFrame(model)) {
		chrRecordLastSeeTargetTime(chr);
		func0f02ed28(chr, 10);
	}
}

void chrTickJumpOut(struct chrdata *chr)
{
	struct model *model = chr->model;

	if (chr->hidden & CHRHFLAG_NEEDANIM) {
		if (modelIsAnimMerging(chr->model)) {
			return;
		}

		chrJumpOutChooseAnimation(chr);
		chr->hidden &= ~CHRHFLAG_NEEDANIM;
	}

	if (modelGetCurAnimFrame(model) >= modelGetAnimEndFrame(model)) {
		chrRecordLastSeeTargetTime(chr);
		chrStop(chr);
	}
}

void chrTickTest(struct chrdata *chr)
{
	struct model *model = chr->model;

	if (modelGetCurAnimFrame(model) >= modelGetAnimEndFrame(model)) {
		chrStand(chr);
	}
}

void chrTickStartAlarm(struct chrdata *chr)
{
	struct model *model = chr->model;

	if (chr->hidden & CHRHFLAG_NEEDANIM) {
		if (modelIsAnimMerging(chr->model)) {
			return;
		}

		chrStartAlarmChooseAnimation(chr);
		chr->hidden &= ~CHRHFLAG_NEEDANIM;
	}

	if (modelGetCurAnimFrame(model) >= 60) {
		alarmActivate();
	}

	if (modelGetCurAnimFrame(model) >= modelGetAnimEndFrame(model)) {
		chrStop(chr);
	}
}

void chrTickSurprised(struct chrdata *chr)
{
	if (chr->hidden & CHRHFLAG_NEEDANIM) {
		if (modelIsAnimMerging(chr->model)) {
			return;
		}

		chrSurprisedChooseAnimation(chr);
		chr->hidden &= ~CHRHFLAG_NEEDANIM;
	}

	if (CHRRACE(chr) == RACE_HUMAN) {
		struct model *model = chr->model;

		if (modelGetCurAnimFrame(model) >= modelGetAnimEndFrame(model)) {
			if (modelGetAnimNum(model) == ANIM_SURRENDER_002E) {
				func0f02ed28(chr, 26);
			} else if (modelGetAnimNum(model) == ANIM_003F) {
				func0f02ed28(chr, 26);
			} else {
				chrStop(chr);
			}
		}
	}
}

void chrUpdateFireslot(struct chrdata *chr, s32 handnum, bool withsound, bool withbeam, struct coord *from, struct coord *to)
{
	struct prop *weaponprop;
	struct weaponobj *weapon;
	s32 weaponnum;
	bool playsound = false;
	u8 duration;
	u16 soundnum;

	weaponprop = chrGetHeldProp(chr, handnum);

	if (weaponprop) {
		weapon = weaponprop->weapon;
		weaponnum = weapon->weaponnum;
		duration = gsetGetFireslotDuration(&weapon->gset);
		soundnum = gsetGetSingleShootSound(&weapon->gset);

		if (chr->fireslots[handnum] < 0) {
			chr->fireslots[handnum] = bgunAllocateFireslot();
		}

		if (chr->fireslots[handnum] >= 0) {
			struct fireslot *fireslot = &g_Fireslots[chr->fireslots[handnum]];

			if (withsound) {
				if (duration > 0) {
#if VERSION >= VERSION_NTSC_1_0
					if (chr->hidden2 & CHRH2FLAG_FIRESOUNDDONE)
#else
					if (chr->hidden & CHRHFLAG_FIRESOUNDDONE)
#endif
					{
						playsound = false;
					} else {
						if (g_Vars.lvframe60 > fireslot->endlvframe) {
							playsound = true;
						} else {
							playsound = false;
						}
					}
				} else {
					playsound = true;
				}
			}

			if (playsound) {
#if VERSION >= VERSION_NTSC_1_0
				psCreate(NULL, chr->prop, soundnum, -1, -1,
						PSFLAG_0400, PSFLAG2_PRINTABLE, PSTYPE_CHRSHOOT, NULL, -1, NULL, -1, -1, -1, -1);
				fireslot->endlvframe = (u32)g_Vars.lvframe60 + duration;
				chr->hidden2 |= CHRH2FLAG_FIRESOUNDDONE;
#else
				psCreate(NULL, chr->prop, soundnum, -1, -1,
						PSFLAG_0400, PSFLAG2_PRINTABLE, PSTYPE_NONE, NULL, -1, NULL, -1, -1, -1, -1);
				fireslot->endlvframe = (u32)g_Vars.lvframe60 + duration;
				chr->hidden |= CHRHFLAG_FIRESOUNDDONE;

				if (chr);
#endif
#ifndef PLATFORM_N64
				// ON-TRANSITION broadcast: tell clients that a sim just fired.
				// Clients gate botTick to server-only (sim AI doesn't run on
				// clients), so without this message they get no muzzle flash
				// and no positional shot sound. Sent on the reliable channel
				// (g_NetMsgRel) so the matching OFF message in chrTickShoot
				// can't outpace this one and leave the gunfire visual stuck on.
				// Sims (aibot) AND campaign co-op NPCs: both are net-replicated chrs
				// whose firing the client doesn't compute locally (botTick / NPC AI
				// is server-gated), so both need the ON message for the shot sound +
				// flash onset. Co-op NPCs aren't aibots, so the aibot test alone left
				// their gunfire silent on clients. Matches the OFF gate below (synced
				// chr) and the co-op gate used elsewhere.
				if (g_NetMode == NETMODE_SERVER && (chr->aibot || g_Vars.coopplayernum >= 0)
						&& chr->prop && chr->prop->syncid) {
					netmsgSvcChrFireWrite(&g_NetMsgRel, chr, (u8)handnum, soundnum);
				}
#endif
			}

			if (withbeam) {
				beamCreate(&fireslot->beam, weaponnum, from, to);
			}
		}
	}
}

/**
 * Returns the chr's turn angle difference to 360 degrees, in radians.
 */
f32 chrGetInverseTheta(struct chrdata *chr)
{
	if (chr->aibot) {
		return chr->aibot->lookangle;
	}

	if (chr->model == NULL && chr->prop && chr->prop->type == PROPTYPE_PLAYER) {
		struct player *player = g_Vars.players[playermgrGetPlayerNumByProp(chr->prop)];
		f32 angle = (360.0f - player->vv_theta) * 0.017450513318181f;

		if (angle >= M_BADTAU) {
			angle -= M_BADTAU;
		} else if (angle < 0) {
			angle += M_BADTAU;
		}

		return angle;
	}

	return modelGetChrRotY(chr->model);
}

void chrSetLookAngle(struct chrdata *chr, f32 angle)
{
	if (chr->aibot) {
		chr->aibot->lookangle = angle;
	} else {
		modelSetChrRotY(chr->model, angle);
	}
}

f32 chrGetRotY(struct chrdata *chr)
{
	if (chr->aibot) {
		return chr->aibot->roty;
	} else {
		return modelGetChrRotY(chr->model);
	}
}

void chrSetRotY(struct chrdata *chr, f32 roty)
{
	if (chr->aibot) {
		chr->aibot->roty = roty;
	} else {
		modelSetChrRotY(chr->model, roty);
	}
}

f32 chrGetAimAngle(struct chrdata *chr)
{
	f32 angle = chrGetInverseTheta(chr) + chr->aimsideback;
	f32 offset = 0;

	if (angle >= M_BADTAU) {
		angle -= M_BADTAU;
	} else if (angle < 0) {
		angle += M_BADTAU;
	}

	if (chr->aibot) {
		if (chr->aibot->attackanimconfig) {
			// empty
		}
	} else if (chr->actiontype == ACT_ATTACK
			|| chr->actiontype == ACT_ATTACKROLL
			|| chr->actiontype == ACT_BOT_ATTACKSTAND
			|| chr->actiontype == ACT_BOT_ATTACKKNEEL
			|| chr->actiontype == ACT_BOT_ATTACKSTRAFE) {
		offset = chr->act_attack.animcfg->unk0c;
	} else if (chr->prop->type == PROPTYPE_PLAYER) {
		offset += g_Vars.players[playermgrGetPlayerNumByProp(chr->prop)]->angleoffset;
	}

	if (offset) {
		if (chr->model->anim->flip) {
			offset = M_BADTAU - offset;
		}

		angle += offset;

		if (angle >= M_BADTAU) {
			angle -= M_BADTAU;
		} else if (angle < M_BADTAU) {
			angle += M_BADTAU;
		}
	}

	return angle;
}

f32 chrGetPitchAngle(struct chrdata *chr)
{
	f32 sum = chr->aimuprshoulder + chr->aimupback;

	if (sum < 0) {
		sum += M_BADTAU;
	}

	return sum;
}

/**
 * Turn the chr slightly towards their target.
 */
s32 chrTurn(struct chrdata *chr, s32 turning, f32 endanimframe, f32 speed, f32 toleranceangle)
{
	if (turning != TURNSTATE_OFF) {
		struct model *model = chr->model;
		f32 curframe = modelGetCurAnimFrame(model);
		u32 stack;
		f32 finalangle = chrGetInverseTheta(chr);
		f32 remainingangle;
		f32 increment = M_BADTAU / 100.0f * speed * g_Vars.lvupdate60f * model->anim->playspeed;

		if (chr->aibot) {
			struct prop *target = chrGetTargetProp(chr);
			remainingangle = chrGetAngleToPos(chr, &target->pos);
		} else if (chr->actiontype == ACT_ATTACK
				|| chr->actiontype == ACT_BOT_ATTACKSTAND
				|| chr->actiontype == ACT_BOT_ATTACKKNEEL
				|| chr->actiontype == ACT_BOT_ATTACKSTRAFE) {
			remainingangle = chrGetAttackEntityRelativeAngle(chr, chr->act_attack.flags, chr->act_attack.entityid);
		} else if (chr->actiontype == ACT_STAND) {
			remainingangle = chrGetAttackEntityRelativeAngle(chr, chr->act_stand.flags, chr->act_stand.entityid);
		} else if (chr->actiontype == ACT_THROWGRENADE) {
			remainingangle = chrGetAttackEntityRelativeAngle(chr, chr->act_throwgrenade.flags, chr->act_throwgrenade.entityid);
		} else {
			struct prop *target = chrGetTargetProp(chr);
			remainingangle = chrGetAngleToPos(chr, &target->pos);
		}

		remainingangle -= toleranceangle;

		if (remainingangle < 0) {
			remainingangle += M_BADTAU;
		}

		if (increment > remainingangle || M_BADTAU - increment < remainingangle) {
			// Close enough to stop
			finalangle += remainingangle;

			if (finalangle >= M_BADTAU) {
				finalangle -= M_BADTAU;
			}

			chrSetLookAngle(chr, finalangle);
			turning = TURNSTATE_ONTARGET;
		} else if (remainingangle < M_PI) {
			// Turning in one direction
			finalangle += increment;

			if (finalangle >= M_BADTAU) {
				finalangle -= M_BADTAU;
			}

			chrSetLookAngle(chr, finalangle);
		} else {
			// Turning in the other direction
			finalangle -= increment;

			if (finalangle < 0) {
				finalangle += M_BADTAU;
			}

			chrSetLookAngle(chr, finalangle);
		}

		if (curframe >= endanimframe) {
			turning = TURNSTATE_OFF;
		}
	}

	return turning;
}

bool func0f03e9f4(struct chrdata *chr, struct attackanimconfig *animcfg, bool firingleft, bool firingright, f32 arg4)
{
	f32 shootrotx = 0.0f;
	f32 aimendsideback = 0.0f;
	u32 flags = ATTACKFLAG_AIMATTARGET;
	s32 entityid = 0;
	bool result = true;

	if (!chr->aibot) {
		if (chr->actiontype == ACT_ATTACK
				|| chr->actiontype == ACT_BOT_ATTACKSTAND
				|| chr->actiontype == ACT_BOT_ATTACKKNEEL
				|| chr->actiontype == ACT_BOT_ATTACKSTRAFE) {
			flags = chr->act_attack.flags;
			entityid = chr->act_attack.entityid;
		} else if (chr->actiontype == ACT_STAND) {
			flags = chr->act_stand.flags;
			entityid = chr->act_stand.entityid;
		}
	}

	if ((flags & ATTACKFLAG_AIMFORWARD) == 0) {
		f32 sp178;
		f32 sp174;
		f32 sp170;
		struct prop *chrprop = chr->prop;
		struct prop *targetprop = chrGetTargetProp(chr);
		f32 sqdist;
		bool holdturn;
		struct coord targetpos;
		RoomNum targetrooms[8];

		sp178 = targetprop->pos.x - chrprop->pos.x;
		sp174 = targetprop->pos.y - chrprop->pos.y;
		sp170 = targetprop->pos.z - chrprop->pos.z;

		sqdist = sp178 * sp178 + sp174 * sp174 + sp170 * sp170;

		if (chr->aibot) {
			holdturn = false;
		} else if (flags & ATTACKFLAG_AIMATTARGET) {
			if (flags & ATTACKFLAG_DONTTURN) {
				holdturn = true;
			} else {
				holdturn = chrHasLosToTarget(chr);
			}
		} else {
			holdturn = true;
		}

		if ((flags & ATTACKFLAG_AIMATTARGET) && targetprop->type == PROPTYPE_PLAYER) {
			f32 eyeheight = g_Vars.players[playermgrGetPlayerNumByProp(targetprop)]->vv_eyeheight;

			targetpos.x = targetprop->pos.x;
			targetpos.y = targetprop->pos.y;
			targetpos.z = targetprop->pos.z;

			if (chr->aibot) {
				sp174 -= eyeheight * (0.4f + (0.05f * RANDOMFRAC() * arg4));
			} else if (chr->chrflags & CHRCFLAG_LOSEXTRAHEIGHT) {
				if (sqdist < 400.0f * 400.0f) {
					if (chrprop->pos.y < targetpos.y - 2.0f * eyeheight) {
						sp174 -= eyeheight * (0.55f + 0.1f * RANDOMFRAC() * arg4);
					} else if (chrprop->pos.y > targetpos.y - eyeheight * 0.5f) {
						sp174 -= eyeheight * (0.15f + 0.1f * RANDOMFRAC() * arg4);
					} else {
						sp174 = (RANDOMFRAC() * 0.1f * arg4 + 1.0f) * 40.0f;
					}
				} else {
					sp174 += eyeheight * (0.025f - 0.05f * RANDOMFRAC() * arg4);
				}
			} else if (sqdist > 1000.0f * 1000.0f) {
				if ((rngRandom() % 3) == 0) {
					sp174 += eyeheight * (0.05f + 0.1f * RANDOMFRAC() * arg4);
				} else {
					sp174 -= eyeheight * (0.05f + 0.55f * RANDOMFRAC() * arg4);
				}
			} else {
				if (chrprop->pos.y < targetpos.y - eyeheight) {
					sp174 -= eyeheight * (0.55f + 0.1f * RANDOMFRAC() * arg4);
				} else if (chrprop->pos.y > targetpos.y - eyeheight * 0.5f) {
					sp174 -= eyeheight * (0.15f + 0.1f * RANDOMFRAC() * arg4);
				} else {
					sp174 = (RANDOMFRAC() * 0.1f * arg4 - 0.05f) * eyeheight;
				}
			}
		} else {
			struct coord chrpos;

			modelGetRootPosition(chr->model, &chrpos);
			chrGetAttackEntityPos(chr, flags, entityid, &targetpos, targetrooms);

			sp178 = targetpos.x - chrpos.x;
			sp174 = targetpos.y - chrpos.y;
			sp170 = targetpos.z - chrpos.z;
		}

		if ((flags & ATTACKFLAG_NOVERTICAL) == 0) {
			shootrotx = atan2f(sp174, sqrtf(sp178 * sp178 + sp170 * sp170));

			if (shootrotx >= M_PI) {
				shootrotx -= M_BADTAU;
			}
		}

		if (holdturn) {
			f32 aimangle = chrGetAimAngle(chr);
			struct prop *gunprop;
			struct modelnode *posnode;
			struct model *gunmodel;
			struct coord sp118;
			s32 sp114;
			struct modelnode *burstnode;
			struct modelrwdata_chrinfo *chrrwdata;
			Mtxf *sp108;
			Mtxf spc8;
			struct modelrodata_chrgunfire *burstrodata;
			struct coord spb8;
			Mtxf *spb4;
			Mtxf *spb0;
			Mtxf sp70;
			Mtxf *sp6c;
			struct coord sp60;
			struct coord sp54;
			struct coord sp48;
			f32 anglev;

			if (flags & ATTACKFLAG_AIMATTARGET) {
				if (firingright) {
					gunprop = chrGetHeldProp(chr, HAND_RIGHT);
				} else {
					gunprop = chrGetHeldProp(chr, HAND_LEFT);
				}

				if (PLAYERCOUNT() == 1
						&& gunprop
						&& (gunprop->flags & PROPFLAG_ONANYSCREENPREVTICK)
						&& sqdist < 1000.0f * 1000.0f) {
					struct defaultobj *gun = gunprop->obj;
					gunmodel = gun->model;
					sp114 = 0;
					burstnode = modelGetPart(gunmodel->definition, MODELPART_CHRGUN_GUNFIRE);

					if (burstnode) {
						sp108 = modelFindNodeMtx(gunmodel, burstnode, 0);
						burstrodata = &burstnode->rodata->chrgunfire;
						spb4 = cam0f0b53a4((u8 *)sp108);

						if (spb4) {
							mtx00016798(sp108, &spc8);
							mtx00015be0(spb4, &spc8);

							spb8.x = burstrodata->pos.x;
							spb8.y = burstrodata->pos.y;
							spb8.z = burstrodata->pos.z;

							mtx4TransformVecInPlace(&spc8, &spb8);

							sp114 = 1;
							sp118.x = spb8.x;
							sp118.y = spb8.y;
							sp118.z = spb8.z;
						}
					} else {
						posnode = modelGetPart(gunmodel->definition, MODELPART_CHRGUN_0001);

						if (posnode) {
							spb0 = modelFindNodeMtx(gunmodel, posnode, 0);
							sp6c = cam0f0b53a4((u8 *)spb0);

							if (sp6c) {
								mtx00016798(spb0, &sp70);
								mtx00015be0(sp6c, &sp70);

								sp114 = 1;
								sp118.x = sp70.m[3][0];
								sp118.y = sp70.m[3][1];
								sp118.z = sp70.m[3][2];
							}
						}
					}

					if (sp114) {
						sp54.x = sinf(aimangle);
						sp54.y = 0.0f;
						sp54.z = cosf(aimangle);

						sp48.x = chrprop->pos.x - sp170;
						sp48.y = chrprop->pos.y;
						sp48.z = chrprop->pos.z + sp178;

						func0f02e3dc(&chrprop->pos, &sp48, &sp118, &sp54, &sp60);

						sp178 = targetpos.x - sp60.x;
						sp170 = targetpos.z - sp60.z;
					}
				}
			}

			anglev = atan2f(sp178, sp170);
			aimendsideback = anglev - aimangle;

			if (anglev < aimangle) {
				aimendsideback += M_BADTAU;
			}

			chrrwdata = modelGetNodeRwData(chr->model, chr->model->definition->rootnode);

			if (chrrwdata->unk5c > 0.0f) {
				aimendsideback -= chrrwdata->unk5c * chrrwdata->unk58;

				if (aimendsideback < 0.0f) {
					aimendsideback += M_BADTAU;
				}

				if (aimendsideback >= M_BADTAU) {
					aimendsideback -= M_BADTAU;
				}
			}

			if (!chr->aibot
					&& (flags & ATTACKFLAG_AIMATTARGET)
					&& ((flags & (ATTACKFLAG_AIMONLY | ATTACKFLAG_DONTTURN)) == 0)
					&& targetprop->type == PROPTYPE_PLAYER) {
				if (1);
				aimendsideback += chrGetAimLimitAngle(sqdist) * 0.5f * sinf((((s32) (g_Vars.lvframe60 * chr->model->anim->playspeed) + chr->chrnum) % 60) * 0.10470308f);

				if (aimendsideback < 0.0f) {
					aimendsideback += M_BADTAU;
				}

				if (aimendsideback >= M_BADTAU) {
					aimendsideback -= M_BADTAU;
				}
			}

			if (aimendsideback >= M_PI) {
				aimendsideback -= M_BADTAU;
			}

			aimendsideback += chr->aimsideback;

			if (animcfg) {
				if (chr->model->anim->flip) {
					if (aimendsideback < -animcfg->unk38) {
						aimendsideback = -animcfg->unk38;
						result = false;
					} else if (aimendsideback > -animcfg->unk3c) {
						aimendsideback = -animcfg->unk3c;
						result = false;
					}
				} else {
					if (aimendsideback > animcfg->unk38) {
						aimendsideback = animcfg->unk38;
						result = false;
					} else if (aimendsideback < animcfg->unk3c) {
						aimendsideback = animcfg->unk3c;
						result = false;
					}
				}
			}
		}
	}

	chrCalculateAimEndProperties(chr, animcfg, firingleft, firingright, shootrotx);

	chr->aimendsideback = aimendsideback;
	chr->aimendcount = 10;

	return result;
}

void chrCalculateAimEndProperties(struct chrdata *chr, struct attackanimconfig *animcfg, bool firingleft, bool firingright, f32 shootrotx)
{
	f32 aimfreeshoulder = 0;
	f32 aimendback = 0;
	f32 aimgunshoulder = shootrotx;

	if (animcfg != NULL) {
		if (shootrotx > animcfg->unk30) {
			aimendback = shootrotx - animcfg->unk30;
			aimgunshoulder = animcfg->unk30;
		} else if (shootrotx < animcfg->unk34) {
			aimendback = shootrotx - animcfg->unk34;
			aimgunshoulder = animcfg->unk34;
		}

		if (aimgunshoulder > 0) {
			aimfreeshoulder = animcfg->unk40 * aimgunshoulder;
		} else {
			aimfreeshoulder = animcfg->unk44 * aimgunshoulder;
		}
	}

	if (firingright) {
		chr->aimendrshoulder = aimgunshoulder;

		if (firingleft) {
			chr->aimendlshoulder = aimgunshoulder;
		} else {
			chr->aimendlshoulder = aimfreeshoulder;
		}
	} else {
		chr->aimendrshoulder = aimfreeshoulder;
		chr->aimendlshoulder = aimgunshoulder;
	}

	chr->aimendback = aimendback;
}

void chrResetAimEndProperties(struct chrdata *chr)
{
	chr->aimendcount = 10;
	chr->aimendrshoulder = 0;
	chr->aimendlshoulder = 0;
	chr->aimendback = 0;
	chr->aimendsideback = 0;
}

void chrSetFiring(struct chrdata *chr, s32 hand, bool firing)
{
	struct prop *prop = chrGetHeldProp(chr, hand);

	chr->prop->forcetick = firing ? true : false;

	if (prop) {
		weaponSetGunfireVisible(prop, firing, chr->prop->rooms[0]);
	}
}

bool chrIsGunfireVisible(struct chrdata *chr, s32 hand)
{
	struct prop *prop = chrGetHeldProp(chr, hand);

	if (prop) {
		return weaponIsGunfireVisible(prop);
	}

	return false;
}

void chrStopFiring(struct chrdata *chr)
{
	u8 race = CHRRACE(chr);

	if (race != RACE_DRCAROLL && race != RACE_EYESPY && chr->aibot == NULL) {
		chrSetFiring(chr, HAND_RIGHT, false);
		chrSetFiring(chr, HAND_LEFT, false);

		chrResetAimEndProperties(chr);

		chr->fireslots[0] = bgunFreeFireslot(chr->fireslots[0]);
		chr->fireslots[1] = bgunFreeFireslot(chr->fireslots[1]);
	}
}

void chrSetHandFiring(struct chrdata *chr, s32 hand, bool firing)
{
	if (firing) {
		if (hand == HAND_LEFT) {
			chr->hidden |= CHRHFLAG_FIRINGLEFT;
		} else {
			chr->hidden |= CHRHFLAG_FIRINGRIGHT;
		}
	} else {
		if (hand == HAND_LEFT) {
			chr->hidden &= ~CHRHFLAG_FIRINGLEFT;
		} else {
			chr->hidden &= ~CHRHFLAG_FIRINGRIGHT;
		}
	}

	if (!firing) {
		chrSetFiring(chr, hand, false);
	}
}

f32 chrGetAimLimitAngle(f32 sqdist)
{
	if (sqdist > 1600 * 1600) {
		return 0.018752790987492f;
	}

	if (sqdist > 800 * 800) {
		return 0.03761787340045f;
	}

	if (sqdist > 400 * 400) {
		return 0.07478791475296f;
	}

	if (sqdist > 200 * 200) {
		return 0.14957582950592f;
	}

	return 0.2512874007225f;
}

/**
 * Calculate whether a chr's shot hits their target on this tick.
 *
 * Each chr maintains a shotbondsum property which is a float ranging from
 * 0 to 1. Each time this function is called, the value is increased slightly
 * based on the chr's accuracy and other factors. Once it reaches 1, the target
 * is considered to be hit and the value is reset to 0.
 *
 * The function writes to the angleokptr argument if the angle to their target
 * is within range, and writes to the hit argument to indicate if the target is
 * being hit or not.
 */
void chrCalculateHit(struct chrdata *chr, bool *angleokptr, bool *hit, struct gset *gset)
{
	struct prop *prop;
	struct prop *target;
	f32 xdist;
	f32 ydist;
	f32 zdist;
	f32 angletotarget;
	f32 angleaiming;
	f32 anglediff;
	f32 limitangle;
	bool angleok;
	u32 stack;
	f32 taperdist;
	f32 sqdist;

	taperdist = 300;

	// Check that the chr's aim angle is within an acceptable range to their
	// target.
	prop = chr->prop;
	target = chrGetTargetProp(chr);

	xdist = target->pos.x - prop->pos.x;
	ydist = target->pos.y - prop->pos.y;
	zdist = target->pos.z - prop->pos.z;

	angletotarget = atan2f(xdist, zdist);
	angleaiming = chrGetAimAngle(chr);
	anglediff = angletotarget - angleaiming;

	sqdist = xdist * xdist + ydist * ydist + zdist * zdist;
	limitangle = chrGetAimLimitAngle(sqdist);

	if (anglediff < 0) {
		anglediff += M_BADTAU;
	}

	angleok = anglediff < limitangle || anglediff > M_BADTAU - limitangle;

	*angleokptr = angleok;
	*hit = false;

	// Determine the distance at which accuracy starts to taper off
	switch (gset->weaponnum) {
	case WEAPON_FALCON2:
	case WEAPON_FALCON2_SILENCER:
	case WEAPON_MAULER:
	case WEAPON_PHOENIX:
	case WEAPON_DY357MAGNUM:
	case WEAPON_DY357LX:
	case WEAPON_CMP150:
	case WEAPON_CYCLONE:
	case WEAPON_CALLISTO:
	case WEAPON_RCP120:
	case WEAPON_LAPTOPGUN:
	case WEAPON_DRAGON:
	case WEAPON_K7AVENGER:
	case WEAPON_AR34:
	case WEAPON_SUPERDRAGON:
	case WEAPON_SHOTGUN:
	case WEAPON_REAPER:
	case WEAPON_DEVASTATOR:
	case WEAPON_ROCKETLAUNCHER:
	case WEAPON_SLAYER:
	case WEAPON_COMBATKNIFE:
	case WEAPON_CROSSBOW:
	case WEAPON_TRANQUILIZER:
	case WEAPON_LASER:
	case WEAPON_GRENADE:
	case WEAPON_NBOMB:
	case WEAPON_TIMEDMINE:
	case WEAPON_PROXIMITYMINE:
	case WEAPON_REMOTEMINE:
	case WEAPON_COMBATBOOST:
	case WEAPON_PP9I:
	case WEAPON_CC13:
	case WEAPON_KL01313:
	case WEAPON_KF7SPECIAL:
	case WEAPON_ZZT:
	case WEAPON_DMC:
	case WEAPON_AR53:
	case WEAPON_RCP45:
	case WEAPON_PSYCHOSISGUN:
	default:
		// Use default distance (300)
		break;
	case WEAPON_FALCON2_SCOPE:
	case WEAPON_MAGSEC4:
		taperdist = 600;
		break;
	case WEAPON_SNIPERRIFLE:
	case WEAPON_FARSIGHT:
		taperdist = 1200;
		break;
	}

	if (angleok) {
		f32 dist = sqrtf(xdist * xdist + ydist * ydist + zdist * zdist);
		f32 accuracy = 0.16f;

		// Decrease accuracy if further than taperdist
		if (dist > taperdist) {
			accuracy *= taperdist / dist;
		}

		// Scale accuracy up or down based on chr's accuracyrating
		if (chr->accuracyrating > 0) {
			accuracy *= 1 + chr->accuracyrating * 0.1f;
		} else if (chr->accuracyrating < 0) {
			if (chr->accuracyrating <= -100) {
				accuracy = 0;
			} else {
				accuracy *= (chr->accuracyrating + 100) * 0.01f;
			}
		}

		// Apply PD mode enemy accuracy setting (default 1 which is no op)
		if (pdmodeGetEnemyAccuracy() <= 1) {
			accuracy *= pdmodeGetEnemyAccuracy();
		} else {
			accuracy *= 9 / (10.001f - pdmodeGetEnemyAccuracy());
		}

		// Apply difficulty multiplier (solo A = 0.6, SA = 0.8, PA = 1.175)
		accuracy *= g_EnemyAccuracyScale;

		// If the weapon fires more than once per tick, double the value to
		// account for it. No weapons meet this criteria, however.
		if (weaponGetNumTicksPerShot(gset->weaponnum, gset->weaponfunc) <= 0) {
			accuracy += accuracy;
		}

		// Shotgun doubles the value due to more bullets
		if (gset->weaponnum == WEAPON_SHOTGUN) {
			accuracy += accuracy;
		}

		chr->shotbondsum += accuracy;

		if (chr->hidden & CHRHFLAG_PERFECTACCURACY) {
			chr->shotbondsum += 1.1f;
		}

		if (chr->shotbondsum >= 1) {
			*hit = true;
			chr->shotbondsum = 0;
		}
	}
}

/**
 * If the chr's gun is on screen, populate gunpos with the world coordinates of
 * their gun and return true.
 *
 * If the chr's gun is off screen, return false without populating gunpos.
 */
bool chrGetGunPos(struct chrdata *chr, s32 handnum, struct coord *gunpos)
{
	struct prop *weaponprop = chrGetHeldProp(chr, handnum);
	struct defaultobj *obj;
	struct model *model;
	bool result = false;
	struct modelnode *part0;
	struct modelnode *part1;
	Mtxf *spac;
	Mtxf sp6c;
	struct modelrodata_chrgunfire *rodata;
	Mtxf *sp64;
	Mtxf sp24;

	if (weaponprop) {
		obj = weaponprop->obj;
		model = obj->model;

		if ((chr->prop->flags & PROPFLAG_ONTHISSCREENTHISTICK) && (weaponprop->flags & PROPFLAG_ONTHISSCREENTHISTICK)) {
			if ((part0 = modelGetPart(model->definition, MODELPART_0000))) {
				spac = modelFindNodeMtx(model, part0, 0);
				rodata = &part0->rodata->chrgunfire;

				gunpos->x = rodata->pos.x;
				gunpos->y = rodata->pos.y;
				gunpos->z = rodata->pos.z;

				mtx00015be4(camGetProjectionMtxF(), spac, &sp6c);
				mtx4TransformVecInPlace(&sp6c, gunpos);
				result = true;
			} else if ((part1 = modelGetPart(model->definition, MODELPART_0001))) {
				sp64 = modelFindNodeMtx(model, part1, 0);

				mtx00015be4(camGetProjectionMtxF(), sp64, &sp24);

				gunpos->x = sp24.m[3][0];
				gunpos->y = sp24.m[3][1];
				gunpos->z = sp24.m[3][2];

				result = true;
			}
		}
	}

	return result;
}

/**
 * For a shielded chr, determine which model node has been shot and populate the
 * last 4 pointer arguments with information about the hit.
 *
 * Shield hits are calculated using the bounding box of each body part.
 * This gives a bit of padding around the chr which can register as a hit.
 *
 * nodeptr    - will be populated with a pointer to the bbox model node.
 * hitpartptr - will be populated with the HITPART constant value.
 * modelptr   - will be populated with a pointer to the chr's model.
 * sideptr    - will be populated with an index in the range 0-5 which
 *              represents which side of the node's bounding box was hit.
 */
void chrCalculateShieldHit(struct chrdata *chr, struct coord *pos, struct coord *vector,
		struct modelnode **nodeptr, s32 *hitpartptr, struct model **modelptr, s32 *sideptr)
{
	u32 stack1;
	struct prop *prop = chr->prop;
	bool done = false;
	bool isdifferentmtx;
	struct coord sp124;
	struct coord sp118;
	s32 i;
	struct modelnode *bestnode;
	u32 stack2[2];
	Mtxf spc8;
	f32 bestvolume;
	Mtxf *worldtoscreenmtx;
	struct modelnode *node;
	f32 x;
	f32 y;
	f32 z;
	f32 volume;
	Mtxf *mtxptr1;
	Mtxf *mtxptr2;
	f32 sides[6];
	u32 stack3;

	if (prop->type != PROPTYPE_PLAYER || g_Vars.normmplayerisrunning || chrGetShield(chr) > 0) {
		if (prop->flags & (PROPFLAG_ONTHISSCREENTHISTICK | PROPFLAG_ONANYSCREENTHISTICK | PROPFLAG_ONANYSCREENPREVTICK)) {
			bestnode = NULL;
			bestvolume = MAXFLOAT;
			worldtoscreenmtx = cam0f0b5050((u8 *)chr->model->matrices);

			if (worldtoscreenmtx) {
				mtx4TransformVec(worldtoscreenmtx, pos, &sp124);
				mtx4RotateVec(worldtoscreenmtx, vector, &sp118);

				isdifferentmtx = (camGetWorldToScreenMtxf() != worldtoscreenmtx);
				node = chr->model->definition->rootnode;

				while (node) {
					if ((node->type & 0xff) == MODELNODETYPE_BBOX) {
						mtxptr1 = modelFindNodeMtx(chr->model, node, 0);

						if (isdifferentmtx) {
							mtx00016798(mtxptr1, &spc8);
							mtxptr1 = &spc8;
						}

						x = mtxptr1->m[3][0] - sp124.f[0];
						y = mtxptr1->m[3][1] - sp124.f[1];
						z = mtxptr1->m[3][2] - sp124.f[2];

						volume = x * x + y * y + z * z;

						if (volume < bestvolume) {
							bestvolume = volume;
							bestnode = node;
						}
					}

					// Iterate all nodes recursively except headspot's chidlren
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

				if (bestvolume);

#if VERSION >= VERSION_NTSC_1_0
				if (bestnode != NULL)
#endif
				{
					Mtxf sp48;
					struct modelrodata_bbox *rodata = &bestnode->rodata->bbox;

					*hitpartptr = rodata->hitpart;
					*nodeptr = bestnode;
					*modelptr = chr->model;
					*sideptr = 0;

					mtxptr2 = modelFindNodeMtx(chr->model, bestnode, 0);

					if (isdifferentmtx) {
						mtx00016798(mtxptr2, &sp48);
						mtxptr2 = &sp48;
					}

					bestvolume = -2;

					x = (sp118.f[0] * mtxptr2->m[0][0]) + (sp118.f[1] * mtxptr2->m[0][1]) + (sp118.f[2] * mtxptr2->m[0][2]);
					y = (sp118.f[0] * mtxptr2->m[1][0]) + (sp118.f[1] * mtxptr2->m[1][1]) + (sp118.f[2] * mtxptr2->m[1][2]);
					z = (sp118.f[0] * mtxptr2->m[2][0]) + (sp118.f[1] * mtxptr2->m[2][1]) + (sp118.f[2] * mtxptr2->m[2][2]);

					sides[0] = x;
					sides[1] = -x;
					sides[2] = y;
					sides[3] = -y;
					sides[4] = z;
					sides[5] = -z;

					for (i = 0; i < ARRAYCOUNT(sides); i++) {
						if (sides[i] > bestvolume) {
							bestvolume = sides[i];
							*sideptr = i;
						}
					}

					done = true;
				}
			}
		}

		// If no node was found above, search the model for the torso bbox
		// and return that.
		if (!done) {
			node = chr->model->definition->rootnode;

			while (node) {

				if ((node->type & 0xff) == MODELNODETYPE_BBOX) {
					struct modelrodata_bbox *rodata = &node->rodata->bbox;

					if (rodata->hitpart == HITPART_TORSO) {
						*hitpartptr = rodata->hitpart;
						*nodeptr = node;
						*modelptr = chr->model;
						*sideptr = 0;
						return;
					}
				}

				if (node->child) {
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
		}
	}
}

/**
 * Calculates the trajectory for thrown items.
 */
void chrCalculateTrajectory(struct coord *frompos, f32 arg1, struct coord *aimpos, struct coord *arg3)
{
	f32 xvel;
	f32 yvel;
	f32 zvel;
	f32 latvel;
	f32 vel;
	f32 sp40;
	f32 sp3c;
	f32 sp38;
	f32 sp30;
	f32 sp2c;
	f32 sp24;
	f32 sp28;
	f32 sp20;

	arg1 *= 0.59999999f;

	xvel = (aimpos->x - frompos->x) * 0.01f;
	yvel = (aimpos->y - frompos->y) * 0.01f;
	zvel = (aimpos->z - frompos->z) * 0.01f;

	vel = sqrtf(xvel * xvel + yvel * yvel + zvel * zvel);
	latvel = sqrtf(xvel * xvel + zvel * zvel);
	sp38 = latvel / vel;
	sp40 = acosf(sp38);

	if (yvel < 0) {
		sp40 = -sp40;
	}

	sp2c = (vel * 9.81f * sp38 * sp38) / (arg1 * arg1) + yvel / vel;

	if (sp2c < -1) {
		sp2c = -1;
	} else if (sp2c > 1) {
		sp2c = 1;
	}

	sp3c = (asinf(sp2c) - sp40) * 0.5f + sp40;
	sp28 = cosf(sp3c);
	sp30 = sinf(sp3c);

	arg3->x = xvel / latvel * sp28;
	arg3->y = sp30;
	arg3->z = zvel / latvel * sp28;
}

const char var7f1a8ae4[] = "aimadjust=%d";

/**
 * Fire the chr's gun, check what was hit and do all the appropriate things
 * such as dealing damage, creating beams and sparks and playing sounds.
 *
 * This should be called on every frame while the chr is shooting.
 * The function takes care of the gun's fire rate.
 */
void chrTickShoot(struct chrdata *chr, s32 handnum)
{
	struct prop *chrprop = chr->prop;
	struct prop *gunprop;
	u8 isaibot = false;
	u8 normalshoot = true;

#ifndef PLATFORM_N64
	// Classic "Damage Invulnerability": bots / NPCs can't fire while inside
	// their own i-frame window. Same TICKS(18) cooldown as the chrDamage
	// gate. The check uses u32 subtraction so a stale stamp (e.g. recycled
	// chrslot whose lastdamagetick60 is ahead of lvframe60) wraps to a
	// huge unsigned value and correctly fails the < TICKS(18) check.
	// Human players already get fire lockout via bgunCurrentPlayerInIframe
	// at the bondgun.c bgunTickInc path — this gate is the bot-side mirror.
	if (classicOptionActive(CHEAT_CLASSIC_IFRAMES, MPOPTION_CLASSIC_IFRAMES)
			&& chr->lastdamagetick60 != 0
			&& ((u32)g_Vars.lvframe60 - (u32)chr->lastdamagetick60) < (u32)TICKS(18)) {
		return;
	}

	// Chaos "Freeze!" (pd.chr_freeze): frozen chrs don't shoot. Pairs with the
	// anim-advance gate in chr0f0220ec (chr.c) — statues don't pull triggers.
	// The Weeping Skedar single-chr freeze rides the same gate.
	{
		extern s32 g_ChaosChrFreeze;
		extern s32 g_ChaosFreezeChrnum;

		if (g_ChaosChrFreeze && chrprop && chrprop->type != PROPTYPE_PLAYER) {
			return;
		}
		if (g_ChaosFreezeChrnum >= 0 && chr != NULL
				&& chr->chrnum == g_ChaosFreezeChrnum
				&& chrprop && chrprop->type != PROPTYPE_PLAYER) {
			return;
		}
	}

	// Chaos "Body Snatch": while the player is disguised as a snatched guard, no
	// NPC returns fire. Hard guarantee on top of the disguise/calm — an already
	// alerted guard (e.g. after the player kills one) keeps its target through
	// the target-search skip and would otherwise shoot, so gate firing here.
	if (g_ChaosSnatchActive && chrprop && chrprop->type != PROPTYPE_PLAYER) {
		return;
	}
#endif

	if (chr->aibot) {
		isaibot = true;
	}

	gunprop = chrGetHeldProp(chr, handnum);

	if (gunprop) {
		bool firingthisframe = false;
		struct weaponobj *weapon = gunprop->weapon;
		struct gset gset;
		struct prop *targetprop = chrGetTargetProp(chr);
		u32 attackflags;
		bool shotdue;
		bool makebeam;
		struct coord gunpos;
		RoomNum gunrooms[8];
		struct coord hitpos;
		bool hitsomething;
		RoomNum hitrooms[8];
		bool queriedhitrooms;
		s32 tickspershot;

		gset = weapon->gset;
		attackflags = ATTACKFLAG_AIMATTARGET;

		if (chr->actiontype == ACT_ATTACK
				|| chr->actiontype == ACT_BOT_ATTACKSTAND
				|| chr->actiontype == ACT_BOT_ATTACKKNEEL
				|| chr->actiontype == ACT_BOT_ATTACKSTRAFE) {
			attackflags = chr->act_attack.flags;
		}

		shotdue = false;
		makebeam = false;
		hitsomething = false;
		queriedhitrooms = false;

		// Most guns can fire at most once every few ticks - even automatics.
		// The chr's firecount property tracks how many ticks have elapsed since
		// the last bullet, which is used to determine if another bullet should
		// be discharged on this tick.
		tickspershot = weaponGetNumTicksPerShot(gset.weaponnum, gset.weaponfunc);

		if (tickspershot <= 0) {
			shotdue = true;
			makebeam = true;
		} else {
			if (chr->aibot
					&& chr->aibot->weaponnum == WEAPON_REAPER
					&& chr->aibot->gunfunc == FUNC_PRIMARY) {
				f32 sp208 = (TICKS(90) - chr->aibot->reaperspeed[handnum]) * (1.0f / TICKS(18.0f));
				tickspershot *= 1 + sp208;
			}

			chr->firecount[handnum] += g_Vars.lvupdate60;

			if (chr->firecount[handnum] >= tickspershot) {
				chr->firecount[handnum] = 0;
				chr->unk32c_12 ^= 1 << handnum;

				shotdue = true;

				if ((chr->unk32c_12 & (1 << handnum)) || gset.weaponnum == WEAPON_LASER) {
					makebeam = true;
				}

				if (chr->actiontype == ACT_ATTACK) {
					if (modelGetAnimNum(chr->model) == ANIM_SNIPING_ONGROUND) {
						chr->act_attack.numshots++;
					}
				}
			}
		}

		if (shotdue) {
			f32 roty;
			f32 rotx;
			bool extracdtypes;

#ifndef PLATFORM_N64
			// Chaos "Frag Out": at the moment a human enemy would fire, lob a
			// grenade instead. Hooked HERE — the real fire chokepoint that runs for
			// both aibot simulants and campaign guards — rather than the AI-list
			// grenade command (0x1b), which most enemies (and all bots) never run,
			// so the old hook did nothing in Combat Sim. chrConsiderGrenadeThrow
			// does the LOS check, equips a grenade if needed, and throws; on success
			// the chr switches to the throw action, so the firing-anim flags stop
			// and throws pace themselves by the throw animation. Skip the bullet.
			if (g_ChaosFragOut && chrConsiderGrenadeThrow(chr, attackflags, 0)) {
				return;
			}

			// Chaos "Pacifists" (pd.on "chrfire"): report the discharge to Lua.
			// Same chokepoint as Frag Out — campaign guards and simulants alike.
			// Player shots report via luaEmitWeaponFire (bondgun.c); the
			// PROPTYPE_PLAYER gate avoids double counting them here.
			if (chrprop->type != PROPTYPE_PLAYER) {
				extern void luaEmitChrFire(s32 chrnum, s32 weaponnum);
				luaEmitChrFire(chr->chrnum, gset.weaponnum);
			}
#endif

			roty = chrGetAimAngle(chr);
			rotx = chrGetPitchAngle(chr);
			extracdtypes = isaibot ? CDTYPE_PLAYERS : 0;

			firingthisframe = true;

			if (!chrGetGunPos(chr, handnum, &gunpos)) {
				// Gun is off screen - use a quick but inexact calculation
				gunpos.x = chrprop->pos.x;
				gunpos.y = chrprop->pos.y + 30;
				gunpos.z = chrprop->pos.z;

				if (handnum == HAND_LEFT) {
					gunpos.x += cosf(roty) * 10.0f;
					gunpos.z += -sinf(roty) * 10.0f;
				} else {
					gunpos.x += -cosf(roty) * 10.0f;
					gunpos.z += sinf(roty) * 10.0f;
				}
			}

			// Check that the chr isn't clipping their gun through anything such
			// as another chr or a closed door. If they are, the shot won't be
			// taken because that wouldn't be fair.
			// How nice of the developers to check for this!
			chrSetPerimEnabled(chr, false);

			if (cdTestLos10(&chrprop->pos, chrprop->rooms, &gunpos, gunrooms,
						CDTYPE_DOORS | CDTYPE_CHRS | CDTYPE_BG | CDTYPE_DOORSWITHOUTFLAG | extracdtypes,
						GEOFLAG_BLOCK_SHOOT) == CDRESULT_COLLISION) {
				firingthisframe = false;
			}

			chrSetPerimEnabled(chr, true);

			if (firingthisframe) {
				bool angleok = false;
				bool hitplayer = false;
				bool effective = true;
				s32 zero = 0;
				struct coord vector;
				f32 xdiff;
				f32 ydiff;
				f32 zdiff;
				f32 sqshotdist;
				struct prop *hitprop = NULL;
				u32 cdtypes = isaibot
					? CDTYPE_OBJS | CDTYPE_DOORS | CDTYPE_CHRS | CDTYPE_PATHBLOCKER | CDTYPE_BG | CDTYPE_DOORSWITHOUTFLAG | CDTYPE_PLAYERS
					: CDTYPE_OBJS | CDTYPE_DOORS | CDTYPE_CHRS | CDTYPE_PATHBLOCKER | CDTYPE_BG | CDTYPE_DOORSWITHOUTFLAG;
				u32 stack;
				bool isshootingeyespy = CHRRACE(targetprop->chr) == RACE_EYESPY && chrGetDistanceToTarget(chr) > 150;
				bool fudgeforeyespy = false;

				if (isshootingeyespy) {
					vector.x = targetprop->pos.x - gunpos.x;
					vector.y = targetprop->pos.y - gunpos.y;
					vector.z = targetprop->pos.z - gunpos.z;

					guNormalize(&vector.x, &vector.y, &vector.z);
					propSetPerimEnabled(targetprop, true);
				} else {
					vector.x = cosf(rotx) * sinf(roty);
					vector.y = sinf(rotx);
					vector.z = cosf(rotx) * cosf(roty);

					if (isaibot) {
						bgunCalculateBotShotSpread(&vector,
								chr->aibot->weaponnum, chr->aibot->gunfunc,
								chr->aibot->burstsdone[handnum], botGuessCrouchPos(chr),
								chr->weapons_held[0] && chr->weapons_held[1]);
					}
				}

				// Handle Farsight shots by aibots specially
				// because they can shoot through walls.
				if (chr->aibot && gset.weaponnum == WEAPON_FARSIGHT && !chr->aibot->targetinsight) {
					makebeam = true;

					// This function can never return 2 though...
					if (botactShootFarsight(chr + zero, 0, &vector, &gunpos) == 2) {
						normalshoot = rngRandom() % 255 > 200;
					}
				}

				// Check if the shot would hit anything
				hitpos.x = gunpos.x + vector.x * 65536.0f;
				hitpos.y = gunpos.y + vector.y * 65536.0f;
				hitpos.z = gunpos.z + vector.z * 65536.0f;

				chrSetPerimEnabled(chr, false);

				if (isaibot) {
					g_Vars.useperimshoot = true;
				}

				if (cdExamLos08(&gunpos, gunrooms, &hitpos, cdtypes, GEOFLAG_BLOCK_SHOOT) == CDRESULT_COLLISION) {
					hitsomething = true;
#if VERSION >= VERSION_JPN_FINAL
					cdGetPos(&hitpos, 12080, "chr/chraction.c");
#elif VERSION >= VERSION_PAL_FINAL
					cdGetPos(&hitpos, 12077, "chr/chraction.c");
#elif VERSION >= VERSION_NTSC_1_0
					cdGetPos(&hitpos, 12072, "chraction.c");
#else
					cdGetPos(&hitpos, 12086, "chraction.c");
#endif
					hitprop = cdGetObstacleProp();
				}

				chrSetPerimEnabled(chr, true);

				if (isaibot) {
					g_Vars.useperimshoot = false;
				}

				// Eyespy is small and hard to hit, so make it a 50/50 chance
				if (hitprop == NULL && isshootingeyespy) {
					fudgeforeyespy = rngRandom() % 100 > 50;

					if (fudgeforeyespy) {
						hitprop = targetprop;

						hitpos.x = hitprop->pos.x;
						hitpos.y = hitprop->pos.y;
						hitpos.z = hitprop->pos.z;
					}
				}

				xdiff = hitpos.x - gunpos.x;
				ydiff = hitpos.y - gunpos.y;
				zdiff = hitpos.z - gunpos.z;

				sqshotdist = xdiff * xdiff + ydiff * ydiff + zdiff * zdiff;

				// Handle projectile launchers specially
				if (gset.weaponnum == WEAPON_ROCKETLAUNCHER
						|| gset.weaponnum == WEAPON_SLAYER
						|| (gset.weaponnum == WEAPON_SUPERDRAGON && gset.weaponfunc == FUNC_SECONDARY)
						|| gset.weaponnum == WEAPON_DEVASTATOR
						|| gset.weaponnum == WEAPON_CROSSBOW
						|| gset.weaponnum == WEAPON_ROCKETLAUNCHER_34) {
					makebeam = false;

					// Solo chrs won't fire their projectile weapon
					// in less than 4 metres of space
					if (isaibot || sqshotdist > 400.0f * 400.0f) {
						struct weaponobj *projectileobj;
						Mtxf identmtx;
						struct coord sp16c;
						f32 sp168;
						struct coord sp15c;
						Mtxf projectilemtx;
						Mtxf yrotmtx;
						struct weapon *weapondef = weaponFindById(gset.weaponnum);
						struct weaponfunc_shootprojectile *func = weapondef->functions[gset.weaponfunc];

						// Handle creating the projectile
						if (gset.weaponnum == WEAPON_ROCKETLAUNCHER
								|| gset.weaponnum == WEAPON_ROCKETLAUNCHER_34
								|| gset.weaponnum == WEAPON_SLAYER) {
							s32 rocketweaponnum = WEAPON_ROCKET;

							if (func->base.base.flags & FUNCFLAG_HOMINGROCKET) {
								rocketweaponnum = WEAPON_HOMINGROCKET;
							}

							projectileobj = weaponCreateProjectileFromWeaponNum(func->projectilemodelnum, rocketweaponnum, chr);
						} else if (gset.weaponnum == WEAPON_CROSSBOW) {
							projectileobj = weaponCreateProjectileFromWeaponNum(func->projectilemodelnum, WEAPON_BOLT, chr);

							if (projectileobj) {
								projectileobj->gunfunc = gset.weaponfunc;
							}
						} else if (gset.weaponnum == WEAPON_DEVASTATOR) {
							projectileobj = weaponCreateProjectileFromWeaponNum(func->projectilemodelnum, WEAPON_GRENADEROUND, chr);

							if (projectileobj) {
								projectileobj->gunfunc = gset.weaponfunc;
							}
						} else if (gset.weaponnum == WEAPON_SUPERDRAGON) {
							projectileobj = weaponCreateProjectileFromWeaponNum(func->projectilemodelnum, WEAPON_GRENADEROUND, chr);

							if (projectileobj) {
								projectileobj->gunfunc = FUNC_2;
							}
						} else {
							// Unreachable
							projectileobj = weaponCreateProjectileFromGset(func->projectilemodelnum, &gset, g_Vars.currentplayer->prop->chr);
						}

						if (projectileobj) {
							f32 spcc;

							sp168 = func->speed * (1.0f / 0.6f) / 60.0f;
							spcc = func->traveldist * (1.0f / 0.6f);

							// AI bots are a bit smarter than solo chrs
							// with regard to how they aim their projectiles
							if (isaibot && chrIsTargetInFov(chr, 30, 0)) {
								bool hasaimpos = false;
								u32 stack;
								struct coord aimpos;

								if (gset.weaponfunc == FUNC_PRIMARY &&
										(gset.weaponnum == WEAPON_ROCKETLAUNCHER
										 || gset.weaponnum == WEAPON_ROCKETLAUNCHER_34
										 || gset.weaponnum == WEAPON_SLAYER)) {
									if (targetprop->type == PROPTYPE_CHR || targetprop->type == PROPTYPE_PLAYER) {
										// Rockets - aim at target's feet
										aimpos.x = targetprop->pos.x;
										aimpos.y = targetprop->chr->manground;
										aimpos.z = targetprop->pos.z;

										vector.x = aimpos.x - gunpos.x;
										vector.y = aimpos.y - gunpos.y;
										vector.z = aimpos.z - gunpos.z;

										guNormalize(&vector.x, &vector.y, &vector.z);
										hasaimpos = true;
									}
								} else if ((gset.weaponnum == WEAPON_DEVASTATOR && gset.weaponfunc == FUNC_PRIMARY)
										|| gset.weaponnum == WEAPON_SUPERDRAGON) {
									if (targetprop->type == PROPTYPE_CHR || targetprop->type == PROPTYPE_PLAYER) {
										// Grenades - aim at target's feet
										aimpos.x = targetprop->pos.x;
										aimpos.y = targetprop->chr->manground;
										aimpos.z = targetprop->pos.z;

										chrCalculateTrajectory(&gunpos, spcc, &aimpos, &vector);
										hasaimpos = true;
									}
								} else if ((gset.weaponnum == WEAPON_DEVASTATOR && gset.weaponfunc == FUNC_SECONDARY)
										|| gset.weaponnum == WEAPON_CROSSBOW) {
									// Wall hugger grenade or crossbow - aim at target directly
									aimpos.x = targetprop->pos.x;
									aimpos.y = targetprop->pos.y;
									aimpos.z = targetprop->pos.z;

									if (targetprop->type == PROPTYPE_PLAYER) {
										aimpos.y -= 25;
									}

									chrCalculateTrajectory(&gunpos, spcc, &aimpos, &vector);
									hasaimpos = true;
								}

								if (hasaimpos) {
									f32 angle = chrGetAngleToPos(chr, &aimpos);
									f32 cos = cosf(angle);
									f32 sin = sinf(angle);
									f32 x = vector.f[0];
									f32 z = vector.f[2];
									u32 stack;

									vector.x = sin * z + cos * x;
									vector.z = cos * z - sin * x;
								}
							}

							// Calculate and projectile's matrix,
							// spawn position and speed
							mtx4LoadIdentity(&identmtx);
							mtx4LoadXRotation(rotx, &projectilemtx);
							mtx4LoadYRotation(roty, &yrotmtx);
							mtx00015be0(&yrotmtx, &projectilemtx);

							sp15c.x = vector.x * sp168;
							sp15c.y = vector.y * sp168;
							sp15c.z = vector.z * sp168;

							sp16c.x = sp15c.f[0] * g_Vars.lvupdate60freal + vector.f[0] * spcc;
							sp16c.y = sp15c.f[1] * g_Vars.lvupdate60freal + vector.f[1] * spcc;
							sp16c.z = sp15c.f[2] * g_Vars.lvupdate60freal + vector.f[2] * spcc;

							projectileobj->timer240 = func->timer60;

							if (projectileobj->timer240 != -1) {
#if PAL
								projectileobj->timer240 = projectileobj->timer240 * 200 / 60;
#else
								projectileobj->timer240 *= 4;
#endif
							}

							bgun0f09ebcc(&projectileobj->base, &gunpos, gunrooms, &projectilemtx, &sp16c, &identmtx, chrprop, &gunpos);

							if (projectileobj->base.hidden & OBJHFLAG_PROJECTILE) {
								if (func->base.base.flags & FUNCFLAG_PROJECTILE_LIGHTWEIGHT) {
									projectileobj->base.projectile->flags |= PROJECTILEFLAG_LIGHTWEIGHT;
								} else if (func->base.base.flags & FUNCFLAG_PROJECTILE_POWERED) {
									projectileobj->base.projectile->flags |= PROJECTILEFLAG_POWERED;
								}

								projectileobj->base.projectile->unk010 = sp15c.x;
								projectileobj->base.projectile->unk014 = sp15c.y;
								projectileobj->base.projectile->unk018 = sp15c.z;

								projectileobj->base.projectile->pickuptimer240 = TICKS(240);
								projectileobj->base.projectile->unk08c = func->reflectangle;
								projectileobj->base.projectile->unk098 = func->unk50 * (1.0f / 0.6f);

								projectileobj->base.projectile->targetprop = chrGetTargetProp(chr);

								// Play sound
								if (func->soundnum > 0) {
									psCreate(NULL, projectileobj->base.prop, func->soundnum, -1,
											-1, 0, 0, PSTYPE_NONE, NULL, -1, NULL, -1, -1, -1, -1);
								}

#ifndef PLATFORM_N64
								// Sim/NPC-fired projectiles: broadcast the spawn so clients
								// see them. The player gun path (bondgun.c) does this; this
								// chr-fire path never did, so sim rockets/grenades were
								// invisible on clients (they still detonated via the
								// syncid-gated SVC_EXPLOSION). Fully launched here
								// (bgun0f09ebcc above set pos/velocity/mtx).
								netSyncPropSpawn(projectileobj->base.prop);
#endif
							}
						}
					} else {
						firingthisframe = false;
					}

					normalshoot = false;
				} else if (gset.weaponnum == WEAPON_MAULER && isaibot && gset.weaponfunc == FUNC_SECONDARY) {
					gset.unk063a = (s32) (chr->aibot->maulercharge[handnum] * 10.0f);
					chr->aibot->maulercharge[handnum] = 0;
				}

				if (normalshoot) {
					if (!isaibot) {
						if ((attackflags & ATTACKFLAG_AIMATTARGET)
								&& targetprop->type == PROPTYPE_PLAYER
								&& chrHasLosToAttackTarget(chr, &gunpos, gunrooms, false)
#if VERSION >= VERSION_NTSC_1_0
								&& chrCompareTeams(targetprop->chr, chr, COMPARE_ENEMIES)
#endif
								) {
							// Solo chr shooting at a player
							xdiff = targetprop->pos.x - gunpos.x - vector.x * 15.0f;
							ydiff = targetprop->pos.y - gunpos.y - vector.y * 15.0f;
							zdiff = targetprop->pos.z - gunpos.z - vector.z * 15.0f;

							if (xdiff * xdiff + ydiff * ydiff + zdiff * zdiff <= sqshotdist) {
								// Player has a chance of being hit
								chrCalculateHit(chr, &angleok, &hitplayer, &gset);

								// If the player was hit then turn off effective
								// (There's no need to check other props for
								// hits later on in this function)
								effective = !hitplayer;

								if (angleok
										&& (chr->actiontype == ACT_ATTACK
										 || chr->actiontype == ACT_ATTACKROLL
										 || chr->actiontype == ACT_BOT_ATTACKSTAND
										 || chr->actiontype == ACT_BOT_ATTACKKNEEL
										 || chr->actiontype == ACT_BOT_ATTACKSTRAFE)) {
									chr->act_attack.lastontarget60 = g_Vars.lvframe60;
								}
							}
						} else {
							// Solo chr shooting at something else
							if (chr->actiontype == ACT_ATTACK
									|| chr->actiontype == ACT_ATTACKROLL
									|| chr->actiontype == ACT_BOT_ATTACKSTAND
									|| chr->actiontype == ACT_BOT_ATTACKKNEEL
									|| chr->actiontype == ACT_BOT_ATTACKSTRAFE) {
								chr->act_attack.lastontarget60 = g_Vars.lvframe60;
							}
						}

						if (hitplayer) {
							f32 damage = gsetGetDamage(&gset);
							struct modelnode *node = NULL;
							struct model *model = NULL;
							s32 side = -1;
							s32 hitpart = HITPART_GENERAL;
							struct chrdata *targetchr = targetprop->chr;

							hitpos.x = targetprop->pos.x;
							hitpos.y = targetprop->pos.y;
							hitpos.z = targetprop->pos.z;

							if (rngRandom() % 2) {
								hitpos.y += 2 + rngRandom() % 10;
							} else {
								hitpos.y -= 2 + rngRandom() % 10;
							}

							bgunPlayPropHitSound(&gset, targetprop, -1);

							if (targetchr->model && chrGetShield(targetchr) > 0) {
								chrCalculateShieldHit(targetchr, &hitpos, &vector, &node, &hitpart, &model, &side);
							}

							func0f0341dc(targetchr, damage, &vector, &gset, chr->prop, HITPART_GENERAL, targetprop, node, model, side, NULL);
						} else if ((hitprop == NULL || (hitprop->type != PROPTYPE_CHR && hitprop->type != PROPTYPE_PLAYER))
								&& sqshotdist < 100.0f * 100.0f) {
							// Hit the background or something other than a
							// player or chr, and the shot distance was less
							// than 1 metre. Don't bother applying damage etc.
							effective = false;
						}
					}

					if (effective) {
						if (hitprop) {
							if (hitprop->type == PROPTYPE_PLAYER || hitprop->type == PROPTYPE_CHR) {
								// Hit a player or chr other than the one they
								// were aiming for
								if (isaibot
										|| fudgeforeyespy
										|| ((chr->chrflags & CHRCFLAG_NOFRIENDLYFIRE) && chrCompareTeams(hitprop->chr, chr, COMPARE_ENEMIES))) {
									struct modelnode *node = NULL;
									struct model *model = NULL;
									s32 side = -1;
									s32 hitpart = HITPART_GENERAL;
									f32 damage = gsetGetDamage(&gset);
									struct chrdata *hitchr = hitprop->chr;

									bgunPlayPropHitSound(&gset, hitprop, -1);

									if (hitchr->model && chrGetShield(hitchr) > 0) {
										chrCalculateShieldHit(hitchr, &hitpos, &vector, &node, &hitpart, &model, &side);
									}

									chrEmitSparks(hitchr, hitprop, hitpart, &hitpos, &vector, chr);
									func0f0341dc(hitchr, damage, &vector, &gset, chr->prop, HITPART_GENERAL, hitprop, node, model, side, NULL);
								} else {
									makebeam = false;
									firingthisframe = false;
								}
							} else if (hitprop->type == PROPTYPE_OBJ
									|| hitprop->type == PROPTYPE_WEAPON
									|| hitprop->type == PROPTYPE_DOOR) {
								// Hit an object
								struct defaultobj *hitobj = hitprop->obj;
								s32 playernum = -1;

								if (g_Vars.mplayerisrunning) {
									playernum = mpPlayerGetIndex(chr);
								}

								bgunPlayPropHitSound(&gset, hitprop, -1);
								func0f065e74(&gunpos, gunrooms, &hitpos, hitrooms);
								queriedhitrooms = true;

								if (chrIsUsingPaintball(chr)) {
									sparksCreate(hitrooms[0], hitprop, &hitpos, NULL, NULL, SPARKTYPE_PAINT);
								} else {
									sparksCreate(hitrooms[0], hitprop, &hitpos, NULL, NULL, SPARKTYPE_DEFAULT);
								}

								if (g_MissionConfig.iscoop && chr->team == TEAM_ALLY
										&& (hitobj->flags2 & OBJFLAG2_IMMUNETOANTI)) {
									// Co-op can't damage mission critical objects
								} else {
									objTakeGunfire(hitobj, gsetGetDamage(&gset), &hitpos, gset.weaponnum, playernum);
								}
							}
						} else if (hitsomething) {
							// Hit the background
							func0f065e74(&gunpos, gunrooms, &hitpos, hitrooms);
							queriedhitrooms = true;
							bgunPlayBgHitSound(&gset, &hitpos, -1, hitrooms);

							if (chrIsUsingPaintball(chr)) {
								sparksCreate(hitrooms[0], 0, &hitpos, NULL, NULL, SPARKTYPE_PAINT);
							} else {
								sparksCreate(hitrooms[0], 0, &hitpos, NULL, NULL, SPARKTYPE_DEFAULT);
							}
						}

						// Create explosion if using Phoenix
						if (gset.weaponnum == WEAPON_PHOENIX && gset.weaponfunc == FUNC_SECONDARY) {
							s32 playernum = chr->aibot ? mpPlayerGetIndex(chr) : g_Vars.currentplayernum;

							if (!queriedhitrooms) {
								func0f065e74(&gunpos, gunrooms, &hitpos, hitrooms);
							}

							explosionCreateSimple(NULL, &hitpos, hitrooms, EXPLOSIONTYPE_PHOENIX, playernum);
						}
					}
				}

				if (isshootingeyespy) {
					propSetPerimEnabled(targetprop, false);
				}
			}
		}

		if (makebeam) {
			switch (gset.weaponnum) {
			case WEAPON_FALCON2:
			case WEAPON_FALCON2_SILENCER:
			case WEAPON_FALCON2_SCOPE:
			case WEAPON_MAGSEC4:
			case WEAPON_MAULER:
			case WEAPON_PHOENIX:
			case WEAPON_DY357MAGNUM:
			case WEAPON_DY357LX:
			case WEAPON_CMP150:
			case WEAPON_CYCLONE:
			case WEAPON_CALLISTO:
			case WEAPON_RCP120:
			case WEAPON_LAPTOPGUN:
			case WEAPON_DRAGON:
			case WEAPON_K7AVENGER:
			case WEAPON_AR34:
			case WEAPON_SUPERDRAGON:
			case WEAPON_REAPER:
			case WEAPON_SNIPERRIFLE:
			case WEAPON_FARSIGHT:
			case WEAPON_TRANQUILIZER:
			case WEAPON_LASER:
			case WEAPON_PP9I:
			case WEAPON_CC13:
			case WEAPON_KL01313:
			case WEAPON_KF7SPECIAL:
			case WEAPON_ZZT:
			case WEAPON_DMC:
			case WEAPON_AR53:
			case WEAPON_RCP45:
				makebeam = true;
				break;
			default:
				makebeam = false;
				break;
			}
		}

		// osSyncPrintf is defined to nothing, so these print statements
		// get optimised out but leave their strings in rodata.
		// The on/off strings are surely used in a ternary statement in the
		// previous string, but making this a ternary creates a mismatch.
		osSyncPrintf("firing(%d) %d, beam(%d) %s", handnum, firingthisframe, handnum);
		osSyncPrintf("on");
		osSyncPrintf("off");

		chrUpdateFireslot(chr, handnum, firingthisframe, firingthisframe && makebeam, &gunpos, &hitpos);

		if (isaibot) {
			if (firingthisframe) {
				osSyncPrintf("firecount(%d) = %d", handnum, chr->aibot->loadedammo[handnum]);

				if (chr->aibot->loadedammo[handnum] > 0) {
					chr->aibot->loadedammo[handnum]--;
				}

				osSyncPrintf("numshots(%d) = %d", handnum, chr->aibot->loadedammo[handnum]);
			}

#ifndef PLATFORM_N64
			// OFF-TRANSITION broadcast: tell clients to clear the sim's muzzle
			// flash when firing stops. The matching on-transition is sent from
			// chrUpdateFireslot above (with soundnum>0). Compare the current
			// gunfire-visible state (`was`) against the value chrSetFiring is
			// about to set (`will`); only send when transitioning visible→hidden,
			// otherwise we'd flood the reliable channel with redundant zeros.
			// Use g_NetMsgRel so an on→off pair can't be dropped out of order.
			// Defensive: weaponIsGunfireVisible derefs heldprop->obj without
			// null-checking, so guard both pointers explicitly.
			if (g_NetMode == NETMODE_SERVER && chr->prop && chr->prop->syncid) {
				struct prop *heldprop = chrGetHeldProp(chr, handnum);
				const bool was = (heldprop && heldprop->obj) ? weaponIsGunfireVisible(heldprop) : false;
				const bool will = firingthisframe && normalshoot;
				if (was && !will) {
					netmsgSvcChrFireWrite(&g_NetMsgRel, chr, (u8)handnum, 0);
				}
			}
#endif
			chrSetFiring(chr, handnum, firingthisframe && normalshoot);
		} else {
			chrSetFiring(chr, handnum, firingthisframe);
		}
	}
}

void func0f041a74(struct chrdata *chr)
{
#if VERSION >= VERSION_NTSC_1_0
	chr->hidden2 &= ~CHRH2FLAG_FIRESOUNDDONE;
#else
	chr->hidden &= ~CHRHFLAG_FIRESOUNDDONE;
#endif

	if (chr->actiontype == ACT_ROBOTATTACK) {
		if (chr->act_robotattack.firing[0]) {
			chr->prop->forcetick = true;
			projectileCreate(chr->prop, chr->unk348[0], &chr->act_robotattack.pos[0],
					&chr->act_robotattack.dir[0], chr->act_robotattack.guntype[0], chrGetTargetProp(chr));
			chr->unk348[1]->unk08 = g_Vars.lvframe60 + 2;
			chr->unk348[1]->unk14 = chr->unk348[0]->unk14;
		} else {
			chr->prop->forcetick = false;
		}

		if (chr->act_robotattack.firing[1]) {
			chr->prop->forcetick = true;
			projectileCreate(chr->prop, chr->unk348[1], &chr->act_robotattack.pos[1],
					&chr->act_robotattack.dir[1], chr->act_robotattack.guntype[1], chrGetTargetProp(chr));
			chr->unk348[0]->unk14 = chr->unk348[1]->unk14;
		} else {
			chr->prop->forcetick = false;
		}

		beamTick(chr->unk348[0]->beam);
		beamTick(chr->unk348[1]->beam);
	} else if (chr->actiontype == ACT_ATTACKAMOUNT) {
		if (chr->act_attack.numshots < chr->act_attack.maxshots
				&& (chr->hidden & CHRHFLAG_FIRINGRIGHT)) {
			chrTickShoot(chr, HAND_RIGHT);
		}
	} else {
		if (chr->hidden & CHRHFLAG_FIRINGRIGHT) {
			chrTickShoot(chr, HAND_RIGHT);
			chr->hidden &= ~CHRHFLAG_FIRINGRIGHT;
		}

		if (chr->hidden & CHRHFLAG_FIRINGLEFT) {
			chrTickShoot(chr, HAND_LEFT);
			chr->hidden &= ~CHRHFLAG_FIRINGLEFT;
		}
	}
}

bool func0f041c44(struct chrdata *chr)
{
	if (CHRRACE(chr) == RACE_HUMAN) {
		if (chr->act_attack.animcfg == &g_RollAttackAnims[2] || chr->act_attack.animcfg == &g_RollAttackAnims[3]) {
			struct model *model = chr->model;
			struct attackanimconfig *animcfg = &g_RollAttackAnims[1];
			bool flip = model->anim->flip;

			chr->act_attack.turning = TURNSTATE_OFF;
			chr->act_attack.animcfg = animcfg;
			chr->sleep = 0;

			modelSetAnimation(model, animcfg->animnum, flip, animcfg->unk1c, chrGetRangedSpeed(chr, 0.7f, 1.12f), 22);

			if (animcfg->unk14 >= 0) {
				modelSetAnimEndFrame(model, animcfg->unk14);
			}

			return true;
		}
	}

	return false;
}

void chrAttackAmountUpdateAnimation(struct chrdata *chr)
{
	struct model *model = chr->model;

	if (chr->act_attack.animcfg->unk24 > 0) {
		modelSetAnimation(model,
				modelGetAnimNum(model),
				model->anim->flip,
				chr->act_attack.animcfg->unk24,
				chrGetRangedSpeed(chr, 0.5f, 0.8f),
				8);
	} else {
		modelSetAnimation(model,
				modelGetAnimNum(model),
				model->anim->flip,
				chr->act_attack.animcfg->unk1c,
				chrGetRangedSpeed(chr, 0.5f, 0.8f),
				8);
	}

	if (chr->act_attack.animcfg->unk14 >= 0) {
		modelSetAnimEndFrame(model, chr->act_attack.animcfg->unk14);
	}
}

void chrTickFire(struct chrdata *chr)
{
	struct model *model = chr->model;
	f32 curframe = modelGetCurAnimFrame(model);
	s32 i;

	if (modelGetAnimNum(model) == ANIM_SNIPING_GETDOWN) {
		return;
	}

	if (chr->act_attack.lastontarget60 < chr->act_attack.lastfire60 - TICKS(30)
			&& model->anim->animnum2 == 0
			&& curframe > chr->act_attack.animcfg->unk18 + 10
			&& curframe < chr->act_attack.animcfg->unk1c
			&& (chr->act_attack.animcfg->unk24 < 0 || curframe < chr->act_attack.animcfg->unk24)) {
		if (!chr->act_attack.dorecoil) {
			if (!func0f041c44(chr)) {
				modelSetAnimation(model, modelGetAnimNum(model), model->anim->flip,
						chr->act_attack.animcfg->unk1c, chrGetRangedSpeed(chr, 0.5f, 0.8f), 8);

				if (chr->act_attack.animcfg->unk14 >= 0) {
					modelSetAnimEndFrame(model, chr->act_attack.animcfg->unk14);
				}
			}
		} else {
			chrAttackAmountUpdateAnimation(chr);
		}

		chr->act_attack.numshots = chr->act_attack.maxshots + 1;

		curframe = modelGetCurAnimFrame(model);
	}

	if (curframe >= modelGetAnimEndFrame(model)) {
		if (modelGetAnimNum(model) != ANIM_SNIPING_ONGROUND
				&& (chr->act_attack.dooneburst || chr->act_attack.numshots > chr->act_attack.maxshots)) {
			if (!func0f041c44(chr)) {
				if (chr->act_attack.flags & ATTACKFLAG_AIMATTARGET) {
					chrRecordLastSeeTargetTime(chr);
				}

				chrStop(chr);
				return;
			}
		} else if (chr->act_attack.numshots == chr->act_attack.maxshots) {
			chr->act_attack.numshots++;
			chrAttackAmountUpdateAnimation(chr);
		} else if (chr->act_attack.fired) {
			f32 f2 = 0.5f;
			f32 startframe;
			f32 endframe;
			f32 diff;

			if (chr->act_attack.dorecoil) {
				if (chr->act_attack.animcfg->unk20 > 0) {
					startframe = chr->act_attack.animcfg->unk20;
				} else {
					startframe = chr->act_attack.animcfg->unk18;
				}

				if (chr->act_attack.animcfg->unk24 > 0) {
					endframe = chr->act_attack.animcfg->unk24;
				} else {
					endframe = chr->act_attack.animcfg->unk1c;
				}
			} else {
				startframe = chr->act_attack.animcfg->unk18;

				if (chr->act_attack.animcfg->unk20 > 0) {
					endframe = chr->act_attack.animcfg->unk20;
				} else {
					endframe = chr->act_attack.animcfg->unk1c;
				}
			}

			diff = endframe - startframe;

			if (diff < 12) {
				f2 = diff * (1.0f / 24.0f);
			} else if (diff > 16) {
				f2 = diff * (1.0f / 32.0f);
			}

			if (chr->act_attack.everytick[HAND_RIGHT] && chr->act_attack.everytick[HAND_LEFT]) {
				f2 = f2 + f2;
			}

			chr->act_attack.fired = false;

			modelSetAnimation(model, modelGetAnimNum(model), model->anim->flip, startframe, f2, 8);
			modelSetAnimEndFrame(model, endframe);
		}

		curframe = modelGetCurAnimFrame(model);
	}

	if (modelGetAnimNum(model) != ANIM_SNIPING_ONGROUND && (chr->act_attack.flags & ATTACKFLAG_DONTTURN) == 0) {
		f32 f2 = chr->act_attack.animcfg->unk0c;
		f32 f12 = chr->act_attack.animcfg->unk04;

		if ((chr->act_attack.flags & ATTACKFLAG_AIMONLY) && f12 > modelGetAnimEndFrame(model)) {
			f12 = modelGetAnimEndFrame(model);
		}

		if (model->anim->flip) {
			f2 = M_BADTAU - f2;
		}

		chr->act_attack.turning = chrTurn(chr, chr->act_attack.turning, f12, chrGetRangedSpeed(chr, 1, 1.6f), f2);
	}

	if ((curframe > chr->act_attack.animcfg->unk28 && curframe < chr->act_attack.animcfg->unk2c)
			|| modelGetAnimNum(model) == ANIM_SNIPING_ONGROUND) {
		func0f03e9f4(chr, chr->act_attack.animcfg, chr->act_attack.firegun[HAND_LEFT], chr->act_attack.firegun[HAND_RIGHT], 1);
	} else {
		chrResetAimEndProperties(chr);
	}

	for (i = 0; i < 2; i++) {
		if (chr->act_attack.firegun[i]) {
			if (chr->act_attack.everytick[i] == 0) {
				if (modelGetAnimNum(model) == ANIM_SNIPING_ONGROUND
						|| (curframe >= chr->act_attack.animcfg->unk18 && curframe < chr->act_attack.animcfg->unk1c)) {
					chrSetHandFiring(chr, i, true);

					chr->act_attack.lastfire60 = g_Vars.lvframe60;

					if (chr->actiontype == ACT_ATTACKROLL) {
						f32 f12 = chr->act_attack.animcfg->unk1c - chr->act_attack.animcfg->unk18;

						if (f12 < 30) {
							if (chr->act_attack.pausecount >= TICKS(60) - (s32)(PAL ? f12 * (50.0f / 60.0f) : f12) * 2) {
								modelSetAnimSpeed(model, 0.5f, 0);
							} else {
								modelSetAnimSpeed(model, 0.1f, 0);
								chr->act_attack.pausecount += g_Vars.lvupdate60;
							}
						} else {
							modelSetAnimSpeed(model, 0.5f, 0);
						}
					} else {
						modelSetAnimSpeed(model, 0.5f, 0);
					}
				} else {
					chrSetHandFiring(chr, i, false);

					if (chr->actiontype == ACT_ATTACKROLL) {
						modelSetAnimSpeed(model, chrGetRangedSpeed(chr, 0.5f, 0.8f), 0);
					} else {
						modelSetAnimSpeed(model, chrGetRangedSpeed(chr, 0.5f, 0.8f), 0);
					}
				}
			} else if (modelGetAnimNum(model) == ANIM_SNIPING_ONGROUND
					|| ((!chr->act_attack.fired && (i == chr->act_attack.nextgun || !chr->act_attack.everytick[chr->act_attack.nextgun]))
						&& ((chr->act_attack.animcfg->unk20 >= 0 && curframe >= chr->act_attack.animcfg->unk20 && curframe <= chr->act_attack.animcfg->unk24)
							|| (chr->act_attack.animcfg->unk20 < 0 && curframe >= chr->act_attack.animcfg->unk18)))) {
				chr->act_attack.fired = true;
				chr->act_attack.nextgun = 1 - chr->act_attack.nextgun;
				chr->act_attack.numshots++;
				chr->act_attack.lastfire60 = g_Vars.lvframe60;

				chrSetHandFiring(chr, i, true);
			} else {
				chrSetHandFiring(chr, i, false);
			}
		} else {
			chrSetHandFiring(chr, i, false);
		}
	}
}

void chrTickAttackAmount(struct chrdata *chr)
{
	struct model *model = chr->model;
	f32 frame = modelGetCurAnimFrame(model);
	f32 unk0c = chr->act_attack.animcfg->unk0c;
	f32 unk04 = chr->act_attack.animcfg->unk04;

	chrTurn(chr, 1, unk04, chrGetRangedSpeed(chr, 1, 1.6f), unk0c);

	if (frame > chr->act_attack.animcfg->unk28
			&& frame < chr->act_attack.animcfg->unk2c) {
		func0f03e9f4(chr, chr->act_attack.animcfg, false, true, 0.2f);
	} else {
		chrResetAimEndProperties(chr);
	}

	if (frame >= chr->act_attack.animcfg->unk18 && chr->act_attack.dooneburst == false) {
		chr->act_attack.dooneburst = true;
	}

	if (chr->act_attack.dooneburst) {
		if (chr->act_attack.numshots++ < chr->act_attack.maxshots) {
			chrSetHandFiring(chr, HAND_RIGHT, true);
		} else {
			chrAttackAmountUpdateAnimation(chr);
			chrSetHandFiring(chr, HAND_RIGHT, false);
		}
	} else {
		chrSetHandFiring(chr, HAND_RIGHT, false);
	}
}

/**
 * Sets the robot's muzzle flash to on or off.
 *
 * There are two muzzles, left and right, which is specified using the `right`
 * argument.
 */
void robotSetMuzzleFlash(struct chrdata *chr, bool right, bool visible)
{
	struct modelnode *node;
	union modelrwdata *rwdata;
	s32 partnum;

	if (right) {
		partnum = MODELPART_ROBOT_RGUNFIRE;
	} else {
		partnum = MODELPART_ROBOT_LGUNFIRE;
	}

	node = modelGetPart(chr->model->definition, partnum);

	if (node) {
		rwdata = modelGetNodeRwData(chr->model, node);
	}

	if (rwdata) {
		rwdata->chrgunfire.visible = visible;
	}
}

void robotAttack(struct chrdata *chr)
{
	u32 numshots = rngRandom() % 20;

	if (chr->unk348[0] && chr->unk348[1]) {
		chr->actiontype = ACT_ROBOTATTACK;

		chr->unk348[0]->beam->age = -1;
		chr->unk348[0]->unk00 = rngRandom() % 3;
		chr->unk348[0]->unk01 = 0;
		chr->unk348[0]->unk08 = -1;
		chr->unk348[0]->unk0c = 0.85f;

		if ((lvGetDifficulty() == DIFF_PA) * 0.2f) {
			chr->unk348[0]->unk10 = 2.0f;
		} else {
			chr->unk348[0]->unk10 = 1.0f;
		}

		chr->unk348[0]->unk14 = 0.0f;

		chr->act_robotattack.pos[0].x = 0.0f;
		chr->act_robotattack.pos[0].y = 0.0f;
		chr->act_robotattack.pos[0].z = 0.0f;
		chr->act_robotattack.dir[0].x = 0.0f;
		chr->act_robotattack.dir[0].y = 0.0f;
		chr->act_robotattack.dir[0].z = 0.0f;
		chr->act_robotattack.guntype[0] = WEAPON_WATCHLASER;
		chr->act_robotattack.firing[0] = false;

		chr->unk348[1]->beam->age = -1;
		chr->unk348[1]->unk00 = rngRandom() % 3;
		chr->unk348[1]->unk01 = 0;
		chr->unk348[1]->unk08 = -1;
		chr->unk348[1]->unk0c = 0.85f;
		chr->unk348[1]->unk10 = 0.2f;
		chr->unk348[1]->unk14 = 0.0f;

		chr->act_robotattack.guntype[1] = WEAPON_WATCHLASER;
		chr->act_robotattack.firing[1] = false;
		chr->act_robotattack.finished = false;
		chr->act_robotattack.numshots[0] = numshots;
		chr->act_robotattack.numshots[1] = numshots;
		chr->act_robotattack.pos[1].x = 0.0f;
		chr->act_robotattack.pos[1].y = 0.0f;
		chr->act_robotattack.pos[1].z = 0.0f;
		chr->act_robotattack.dir[1].x = 0.0f;
		chr->act_robotattack.dir[1].y = 0.0f;
		chr->act_robotattack.dir[1].z = 0.0f;

		chrChooseStandAnimation(chr, 16);
	}
}

void func0f0429d8(struct chrdata *chr, f32 arg1, f32 arg2)
{
	struct prop *prop = chrGetTargetProp(chr);
	f32 distance = atan2f(prop->pos.x - chr->prop->pos.x, prop->pos.z - chr->prop->pos.z);
	f32 value = modelTweenRotAxis(arg2, distance, arg1);
	chrSetLookAngle(chr, value);
}

void chrTickRobotAttack(struct chrdata *chr)
{
	s32 i;
	f32 roty = 0.0f;
	f32 rotx = 0.0f;
	struct prop *targetprop = chrGetTargetProp(chr);
	bool firing;
	bool empty;
	f32 invtheta = chrGetInverseTheta(chr);
	struct act_robotattack *act = &chr->act_robotattack;

	func0f0429d8(chr, 0.085f, invtheta);

	if (chr->model->definition->skel != &g_SkelRobot) {
		act->finished = true;
		return;
	}

	for (i = 0; i < 2; i++) {
		empty = false;

		if (act->numshots[i] > 0) {
			chr->unk348[i]->unk01 = !(chr->unk348[i]->unk00 % 3);
			firing = !(chr->unk348[i]->unk00 % 2);
		} else {
			chr->unk348[i]->unk01 = 0;
			firing = false;
		}

		act->firing[i] = firing;

		if (act->numshots[0] <= 0 && act->numshots[1] <= 0) {
			empty = true;

			if (ABS(chr->gunroty[0]) < 0.03f
					&& ABS(chr->gunrotx[0]) < 0.03f
					&& ABS(chr->gunroty[1]) < 0.03f
					&& ABS(chr->gunrotx[1]) < 0.03f) {
				act->finished = true;
			}
		}

		if (empty);
		if ((f32)empty);

		if (!empty) {
			f32 aimy;
			union modelrodata *rodata;
			struct coord spe4;
			Mtxf spa4;

			aimy = targetprop->pos.y - 20.0f;
			rodata = modelGetPartRodata(chr->model->definition, (i ? MODELPART_ROBOT_0000 : MODELPART_ROBOT_0001));

			act->pos[i].x = rodata->position.pos.x;
			act->pos[i].y = rodata->position.pos.y - 300.0f;
			act->pos[i].z = rodata->position.pos.z;

			mtx4LoadYRotation(invtheta, &spa4);
			mtx4RotateVec(&spa4, &act->pos[i], &spe4);

			spe4.x *= chr->model->scale;
			spe4.y *= chr->model->scale;
			spe4.z *= chr->model->scale;

			act->pos[i].x = spe4.x + chr->prop->pos.x;
			act->pos[i].y = spe4.y + chr->prop->pos.y;
			act->pos[i].z = spe4.z + chr->prop->pos.z;

			roty = atan2f(targetprop->pos.x - act->pos[i].x, targetprop->pos.z - act->pos[i].z) - invtheta;

			if (roty < 0.0f) {
				roty += M_BADTAU;
			}

			if (roty > M_BADPI) {
				roty -= M_BADTAU;
			}

			if (roty < -0.524f) {
				roty = -0.524f;
			}

			if (roty > 0.524f) {
				roty = 0.524f;
			}

#define X() (targetprop->pos.x - act->pos[i].x)
#define Z() (targetprop->pos.z - act->pos[i].z)

			rotx = M_BADTAU - atan2f(aimy - act->pos[i].y, sqrtf(Z() * Z() + X() * X()));

			if (rotx > M_BADPI) {
				rotx -= M_BADTAU;
			}

			if (rotx < -0.524f) {
				rotx = -0.524f;
			}

			if (rotx > 0.524f) {
				rotx = 0.524f;
			}
		}

		chr->gunroty[i] += (roty - chr->gunroty[i]) * 0.15f;
		chr->gunrotx[i] += (rotx - chr->gunrotx[i]) * 0.15f;

		if (!empty) {
			if (firing) {
				f32 gunrotx = chr->gunrotx[i];
				f32 gunroty = chr->gunroty[i];

				if (gunrotx < 0.0f) {
					gunrotx += M_BADTAU;
				}

				if (gunroty < 0.0f) {
					gunroty += M_BADTAU;
				}

				gunroty += invtheta;

				if (gunroty >= M_BADTAU) {
					gunroty -= M_BADTAU;
				}

				act->dir[i].x = sinf(gunroty) * cosf(gunrotx);
				act->dir[i].y = -sinf(gunrotx);
				act->dir[i].z = cosf(gunroty) * cosf(gunrotx);

				robotSetMuzzleFlash(chr, i, true);

				act->numshots[i]--;
			}

			chr->unk348[i]->unk00++;
		}
	}
}

void chrTickAttack(struct chrdata *chr)
{
	struct model *model = chr->model;
	f32 curframe = modelGetCurAnimFrame(model);

	if (chr->hidden & CHRHFLAG_NEEDANIM) {
		if (modelIsAnimMerging(chr->model)) {
			return;
		}

		modelSetAnimation(model, chr->act_attack.animcfg->animnum, chr->act_attack.flip,
				chr->act_attack.animcfg->unk10, chrGetRangedSpeed(chr, 0.5f, 0.8f), 16);
		func0f031254(chr);
		chr->hidden &= ~CHRHFLAG_NEEDANIM;
	}

	if (chr->act_attack.animcfg->animnum == ANIM_SNIPING_GETDOWN) {
		if (curframe >= modelGetAnimEndFrame(model)) {
			chr->act_attack.animcfg = var80067d70;

			modelSetAnimation(model, chr->act_attack.animcfg->animnum, chr->act_attack.flip,
					chr->act_attack.animcfg->unk10, chrGetRangedSpeed(chr, 0.5f, 0.8f), 16);
		}
	}

	if (!chr->aibot && chr->act_attack.reaim != 0) {
		if (chr->act_attack.reaim == 1) {
			f32 startframe;

			if (chr->act_attack.animcfg->unk24 >= 0) {
				startframe = chr->act_attack.animcfg->unk24;
			} else {
				startframe = chr->act_attack.animcfg->unk1c;
			}

			modelSetAnimation(model, modelGetAnimNum(model), model->anim->flip, startframe, chrGetRangedSpeed(chr, 0.5f, 0.8f), 16);

			if (chr->act_attack.animcfg->unk14 >= 0) {
				modelSetAnimEndFrame(model, chr->act_attack.animcfg->unk14);
			}

			chr->act_attack.reaim = 2;
			chrResetAimEndProperties(chr);
			return;
		}

		if (chr->act_attack.reaim == 2) {
			if (curframe >= modelGetAnimEndFrame(model)) {
				chr->act_attack.flags &= ~ATTACKFLAG_DONTTURN;

				if (chr->act_attack.standing) {
					chrAttackStand(chr, chr->act_attack.flags, chr->act_attack.entityid);
				} else {
					chrAttackKneel(chr, chr->act_attack.flags, chr->act_attack.entityid);
				}
			}
			return;
		}
	}

	if (!chr->aibot && (chr->act_attack.flags & ATTACKFLAG_AIMONLY)) {
		if (chr->act_attack.flags & ATTACKFLAG_DONTTURN) {
			if (!func0f03e9f4(chr, chr->act_attack.animcfg, chr->act_attack.firegun[HAND_LEFT], chr->act_attack.firegun[HAND_RIGHT], 0.2f)) {
				chr->act_attack.reaim = 1;
			}
			return;
		}

		if (curframe >= modelGetAnimEndFrame(model)) {
			chr->act_attack.flags |= ATTACKFLAG_DONTTURN;
			chr->act_attack.turning = TURNSTATE_OFF;
			return;
		}
	}

	if (!chr->aibot
			&& chr->act_attack.dorecoil == 0
			&& chr->act_attack.animcfg->unk24 > 0
			&& curframe <= chr->act_attack.animcfg->unk24
			&& curframe >= modelGetAnimEndFrame(model)) {
		modelSetAnimation(model, modelGetAnimNum(model), model->anim->flip,
				chr->act_attack.animcfg->unk24, chrGetRangedSpeed(chr, 0.5f, 0.8f), 16);

		if (chr->act_attack.dooneburst) {
			if (chr->act_attack.animcfg->unk14 >= 0) {
				modelSetAnimEndFrame(model, chr->act_attack.animcfg->unk14);
			}
		} else {
			modelSetAnimEndFrame(model, chr->act_attack.animcfg->unk1c);
		}
	}

	chrTickFire(chr);
}

void chrTickAttackRoll(struct chrdata *chr)
{
	if (chr->hidden & CHRHFLAG_NEEDANIM) {
		if (modelIsAnimMerging(chr->model)) {
			return;
		}

		chrAttackRollChooseAnimation(chr);

		chr->hidden &= ~CHRHFLAG_NEEDANIM;
	}

	if (chr->act_attack.onehanded) {
		struct model *model = chr->model;
		f32 curframe = modelGetCurAnimFrame(model);

		if (chr->act_attack.animcfg == &g_RollAttackAnims[4]
				|| chr->act_attack.animcfg == &g_RollAttackAnims[5]
				|| chr->act_attack.animcfg == &g_RollAttackAnims[6]
				|| chr->act_attack.animcfg == &g_RollAttackAnims[7]) {
			if (curframe >= chr->act_attack.animcfg->unk14) {
				struct attackanimconfig *newanimcfg = chr->act_attack.animcfg + 4;
				bool flip = model->anim->flip;
				f32 sp34 = 16;

				if (chr->act_attack.firegun[HAND_LEFT] && chr->act_attack.firegun[HAND_RIGHT]) {
					if (rngRandom() % 2 == 0) {
						newanimcfg += 4;
					} else {
						newanimcfg += 8;
					}
				}

				if (newanimcfg == &g_RollAttackAnims[8]) {
					sp34 = 24;
				} else if (newanimcfg == &g_RollAttackAnims[9]) {
					sp34 = 24;
				} else if (newanimcfg == &g_RollAttackAnims[10]) {
					sp34 = 32;
				} else if (newanimcfg == &g_RollAttackAnims[11]) {
					sp34 = 44;
				} else if (newanimcfg == &g_RollAttackAnims[12]) {
					sp34 = 24;
				} else if (newanimcfg == &g_RollAttackAnims[13]) {
					sp34 = 34;
				} else if (newanimcfg == &g_RollAttackAnims[14]) {
					sp34 = 32;
				} else if (newanimcfg == &g_RollAttackAnims[15]) {
					sp34 = 44;
				} else if (newanimcfg == &g_RollAttackAnims[16]) {
					sp34 = 24;
				} else if (newanimcfg == &g_RollAttackAnims[17]) {
					sp34 = 34;
				} else if (newanimcfg == &g_RollAttackAnims[18]) {
					sp34 = 32;
				} else if (newanimcfg == &g_RollAttackAnims[19]) {
					sp34 = 44;
				}

				chr->act_attack.turning = TURNSTATE_OFF;
				chr->act_attack.animcfg = newanimcfg;
				chr->sleep = 0;

				modelSetAnimation(model, newanimcfg->animnum, flip, newanimcfg->unk10,
						chrGetRangedSpeed(chr, 0.5f, 0.8f), sp34);

				if (chr->act_attack.dorecoil) {
					if (newanimcfg->unk24 >= 0.0f) {
						modelSetAnimEndFrame(model, newanimcfg->unk24);
					} else {
						modelSetAnimEndFrame(model, newanimcfg->unk1c);
					}
				} else {
					if (newanimcfg->unk20 >= 0.0f) {
						modelSetAnimEndFrame(model, newanimcfg->unk20);
					} else {
						if (newanimcfg->unk14 >= 0.0f) {
							modelSetAnimEndFrame(model, newanimcfg->unk14);
						}
					}
				}

				if (chr->act_attack.animcfg->unk0c != 0.0f) {
					union modelrwdata *rwdata = modelGetNodeRwData(model, model->definition->rootnode);
					rwdata->chrinfo.unk5c = sp34;
					rwdata->chrinfo.unk58 = -chr->act_attack.animcfg->unk0c / sp34;

					if (flip) {
						rwdata->chrinfo.unk58 = -rwdata->chrinfo.unk58;
					}
				}
			}
		} else if (chr->act_attack.animcfg == &g_RollAttackAnims[8]
				|| chr->act_attack.animcfg == &g_RollAttackAnims[9]
				|| chr->act_attack.animcfg == &g_RollAttackAnims[10]
				|| chr->act_attack.animcfg == &g_RollAttackAnims[11]
				|| chr->act_attack.animcfg == &g_RollAttackAnims[12]
				|| chr->act_attack.animcfg == &g_RollAttackAnims[13]
				|| chr->act_attack.animcfg == &g_RollAttackAnims[14]
				|| chr->act_attack.animcfg == &g_RollAttackAnims[15]
				|| chr->act_attack.animcfg == &g_RollAttackAnims[16]
				|| chr->act_attack.animcfg == &g_RollAttackAnims[17]
				|| chr->act_attack.animcfg == &g_RollAttackAnims[18]
				|| chr->act_attack.animcfg == &g_RollAttackAnims[19]) {
			if (!chr->act_attack.dorecoil
					&& chr->act_attack.animcfg->unk24 > 0
					&& curframe <= chr->act_attack.animcfg->unk24
					&& curframe >= modelGetAnimEndFrame(model)) {
				modelSetAnimation(model, modelGetAnimNum(model), model->anim->flip,
						chr->act_attack.animcfg->unk24, chrGetRangedSpeed(chr, 0.5f, 0.8f), 16);

				if (chr->act_attack.dooneburst) {
					if (chr->act_attack.animcfg->unk14 >= 0) {
						modelSetAnimEndFrame(model, chr->act_attack.animcfg->unk14);
					}
				} else {
					u32 stack;
					modelSetAnimEndFrame(model, chr->act_attack.animcfg->unk1c);
				}
			}
		}
	}

	chrTickFire(chr);
}

void propPrintDangerous(void)
{
	u8 i;

	osSyncPrintf("Current dangerous items:");

	for (i = 0; i < ARRAYCOUNT(g_DangerousProps); i++) {
		struct prop *prop = g_DangerousProps[i];

		if (prop) {
			if (prop->weapon
					&& prop->weapon->weaponnum == WEAPON_GRENADE
					&& prop->type == PROPTYPE_WEAPON) {
				osSyncPrintf("    Grenade %x", prop);
			} else if (prop->type == PROPTYPE_EXPLOSION) {
				osSyncPrintf("    Explosion %x", prop);
			} else {
				osSyncPrintf("misc dangerous prop");
			}
		}
	}
}

void propUnsetDangerous(struct prop *prop)
{
	s32 i;

	for (i = 0; i < ARRAYCOUNT(g_DangerousProps); i++) {
		if (g_DangerousProps[i] == prop) {
			g_DangerousProps[i] = NULL;
			return;
		}
	}
}

void propSetDangerous(struct prop *prop)
{
	s32 i;

	for (i = 0; i < ARRAYCOUNT(g_DangerousProps); i++) {
		if (g_DangerousProps[i] == NULL) {
			g_DangerousProps[i] = prop;
			return;
		}
	}
}

void chrTickThrowGrenade(struct chrdata *chr)
{
	struct model *model;
	f32 frame;
	u32 hand;
	struct prop *weaponprop;
	struct defaultobj *obj;
	struct weaponobj *weapon;
	f32 frame2;

	if (chr->hidden & CHRHFLAG_NEEDANIM) {
		if (modelIsAnimMerging(chr->model)) {
			return;
		}

		chrThrowGrenadeChooseAnimation(chr);
		chr->hidden &= ~CHRHFLAG_NEEDANIM;
	}

	model = chr->model;
	frame = modelGetCurAnimFrame(model);
	hand = model->anim->flip ? HAND_LEFT : HAND_RIGHT;
	weaponprop = chrGetHeldProp(chr, hand);

	// Decide at which frame the grenade should become visible in the chr's hand
	if ((frame >= 20 && weaponprop && modelGetAnimNum(model) == ANIM_THROWGRENADE_STANDING)
			|| (frame >= 1 && weaponprop && modelGetAnimNum(model) == ANIM_THROWGRENADE_NOPIN)
			|| (frame >= 1 && weaponprop && modelGetAnimNum(model) == ANIM_THROWGRENADE_CROUCHING)) {
		obj = weaponprop->obj;
		obj->hidden &= ~OBJHFLAG_GONE;
	}

	// Decide at which frame the grenade leaves the chr's hand
	if ((frame >= 119 && weaponprop && modelGetAnimNum(model) == ANIM_THROWGRENADE_STANDING)
			|| (frame >= 57 && weaponprop && modelGetAnimNum(model) == ANIM_THROWGRENADE_NOPIN)
			|| (frame >= 58 && weaponprop && modelGetAnimNum(model) == ANIM_THROWGRENADE_CROUCHING)) {
		weapon = weaponprop->weapon;
		objSetDropped(weaponprop, DROPTYPE_THROWGRENADE);
		chr->hidden |= CHRHFLAG_DROPPINGITEM;
		weapon->timer240 = TICKS(240);
	}

	frame2 = modelGetCurAnimFrame(model);

	if (frame2 >= modelGetAnimEndFrame(model)) {
		chrStop(chr);
	} else {
		if ((frame >= 87 && frame <= 110 && modelGetAnimNum(model) == ANIM_THROWGRENADE_STANDING)
				|| (frame >= 5 && frame <= 45 && modelGetAnimNum(model) == ANIM_THROWGRENADE_NOPIN)
				|| ((frame >= 20 && frame <= 45 && modelGetAnimNum(model) == ANIM_THROWGRENADE_CROUCHING))) {
			f32 value = chrGetRangedSpeed(chr, 1, 3.2);
			chrTurn(chr, 1, 110, value, 0);
		}
	}
}

bool chrDetectDangerousObject(struct chrdata *chr, u8 flags)
{
	s32 i;

	for (i = 0; i < ARRAYCOUNT(g_DangerousProps); i++) {
		struct prop *prop = g_DangerousProps[i];
		bool pass = false;

		if (prop) {
			if ((flags & 1) && prop->weapon &&
					prop->weapon->weaponnum == WEAPON_GRENADE &&
					prop->weapon->timer240 < TICKS(480)) {
				pass = true;
			}

			if ((flags & 2) && prop->type == PROPTYPE_EXPLOSION) {
				pass = true;
			}

			if (pass && chrGetSquaredDistanceToCoord(chr, &prop->pos) < 1600) {
				chr->runfrompos.x = g_DangerousProps[i]->pos.x;
				chr->runfrompos.y = g_DangerousProps[i]->pos.y;
				chr->runfrompos.z = g_DangerousProps[i]->pos.z;

				if (chr->aibot) {
					chr->aibot->flags |= BOTFLAG_AVOIDINGDANGEROUSPROP;
					chr->aibot->dangerouspropnum = i;
				}

				return true;
			}
		}
	}

	if (chr->aibot) {
		chr->aibot->flags &= ~BOTFLAG_AVOIDINGDANGEROUSPROP;
		chr->aibot->dangerouspropnum = -1;
	}

	return false;
}

void chrTickBondDie(struct chrdata *chr)
{
	// empty
}

bool func0f043f2c(struct chrdata *chr, struct coord *runpos, u32 arg2, f32 *turnspeed)
{
	struct prop *prop = chr->prop;
	struct model *model = chr->model;
	f32 accel;
	f32 maxspeed;
	f32 angle1;
	f32 finalangle;
	f32 angle2;
	f32 angle3;
	f32 xdiff = runpos->f[0] - prop->pos.f[0];
	f32 zdiff = runpos->f[2] - prop->pos.f[2];
	bool result;

	result = false;
	angle1 = atan2f(xdiff, zdiff);
	finalangle = chrGetRotY(chr);
	angle2 = angle1 - finalangle;

	if (finalangle > angle1) {
		angle2 += M_BADTAU;
	}

	if (chr->aibot) {
		if (chr->blurdrugamount > 0) {
			angle1 += chr->blurdrugamount * PALUPF(0.00031410926021636f) * sinf((g_Vars.lvframe60 % TICKS(1200)) * PALUPF(0.0052351541817188f));

			if (angle1 >= M_BADTAU) {
				angle1 -= M_BADTAU;
			}

			angle1 += M_BADTAU;
		}

		finalangle = angle1;
		*turnspeed = 0;
		result = true;
	} else {
		angle3 = angle2;

		if (angle2 > M_BADPI) {
			angle3 = M_BADTAU - angle3;
		}

		if ((arg2 % 4) == 2) {
			maxspeed = 0.29915165901184f;
			accel = 0.014957583509386f;
		} else if ((arg2 % 4) == 1) {
			if (angle3 < 0.3926365673542f) {
				maxspeed = 0.019631829112768f;
			} else if (angle3 < 1.2564370632172f) {
				maxspeed = 0.098159141838551f;
			} else {
				maxspeed = 0.1963182836771f;
			}

			accel = 0.014957583509386f;
		} else {
			if (angle3 < 0.3926365673542f) {
				maxspeed = 0.0098159145563841f;
			} else if (angle3 < 1.2564370632172f) {
				maxspeed = 0.049079570919275f;
			} else {
				maxspeed = 0.12564370036125f;
			}

			accel = 0.0098159145563841f;
		}

		maxspeed *= model->anim->playspeed;
		accel *= model->anim->playspeed;

		applyRotation(&finalangle, angle1, turnspeed, accel, accel + accel, maxspeed);

		if (ABS(finalangle - angle1) < 0.01f) {
			*turnspeed = 0;
			result = true;
		}
	}

	chrSetRotY(chr, finalangle);

	return result;
}

void chrTickAttackWalk(struct chrdata *chr)
{
	struct model *model = chr->model;
	struct prop *prop = chr->prop;
	struct prop *targetprop = chrGetTargetProp(chr);
	s32 i;
	f32 xdiff;
	f32 zdiff;

	if (chr->hidden & CHRHFLAG_NEEDANIM) {
		if (modelIsAnimMerging(chr->model)) {
			return;
		}

		chrAttackWalkChooseAnimation(chr);

		chr->hidden &= ~CHRHFLAG_NEEDANIM;
	}

	chr->act_attackwalk.frame60count += g_Vars.lvupdate60;
	chr->lastwalk60 = g_Vars.lvframe60;

	// If stuck or time exceeded, stop
	if (chr->invalidmove == 1
			|| chr->lastmoveok60 < g_Vars.lvframe60 - TICKS(60)
			|| chr->act_attackwalk.frame60count > chr->act_attackwalk.frame60max) {
		if (modelGetCurAnimFrame(model) > modelGetNumAnimFrames(model) * 0.5f) {
			modelSetAnimSpeedAuto(model, 0, 16);
		} else {
			modelSetAnimSpeedAuto(model, modelGetNumAnimFrames(model) * 0.5f, 16);
		}

		chrRecordLastSeeTargetTime(chr);
		chrStop(chr);
		return;
	}

	// If within 3 metres of the player, stop
	xdiff = targetprop->pos.x - prop->pos.x;
	zdiff = targetprop->pos.z - prop->pos.z;

	if (xdiff < 300 && xdiff > -300 && zdiff < 300 && zdiff > -300) {
		chrRecordLastSeeTargetTime(chr);
		chrStop(chr);
		return;
	}

	if (func0f043f2c(chr, &targetprop->pos, 0, &chr->act_attackwalk.turnspeed)) {
		chr->act_attackwalk.facedtarget = true;
	}

	if (chr->act_attackwalk.frame60count > TICKS(20)) {
		func0f03e9f4(chr, chr->act_attackwalk.animcfg,
				chr->act_attackwalk.firegun[HAND_LEFT],
				chr->act_attackwalk.firegun[HAND_RIGHT], 1);
	} else {
		chrResetAimEndProperties(chr);
	}

	if (chr->act_attackwalk.facedtarget && chr->act_attackwalk.frame60count > TICKS(30)) {
		for (i = 0; i < 2; i++) {
			if (chr->act_attackwalk.firegun[i]) {
				if (!chr->act_attackwalk.everytick[i]) {
					chrSetHandFiring(chr, i, true);
				} else if (chr->act_attackwalk.frame60count > chr->act_attackwalk.nextshot60
						&& (i == chr->act_attackwalk.nextgun || chr->act_attackwalk.everytick[chr->act_attackwalk.nextgun] == 0)) {
					chr->act_attackwalk.nextshot60 = chr->act_attackwalk.frame60count;

					if (chr->act_attackwalk.everytick[1 - i]) {
						if (chr->act_attackwalk.singleshot[i]) {
							chr->act_attackwalk.nextshot60 += TICKS(90);
						} else {
							chr->act_attackwalk.nextshot60 += TICKS(20);
						}
					} else {
						if (chr->act_attackwalk.singleshot[i]) {
							chr->act_attackwalk.nextshot60 += TICKS(180);
						} else {
							chr->act_attackwalk.nextshot60 += TICKS(40);
						}
					}

					chr->act_attackwalk.nextgun = 1 - chr->act_attackwalk.nextgun;

					chrSetHandFiring(chr, i, true);
				} else {
					chrSetHandFiring(chr, i, false);
				}
			} else {
				chrSetHandFiring(chr, i, false);
			}
		}
	} else {
		chrSetHandFiring(chr, HAND_LEFT, false);
		chrSetHandFiring(chr, HAND_RIGHT, false);
	}
}

/**
 * This function might be misnamed. It was named by isolating it and testing it
 * with different inputs to see what it returns, but I couldn't determine how
 * the range affects the latter part of the function.
 */
bool posIsMovingTowardsPosOrStoppedInRange(struct coord *prevpos, struct coord *moveddelta, struct coord *targetpos, f32 range)
{
	struct coord prevdist;
	f32 tmp;

	prevdist.x = targetpos->x - prevpos->x;
	prevdist.z = targetpos->z - prevpos->z;

	if (moveddelta->f[0] == 0 && moveddelta->f[2] == 0) {
		return prevdist.f[0] * prevdist.f[0] + prevdist.f[2] * prevdist.f[2] <= range * range;
	}

	tmp = moveddelta->f[0] * prevdist.f[0] + moveddelta->f[2] * prevdist.f[2];

	if (tmp > 0) {
		f32 sqmoveddist = moveddelta->f[0] * moveddelta->f[0] + moveddelta->f[2] * moveddelta->f[2];
		f32 sqprevdist = prevdist.f[0] * prevdist.f[0] + prevdist.f[2] * prevdist.f[2];

		if ((sqprevdist - range * range) * sqmoveddist <= tmp * tmp) {
			return true;
		}

		return false;
	}

	return false;
}

/**
 * Return true if:
 * - either prevpos or curpos is within the given range of targetpos, and
 * - the prev -> cur delta is moving towards the targetpos or is stopped inside
 *   the range.
 *
 * This is a lateral check, meaning the Y value is not considered.
 */
bool posIsArrivingLaterallyAtPos(struct coord *prevpos, struct coord *curpos, struct coord *targetpos, f32 range)
{
	struct coord moveddelta;

	if (prevpos->x <= targetpos->x - range && curpos->x <= targetpos->x - range) {
		return false;
	}

	if (prevpos->x >= targetpos->x + range && curpos->x >= targetpos->x + range) {
		return false;
	}

	if (prevpos->z <= targetpos->z - range && curpos->z <= targetpos->z - range) {
		return false;
	}

	if (prevpos->z >= targetpos->z + range && curpos->z >= targetpos->z + range) {
		return false;
	}

	moveddelta.x = curpos->x - prevpos->x;
	moveddelta.y = 0;
	moveddelta.z = curpos->z - prevpos->z;

	return posIsMovingTowardsPosOrStoppedInRange(prevpos, &moveddelta, targetpos, range);
}

/**
 * Return true if:
 * - either prevpos or curpos is within the given range of targetpos,
 * - the prev -> cur delta is moving towards the targetpos or is stopped inside
 *   the range, and
 * - either prevpos or curpos is within 150cm vertically of targetpos.
 */
bool posIsArrivingAtPos(struct coord *prevpos, struct coord *curpos, struct coord *targetpos, f32 range)
{
	if (prevpos->y <= targetpos->y - 150 && curpos->y <= targetpos->y - 150) {
		return false;
	}

	if (prevpos->y >= targetpos->y + 150 && curpos->y >= targetpos->y + 150) {
		return false;
	}

	return posIsArrivingLaterallyAtPos(prevpos, curpos, targetpos, range);
}

void chrTickRunPos(struct chrdata *chr)
{
	struct prop *prop = chr->prop;
	struct model *model = chr->model;
	u32 race = CHRRACE(chr);
	f32 fVar6;
	f32 zero;
	f32 fVar7;

	if (chr->hidden & CHRHFLAG_NEEDANIM) {
		if (modelIsAnimMerging(chr->model)) {
			return;
		}

		chrRunPosChooseAnimation(chr);
		chr->hidden &= ~CHRHFLAG_NEEDANIM;
	}

	chr->lastwalk60 = g_Vars.lvframe60;

	if (chr->invalidmove == 1
			|| g_Vars.lvframe60 - TICKS(60) > chr->lastmoveok60
			|| posIsArrivingLaterallyAtPos(&chr->prevpos, &prop->pos, &chr->act_runpos.pos, chr->act_runpos.neardist)) {
		if (race == RACE_HUMAN) {
			modelGetAnimNum(model);
		}

		zero = 0;
		fVar7 = modelGetCurAnimFrame(model);
		fVar6 = fVar7 - zero;

		if (fVar7 < 0) {
			fVar6 += modelGetNumAnimFrames(model);
		}

		if (modelGetNumAnimFrames(model) * 0.5f < fVar6) {
			zero = 0;
			modelSetAnimSpeedAuto(model, modelGetNumAnimFrames(model) - zero, 16);
		} else {
			zero = 0;
			fVar7 = modelGetNumAnimFrames(model) * 0.5f;
			fVar6 = fVar7 - zero;

			if (fVar7 < 0) {
				fVar6 += modelGetNumAnimFrames(model);
			}

			modelSetAnimSpeedAuto(model, fVar6, 16);
		}

		chrStop(chr);
		return;
	}

	func0f043f2c(chr, &chr->act_runpos.pos, 1, &chr->act_runpos.turnspeed);

	if (chr->act_runpos.eta60 > 0) {
		chr->act_runpos.eta60 -= g_Vars.lvupdate60;
	} else {
		fVar7 = 1;

		if (race == RACE_HUMAN) {
			if (modelGetAnimNum(model) == ANIM_RUNNING_ONEHANDGUN) {
				fVar7 = func0f02dff0(ANIM_RUNNING_ONEHANDGUN);
			} else {
				fVar7 = func0f02dff0(ANIM_RUNNING_TWOHANDGUN);
			}
		} else if (race == RACE_SKEDAR) {
			fVar7 = func0f02dff0(ANIM_SKEDAR_RUNNING);
		}

		chr->act_runpos.neardist += fVar7 * g_Vars.lvupdate60freal * modelGetAbsAnimSpeed(model);
	}
}

void func0f044b68(struct coord *arg0, struct coord *arg1, struct coord *arg2)
{
	struct coord sp0c;
	struct coord sp00;

	sp0c.x = arg1->x - arg0->x;
	sp0c.y = arg1->y - arg0->y;
	sp0c.z = arg1->z - arg0->z;

	sp00.x = -arg2->z;
	sp00.y = 0;
	sp00.z = arg2->x;

	if (sp00.f[0] * sp0c.f[0] + sp00.f[2] * sp0c.f[2] > 0) {
		// empty
	} else {
		sp0c.x = arg0->x;
		sp0c.y = arg0->y;
		sp0c.z = arg0->z;

		arg0->x = arg1->x;
		arg0->y = arg1->y;
		arg0->z = arg1->z;

		arg1->x = sp0c.x;
		arg1->y = sp0c.y;
		arg1->z = sp0c.z;
	}
}

/**
 * Check to see if the chr can see the next pos in their route.
 *
 * If an obstacle is found, set the leftpos and rightpos coordinates to the
 * left and right corners of the object from the chr's perspective and return
 * false.
 *
 * Return true if path ahead is clear.
 *
 * This is similar to chrNavCheckForObstacle. The difference between the two are
 * not yet understood.
 */
bool chrNavCanSeeNextPos(struct chrdata *chr, struct coord *chrpos, RoomNum *chrrooms, struct coord *aimpos, struct coord *leftpos, struct coord *rightpos, f32 negchrradius, f32 chrradius, s32 cdtypes, s32 arg9)
{
	struct coord spd4;
	f32 spd0;
	f32 spcc;
	f32 spc8;
	f32 spc4;
	f32 norm;
	bool spbc = false;
	bool spb8 = false;
	struct coord spac;
	struct coord spa0;
	struct coord sp94;
	struct coord sp88;
	bool result = false;
	f32 ymax;
	f32 ymin;
	f32 radius2;
	struct coord sp6c;
	struct coord sp60;
	RoomNum sp50[8];
	RoomNum sp40[8];
	struct prop *prop = chr->prop;

	chrGetBbox(prop, &radius2, &ymax, &ymin);

	spd4.x = aimpos->x - chrpos->x;
	spd4.y = 0.0f;
	spd4.z = aimpos->z - chrpos->z;

	if (spd4.f[0] == 0.0f && spd4.f[2] == 0.0f) {
		return true;
	}

	norm = 1.0f / sqrtf(spd4.f[0] * spd4.f[0] + spd4.f[2] * spd4.f[2]);
	spd4.x *= norm;
	spd4.z *= norm;

	spd0 = spd4.x * chrradius * 0.95f;
	spcc = spd4.z * chrradius * 0.95f;
	spc8 = spd4.x * chrradius * 1.2f;
	spc4 = spd4.z * chrradius * 1.2f;

	chrSetPerimEnabled(chr, false);

	sp6c.x = chrpos->x + spcc;
	sp6c.y = chrpos->y;
	sp6c.z = chrpos->z - spd0;
	sp60.x = (spd4.x * negchrradius) + (aimpos->x + spc4);
	sp60.y = aimpos->y;
	sp60.z = (spd4.z * negchrradius) + (aimpos->z - spc8);

	if (cdExamCylMove07(chrpos, chrrooms, &sp6c, sp50, cdtypes, 1, ymax - prop->pos.y, ymin - prop->pos.y) == CDRESULT_COLLISION
			|| cdExamCylMove03(&sp6c, sp50, &sp60, cdtypes, 1, ymax - prop->pos.y, ymin - prop->pos.y) == CDRESULT_COLLISION) {
		spbc = true;
#if VERSION >= VERSION_JPN_FINAL
		cdGetEdge(&spac, &spa0, 14154, "chr/chraction.c");
#elif VERSION >= VERSION_PAL_FINAL
		cdGetEdge(&spac, &spa0, 14151, "chr/chraction.c");
#elif VERSION >= VERSION_PAL_BETA
		cdGetEdge(&spac, &spa0, 14146, "chraction.c");
#elif VERSION >= VERSION_NTSC_1_0
		cdGetEdge(&spac, &spa0, 14145, "chraction.c");
#else
		cdGetEdge(&spac, &spa0, 14158, "chraction.c");
#endif
		func0f044b68(&spac, &spa0, &spd4);
	}

	sp6c.x = chrpos->x - spcc;
	sp6c.y = chrpos->y;
	sp6c.z = chrpos->z + spd0;

	sp60.x = (spd4.x * negchrradius) + (aimpos->x - spc4);
	sp60.y = aimpos->y;
	sp60.z = (spd4.z * negchrradius) + (aimpos->z + spc8);

	if (cdExamCylMove07(chrpos, chrrooms, &sp6c, sp50, cdtypes, 1, ymax - prop->pos.y, ymin - prop->pos.y) == CDRESULT_COLLISION
			|| cdExamCylMove03(&sp6c, chrrooms, &sp60, cdtypes, 1, ymax - prop->pos.y, ymin - prop->pos.y) == CDRESULT_COLLISION) {
		spb8 = true;
#if VERSION >= VERSION_JPN_FINAL
		cdGetEdge(&sp94, &sp88, 14169, "chr/chraction.c");
#elif VERSION >= VERSION_PAL_FINAL
		cdGetEdge(&sp94, &sp88, 14166, "chr/chraction.c");
#elif VERSION >= VERSION_PAL_BETA
		cdGetEdge(&sp94, &sp88, 14161, "chraction.c");
#elif VERSION >= VERSION_NTSC_1_0
		cdGetEdge(&sp94, &sp88, 14160, "chraction.c");
#else
		cdGetEdge(&sp94, &sp88, 14173, "chraction.c");
#endif
		func0f044b68(&sp94, &sp88, &spd4);
	}

	if (spbc && spb8) {
		func0f044b68(&spac, &sp94, &spd4);
		func0f044b68(&spa0, &sp88, &spd4);

		leftpos->x = spac.x;
		leftpos->y = spac.y;
		leftpos->z = spac.z;

		rightpos->x = sp88.x;
		rightpos->y = sp88.y;
		rightpos->z = sp88.z;
	} else if (spbc) {
		leftpos->x = spac.x;
		leftpos->y = spac.y;
		leftpos->z = spac.z;

		rightpos->x = spa0.x;
		rightpos->y = spa0.y;
		rightpos->z = spa0.z;
	} else if (spb8) {
		leftpos->x = sp94.x;
		leftpos->y = sp94.y;
		leftpos->z = sp94.z;

		rightpos->x = sp88.x;
		rightpos->y = sp88.y;
		rightpos->z = sp88.z;
	} else if (cdExamCylMove07(chrpos, chrrooms, aimpos, sp40, cdtypes, 1, ymax - prop->pos.y, ymin - prop->pos.y) != CDRESULT_COLLISION
			&& (!arg9 || cdExamCylMove01(chrpos, aimpos, chrradius, sp40, cdtypes, CHECKVERTICAL_YES, ymax - prop->pos.y, ymin - prop->pos.y) != CDRESULT_COLLISION)) {
		result = true;
	} else {
#if VERSION >= VERSION_JPN_FINAL
		cdGetEdge(leftpos, rightpos, 14239, "chr/chraction.c");
#elif VERSION >= VERSION_PAL_FINAL
		cdGetEdge(leftpos, rightpos, 14236, "chr/chraction.c");
#elif VERSION >= VERSION_PAL_BETA
		cdGetEdge(leftpos, rightpos, 14231, "chraction.c");
#elif VERSION >= VERSION_NTSC_1_0
		cdGetEdge(leftpos, rightpos, 14230, "chraction.c");
#else
		cdGetEdge(leftpos, rightpos, 14243, "chraction.c");
#endif
		func0f044b68(leftpos, rightpos, &spd4);
	}

	chrSetPerimEnabled(chr, true);

	return result;
}

/**
 * Check if the path ahead contains an obstacle that the chr should route
 * around.
 *
 * If an obstacle is found, set the leftpos and rightpos coordinates to the
 * left and right corners of the object from the chr's perspective and return
 * false.
 *
 * Return true if path ahead is clear.
 *
 * This is similar to chrNavCanSeeNextPos. The only difference is this one uses
 * the value1 and value2 variables.
 */
bool chrNavCheckForObstacle(struct chrdata *chr, struct coord *chrpos, RoomNum *chrrooms, struct coord *aimpos, struct coord *leftpos, struct coord *rightpos, f32 negchrradius, f32 chrradius, s32 cdtypes, bool hasobstacle)
{
	struct coord spd4;
	f32 spd0;
	f32 spcc;
	f32 spc8;
	f32 spc4;
	f32 norm;
	bool spbc = false;
	bool spb8 = false;
	struct coord spac;
	struct coord spa0;
	struct coord sp94;
	struct coord sp88;
	bool result = false;
	f32 value1;
	f32 value2;
	f32 ymax;
	f32 ymin;
	f32 radius2;
	struct coord sp6c;
	struct coord sp60;
	RoomNum sp50[8];
	RoomNum sp40[8];
	struct prop *prop = chr->prop;

	chrGetBbox(prop, &radius2, &ymax, &ymin);

	spd4.x = aimpos->x - chrpos->x;
	spd4.y = 0.0f;
	spd4.z = aimpos->z - chrpos->z;

	if (spd4.f[0] == 0.0f && spd4.f[2] == 0.0f) {
		return true;
	}

	norm = 1.0f / sqrtf(spd4.f[0] * spd4.f[0] + spd4.f[2] * spd4.f[2]);
	spd4.x *= norm;
	spd4.z *= norm;

	spd0 = spd4.x * chrradius * 0.95f;
	spcc = spd4.z * chrradius * 0.95f;
	spc8 = spd4.x * chrradius * 1.2f;
	spc4 = spd4.z * chrradius * 1.2f;

	chrSetPerimEnabled(chr, false);

	sp6c.x = chrpos->x + spcc;
	sp6c.y = chrpos->y;
	sp6c.z = chrpos->z - spd0;
	sp60.x = (spd4.x * negchrradius) + (aimpos->x + spc4);
	sp60.y = aimpos->y;
	sp60.z = (spd4.z * negchrradius) + (aimpos->z - spc8);

	if (cdExamCylMove07(chrpos, chrrooms, &sp6c, sp50, cdtypes, 1, ymax - prop->pos.y, ymin - prop->pos.y) == CDRESULT_COLLISION
			|| cdExamCylMove03(&sp6c, sp50, &sp60, cdtypes, 1, ymax - prop->pos.y, ymin - prop->pos.y) == CDRESULT_COLLISION) {
		spbc = true;
#if VERSION >= VERSION_JPN_FINAL
		cdGetEdge(&spac, &spa0, 14319, "chr/chraction.c");
#elif VERSION >= VERSION_PAL_FINAL
		cdGetEdge(&spac, &spa0, 14316, "chr/chraction.c");
#elif VERSION >= VERSION_PAL_BETA
		cdGetEdge(&spac, &spa0, 14311, "chraction.c");
#elif VERSION >= VERSION_NTSC_1_0
		cdGetEdge(&spac, &spa0, 14310, "chraction.c");
#else
		cdGetEdge(&spac, &spa0, 14323, "chraction.c");
#endif
		func0f044b68(&spac, &spa0, &spd4);
		value1 = cd00024e40();
	}

	sp6c.x = chrpos->x - spcc;
	sp6c.y = chrpos->y;
	sp6c.z = chrpos->z + spd0;

	sp60.x = (spd4.x * negchrradius) + (aimpos->x - spc4);
	sp60.y = aimpos->y;
	sp60.z = (spd4.z * negchrradius) + (aimpos->z + spc8);

	if (cdExamCylMove07(chrpos, chrrooms, &sp6c, sp50, cdtypes, 1, ymax - prop->pos.y, ymin - prop->pos.y) == CDRESULT_COLLISION
			|| cdExamCylMove03(&sp6c, chrrooms, &sp60, cdtypes, 1, ymax - prop->pos.y, ymin - prop->pos.y) == CDRESULT_COLLISION) {
		spb8 = true;
#if VERSION >= VERSION_JPN_FINAL
		cdGetEdge(&sp94, &sp88, 14334, "chr/chraction.c");
#elif VERSION >= VERSION_PAL_FINAL
		cdGetEdge(&sp94, &sp88, 14331, "chr/chraction.c");
#elif VERSION >= VERSION_PAL_BETA
		cdGetEdge(&sp94, &sp88, 14326, "chraction.c");
#elif VERSION >= VERSION_NTSC_1_0
		cdGetEdge(&sp94, &sp88, 14325, "chraction.c");
#else
		cdGetEdge(&sp94, &sp88, 14338, "chraction.c");
#endif
		func0f044b68(&sp94, &sp88, &spd4);
		value2 = cd00024e40();
	}

	if (spbc && spb8) {
		if (value1 < value2) {
			leftpos->x = spac.x;
			leftpos->y = spac.y;
			leftpos->z = spac.z;

			rightpos->x = spa0.x;
			rightpos->y = spa0.y;
			rightpos->z = spa0.z;
		} else {
			leftpos->x = sp94.x;
			leftpos->y = sp94.y;
			leftpos->z = sp94.z;

			rightpos->x = sp88.x;
			rightpos->y = sp88.y;
			rightpos->z = sp88.z;
		}
	} else if (spbc) {
		leftpos->x = spac.x;
		leftpos->y = spac.y;
		leftpos->z = spac.z;

		rightpos->x = spa0.x;
		rightpos->y = spa0.y;
		rightpos->z = spa0.z;
	} else if (spb8) {
		leftpos->x = sp94.x;
		leftpos->y = sp94.y;
		leftpos->z = sp94.z;

		rightpos->x = sp88.x;
		rightpos->y = sp88.y;
		rightpos->z = sp88.z;
	} else if (cdExamCylMove07(chrpos, chrrooms, aimpos, sp40, cdtypes, 1, ymax - prop->pos.y, ymin - prop->pos.y) != CDRESULT_COLLISION
			&& (!hasobstacle || cdExamCylMove01(chrpos, aimpos, chrradius, sp40, cdtypes, CHECKVERTICAL_YES, ymax - prop->pos.y, ymin - prop->pos.y) != CDRESULT_COLLISION)) {
		result = true;
	} else {
#if VERSION >= VERSION_JPN_FINAL
		cdGetEdge(leftpos, rightpos, 14404, "chr/chraction.c");
#elif VERSION >= VERSION_PAL_FINAL
		cdGetEdge(leftpos, rightpos, 14401, "chr/chraction.c");
#elif VERSION >= VERSION_PAL_BETA
		cdGetEdge(leftpos, rightpos, 14396, "chraction.c");
#elif VERSION >= VERSION_NTSC_1_0
		cdGetEdge(leftpos, rightpos, 14395, "chraction.c");
#else
		cdGetEdge(leftpos, rightpos, 14408, "chraction.c");
#endif
		func0f044b68(leftpos, rightpos, &spd4);
	}

	chrSetPerimEnabled(chr, true);

	return result;
}

bool chrNavTryObstacle(struct chrdata *chr, struct coord *arg1, bool arg2, struct coord *arg3, f32 radius, bool arg5, struct coord *nextpos, struct waydata *waydata, f32 arg8, s32 cdtypes, s32 arg10)
{
	struct prop *prop = chr->prop;
	struct coord sp68;
	struct coord sp5c;
	f32 norm;
	f32 angle;
	struct coord sp48;
	struct coord *sp44;
	struct coord *sp40;

	if (arg2) {
		sp44 = arg1;
		sp40 = arg3;
	} else {
		sp44 = arg3;
		sp40 = arg1;
	}

	sp68.x = arg1->x - prop->pos.x;
	sp68.y = 0.0f;
	sp68.z = arg1->z - prop->pos.z;

	if (sp68.f[0] != 0.0f || sp68.f[2] != 0.0f) {
		norm = sqrtf(sp68.f[0] * sp68.f[0] + sp68.f[2] * sp68.f[2]);

		if (norm > 0.0f) {
			norm = 1.0f / norm;
			sp68.x *= radius * norm;
			sp68.z *= radius * norm;
		} else {
			norm = 1.0f;
			sp68.z = radius * norm;
		}
	} else {
		norm = 1.0f;
		sp68.z = radius * norm;
	}

	if (radius * norm > 1.0f) {
		angle = 0.7852731347084f;
	} else {
		angle = acosf(radius * norm);
	}

	if (!arg2 && angle != 0.0f) {
		angle = M_BADTAU - angle;
	}

	sp48.x = -cosf(angle) * sp68.f[0] + sinf(angle) * sp68.f[2];
	sp48.y = 0.0f;
	sp48.z = -sinf(angle) * sp68.f[0] - cosf(angle) * sp68.f[2];

	sp5c.x = arg1->x + sp48.f[0];
	sp5c.y = arg1->y;
	sp5c.z = arg1->z + sp48.f[2];

	if (chrNavCanSeeNextPos(chr, &prop->pos, prop->rooms, &sp5c, sp44, sp40, arg8, chr->radius, cdtypes, 1)) {
		if (!arg5 || func0f03645c(chr, &prop->pos, prop->rooms, &sp5c, nextpos, cdtypes)) {
			if (arg10) {
				waydata->gotaimposobj = true;
				waydata->aimposobj.x = sp5c.x;
				waydata->aimposobj.y = sp5c.y;
				waydata->aimposobj.z = sp5c.z;
			} else {
				waydata->gotaimpos = true;
				waydata->aimpos.x = sp5c.x;
				waydata->aimpos.y = sp5c.y;
				waydata->aimpos.z = sp5c.z;
			}

			return true;
		}
	}

	return false;
}

#if VERSION >=  VERSION_NTSC_1_0
const char var7f1a8be8[] = "CHARS -> FRAMETIMESCALEI(240)  = %d";
const char var7f1a8c0c[] = "CHARS -> numseenbond1      \t= %d/%d";
const char var7f1a8c30[] = "CHARS -> numseenbond2      \t= %d/%d";
const char var7f1a8c54[] = "CHARS -> numseenbond3      \t= %d/%d";
const char var7f1a8c78[] = "CHARS -> numseenbond       \t= %d/%d";
#endif

const char var7f1a8c9c[] = "CHARS -> DEAD = %d/%d";
const char var7f1a8cb4[] = "chrdisttopad : %x -> %d : Dist=%f";

/**
 * Check for doors in front of the chr and open them if possible.
 *
 * I'm guessing the coord argument is a position in front of the chr, and the
 * collision check is looking for doors between the chr and that point.
 *
 * - Multiplayer simulants can open any doors
 * - Solo chrs cannot open doors with OBJFLAG2_AICANNOTUSE
 * - In ntsc-beta, solo chrs can bring down lasers even if they have that flag
 *
 * The chr must be within 200cm of the door unless it's a laser.
 */
struct prop *chrOpenDoor(struct chrdata *chr, struct coord *rangepos)
{
	struct prop *doorprop = NULL;

	if (cdExamCylMove03(&chr->prop->pos, chr->prop->rooms, rangepos,
				CDTYPE_BG | CDTYPE_CLOSEDDOORS | CDTYPE_AJARDOORS,
				1, 0, 0) == CDRESULT_COLLISION) {
		doorprop = cdGetObstacleProp();
	}

	if (doorprop) {
		struct doorobj *door = doorprop->door;

#if VERSION >= VERSION_NTSC_1_0
		if (chr->aibot || (door->base.flags2 & OBJFLAG2_AICANNOTUSE) == 0)
#else
		if (chr->aibot
				|| (door->base.flags2 & OBJFLAG2_AICANNOTUSE) == 0
				|| (door->doorflags & DOORFLAG_DAMAGEONCONTACT))
#endif
		{
			f32 xdiff = doorprop->pos.x - chr->prop->pos.x;
			f32 zdiff = doorprop->pos.z - chr->prop->pos.z;

			if (xdiff * xdiff + zdiff * zdiff < 200 * 200 || (door->doorflags & DOORFLAG_DAMAGEONCONTACT)) {
				chrGoPosClearRestartTtl(chr);
				doorsChooseSwingDirection(chr->prop, doorprop->door);

				if (!doorCallLift(doorprop, false)) {
					doorsRequestMode(doorprop->door, DOORMODE_OPENING);
				}
			} else {
				doorprop = NULL;
			}
		} else {
			doorprop = NULL;
		}
	}

	return doorprop;
}

/**
 * Tick a chr's navigation process when they are using the "main" method of
 * navigating.
 *
 * Main refers to the chr being on screen or close to on screen, where all
 * collision checks are enabled. This is opposed to the magic method, where
 * they are off screen and the collision checks are skipped.
 *
 * The chr will generally move towards the nextpos, which is the position of the
 * next pad/corner in the route, and open doors along the way. However they must
 * also check for obstacles ahead of them such as other chrs or doors which have
 * opened into their path. If one is found, the chr routes around the obstacle
 * and then to the next pos again.
 */
void chrNavTickMain(struct chrdata *chr, struct coord *nextpos, struct waydata *waydata, bool arg3)
{
	struct prop *prop = chr->prop;
	struct coord sp100;
	struct coord spf4;
	s32 i;
	s32 cdtypes;

	// By default, do collision checks for everything except doors that AI can
	// open, but if they're blocking a door then include all doors
	cdtypes = (CDTYPE_ALL & ~CDTYPE_DOORS) | CDTYPE_DOORSLOCKEDTOAI;

	if (chr->hidden & CHRHFLAG_BLOCKINGDOOR) {
		cdtypes = CDTYPE_ALL;
	}

	for (i = 0; i < 1; i++) {
		if (0.0f);

		if (waydata->mode == WAYMODE_INIT || waydata->mode == WAYMODE_RETRY) {
			sp100.x = nextpos->x;
			sp100.y = nextpos->y;
			sp100.z = nextpos->z;

			// Check to see if the chr can see the next pad. This is almost
			// always true, but if the chr has tried to avoid an object they
			// may have gone behind a wall and can't see the pad any more.
			if (chrNavCanSeeNextPos(chr, &prop->pos, prop->rooms, &sp100, &waydata->obstacleleft, &waydata->obstacleright, -chr->radius, chr->radius, CDTYPE_PATHBLOCKER | CDTYPE_BG, arg3)) {
				// Can see the next pad
				waydata->gotaimpos = true;
				waydata->aimpos.x = sp100.x;
				waydata->aimpos.y = sp100.y;
				waydata->aimpos.z = sp100.z;
				waydata->mode = WAYMODE_HAVEAIMPOS;
			} else if (waydata->mode == WAYMODE_INIT) {
				waydata->mode = WAYMODE_LOST1;
				waydata->iter = 0;
			} else if (waydata->mode == WAYMODE_RETRY) {
				waydata->mode = WAYMODE_LOST2;
				waydata->iter = 0;
			}
		} else if (waydata->mode == WAYMODE_LOST1) {
			// Chr has tried to avoid an obstacle, and in the process can no
			// longer see the next pad. The chr will try to get back on the
			// route by navigating to either side of the obstacle they were
			// trying to avoid.
			f32 wantclearance = chr->radius * 1.26f;

			if (chrNavTryObstacle(chr, &waydata->obstacleleft, true, &spf4, wantclearance, true, nextpos, waydata, 0, CDTYPE_PATHBLOCKER | CDTYPE_BG, 0)) {
				// Will go to left side
				waydata->mode = WAYMODE_HAVEAIMPOS;
			} else if (chrNavTryObstacle(chr, &waydata->obstacleright, false, &spf4, wantclearance, true, nextpos, waydata, 0, CDTYPE_PATHBLOCKER | CDTYPE_BG, 0)) {
				// Will go to right side
				waydata->mode = WAYMODE_HAVEAIMPOS;
			} else {
				// Can't see the obstacle either!
				// Remain in LOST1 for 5 iterations to see if line of sight
				// comes back. If not, retry the next pad again.
				waydata->iter++;

				if (waydata->iter > 5) {
					waydata->mode = WAYMODE_RETRY;
				}
			}
		} else if (waydata->mode == WAYMODE_LOST2) {
			// Chr has gone through LOST1 without seeing the obstacle, then
			// tried to find the next pad but can't see that either. Try
			// navigating to the obstacle again, but with different arguments.
			// This is a more desparate attempt at returning to the path.
			f32 wantclearance = chr->radius * 1.26f;
			u32 stack;

			if (chrNavTryObstacle(chr, &waydata->obstacleleft, true, &spf4, wantclearance, false, NULL, waydata, 0, CDTYPE_PATHBLOCKER | CDTYPE_BG, 0)) {
				// Will go to left side
				waydata->mode = WAYMODE_HAVEAIMPOS;
			} else if (chrNavTryObstacle(chr, &waydata->obstacleright, false, &spf4, wantclearance, false, NULL, waydata, 0, CDTYPE_PATHBLOCKER | CDTYPE_BG, 0)) {
				// Will go to right side
				waydata->mode = WAYMODE_HAVEAIMPOS;
			} else {
				// Still can't see the obstacle using the desparate check.
				// Remain in LOST2 for 5 iterations to see if line of sight
				// comes back.
				waydata->iter++;

				if (waydata->iter > 5) {
					// Clear any references to the obstacle we were trying to
					// avoid and attempt to restart the process. I don't think
					// this is likely to work, but there's nothing else that can
					// be done.
					waydata->gotaimposobj = waydata->gotaimpos = false;
					waydata->aimposobj.x = waydata->aimpos.x = nextpos->x;
					waydata->aimposobj.y = waydata->aimpos.y = nextpos->y;
					waydata->aimposobj.z = waydata->aimpos.z = nextpos->z;
					waydata->mode = WAYMODE_INIT;
				}
			}
		} else if (waydata->mode == WAYMODE_HAVEAIMPOS) {
			// Chr is happily walking towards a position (aim pos).
			// The position could be the next pad, or it could be an arbitrary
			// position next to an obstacle that has to be routed around.
			// Either way, check that there's no new obstacles between the chr
			// and their aim pos.
			bool hasobstacle = true;

			if (!arg3
					&& nextpos->x == waydata->aimpos.x
					&& nextpos->y == waydata->aimpos.y
					&& nextpos->z == waydata->aimpos.z) {
				hasobstacle = false;
			}

			if (chrNavCheckForObstacle(chr, &prop->pos, prop->rooms, &waydata->aimpos, &waydata->obstacleleft, &waydata->obstacleright, -chr->radius, chr->radius, cdtypes, hasobstacle)) {
				// No obstacle ahead
				waydata->gotaimposobj = true;
				waydata->mode = WAYMODE_INIT;
				waydata->aimposobj.x = waydata->aimpos.x;
				waydata->aimposobj.y = waydata->aimpos.y;
				waydata->aimposobj.z = waydata->aimpos.z;
			} else {
				// Obstacle ahead
				waydata->mode = WAYMODE_NEWOBSTACLE;
				waydata->iter = 0;
			}
		} else if (waydata->mode == WAYMODE_NEWOBSTACLE) {
			// Chr has just noticed an obstacle ahead of them which they need to
			// route around. The obstacleleft and obstacleright coordinates are
			// already populated and they indicate the left and right corners of
			// the object. This part of the code decides which side to use and
			// calculates some turning angles.
			f32 f20;
			u32 stack;
			f32 spd0;
			f32 spcc;
			u32 stack2;
			f32 f24 = chr->radius * 1.26f;

			f20 = atan2f(waydata->aimpos.x - prop->pos.x, waydata->aimpos.z - prop->pos.z);
			spd0 = f20 - atan2f(waydata->obstacleleft.x - prop->pos.x, waydata->obstacleleft.z - prop->pos.z);
			spcc = f20 - atan2f(waydata->obstacleright.x - prop->pos.x, waydata->obstacleright.z - prop->pos.z);

			if (spd0 < 0) {
				spd0 += M_BADTAU;
			}

			if (spd0 >= M_BADPI) {
				spd0 -= M_BADTAU;
			}

			if (spd0 < 0) {
				spd0 = -spd0;
			}

			if (spcc < 0) {
				spcc += M_BADTAU;
			}

			if (spcc >= M_BADPI) {
				spcc -= M_BADTAU;
			}

			if (spcc < 0) {
				spcc = -spcc;
			}

			if (spd0 < spcc) {
				f32 spc0;
				f32 spbc;
				u32 stack;
				f32 f22 = f24 * 1.1f;

				if (chrNavTryObstacle(chr, &waydata->obstacleleft, true, &spf4, f24, false, NULL, waydata, f22, cdtypes, 1)) {
					waydata->mode = WAYMODE_INIT;
					break;
				}

				spc0 = f20 - atan2f(waydata->obstacleleft.x - prop->pos.x, waydata->obstacleleft.z - prop->pos.z);
				spbc = f20 - atan2f(spf4.x - prop->pos.x, spf4.z - prop->pos.z);

				if (spc0 < 0) {
					spc0 += M_BADTAU;
				}

				if (spc0 >= M_BADPI) {
					spc0 -= M_BADTAU;
				}

				if (spc0 < 0) {
					spc0 = -spc0;
				}

				if (spbc < 0) {
					spbc += M_BADTAU;
				}

				if (spbc >= M_BADPI) {
					spbc -= M_BADTAU;
				}

				if (spbc < 0) {
					spbc = -spbc;
				}

				if (spbc < spc0) {
					if (chrNavTryObstacle(chr, &spf4, false, &spf4, f24, false, NULL, waydata, f22, cdtypes, 1)) {
						waydata->mode = WAYMODE_INIT;
						break;
					}
				}
			} else {
				f32 spb0;
				f32 spac;
				f32 f22 = f24 * 1.1f;

				if (chrNavTryObstacle(chr, &waydata->obstacleright, false, &spf4, f24, false, NULL, waydata, f22, cdtypes, 1)) {
					waydata->mode = WAYMODE_INIT;
					break;
				}

				spb0 = f20 - atan2f(waydata->obstacleright.x - prop->pos.x, waydata->obstacleright.z - prop->pos.z);
				spac = f20 - atan2f(spf4.x - prop->pos.x, spf4.z - prop->pos.z);

				if (spb0 < 0) {
					spb0 += M_BADTAU;
				}

				if (spb0 >= M_BADPI) {
					spb0 -= M_BADTAU;
				}

				if (spb0 < 0) {
					spb0 = -spb0;
				}

				if (spac < 0) {
					spac += M_BADTAU;
				}

				if (spac >= M_BADPI) {
					spac -= M_BADTAU;
				}

				if (spac < 0) {
					spac = -spac;
				}

				if (spac < spb0) {
					if (chrNavTryObstacle(chr, &spf4, true, &spf4, f24, false, NULL, waydata, f22, cdtypes, 1)) {
						waydata->mode = WAYMODE_INIT;
						break;
					}
				}
			}

			waydata->iter++;

			if (waydata->iter > 5) {
				waydata->gotaimposobj = false;
				waydata->mode = WAYMODE_INIT;
			}
		}
	}

	if (!waydata->gotaimposobj) {
		waydata->aimposobj.x = waydata->aimpos.x;
		waydata->aimposobj.y = waydata->aimpos.y;
		waydata->aimposobj.z = waydata->aimpos.z;
	}

	// Every 10 ticks, attempt to open any door in front of the chr
	if (waydata->age % 10 == 0) {
		struct prop *doorprop = chrOpenDoor(chr, &waydata->aimposobj);

		// Consider returning to stand animation while door is opening
		if (doorprop
				&& chr->aibot == NULL
				&& (chr->hidden & CHRHFLAG_BLOCKINGDOOR) == 0
				&& !chrGoPosIsWaiting(chr)) {
			chrChooseStandAnimation(chr, 16);
			chr->lastmoveok60 = g_Vars.lvframe60;
		}

		// Resume moving if there's no longer a door in the way
		// or if the chr is blocking the door
		if (!doorprop || (chr->hidden & CHRHFLAG_BLOCKINGDOOR)) {
			if (chr->aibot == NULL
					&& chrGoPosIsWaiting(chr)
					&& chr->liftaction != LIFTACTION_WAITINGFORLIFT
					&& chr->liftaction != LIFTACTION_WAITINGONLIFT) {
				if (chr->actiontype == ACT_PATROL) {
					chrPatrolChooseAnimation(chr);
				} else {
					chrGoPosChooseAnimation(chr);
				}
			}

			if (!doorprop) {
				chr->hidden &= ~CHRHFLAG_BLOCKINGDOOR;
			}
		}
	}

	// Handle turning and anim speed
	if (chr->actiontype == ACT_PATROL) {
		func0f043f2c(chr, &waydata->aimposobj, 0, &chr->act_patrol.turnspeed);
	} else {
		chr->unk32c_21 = chr->act_gopos.turnspeed != 0;

		func0f043f2c(chr, &waydata->aimposobj, chr->act_gopos.flags, &chr->act_gopos.turnspeed);

		if (chr->aibot == NULL && !chrGoPosIsWaiting(chr)) {
			if ((chr->act_gopos.flags & GOPOSMASK_SPEED) == GOPOSFLAG_RUN) {
				if (chr->act_gopos.turnspeed) {
					if (!chr->unk32c_21) {
						modelSetAnimSpeed(chr->model, 0.25f, 8);
					}
				} else {
					if (chr->unk32c_21) {
						if (chr->chrflags & CHRCFLAG_RUNFASTER) {
							modelSetAnimSpeed(chr->model, 0.65f, 32);
						} else {
							modelSetAnimSpeed(chr->model, 0.5f, 32);
						}
					}
				}
			} else if ((chr->act_gopos.flags & GOPOSMASK_SPEED) == GOPOSFLAG_JOG) {
				if (chr->act_gopos.turnspeed) {
					modelSetAnimSpeed(chr->model, 0.4f, 0);
				} else {
					modelSetAnimSpeed(chr->model, 0.5f, 0);
				}
			}
		}
	}
}

bool chrGoPosUpdateLiftAction(struct chrdata *chr, u32 curpadflags, bool arg2, bool arrivingatlift, s16 curpadnum, s32 nextpadnum)
{
	bool advance = false;
	struct pad nextpad;
	u32 nextpadflags = 0;
	f32 nextground;
	f32 lifty;
	struct liftobj *lift;
	struct prop *liftprop = liftFindByPad(curpadnum);
	u32 stack;

	if (!liftprop) {
		return false;
	}

	lift = (struct liftobj *) liftprop->obj;

	lifty = liftGetY(lift);

	if (nextpadnum >= 0) {
		padUnpack(nextpadnum, PADFIELD_POS | PADFIELD_ROOM | PADFIELD_FLAGS, &nextpad);
		nextpadflags = nextpad.flags;
	}

	if (curpadflags & PADFLAG_AIWAITLIFT) {
		if (nextpadflags & PADFLAG_AIONLIFT) {
			if (arrivingatlift || chr->liftaction == LIFTACTION_WAITINGFORLIFT) {
				if (nextpadflags);

				// Begin entering lift if lift is under 40cm above this level
				advance = (lifty <= chr->manground + 40);

				// ...and (if solo mode) lift is over 1m under this level
				// (this logic allows MP simulants to drop down onto lifts)
				if (!g_Vars.normmplayerisrunning && advance) {
					advance = (lifty > chr->manground - 100);
				}

				// ...and if the lift has a door, is at least halfway open
				if (advance && lift->doors[lift->levelcur] && lift->doors[lift->levelcur]->frac < 0.5f) {
					advance = false;
				}
			}

			if (!advance) {
				if (arrivingatlift && chr->liftaction != LIFTACTION_WAITINGFORLIFT) {
					// Just arrived at lift
					chr->liftaction = LIFTACTION_WAITINGFORLIFT;

					chrChooseStandAnimation(chr, 16);

					if (nextpadnum >= 0) {
						// Call the lift
						chrOpenDoor(chr, &nextpad.pos);
					}
				}
			} else {
				// Enter lift
				chr->liftaction = LIFTACTION_NOTUSINGLIFT;

				if (chrGoPosIsWaiting(chr)) {
					if (chr->actiontype == ACT_PATROL) {
						chrPatrolChooseAnimation(chr);
					} else {
						chrGoPosChooseAnimation(chr);
					}
				}
			}
		} else {
			// On a wait lift pad, but the next pad in the route is not on the
			// lift, so the chr is running past the lift without using it.
			if (arrivingatlift) {
				advance = true;
				chr->liftaction = LIFTACTION_NOTUSINGLIFT;
			}
		}
	} else if (curpadflags & PADFLAG_AIONLIFT) {
		if (nextpadflags & PADFLAG_AIWAITLIFT) {
			// Waiting for door to close or lift to arrive at destination
			if (arg2 || chr->liftaction == LIFTACTION_WAITINGONLIFT) {
				// Continue waiting
				RoomNum rooms[] = {0, -1};
				u32 stack2;

				rooms[0] = nextpad.room;

				nextground = cdFindFloorYColourTypeAtPos(&nextpad.pos, rooms, NULL, NULL);

				// Begin exiting lift if lift is 30cm under destination or higher
				advance = (lifty >= nextground - 30);

				// ...and (if solo) lift is under 1m above destination
				if (!g_Vars.normmplayerisrunning && advance) {
					advance = (lifty < nextground + 100);
				}

				// ...and if the lift has a door, is at least halfway open
				if (advance && lift->doors[lift->levelcur] && lift->doors[lift->levelcur]->frac < 0.5f) {
					advance = false;
				}
			}

			if (!advance) {
				if (arg2 && chr->liftaction != LIFTACTION_WAITINGONLIFT) {
					// Just arrived inside lift
					chr->liftaction = LIFTACTION_WAITINGONLIFT;
					chrChooseStandAnimation(chr, 16);
				}
			} else {
				// Start disembarking
				chr->liftaction = LIFTACTION_ONLIFT;

				if (chrGoPosIsWaiting(chr)) {
					if (chr->actiontype == ACT_PATROL) {
						chrPatrolChooseAnimation(chr);
					} else {
						chrGoPosChooseAnimation(chr);
					}
				}
			}
		} else {
			// Not our stop? Not sure why advance is true here. I guess the chr
			// can't go anywhere anyway because the next pad is above or below
			// them.
			if (arg2) {
				advance = true;
				chr->liftaction = LIFTACTION_ONLIFT;
			}
		}
	}

	return advance;
}

s32 chrIsUsingLift(struct chrdata *chr)
{
	return (chr->actiontype == ACT_GOPOS || chr->actiontype == ACT_PATROL) && chr->liftaction > 0;
}

s16 chrGoPosGetNextPadNum(struct chrdata *chr)
{
	if (chr->act_gopos.waypoints[chr->act_gopos.curindex + 1]) {
		return chr->act_gopos.waypoints[chr->act_gopos.curindex + 1]->padnum;
	}

	return -1;
}

void chrTickGoPos(struct chrdata *chr)
{
	struct waypoint *waypoint;
	struct coord nextpos;
	RoomNum nextrooms[8];
	struct prop *prop = chr->prop;
	bool enteringmagic = false;
	struct pad pad;
	bool sp240 = true;
	struct coord curwppos;
	RoomNum curwprooms[8];
	u32 curwpflags;

	chr->act_gopos.flags &= ~(GOPOSFLAG_CROUCH | GOPOSFLAG_DUCK);

	if (chr->hidden & CHRHFLAG_NEEDANIM) {
		if (modelIsAnimMerging(chr->model)) {
			return;
		}

		chrGoPosChooseAnimation(chr);

		chr->hidden &= ~CHRHFLAG_NEEDANIM;
	}

	chr->act_gopos.waydata.age++;

	// If stuck for 1 second
	if (chr->lastmoveok60 < g_Vars.lvframe60 - TICKS(60)) {
#if VERSION >= VERSION_NTSC_1_0
		if (chr->goposforce >= 0) {
			// Try and warp the chr past whatever obstacle is blocking them?
			struct coord sp196 = {0, 0, 0};

			chrDamageByMisc(chr, 1, &sp196, NULL, NULL);

			chr->lastmoveok60 = g_Vars.lvframe60;
			return;
		}
#endif

		// Goposforce was not set - restart the action to try and find a new route
		chrGoToRoomPos(chr, &chr->act_gopos.endpos, chr->act_gopos.endrooms, chr->act_gopos.flags);
	}

	chrGoPosConsiderRestart(chr);
	chrGoPosGetCurWaypointInfoWithFlags(chr, &curwppos, curwprooms, &curwpflags);

	// If magic mode ended over 3 seconds ago, not multiplayer, not in view of
	// eyespy, pad is nothing special and not in lift, then enter the magic move
	// mode.
	if (chr->act_gopos.waydata.mode != WAYMODE_MAGIC
			&& chr->act_gopos.waydata.lastvisible60 + TICKS(180) < g_Vars.lvframe60
			&& g_Vars.normmplayerisrunning == false
			&& chrIsRoomOffScreen(chr, &curwppos, curwprooms) // related to eyespy
			&& (curwpflags & (PADFLAG_AIWAITLIFT | PADFLAG_AIONLIFT)) == 0
			&& chr->inlift == false) {
		enteringmagic = true;
		chrGoPosInitMagic(chr, &chr->act_gopos.waydata, &curwppos, &prop->pos);
	}

#if VERSION >= VERSION_NTSC_1_0
	if (g_NumChrsSeenPlayerRecently2 >= 9
			&& (chr->hidden & CHRHFLAG_BASICGUARD)
			&& (chr->flags & CHRFLAG0_ACTIVATEALARM) == 0) {
		chrStop(chr);
		return;
	}
#else
	if (g_NumChrsSeenPlayerRecently2 >= 10) {
		chrStop(chr);
		return;
	}
#endif

	// If goposforce is set then decrease it on each tick. If it's reached -1
	// then stop the chr. I guess goposforce is not only used to warp past
	// obstacles, but is also used as a run countdown timer.
#if VERSION >= VERSION_NTSC_1_0
	if (chr->goposforce >= 0) {
		chr->goposforce -= g_Vars.lvupdate60;

		if (chr->goposforce < 0) {
			chrStop(chr);
			return;
		}
	}
#endif

	// Check if chr needs to exit magic mode
	if (chr->act_gopos.waydata.mode == WAYMODE_MAGIC) {
		if ((!enteringmagic && ((prop->flags & (PROPFLAG_ONANYSCREENPREVTICK | PROPFLAG_ONANYSCREENTHISTICK | PROPFLAG_ONTHISSCREENTHISTICK)) || !chrIsRoomOffScreen(chr, &curwppos, curwprooms)))
				|| (curwpflags & (PADFLAG_AIWAITLIFT | PADFLAG_AIONLIFT))
				|| chr->inlift) {
			// Exiting magic mode
			chrGoPosInitExpensive(chr);
			chr->act_gopos.waydata.lastvisible60 = g_Vars.lvframe60;
			return;
		}

		chrNavTickMagic(chr, &chr->act_gopos.waydata, func0f0370a8(chr), &curwppos, curwprooms);
	} else {
		bool advance = false;
		bool arrivingxyz;
		bool arrivingxz;
		f32 sp180;
		f32 sp176;
		f32 sp172;
		f32 sp168;
		bool candosomething;
		f32 sp160;
		f32 sp156;
		struct waypoint *next;
		struct pad pad2;

		waypoint = chr->act_gopos.waypoints[chr->act_gopos.curindex];

		if (waypoint) {
			padUnpack(waypoint->padnum, PADFIELD_FLAGS | PADFIELD_POS, &pad);

			arrivingxyz = posIsArrivingAtPos(&chr->prevpos, &prop->pos, &pad.pos, 30);
			arrivingxz = posIsArrivingLaterallyAtPos(&chr->prevpos, &prop->pos, &pad.pos, 30);

			if (pad.flags & PADFLAG_AICROUCH) {
				chr->act_gopos.flags |= GOPOSFLAG_CROUCH;
			} else if (pad.flags & PADFLAG_AIDUCK) {
				chr->act_gopos.flags |= GOPOSFLAG_DUCK;
			}

			if ((pad.flags & PADFLAG_AIWAITLIFT) || (pad.flags & PADFLAG_AIONLIFT)) {
				advance = chrGoPosUpdateLiftAction(chr, pad.flags, arrivingxz, arrivingxyz, waypoint->padnum, chrGoPosGetNextPadNum(chr));
			} else {
				if (arrivingxyz || (arrivingxz && (chr->inlift || (pad.flags & PADFLAG_AIIGNOREY)))) {
					advance = true;
				}
			}
		} else {
			// No more waypoints - chr is finished
			if (posIsArrivingAtPos(&chr->prevpos, &prop->pos, &chr->act_gopos.endpos, 30) ||
					(chr->inlift && posIsArrivingLaterallyAtPos(&chr->prevpos, &prop->pos, &chr->act_gopos.endpos, 30))) {
				if (chr->act_gopos.flags & GOPOSFLAG_FORPATHSTART) {
					chrTryStartPatrol(chr);
					return;
				}

				chrStop(chr);
				return;
			}
		}

		if (advance) {
			chrGoPosAdvanceWaypoint(chr);
		}

		// Every 10 ticks: Check something a couple of waypoints ahead
		// This might be checking if the chr has line of sight to that pad and can
		// walk straight to it.
		if (chr->act_gopos.waydata.age % 10 == 5 || (chr->act_gopos.flags & GOPOSFLAG_INIT)) {
			// Load waypoint that the chr is running to
			waypoint = chr->act_gopos.waypoints[chr->act_gopos.curindex];

			if (waypoint) {
				padUnpack(waypoint->padnum, PADFIELD_FLAGS, &pad);

				if ((pad.flags & PADFLAG_AIWALKDIRECT) == 0) {
					// The waypoint the chr is running to doesn't have
					// PADFLAG_AIWALKDIRECT, so the chr is able to ignore it and run
					// towards the next one if it's in sight.

					// Load the next waypoint after the one the chr is running to
					waypoint = chr->act_gopos.waypoints[chr->act_gopos.curindex + 1];

					if (waypoint) {
						padUnpack(waypoint->padnum, PADFIELD_FLAGS, &pad);

						if ((pad.flags & PADFLAG_AIWALKDIRECT) == 0) {
							// And this one doesn't have PADFLAG_AIWALKDIRECT either,
							// so the chr can consider skipping this one too.
							waypoint = chr->act_gopos.waypoints[chr->act_gopos.curindex + 2];

							if (waypoint) {
								padUnpack(waypoint->padnum, PADFIELD_ROOM | PADFIELD_POS, &pad);

								nextpos.x = pad.pos.x;
								nextpos.y = pad.pos.y;
								nextpos.z = pad.pos.z;

								nextrooms[0] = pad.room;
								nextrooms[1] = -1;
							} else {
								nextpos.x = chr->act_gopos.endpos.x;
								nextpos.y = chr->act_gopos.endpos.y;
								nextpos.z = chr->act_gopos.endpos.z;

								roomsCopy(chr->act_gopos.endrooms, nextrooms);
							}

							// Some bbox related check
							if (func0f03654c(chr, &prop->pos, prop->rooms, &nextpos, nextrooms, NULL, chr->radius * 1.2f, CDTYPE_PATHBLOCKER | CDTYPE_BG)) {
								chrGoPosAdvanceWaypoint(chr);
								chrGoPosAdvanceWaypoint(chr);
							}
						}
					}
				}
			}
		}

		if (chr->act_gopos.waydata.age % 10 == 0 || (chr->act_gopos.flags & GOPOSFLAG_INIT)) {
			waypoint = chr->act_gopos.waypoints[chr->act_gopos.curindex];

			if (waypoint) {
				candosomething = (chr->act_gopos.flags & GOPOSFLAG_INIT) != 0;
				padUnpack(waypoint->padnum, PADFIELD_FLAGS | PADFIELD_POS, &pad);

				next = chr->act_gopos.waypoints[chr->act_gopos.curindex + 1];

				if (next) {
					padUnpack(next->padnum, PADFIELD_ROOM | PADFIELD_POS, &pad2);

					if ((pad.flags & (PADFLAG_AIWAITLIFT | PADFLAG_AIONLIFT))
							&& (pad2.flags & (PADFLAG_AIWAITLIFT | PADFLAG_AIONLIFT))) {
						candosomething = false;
					}
				}

				if ((pad.flags & PADFLAG_AIWALKDIRECT) == 0 || candosomething) {
					if (next) {
						nextpos.x = pad2.pos.x;
						nextpos.y = pad2.pos.y;
						nextpos.z = pad2.pos.z;

						nextrooms[0] = pad2.room;
						nextrooms[1] = -1;
					} else {
						nextpos.x = chr->act_gopos.endpos.x;
						nextpos.y = chr->act_gopos.endpos.y;
						nextpos.z = chr->act_gopos.endpos.z;

						roomsCopy(chr->act_gopos.endrooms, nextrooms);
					}

					// I suspect this is making the chr turn to face the next pad
					if ((pad.flags & PADFLAG_AIWALKDIRECT) && candosomething) {
						if (true) {
							sp180 = prop->pos.x - pad.pos.x;
							sp176 = prop->pos.z - pad.pos.z;
							sp172 = nextpos.x - pad.pos.x;
							sp168 = nextpos.z - pad.pos.z;
						}

						sp156 = sqrtf((sp180 * sp180 + sp176 * sp176) * (sp172 * sp172 + sp168 * sp168));

						if (sp156 > 0) {
							sp160 = acosf((sp180 * sp172 + sp176 * sp168) / sp156);

							// sp160 < DEG2RAD(45) || sp160 > DEG2RAD(315)
							if (sp160 < 0.7852731347084f || sp160 > 5.4969120025635f) {
								if (func0f03654c(chr, &prop->pos, prop->rooms, &nextpos, nextrooms, NULL, chr->radius * 1.2f, CDTYPE_PATHBLOCKER | CDTYPE_BG)) {
									chrGoPosAdvanceWaypoint(chr);
								}
							}
						}
					} else {
						if (func0f03654c(chr, &prop->pos, prop->rooms, &nextpos, nextrooms, NULL, chr->radius * 1.2f, CDTYPE_PATHBLOCKER | CDTYPE_BG)) {
							chrGoPosAdvanceWaypoint(chr);
						}
					}
				}
			}

			chr->act_gopos.flags &= ~GOPOSFLAG_INIT;
		}

		waypoint = chr->act_gopos.waypoints[chr->act_gopos.curindex];

		if (waypoint) {
			padUnpack(waypoint->padnum, PADFIELD_POS, &pad);

			nextpos.x = pad.pos.x;
			nextpos.y = pad.pos.y;
			nextpos.z = pad.pos.z;
		} else {
			nextpos.x = chr->act_gopos.endpos.x;
			nextpos.y = chr->act_gopos.endpos.y;
			nextpos.z = chr->act_gopos.endpos.z;

			if (chr->aibot && chr->myaction == MA_AIBOTGETITEM) {
				sp240 = false;
			}
		}

		chrNavTickMain(chr, &nextpos, &chr->act_gopos.waydata, sp240);
	}
}

void chrTickPatrol(struct chrdata *chr)
{
	struct prop *prop = chr->prop;
	bool enteringmagic;
	u32 flags = 0;
	struct coord sp58;
	RoomNum sp48[8];
	s32 arrivinglaterally;
	s32 arriving;
	bool advance;

	enteringmagic = 0;

	if (chr->hidden & CHRHFLAG_NEEDANIM) {
		if (modelIsAnimMerging(chr->model)) {
			return;
		}

		chrPatrolChooseAnimation(chr);
		chr->hidden &= ~CHRHFLAG_NEEDANIM;
	}

	chrPatrolGetCurWaypointInfoWithFlags(chr, &sp58, sp48, &flags);

	chr->act_patrol.waydata.age++;

	// Consider starting magic
	if (chr->act_patrol.waydata.mode != WAYMODE_MAGIC
			&& g_Vars.lvframe60 > chr->act_patrol.waydata.lastvisible60 + TICKS(180)
			&& !g_Vars.normmplayerisrunning
			&& chrIsRoomOffScreen(chr, &sp58, sp48)
			&& (flags & (PADFLAG_AIWAITLIFT | PADFLAG_AIONLIFT)) == 0
			&& !chr->inlift) {
		enteringmagic = true;
		chrGoPosInitMagic(chr, &chr->act_patrol.waydata, &sp58, &prop->pos);
	}

	if (chr->act_patrol.waydata.mode == WAYMODE_MAGIC) {
		if ((!enteringmagic && ((prop->flags & (PROPFLAG_ONTHISSCREENTHISTICK | PROPFLAG_ONANYSCREENTHISTICK | PROPFLAG_ONANYSCREENPREVTICK)) || !chrIsRoomOffScreen(chr, &sp58, sp48)))
				|| (flags & (PADFLAG_AIWAITLIFT | PADFLAG_AIONLIFT))
				|| chr->inlift) {
			// Exit magic for lifts
			chr->act_patrol.waydata.lastvisible60 = g_Vars.lvframe60;
			func0f037580(chr);
		} else {
			// Continue magic
			chrNavTickMagic(chr, &chr->act_patrol.waydata, func0f0370a8(chr), &sp58, sp48);
		}

		footstepCheckMagic(chr);
	} else {
		arrivinglaterally = posIsArrivingLaterallyAtPos(&chr->prevpos, &prop->pos, &sp58, 30);
		arriving = posIsArrivingAtPos(&chr->prevpos, &prop->pos, &sp58, 30);
		advance = false;

		if ((flags & PADFLAG_AIWAITLIFT) || (flags & PADFLAG_AIONLIFT)) {
			advance = chrGoPosUpdateLiftAction(chr, flags, arrivinglaterally, arriving,
					chrPatrolCalculatePadNum(chr, 0),
					chrPatrolCalculatePadNum(chr, 1));
		} else if (arriving) {
			advance = true;
		}

		if (advance) {
			func0f0375b0(chr);
			chrPatrolGetCurWaypointInfo(chr, &sp58, sp48);
		}

		chrNavTickMain(chr, &sp58, &chr->act_patrol.waydata, true);
		footstepCheckDefault(chr);
	}
}

bool chrTrySkJump(struct chrdata *chr, u8 arg1, u8 arg2, s32 arg3, u8 arg4)
{
	if (chr && chr->actiontype != ACT_SKJUMP
			&& chrIsReadyForOrders(chr)
			&& CHRRACE(chr) == RACE_SKEDAR) {
		return chrStartSkJump(chr, arg1, arg2, arg3, arg4);
	}

	return false;
}

bool chrStartSkJump(struct chrdata *chr, u8 arg1, u8 arg2, s32 arg3, u8 arg4)
{
	f32 radius;
	f32 ymax;
	f32 ymin;
	struct prop *prop = chr->prop;
	struct prop *target = chrGetTargetProp(chr);
	bool iVar2;
	f32 distance = chrGetDistanceToCoord(chr, &target->pos);
	f32 diffs[2];
	f32 thing;
	s32 time60;

	if (distance < 200 || distance > 550 || !target) {
		return false;
	}

	chrGetBbox(prop, &radius, &ymax, &ymin);
	chrSetPerimEnabled(chr, false);
	propSetPerimEnabled(target, false);
	iVar2 = cdTestCylMove01(&prop->pos, prop->rooms, &target->pos,
			CDTYPE_OBJS | CDTYPE_DOORS | CDTYPE_PATHBLOCKER | CDTYPE_BG,
			1, ymax - prop->pos.y, ymin - prop->pos.y);
	chrSetPerimEnabled(chr, true);
	propSetPerimEnabled(target, true);

	if (iVar2) {
		diffs[0] = target->pos.x - chr->prop->pos.x;
		diffs[1] = target->pos.z - chr->prop->pos.z;
		thing = sqrtf(diffs[0] * diffs[0] + diffs[1] * diffs[1]) * 2.5f / PALUPF(21.0f);
		time60 = thing;

		if (time60 < TICKS(10)) {
			time60 = TICKS(10);
		}

		chr->act_skjump.vel[0] = diffs[0] / time60;
		chr->act_skjump.vel[1] = diffs[1] / time60;
		chr->act_skjump.roty = chrGetInverseTheta(chr) + chrGetAngleToPos(chr, &target->pos);
		chr->act_skjump.hit = false;
		chr->act_skjump.timer60 = time60;
		chr->act_skjump.total60 = time60;
		chr->act_skjump.ground = cdFindGroundAtCyl(&chr->prop->pos, chr->radius, chr->prop->rooms, NULL, NULL);
	} else {
		return false;
	}

	chr->actiontype = ACT_SKJUMP;
	chr->act_skjump.needsnewanim = true;
	chr->act_skjump.state = SKJUMPSTATE_TAKEOFF;

	return true;
}

void chrTickSkJump(struct chrdata *chr)
{
	if (g_Vars.lvupdate60 == 0) {
		return;
	}

	if (chr->act_skjump.needsnewanim) {
		chr->act_skjump.needsnewanim = false;

		switch (chr->act_skjump.state) {
		case SKJUMPSTATE_TAKEOFF:
			modelSetAnimation(chr->model, ANIM_SKEDAR_JUMPSTART, 0, 0, -1, 8);
			modelSetAnimSpeed(chr->model, 2.5, 0);
			break;
		case SKJUMPSTATE_AIRBORNE: {
				u16 sounds[] = {
					SFX_SKEDAR_ROAR_0532,
					SFX_SKEDAR_ROAR_0533,
					SFX_SKEDAR_ROAR_0534,
				};

				psCreate(NULL, chr->prop, sounds[rngRandom() % 3], -1,
						-1, 0, 0, PSTYPE_NONE, 0, -1, 0, -1, -1, -1, -1);
				modelSetAnimation(chr->model, ANIM_SKEDAR_JUMPAIR, 0, 0, -1, 16);
				modelSetAnimSpeed(chr->model, 1, 0);
			}
			break;
		}
	}

	{
		f32 fVar5;
		f32 fVar6;
		f32 fVar7;
		u32 stack[1];
		f32 frame;

		switch (chr->act_skjump.state) {
		case SKJUMPSTATE_TAKEOFF:
			fVar6 = chrGetInverseTheta(chr);
			fVar5 = modelTweenRotAxis(fVar6, chr->act_skjump.roty, 0.35);
			chrSetLookAngle(chr, fVar5);
			frame = modelGetCurAnimFrame(chr->model);

			if (frame >= modelGetAnimEndFrame(chr->model)) {
				chr->act_skjump.state++;
				chr->act_skjump.needsnewanim = true;
			}
			break;
		case SKJUMPSTATE_AIRBORNE:
			chr->act_skjump.pos.x = chr->act_skjump.vel[0] * g_Vars.lvupdate60 + chr->prop->pos.x;
			chr->act_skjump.pos.z = chr->act_skjump.vel[1] * g_Vars.lvupdate60 + chr->prop->pos.z;

			if (chr->act_skjump.total60 > 0) {
				fVar6 = 1.0f - chr->act_skjump.timer60 / (f32)chr->act_skjump.total60;
				fVar7 = sinf(M_PI * fVar6);
				fVar7 = fVar7 * 160.0f + chr->act_skjump.ground;
			} else {
				fVar6 = 1;
				fVar7 = chr->act_skjump.ground;
			}

			chr->act_skjump.pos.y = fVar7 - chr->prop->pos.y;

			if (fVar6 < 0.5f && chr->act_skjump.pos.y < 0.0f) {
				chr->act_skjump.pos.y = 0;
			}

			if (!chr->act_skjump.hit && chrGetDistanceToTarget(chr) < 150.0f) {
				chrPunchInflictDamage(chr, 3, 150, false);
				chr->act_skjump.hit = true;
			}

			if (chr->act_skjump.timer60 > 0) {
				chr->act_skjump.timer60 -= g_Vars.lvupdate60;
			} else {
				chrTryStop(chr);
			}
			break;
		}
	}
}

void chraTick(struct chrdata *chr)
{
	u32 race = CHRRACE(chr);

	if (g_Vars.lvupdate240 < 1) {
		return;
	}

	if (chr->actiontype == ACT_INIT) {
		chr->chrflags |= CHRCFLAG_FORCETOGROUND;
		func0f02e9a0(chr, 0);
		chr->sleep = 0;
	}

	if (race == RACE_DRCAROLL) {
		g_DrCarollDyingTimer += g_Vars.lvupdate60;
	}

	chr->soundtimer += g_Vars.lvupdate60;
	chr->talktimer += g_Vars.lvupdate60;

#if VERSION < VERSION_PAL_BETA
	if (chr->aibot) {
		chr->aibot->unk030 += g_Vars.lvupdate60;
	}
#endif

	if (chr->hidden & CHRHFLAG_TIMER_RUNNING) {
		chr->timer60 += g_Vars.lvupdate60;
	}

	chr->sleep -= g_Vars.lvupdate60;

	if (chr->sleep < 0
			|| (chr->chrflags & CHRCFLAG_NEVERSLEEP)
			|| chr->alertness >= 65
			|| (chr->aibot && (chr->actiontype == ACT_DIE || chr->actiontype == ACT_DEAD))) {
		u8 pass = race == RACE_HUMAN || race == RACE_SKEDAR;
		chr->sleep = 0;

#ifndef PLATFORM_N64
		// Campaign co-op (host-authoritative AI): when idle (target == -1) an NPC
		// perceives only the single player indexed by chr->p1p2. Native splitscreen
		// co-op alternates it via the chr_toggle_p1p2 ailist command, but not every
		// NPC/trigger toggles, so remote net players go unnoticed — e.g. a client
		// entering a scripted region never trips the trigger. Hold each living player
		// for a short window then advance, so every player is perceived within
		// ~window ticks (long enough for accumulating sight timers to latch) and can
		// trip sight/region scripts. Only while idle (target == -1) so an NPC already
		// engaged with a specific target is untouched. Host + net co-op only
		// (NETMODE_SERVER excludes splitscreen, which keeps its native toggle); skips
		// aibots (Combat Sim sims have their own targeting).
		if (g_NetMode == NETMODE_SERVER && g_Vars.coopplayernum >= 0
				&& !chr->aibot && chr->target == -1) {
			const s32 pcount = PLAYERCOUNT();
			if (pcount > 1) {
				const s32 base = (g_Vars.lvframe60 / 20) % pcount;
				for (s32 n = 0; n < pcount; n++) {
					const s32 p = (base + n) % pcount;
					if (g_Vars.players[p] && !g_Vars.players[p]->isdead) {
						chr->p1p2 = p;
						break;
					}
				}
			}
		}
#endif

#ifndef PLATFORM_N64
		// Campaign co-op: NPC AI is host-authoritative. On clients, skip the AI
		// bytecode for synced NPCs — they're position/anim-driven by the host's
		// chr-state broadcast and forced to ACT_STAND, so running the ailist here
		// would make local combat/navigation decisions that desync (client-side
		// projectiles, drift). Gated to co-op so Combat Sim sims (whose chraiExecute
		// runs locally and is harmlessly overwritten by chr-state) are unaffected.
		//
		// NOTE: objective/trigger MONITOR chrs (the setup's beginloop scripts, e.g.
		// ame func100e_check_ecm_mines) have no syncid and so DO run here — that is
		// deliberate: those same scripts also hand out mission-start equipment via
		// give_object_to_chr (the client equips itself locally; on Perfect Agent the
		// ECM mines + data uplink). Their message/sound re-trigger spam (when a loop
		// guard stage flag won't latch because the host's SVC_STAGE_FLAGS mirror keeps
		// clearing the client's local set) is handled in netmsgSvcStageFlagsRead, which
		// OR-merges flags the client set locally so the guard latches — instead of
		// gating these scripts off and losing the equipment gives.
		if (!(g_NetMode == NETMODE_CLIENT && g_Vars.coopplayernum >= 0
				&& chr->prop && chr->prop->syncid))
#endif
		chraiExecute(chr, PROPTYPE_CHR);

		// Consider setting shootingatmelist
		if (chr->prop) {
			if (chr->aimtesttimer60 < 1) {
				chr->aimtesttimer60 = TICKS(30);

				if (chr->aishootingatmelist >= 0
						&& ailistFindById(chr->aishootingatmelist) != chr->ailist
						&& chrIsTargetAimingAtMe(chr)) {
					chr->chrflags |= CHRCFLAG_CONSIDER_DODGE;
				}
			} else {
				chr->aimtesttimer60 -= g_Vars.lvupdate60;
			}
		}

		// Consider setting darkroomlist
		if (chr->prop
				&& chr->aidarkroomlist >= 0
				&& roomGetSettledLocalBrightness(chr->prop->rooms[0]) < 25
				&& ailistFindById(chr->aidarkroomlist) != chr->ailist) {
			chr->darkroomthing = true;
		}

		// Consider setting playerdeadlist
		if (chr->prop && chr->aiplayerdeadlist >= 0 && g_Vars.currentplayer->isdead) {
			u32 prevplayernum = g_Vars.currentplayernum;
			s32 i;
			s32 playercount = PLAYERCOUNT();
			bool alldead = true;

			if (playercount >= 2) {
				for (i = 0; i < playercount && alldead; i++) {
					if (i != prevplayernum) {
						setCurrentPlayerNum(i);

						if (g_Vars.currentplayer->isdead == false) {
							alldead = false;
						}
					}
				}

				setCurrentPlayerNum(prevplayernum);
			}

			if (alldead && ailistFindById(chr->aiplayerdeadlist) != chr->ailist) {
				chr->playerdeadthing = true;
			}
		}

		if (race == RACE_ROBOT) {
			robotSetMuzzleFlash(chr, 0, false);
			robotSetMuzzleFlash(chr, 1, false);
		}

		if (chr->prop) {
			if (g_Vars.in_cutscene) {
				switch (chr->actiontype) {
				case ACT_ANIM:   chrTickAnim(chr);   break;
				case ACT_PATROL: chrTickPatrol(chr); pass = false; break;
				}
			}
#ifndef PLATFORM_N64
			// Skip the per-action tick for net-replicated chrs on a client:
			// Combat Sim sim bots (chr->aibot) AND campaign co-op NPCs (any
			// synced chr while coopplayernum is set — co-op NPCs are NOT aibots,
			// so the aibot test alone misses them). The server's actiontype isn't
			// synced (would crash because the chr->act_* union data isn't synced
			// either — see netmsgSvcPropMoveRead chr-state block), and running
			// chrTickStand here every frame would clobber the synced anim/yrot
			// from SVC_PROP_MOVE bit-4 with stand-pose defaults. That fight is
			// what made dying co-op NPCs flicker between the death anim (re-applied
			// by netChrInterpolate) and an idle pose (re-chosen by
			// chrChooseStandAnimation when the non-looping death anim finished).
			// The model anim frame still advances during render via modelTickAnim,
			// so skipping the action tick is purely a positive: no overwrite of
			// synced state, no crash from missing union data.
			else if (g_NetMode == NETMODE_CLIENT && (chr->aibot
					|| (g_Vars.coopplayernum >= 0 && chr->prop && chr->prop->syncid))) {
				// intentionally empty — anim/orientation come from net sync
			}
#endif
			else {
				switch (chr->actiontype) {
				case ACT_STAND:           chrTickStand(chr);           break;
				case ACT_KNEEL:           chrTickKneel(chr);           break;
				case ACT_ANIM:            chrTickAnim(chr);            break;
				case ACT_DIE:             chrTickDie(chr);             break;
				case ACT_ARGH:            chrTickArgh(chr);            break;
				case ACT_PREARGH:         chrTickPreArgh(chr);         break;
				case ACT_SIDESTEP:        chrTickSidestep(chr);        break;
				case ACT_JUMPOUT:         chrTickJumpOut(chr);         break;
				case ACT_DEAD:            chrTickDead(chr);            break;
				case ACT_ATTACK:          chrTickAttack(chr);          break;
				case ACT_ATTACKWALK:      chrTickAttackWalk(chr);      break;
				case ACT_ATTACKROLL:      chrTickAttackRoll(chr);      break;
				case ACT_RUNPOS:          chrTickRunPos(chr);          break;
				case ACT_PATROL:          chrTickPatrol(chr);          pass = false; break;
				case ACT_GOPOS:           chrTickGoPos(chr);           break;
				case ACT_SURRENDER:       chrTickSurrender(chr);       break;
				case ACT_TEST:            chrTickTest(chr);            break;
				case ACT_SURPRISED:       chrTickSurprised(chr);       break;
				case ACT_STARTALARM:      chrTickStartAlarm(chr);      break;
				case ACT_THROWGRENADE:    chrTickThrowGrenade(chr);    break;
				case ACT_BONDDIE:         chrTickBondDie(chr);         break;
				case ACT_DRUGGEDCOMINGUP: chrTickDruggedComingUp(chr); break;
				case ACT_DRUGGEDDROP:     chrTickDruggedDrop(chr);     break;
				case ACT_DRUGGEDKO:       chrTickDruggedKo(chr);       break;
				case ACT_ATTACKAMOUNT:    chrTickAttackAmount(chr);    break;
				case ACT_ROBOTATTACK:     chrTickRobotAttack(chr);     break;
				case ACT_SKJUMP:          chrTickSkJump(chr);          break;
				}
			}
		}

#if VERSION >= VERSION_NTSC_1_0
		chr->hidden &= ~CHRHFLAG_IS_HEARING_TARGET;
		chr->hidden2 &= ~CHRH2FLAG_CONSIDERPROXIES;
#else
		chr->hidden &= ~(CHRHFLAG_IS_HEARING_TARGET | CHRHFLAG_CONSIDERPROXIES);
#endif

		if (pass) {
			footstepCheckDefault(chr);
		}
	} else {
		footstepCheckMagic(chr);
	}
}

void cutsceneStart(u32 ailistid)
{
	struct prop *prop;

#if PAL
	var8009e388pf = 0;
#else
	g_CutsceneFrameOverrun240 = 0;
#endif
	g_CutsceneSkipRequested = false;
	g_CutsceneCurTotalFrame60f = 0;

	prop = g_Vars.activeprops;

	while (prop) {
		prop->lastupdateframe = 0xffff;
		prop->propupdate240 = 0;
		prop->propupdate60err = 2;

		prop = prop->next;
	}

	g_BgChrs[g_NumBgChrs - 1].ailist = ailistFindById(ailistid);
	g_BgChrs[g_NumBgChrs - 1].aioffset = 0;
	g_BgChrs[g_NumBgChrs - 1].aireturnlist = -1;
}

/**
 * Execute background tasks.
 *
 * - Calculate number of engaged chrs
 * - Handle switching to new cutscenes when playing all
 * - Run BG scripts
 * - Choose which corpses to fade
 */
void chraTickBg(void)
{
	s32 i;
	s32 numchrs = chrsGetNumSlots();
	s32 numaliveonscreen;
	s32 numdeadonscreen;
	s32 onscreenlen;
	s32 offscreenlen;
#if VERSION >= VERSION_NTSC_1_0
	s32 spawnslen;
#endif
	struct chrdata *onscreen[5];
	struct chrdata *offscreen[5];
#if VERSION >= VERSION_NTSC_1_0
	struct chrdata *spawns[10];
#endif
	s32 writeindex;
	s32 maxdeadonscreen;

#if VERSION >= VERSION_NTSC_1_0
	static u32 var80068454 = 0;
#endif

	static s32 mosteveralive = 0;

	numaliveonscreen = 0;

#if VERSION >= VERSION_NTSC_1_0
	spawnslen = 0;
	numdeadonscreen = 0;
#else
	numdeadonscreen = 0;
	onscreenlen = 0;
	offscreenlen = 0;
#endif

#if VERSION >= VERSION_NTSC_1_0
	// Count the number of chrs who are engaged with the player.
	// When these numbers are high, chrs can ignore gopos commands.
	// NTSC beta has a simple version of this logic in a loop
	// near the end of this function.
	g_NumChrsWithPlayerTarget = 0;
	g_NumChrsSeenPlayer = 0;
	g_NumChrsSeenPlayerRecently = 0;
	g_NumChrsSeenPlayerRecently2 = 0;

	if (g_Vars.normmplayerisrunning == false) {
		for (i = 0; i < numchrs; i++) {
			struct chrdata *chr = &g_ChrSlots[i];

			if (chr->model && chr->prop && !chrIsDead(chr)) {
				struct prop *targetprop = chrGetTargetProp(chr);

				if (targetprop && (targetprop->type == PROPTYPE_CHR || targetprop->type == PROPTYPE_PLAYER)) {
					if ((targetprop->type == PROPTYPE_PLAYER
								&& !(g_Vars.antiplayernum >= 0 && g_Vars.anti && PROP_IS_FOR_ANTI_PLAYER(targetprop))
								&& chrCompareTeams(chr, targetprop->chr, COMPARE_ENEMIES))
							|| CHRRACE(targetprop->chr) == RACE_EYESPY) {
						s32 time60;
						s32 lastsee;
						s32 lastvis;

						g_NumChrsWithPlayerTarget++;

						lastsee = chr->lastseetarget60;
						lastvis = chr->lastvisibletarget60;
						time60 = lastsee > lastvis ? lastsee : lastvis;

						if (time60) {
							g_NumChrsSeenPlayer++;

							if (g_Vars.lvframe60 - time60 < TICKS(240)) {
								g_NumChrsSeenPlayerRecently++;
								g_NumChrsSeenPlayerRecently2++;
							}
						}
					}
				}
			}
		}
	}

	var80068454++;

	if (var80068454 > 10) {
		var80068454 = 0;
	}
#endif

	// If enabled, print a list of dangerous props to the developer's console
	if (debugDangerousProps()) {
		propPrintDangerous();
	}

	// Handle switching to a new cutscene when using "Play All" from the menu
	if (g_Vars.autocutnum >= 0) {
		cutsceneStart(g_Vars.autocutnum + 0xc00);
		g_Vars.autocutnum = -1;
		g_Vars.autocutplaying = true;
	}

	// Run BG scripts
	for (i = 0; i < g_NumBgChrs; i++) {
		if (!g_Vars.autocutplaying || (g_BgChrs[i].hidden2 & CHRH2FLAG_TICKDURINGAUTOCUT)) {
			chraTick(&g_BgChrs[i]);
		}
	}

	// Calculate alive/dead counters. For *spawned* chrs that have died,
	// allow 10 corpses and start fading if there's more.
	{
		s32 numalive = 0;

		for (i = 0; i < numchrs; i++) {
			struct chrdata *chr = &g_ChrSlots[i];

			if (chr->model && chr->prop) {
				if (chr->actiontype != ACT_DEAD && chr->actiontype != ACT_DRUGGEDKO) {
					numalive++;
				}

				if (chr->prop->flags & PROPFLAG_ONANYSCREENPREVTICK) {
					if (chr->actiontype != ACT_DEAD && chr->actiontype != ACT_DRUGGEDKO) {
						numaliveonscreen++;
					} else if (chr->actiontype == ACT_DRUGGEDKO) {
						if ((chr->chrflags & CHRCFLAG_KEEPCORPSEKO) == 0) {
							numdeadonscreen++;
						}
					} else {
						numdeadonscreen++;
					}
				}

#if VERSION >= VERSION_NTSC_1_0
				if (chr->actiontype == ACT_DEAD
						|| (chr->actiontype == ACT_DRUGGEDKO && (chr->chrflags & CHRCFLAG_KEEPCORPSEKO) == 0)) {
					if (chr->hidden2 & CHRH2FLAG_SPAWNED) {
						spawns[spawnslen] = chr;
						spawnslen++;

						if (spawnslen >= 10) {
							writeindex = rngRandom() % spawnslen;
							chrFadeCorpse(spawns[writeindex]);
							spawns[writeindex] = spawns[spawnslen - 1];
							spawnslen--;
						}
					}
				}
#endif
			}
		}

		if (numalive > mosteveralive) {
			mosteveralive = numalive;
		}
	}

	// Calculate how many corpses are allowed on screen. Ideally no more than
	// 5 chrs (alive or dead) to keep the number of polygons low.
	maxdeadonscreen = 5 - numaliveonscreen;

	// If the game is lagging, take the number of 60ths over 6 since the
	// previous frame and reduce the limit by that amount. So if it's been, say,
	// 8 60ths since the last frame then the max corpses will be reduced by 2.
	// This is why corpses are more likely to fade on screen when you change
	// weapons - the lag spike from changing guns triggers a lower corpse limit.
	if (g_Vars.lvupdate60 > 6) {
		maxdeadonscreen = maxdeadonscreen - g_Vars.lvupdate60 + 6;
	}

	if (maxdeadonscreen < 0) {
		maxdeadonscreen = 0;
	}

#if VERSION >= VERSION_JPN_FINAL
	// JPN fades corpses immediately
	if (numdeadonscreen && g_Vars.stagenum != STAGE_CITRAINING) {
		for (i = 0; i < numchrs; i++) {
			struct chrdata *chr = &g_ChrSlots[i];

			if (chr->model
					&& (chr->actiontype == ACT_DEAD || (chr->actiontype == ACT_DRUGGEDKO
							&& chr->prop && (chr->chrflags & CHRCFLAG_KEEPCORPSEKO) == 0))
					&& chr->act_dead.fadetimer60 < 0
					&& !chr->act_dead.fadenow) {
				chrFadeCorpse(chr);
			}
		}
	}
#else
	// Decide which corpses to fade.
	// NTSC Beta implements its engagement counter here.
	if (numdeadonscreen) {
		numdeadonscreen = 0;
#if VERSION >= VERSION_NTSC_1_0
		onscreenlen = 0;
		offscreenlen = 0;
#else
		g_NumChrsSeenPlayerRecently2 = 0;
#endif

		for (i = 0; i < numchrs; i++) {
			struct chrdata *chr = &g_ChrSlots[i];

			if (chr->model) {
#if VERSION < VERSION_NTSC_1_0
				if (!g_Vars.mplayerisrunning
						&& chrGetTargetProp(chr)->type == PROPTYPE_PLAYER
						&& chr->lastseetarget60 > 0
						&& g_Vars.lvframe60 - chr->lastseetarget60 < TICKS(120)) {
					g_NumChrsSeenPlayerRecently2++;
				}
#endif

				if (chr->actiontype == ACT_DEAD
						|| (chr->actiontype == ACT_DRUGGEDKO && chr->prop && (chr->chrflags & CHRCFLAG_KEEPCORPSEKO) == 0)) {
					if (chr->prop->flags & PROPFLAG_ONANYSCREENPREVTICK) {
						// On-screen
						if (chr->act_dead.fadetimer60 < 0 && !chr->act_dead.fadenow) {
							numdeadonscreen++;

							// If there's too many corpses on screen, start fading.
							if (numdeadonscreen > maxdeadonscreen || chr->aibot) {
								chrFadeCorpse(chr);
								numdeadonscreen--;
							} else if (!chr->act_dead.fadewheninvis) {
								// If there are 2 or more corpses on screen,
								// start marking them to be removed once off screen
								onscreen[onscreenlen] = chr;
								onscreenlen++;

								if (onscreenlen >= (VERSION >= VERSION_NTSC_1_0 ? 2 : 3)) {
									writeindex = rngRandom() % onscreenlen;
									chrFadeCorpseWhenOffScreen(onscreen[writeindex]);
									onscreen[writeindex] = onscreen[onscreenlen - 1];
									onscreenlen--;
								}
							}
						}
					} else {
						// Off-screen
						if (!chr->act_dead.fadewheninvis) {
							offscreen[offscreenlen] = chr;
							offscreenlen++;

							// Allow up to 5 corpses off-screen
							if (offscreenlen >= (VERSION >= VERSION_NTSC_1_0 ? 5 : 6)) {
								writeindex = rngRandom() % offscreenlen;

								if (offscreen[writeindex]->actiontype != ACT_DEAD) {
									chrBeginDead(offscreen[writeindex]);
								}

								chrFadeCorpseWhenOffScreen(offscreen[writeindex]);
								offscreen[writeindex] = offscreen[offscreenlen - 1];
								offscreenlen--;
							}
						}
					}
				}
			}
		}
	}
#endif

	if (1);
}

bool chrSawTargetRecently(struct chrdata *chr)
{
	if (chr->lastseetarget60 > 0 && g_Vars.lvframe60 - chr->lastseetarget60 < TICKS(600)) {
		return true;
	}

	return false;
}

bool chrHeardTargetRecently(struct chrdata *chr)
{
	if (chr->lastheartarget60 > 0 && g_Vars.lvframe60 - chr->lastheartarget60 < TICKS(600)) {
		return true;
	}

	return false;
}

f32 chrGetAngleToPos(struct chrdata *chr, struct coord *pos)
{
	f32 fVar3;
	f32 fVar2;
	f32 fVar4;
	struct prop *prop;

	if (chr->prop->type == PROPTYPE_PLAYER) {
		u32 playernum = playermgrGetPlayerNumByProp(chr->prop);
		fVar3 = (360 - g_Vars.players[playernum]->vv_theta) * (M_BADTAU / 360);
	} else {
		fVar3 = chrGetInverseTheta(chr);
	}

	prop = chr->prop;
	fVar2 = atan2f(pos->x - prop->pos.x, pos->z - prop->pos.z);
	fVar4 = fVar2 - fVar3;

	if (fVar2 < fVar3) {
		fVar4 += M_BADTAU;
	}

	return fVar4;
}

f32 chrGetAngleToTarget(struct chrdata *chr)
{
	struct prop *prop = chrGetTargetProp(chr);
	return chrGetAngleToPos(chr, &prop->pos);
}

void chrGetAttackEntityPos(struct chrdata *chr, u32 attackflags, s32 entityid, struct coord *pos, RoomNum *rooms)
{
	struct prop *targetprop;
	struct chrdata *targetchr;
	struct pad pad;

	if (attackflags & ATTACKFLAG_AIMATCHR) {
		// Aiming at a chr by chrnum
		targetchr = chrFindById(chr, entityid);

		if (!targetchr || !targetchr->prop) {
			targetchr = chr;
		}

		pos->x = targetchr->prop->pos.x;
		pos->y = targetchr->prop->pos.y;
		pos->z = targetchr->prop->pos.z;

		if (targetchr) {
#ifdef PLATFORM_N64
			chr = targetprop->chr; // bug? targetprop is never set before this
#else
			chr = targetchr;
#endif
		} else {
			chr = NULL;
		}

		if (chr);

		roomsCopy(targetchr->prop->rooms, rooms);
	} else if (attackflags & ATTACKFLAG_AIMATPAD) {
		// Aiming at a pad by padnum
		s32 padnum = chrResolvePadId(chr, entityid);
		padUnpack(padnum, PADFIELD_POS | PADFIELD_ROOM, &pad);

		pos->x = pad.pos.x;
		pos->y = pad.pos.y;
		pos->z = pad.pos.z;

		rooms[0] = pad.room;
		rooms[1] = -1;
	} else {
		// Aiming at the chr's preconfigured target
		targetprop = chrGetTargetProp(chr);

		pos->x = targetprop->pos.x;
		pos->y = targetprop->pos.y;
		pos->z = targetprop->pos.z;

		if (targetprop->type == PROPTYPE_CHR && targetprop->chr) {
			chr = targetprop->chr;
		} else {
			chr = NULL;
		}

		if (chr);

		roomsCopy(targetprop->rooms, rooms);
	}
}

f32 chrGetAngleFromTargetsFov(struct chrdata *chr)
{
	f32 targetfacingangle = 0;
	struct prop *prop = chr->prop;
	struct prop *target = chrGetTargetProp(chr);
	f32 xdiff;
	f32 zdiff;
	f32 angletotarget;
	f32 result;

	if (prop && target) {
		xdiff = prop->pos.f[0] - target->pos.f[0];
		zdiff = prop->pos.f[2] - target->pos.f[2];
		angletotarget = atan2f(xdiff, zdiff);

		if (target->type == PROPTYPE_PLAYER) {
			s32 playernum = playermgrGetPlayerNumByProp(target);
			targetfacingangle = (360.0f - g_Vars.players[playernum]->vv_theta) * M_BADTAU / 360.0f;
		} else if (target->type == PROPTYPE_CHR) {
			targetfacingangle = chrGetInverseTheta(target->chr);
		}

		result = angletotarget - targetfacingangle;

		if (angletotarget < targetfacingangle) {
			result += M_BADTAU;
		}
	}

	return result;
}

f32 chrGetVerticalAngleToTarget(struct chrdata *chr)
{
	struct prop *prop = chr->prop;
	struct prop *target = chrGetTargetProp(chr);
	f32 result = 0;

	if (prop && target) {
		f32 ydiff;
		f32 xdiff;
		f32 zdiff;

		xdiff = prop->pos.x - target->pos.x;
		ydiff = prop->pos.y - target->pos.y;
		zdiff = prop->pos.z - target->pos.z;

		result = atan2f(ydiff, sqrtf(xdiff * xdiff + zdiff * zdiff));

		if (result < 0) {
			result += M_BADTAU;
		}
	}

	return result;
}

bool chrIsInTargetsFovX(struct chrdata *chr, u8 fov360)
{
	f32 angle = chrGetAngleFromTargetsFov(chr);

	if ((angle < fov360 * 0.024539785459638f && angle < M_PI)
			|| (angle > M_BADTAU - fov360 * 0.024539785459638f && angle > M_PI)) {
		return true;
	}

	return false;
}

bool chrIsVerticalAngleToTargetWithin(struct chrdata *chr, u8 fov360)
{
	f32 val = chrGetVerticalAngleToTarget(chr);

	if ((val < fov360 * 0.024539785459638f && val < M_PI)
			|| (val > M_BADTAU - fov360 * 0.024539785459638f && val > M_PI)) {
		return true;
	}

	return false;
}

f32 func0f048fcc(struct chrdata *chr, u8 reverse)
{
	f32 result;

	struct prop *chrprop = chr->prop;
	struct prop *targetprop = chrGetTargetProp(chr);

	f32 xdiff = chrprop->pos.x - targetprop->pos.x;
	f32 zdiff = chrprop->pos.z - targetprop->pos.z;

	f32 angle1 = atan2f(-xdiff, -zdiff);
	f32 angle2 = chrGetInverseTheta(chr) + M_PI * (s32)reverse;

	result = angle1 - angle2;

	if (angle1 < angle2) {
		result += M_BADTAU;
	}

	return result;
}

bool chrIsTargetInFov(struct chrdata *chr, u8 arg1, u8 reverse)
{
	f32 angle = func0f048fcc(chr, reverse);

	if ((angle < arg1 * 0.024539785459638f && angle < M_PI)
			|| (angle > M_BADTAU - arg1 * 0.024539785459638f && angle > M_PI)) {
		return true;
	}

	return false;
}

bool chrIsLookingAtPos(struct chrdata *chr, struct coord *pos, u8 arg2)
{
	f32 angle = chrGetAngleToPos(chr, pos);

	if ((angle < arg2 * 0.024539785459638f && angle < M_PI) ||
			(M_BADTAU - arg2 * 0.024539785459638f < angle && M_PI < angle)) {
		return true;
	}

	return false;
}

f32 chrGetDistanceToTarget(struct chrdata *chr)
{
	struct prop *prop = chrGetTargetProp(chr);
	return propGetDistanceToProp(chr->prop, prop);
}

// Redundant function - it's the same as above
f32 chrGetDistanceToTarget2(struct chrdata *chr)
{
	struct prop *prop = chrGetTargetProp(chr);
	return propGetDistanceToProp(chr->prop, prop);
}

f32 chrGetDistanceToCurrentPlayer(struct chrdata *chr)
{
	return propGetDistanceToProp(chr->prop, g_Vars.currentplayer->prop);
}

f32 propGetDistanceToProp(struct prop *a, struct prop *b)
{
	f32 xdiff = a->pos.x - b->pos.x;
	f32 ydiff = a->pos.y - b->pos.y;
	f32 zdiff = a->pos.z - b->pos.z;

	return sqrtf(xdiff * xdiff + ydiff * ydiff + zdiff * zdiff);
}

f32 propGetLateralDistanceToProp(struct prop *a, struct prop *b)
{
	f32 xdiff = a->pos.x - b->pos.x;
	f32 zdiff = a->pos.z - b->pos.z;

	return sqrtf(xdiff * xdiff + zdiff * zdiff);
}

f32 chrGetDistanceToPad(struct chrdata *chr, s32 pad_id)
{
	struct prop *prop = chr->prop;
	f32 xdiff, ydiff, zdiff;
	f32 distance = 0;
	struct pad pad;
	pad_id = chrResolvePadId(chr, pad_id);

#if VERSION >= VERSION_NTSC_1_0
	if (pad_id >= 0)
#endif
	{
		padUnpack(pad_id, PADFIELD_POS, &pad);
		xdiff = pad.pos.x - prop->pos.x;
		ydiff = pad.pos.y - prop->pos.y;
		zdiff = pad.pos.z - prop->pos.z;
		distance = sqrtf(xdiff * xdiff + ydiff * ydiff + zdiff * zdiff);
	}

	return distance;
}

#if VERSION >= VERSION_NTSC_1_0
f32 chrGetSameFloorDistanceToPad(struct chrdata *chr, s32 pad_id)
{
	struct prop *prop = chr->prop;
	f32 xdiff, ydiff, zdiff, ydiff_absolute;
	struct pad pad;
	f32 ret;

	pad_id = chrResolvePadId(chr, pad_id);
	padUnpack(pad_id, PADFIELD_POS, &pad);
	xdiff = pad.pos.x - prop->pos.x;
	ydiff = pad.pos.y - prop->pos.y;
	zdiff = pad.pos.z - prop->pos.z;

	if (ydiff > 0) {
		ydiff_absolute = ydiff;
	} else {
		ydiff_absolute = -ydiff;
	}

	if (ydiff_absolute < 150) {
		ret = sqrtf(xdiff * xdiff + zdiff * zdiff);
	} else {
		ret = 100000000;
	}

	return ret;
}
#endif

f32 chrGetDistanceToCoord(struct chrdata *chr, struct coord *pos)
{
	f32 xdiff = pos->x - chr->prop->pos.x;
	f32 ydiff = pos->y - chr->prop->pos.y;
	f32 zdiff = pos->z - chr->prop->pos.z;

	return sqrtf(xdiff * xdiff + ydiff * ydiff + zdiff * zdiff);
}

f32 chrGetLateralDistanceToCoord(struct chrdata *chr, struct coord *pos)
{
	f32 xdiff = pos->x - chr->prop->pos.x;
	f32 zdiff = pos->z - chr->prop->pos.z;

	return sqrtf(xdiff * xdiff + zdiff * zdiff);
}

f32 chrGetLateralDistanceToPad(struct chrdata *chr, s32 pad_id)
{
	struct prop *prop = chr->prop;
	f32 xdiff, zdiff;
	struct pad pad;

#if VERSION >= VERSION_NTSC_1_0
	f32 distance = 0;

	pad_id = chrResolvePadId(chr, pad_id);

	if (pad_id >= 0) {
		padUnpack(pad_id, PADFIELD_POS, &pad);
		xdiff = pad.pos.x - prop->pos.x;
		zdiff = pad.pos.z - prop->pos.z;
		distance = sqrtf(xdiff * xdiff + zdiff * zdiff);
	}

	return distance;
#else
	pad_id = chrResolvePadId(chr, pad_id);
	padUnpack(pad_id, PADFIELD_POS, &pad);
	xdiff = pad.pos.x - prop->pos.x;
	zdiff = pad.pos.z - prop->pos.z;
	return sqrtf(xdiff * xdiff + zdiff * zdiff);
#endif
}

f32 chrGetSquaredDistanceToCoord(struct chrdata *chr, struct coord *pos)
{
	f32 xdiff = pos->x - chr->prop->pos.x;
	f32 ydiff = pos->y - chr->prop->pos.y;
	f32 zdiff = pos->z - chr->prop->pos.z;

	return xdiff * xdiff + ydiff * ydiff + zdiff * zdiff;
}

f32 coordGetSquaredDistanceToCoord(struct coord *a, struct coord *b)
{
	f32 xdiff = a->x - b->x;
	f32 ydiff = a->y - b->y;
	f32 zdiff = a->z - b->z;

	return xdiff * xdiff + ydiff * ydiff + zdiff * zdiff;
}

s32 chrGetPadRoom(struct chrdata *chr, s32 pad_id)
{
	s32 ret = -1;
	s32 pad_id_backup = pad_id;
	struct pad pad;

	if (pad_id >= 10000) {
		s32 resolved_pad_id = chrResolvePadId(chr, pad_id - 10000);

#if VERSION >= VERSION_NTSC_1_0
		if (resolved_pad_id >= 0)
#endif
		{
			padUnpack(resolved_pad_id, PADFIELD_ROOM, &pad);
			ret = pad.room;
		}
	} else {
		ret = pad_id;
	}

	return ret;
}

s32 chrResolvePadId(struct chrdata *chr, s32 pad_id)
{
	if (pad_id == 9000) {
		pad_id = chr->padpreset1;
	}

	return pad_id;
}

/**
 * For all chrs, clear their target and p1p2 values if set to the given player.
 *
 * This function is called when the given player has died. It causes all guards
 * to switch their focus to the remaining coop player.
 */
void chrsClearRefsToPlayer(s32 playernum)
{
	s32 otherplayernum;
	s32 playerpropnum;
	s32 i;

	if (g_Vars.coopplayernum >= 0) {
		if (playernum == g_Vars.bondplayernum) {
			otherplayernum = g_Vars.coopplayernum;
			playerpropnum = g_Vars.bond->prop - g_Vars.props;
		} else {
			otherplayernum = g_Vars.bondplayernum;
			playerpropnum = g_Vars.coop->prop - g_Vars.props;
		}

		for (i = 0; i < chrsGetNumSlots(); i++) {
			if (g_ChrSlots[i].p1p2 == playernum) {
				g_ChrSlots[i].p1p2 = otherplayernum;
			}

			if (g_ChrSlots[i].target == playerpropnum) {
				g_ChrSlots[i].target = -1;
			}
		}

		for (i = 0; i < g_NumBgChrs; i++) {
			if (g_BgChrs[i].p1p2 == playernum) {
				g_BgChrs[i].p1p2 = otherplayernum;
			}

			if (g_BgChrs[i].target == playerpropnum) {
				g_BgChrs[i].target = -1;
			}
		}
	}
}

#ifndef PLATFORM_N64
// Net co-op client: CHR_BOND / CHR_COOP are SLOT-SEMANTIC — scripts (and the
// host, whose local slots ARE the wire slots) mean "the player at wire slot
// bondplayernum/coopplayernum", but g_Vars.bond/coop point at LOCAL slots,
// which netPlayersAllocate transposed (local player -> slot 0). Resolve the
// pawn at the transposed local slot instead, so EVERY chrFindById consumer —
// scripted gives, control grants/revokes, hudmsgs/subtitles, disarms,
// draw-weapon, fades, autowalk, and all the aiIf* conditionals — targets the
// same PHYSICAL player as the host (known-issues "option 3", generalised from
// the original aiGiveObjectToChr-only fix; see PORT_NET_KNOWN_ISSUES.md).
// Deliberately NOT applied to CHR_TARGET / CHR_P1P2 / chr->target paths:
// those hold locally-perceived (already-physical) player references, and
// re-transposing them would corrupt them. Identity outside client co-op and
// on no-swap clients.
static struct player *chrResolveCoopSlotPlayer(struct player *vanillapl, s32 playernum)
{
	if (g_NetMode == NETMODE_CLIENT && g_Vars.coopplayernum >= 0
			&& playernum >= 0 && playernum < MAX_PLAYERS) {
		const s32 localslot = netCoopRemapWirePlayernum(playernum);

		if (localslot != playernum && localslot >= 0 && localslot < MAX_PLAYERS
				&& g_Vars.players[localslot]) {
			return g_Vars.players[localslot];
		}
	}

	return vanillapl;
}
#endif

s32 chrResolveId(struct chrdata *ref, s32 id)
{
	if (ref) {
		switch (id) {
		case CHR_SEESHOT:
			id = ref->chrseeshot;
			break;
		case CHR_SEEDIE:
			id = ref->chrseedie;
			break;
		case CHR_PRESET:
			id = ref->chrpreset1;
			break;
		case CHR_SELF:
			id = ref->chrnum;
			break;
		case CHR_CLONE:
			id = ref->chrdup;
			break;
		case CHR_BOND:
#ifndef PLATFORM_N64
			{
				struct player *pl = chrResolveCoopSlotPlayer(g_Vars.bond, g_Vars.bondplayernum);

				if (pl && pl->prop && pl->prop->chr) {
					id = pl->prop->chr->chrnum;
				}
			}
#else
			if (g_Vars.bond && g_Vars.bond->prop && g_Vars.bond->prop->chr) {
				id = g_Vars.bond->prop->chr->chrnum;
			}
#endif
			break;
		case CHR_COOP:
#ifndef PLATFORM_N64
			{
				struct player *pl = chrResolveCoopSlotPlayer(g_Vars.coop, g_Vars.coopplayernum);

				if (pl && pl->prop && pl->prop->chr) {
					id = pl->prop->chr->chrnum;
				}
			}
#else
			if (g_Vars.coop && g_Vars.coop->prop && g_Vars.coop->prop->chr) {
				id = g_Vars.coop->prop->chr->chrnum;
			}
#endif
			break;
		case CHR_ANTI:
			if (g_Vars.anti && g_Vars.anti->prop && g_Vars.anti->prop->chr) {
				id = g_Vars.anti->prop->chr->chrnum;
			}
			break;
		case CHR_P1P2:
			{
				u32 index = g_Vars.coopplayernum >= 0 ? ref->p1p2 : g_Vars.bondplayernum;
				struct player *player = g_Vars.players[index];
				if (player && player->prop && player->prop->chr) {
					id = player->prop->chr->chrnum;
				}
			}
			break;
		case CHR_P1P2_OPPOSITE:
			if (g_Vars.coopplayernum >= 0) {
				struct player *player = g_Vars.players[1 - ref->p1p2];
				if (player && player->prop && player->prop->chr) {
					id = player->prop->chr->chrnum;
				}
			}
			break;
		case CHR_TARGET:
			{
				struct prop *target = chrGetTargetProp(ref);
				if ((target->type == PROPTYPE_CHR || target->type == PROPTYPE_PLAYER) && target->chr) {
					id = target->chr->chrnum;
				}
			}
			break;
		}
	} else { // ref is NULL
		switch (id) {
		case CHR_BOND:
#ifndef PLATFORM_N64
			{
				struct player *pl = chrResolveCoopSlotPlayer(g_Vars.bond, g_Vars.bondplayernum);

				if (pl && pl->prop && pl->prop->chr) {
					id = pl->prop->chr->chrnum;
				}
			}
#else
			if (g_Vars.bond && g_Vars.bond->prop && g_Vars.bond->prop->chr) {
				id = g_Vars.bond->prop->chr->chrnum;
			}
#endif
			break;
		case CHR_COOP:
#ifndef PLATFORM_N64
			{
				struct player *pl = chrResolveCoopSlotPlayer(g_Vars.coop, g_Vars.coopplayernum);

				if (pl && pl->prop && pl->prop->chr) {
					id = pl->prop->chr->chrnum;
				}
			}
#else
			if (g_Vars.coop && g_Vars.coop->prop && g_Vars.coop->prop->chr) {
				id = g_Vars.coop->prop->chr->chrnum;
			}
#endif
			break;
		case CHR_ANTI:
			if (g_Vars.anti && g_Vars.anti->prop && g_Vars.anti->prop->chr) {
				id = g_Vars.anti->prop->chr->chrnum;
			}
			break;
		case CHR_P1P2:
			{
				struct player *player = g_Vars.players[g_Vars.bondplayernum];
				if (player && player->prop && player->prop->chr) {
					id = player->prop->chr->chrnum;
				}
			}
			break;
		case CHR_P1P2_OPPOSITE:
			if (g_Vars.coopplayernum >= 0) {
				struct player *player = g_Vars.players[g_Vars.coopplayernum];
				if (player && player->prop && player->prop->chr) {
					id = player->prop->chr->chrnum;
				}
			}
			break;
		}
	}

	return id;
}

struct chrdata *chrFindById(struct chrdata *basechr, s32 chrnum)
{
	struct chrdata *chr;
	s32 lower;
	s32 upper;
	s32 i;

	chrnum = chrResolveId(basechr, chrnum);
	chr = chrFindByLiteralId(chrnum);

	if (chr) {
		return chr;
	}

	lower = 0;
	upper = g_NumBgChrs;

	while (upper >= lower) {
		i = (lower + upper) / 2;

		if (chrnum == g_BgChrnums[i]) {
			return &g_BgChrs[i];
		}

		if (chrnum < g_BgChrnums[i]) {
			upper = i - 1;
		} else {
			lower = i + 1;
		}
	}

	return NULL;
}

s32 propGetIndexByChrId(struct chrdata *basechr, s32 chrnum)
{
	s32 index;

	if (chrnum == CHR_BOND || chrnum == CHR_BOND) {
		index = g_Vars.bond->prop - g_Vars.props;
	} else {
		struct chrdata *chr = chrFindById(basechr, chrnum);

		if (chr && chr->prop) {
			index = chr->prop - g_Vars.props;
		} else {
			index = -1;
		}
	}

	return index;
}

f32 chrGetDistanceToChr(struct chrdata *chr1, s32 chr2num)
{
	struct prop *prop1 = chr1->prop;
	struct chrdata *chr2 = chrFindById(chr1, chr2num);
	f32 distance = 0;

	if (chr2 && chr2->prop) {
		f32 xdiff = chr2->prop->pos.x - prop1->pos.x;
		f32 ydiff = chr2->prop->pos.y - prop1->pos.y;
		f32 zdiff = chr2->prop->pos.z - prop1->pos.z;
		distance = sqrtf(xdiff * xdiff + ydiff * ydiff + zdiff * zdiff);
	}

	return distance;
}

f32 chrGetDistanceFromTargetToPad(struct chrdata *chr, s32 pad_id)
{
	struct prop *prop = chrGetTargetProp(chr);
	f32 xdiff, ydiff, zdiff;
	struct pad pad;
	f32 distance = 0;
	pad_id = chrResolvePadId(chr, pad_id);

#if VERSION >= VERSION_NTSC_1_0
	if (pad_id >= 0)
#endif
	{
		padUnpack(pad_id, PADFIELD_POS, &pad);
		xdiff = pad.pos.x - prop->pos.x;
		ydiff = pad.pos.y - prop->pos.y;
		zdiff = pad.pos.z - prop->pos.z;
		distance = sqrtf(xdiff * xdiff + ydiff * ydiff + zdiff * zdiff);
	}

	return distance;
}

void chrSetFlags(struct chrdata *chr, u32 flags, u8 bank)
{
	if (bank == 0) {
		chr->flags |= flags;
	} else {
		chr->flags2 |= flags;
	}
}

void chrUnsetFlags(struct chrdata *chr, u32 flags, u8 bank)
{
	if (bank == 0) {
		chr->flags &= ~flags;
	} else {
		chr->flags2 &= ~flags;
	}
}

bool chrHasFlag(struct chrdata *chr, u32 flag, u8 bank)
{
	if (bank == 0) {
		return (chr->flags & flag) != 0;
	} else {
		return (chr->flags2 & flag) != 0;
	}
}

void chrSetFlagsById(struct chrdata *ref, u32 chrnum, u32 flags, u32 bank)
{
	struct chrdata *chr = chrFindById(ref, chrnum);

	if (chr) {
		chrSetFlags(chr, flags, bank);
	}
}

void chrUnsetFlagsById(struct chrdata *ref, u32 chrnum, u32 flags, u32 bank)
{
	struct chrdata *chr = chrFindById(ref, chrnum);

	if (chr) {
		chrUnsetFlags(chr, flags, bank);
	}
}

bool chrHasFlagById(struct chrdata *ref, u32 chrnum, u32 flag, u32 bank)
{
	struct chrdata *chr = chrFindById(ref, chrnum);

	if (chr) {
		return chrHasFlag(chr, flag, bank);
	}

	return false;
}

void chrSetStageFlag(struct chrdata *chr, u32 flag)
{
	g_StageFlags |= flag;

#ifndef PLATFORM_N64
	// Co-op client: remember flags our own scripts set so the host's SVC_STAGE_FLAGS
	// mirror (which overwrites g_StageFlags) can't clear them out from under a script
	// loop guard. Without this an objective monitor whose "complete" guard is a stage
	// flag the host hasn't set yet re-enters its branch every mirror tick, re-firing
	// show_hudmsg + set_object_sound_playing (the ECM-mine message + sound spam).
	if (g_NetMode == NETMODE_CLIENT && g_Vars.coopplayernum >= 0) {
		g_NetCoopLocalStageFlags |= flag;
	}
#endif
}

void chrUnsetStageFlag(struct chrdata *chr, u32 flag)
{
	g_StageFlags = g_StageFlags & ~flag;

#ifndef PLATFORM_N64
	if (g_NetMode == NETMODE_CLIENT && g_Vars.coopplayernum >= 0) {
		g_NetCoopLocalStageFlags &= ~flag;
	}
#endif
}

bool chrHasStageFlag(struct chrdata *chr, u32 flag)
{
	return (g_StageFlags & flag) != 0;
}

bool chrIsHearingTarget(struct chrdata *chr)
{
	return (chr->hidden & CHRHFLAG_IS_HEARING_TARGET) != 0;
}

void chrRestartTimer(struct chrdata *chr)
{
	chr->timer60 = 0;
	chr->hidden |= CHRHFLAG_TIMER_RUNNING;
}

f32 chrGetTimer(struct chrdata *chr)
{
	return chr->timer60 * FRAMEDURATION;
}

bool chrIsTargetAimingAtMe(struct chrdata *chr)
{
	struct prop *target = chrGetTargetProp(chr);

	if (target) {
		if (target->type == PROPTYPE_CHR) {
			if (!chrHasLosToTarget(chr)) {
				return false;
			}

			return chrIsInTargetsFovX(chr, 20);
		}

		if (target->type == PROPTYPE_PLAYER) {
			if (g_Vars.bondvisible &&
					(cdTestLos05(&target->pos, target->rooms, &chr->prop->pos, chr->prop->rooms,
									  CDTYPE_OBJS | CDTYPE_DOORS | CDTYPE_PATHBLOCKER | CDTYPE_BG,
									  GEOFLAG_BLOCK_SIGHT))) {
				struct model *model = chr->model;
				struct coord sp68;
				struct coord sp56;
				struct coord sp44;
				f32 somefloat = modelGetEffectiveScale(model) * 0.8f;

				bgun0f0a0c08(&sp68, &sp56);
				modelGetRootPosition(model, &sp44);
				mtx4TransformVecInPlace(camGetWorldToScreenMtxf(), &sp44);

				if (func0f06b39c(&sp68, &sp56, &sp44, somefloat)) {
					return true;
				}
			}
		}
	}

	return false;
}

bool chrResetNearMiss(struct chrdata *chr)
{
	bool has_flag = (chr->chrflags & CHRCFLAG_NEAR_MISS) != 0;
	chr->chrflags &= ~CHRCFLAG_NEAR_MISS;

	return has_flag;
}

s32 chrGetNumArghs(struct chrdata *chr)
{
	return chr->numarghs;
}

s32 chrGetNumCloseArghs(struct chrdata *chr)
{
	return chr->numclosearghs;
}

bool chrSawInjury(struct chrdata *chr, u8 arg1)
{
	bool saw_injury = chr->chrseeshot >= 0;

	if (saw_injury && arg1 == 0) {
		chr->chrseeshot = -1;
	} else if (saw_injury && arg1 == 1) {
		struct chrdata *victim = chrFindById(chr, chr->chrseeshot);

		if (victim && !chrCompareTeams(chr, victim, COMPARE_FRIENDS)) {
			saw_injury = false;
		}
	} else {
		chr->chrseeshot = -1;
	}

	return saw_injury;
}

bool chrSawDeath(struct chrdata *chr, u8 arg1)
{
	bool saw_death = chr->chrseedie >= 0;

	// The commented line below was likely originally there but removed before
	// the final version. Compare with chrSawInjury above.
	if (saw_death && arg1 == 0) {
		//chr->chrseedie = -1;
	} else if (saw_death && arg1 == 1) {
		struct chrdata *victim = chrFindById(chr, chr->chrseedie);

		if (victim && !chrCompareTeams(chr, victim, COMPARE_FRIENDS)) {
			saw_death = false;
			chr->chrseedie = -1;
		}
	} else {
		chr->chrseedie = -1;
	}

	return saw_death;
}

void decrementByte(u8 *dst, u8 amount)
{
	if (*dst < amount) {
		*dst = 0;
		return;
	}

	*dst -= amount;
}

void incrementByte(u8 *dst, u8 amount)
{
	if (0xff - amount < *dst) {
		*dst = 0xff;
		return;
	}

	*dst += amount;
}

bool chrCanHearAlarm(struct chrdata *chr)
{
	return alarmIsActive();
}

bool waypointIsWithin90DegreesOfPosAngle(struct waypoint *waypoint, struct coord *pos, f32 angle)
{
	u32 stack[3];
	f32 diffangle;
	struct pad pad;

	padUnpack(waypoint->padnum, PADFIELD_POS, &pad);

	diffangle = angle - atan2f(pad.pos.x - pos->x, pad.pos.z - pos->z);

	if (diffangle < 0) {
		diffangle += M_BADTAU;
	}

	if (diffangle < 1.5705462694168f || diffangle > 4.7116389274597f) {
		return true;
	}

	return false;
}

/**
 * Attempt to find a waypoint near pos which is in a particular quadrant to pos,
 * then return its padnum.
 *
 * For example, pos is typically the player's position, angle is the direction
 * the player is facing, and quadrant is which quadrant (front/back/left/right)
 * that is desired relative to the player's position and angle.
 *
 * The function starts by finding the closest waypoint to the pos. If it's not
 * in the quadrant then its neighouring waypoints are checked too. If none of
 * those are in the quadrant then no further checks are made and the function
 * returns -1.
 */
s32 chrFindWaypointWithinPosQuadrant(struct coord *pos, RoomNum *rooms, f32 angle, u8 quadrant)
{
	struct waypoint *waypoint = waypointFindClosestToPos(pos, rooms);
	s32 neighbournum;
	s32 i;

	if (waypoint) {
		switch (quadrant) {
		case QUADRANT_BACK:
			angle += M_BADPI;
			break;
		case QUADRANT_SIDE1:
			angle += 1.5705462694168f;
			break;
		case QUADRANT_SIDE2:
			angle += 4.7116389274597f;
			break;
		case QUADRANT_FRONT:
			break;
		}

		if (angle >= M_BADTAU) {
			angle -= M_BADTAU;
		}

		if (waypointIsWithin90DegreesOfPosAngle(waypoint, pos, angle)) {
			return waypoint->padnum;
		}

		for (i = 0; (neighbournum = waypoint->neighbours[i]) >= 0; i++) {
			if ((neighbournum & 0x8000) == 0) {
				neighbournum &= 0x3fff;

				if (waypointIsWithin90DegreesOfPosAngle(&g_StageSetup.waypoints[neighbournum], pos, angle)) {
					return g_StageSetup.waypoints[neighbournum].padnum;
				}
			}

		}
	}

	return -1;
}

bool func0f04a4ec(struct chrdata *chr, u8 quadrant)
{
	if (quadrant == QUADRANT_TOWARDSTARGET || quadrant == QUADRANT_AWAYFROMTARGET) {
		struct prop *prop = chr->prop;
		struct prop *target = chrGetTargetProp(chr);

		struct waypoint *chrwp = waypointFindClosestToPos(&prop->pos, prop->rooms);
		struct waypoint *tarwp = waypointFindClosestToPos(&target->pos, target->rooms);

		struct waypoint *waypoints[3];
		s32 numwaypoints;

		if (chrwp && tarwp) {
			if (quadrant == QUADRANT_TOWARDSTARGET) {
				navSetSeed(CHRNAVSEED(chr), CHRNAVSEED(chr));
				numwaypoints = navFindRoute(chrwp, tarwp, waypoints, 3);
				navSetSeed(0, 0);

				if (numwaypoints >= 3) {
					chr->padpreset1 = waypoints[1]->padnum;
					return true;
				}
			} else {
				navSetSeed(CHRNAVSEED(chr), CHRNAVSEED(chr));
				chrwp = navChooseRetreatPoint(chrwp, tarwp);
				navSetSeed(0, 0);

				if (chrwp) {
					chr->padpreset1 = chrwp->padnum;
					return true;
				}
			}
		}
	} else {
		s32 padnum = chrFindWaypointWithinPosQuadrant(&chr->prop->pos, chr->prop->rooms, chrGetInverseTheta(chr), quadrant);

		if (padnum >= 0) {
			chr->padpreset1 = padnum;
			return true;
		}
	}

	return false;
}

bool chrSetPadPresetToWaypointWithinTargetQuadrant(struct chrdata *chr, u8 quadrant)
{
	f32 angle;
	s32 padnum;
	struct prop *prop;

	if (quadrant == QUADRANT_TOWARDSTARGET || quadrant == QUADRANT_AWAYFROMTARGET) {
		return func0f04a4ec(chr, quadrant);
	}

	angle = 0;
	prop = chrGetTargetProp(chr);

	if (prop->type == PROPTYPE_PLAYER) {
		angle = (360.0f - g_Vars.players[playermgrGetPlayerNumByProp(prop)]->vv_theta) * M_BADTAU / 360.0f;
	} else if (prop->type == PROPTYPE_CHR) {
		angle = chrGetInverseTheta(prop->chr);
	}

	padnum = chrFindWaypointWithinPosQuadrant(&prop->pos, prop->rooms, angle, quadrant);

	if (padnum >= 0) {
		chr->padpreset1 = padnum;
		return true;
	}

	return false;
}

bool chrSetChrPresetToAnyChrNearSelf(struct chrdata *chr, f32 distance)
{
	return chrSetChrPresetToChrNearSelf(COMPARE_ANY, chr, distance);
}

bool chrSetChrPresetToChrNearSelf(u8 checktype, struct chrdata *chr, f32 distance)
{
	return chrSetChrPresetToChrNearPos(checktype, chr, distance, &chr->prop->pos, chr->prop->rooms);
}

bool chrSetChrPresetToChrNearPad(u32 checktype, struct chrdata *chr, f32 distance, s32 padnum)
{
	struct pad pad;
	RoomNum rooms[2];

	padnum = chrResolvePadId(chr, padnum);
	padUnpack(padnum, PADFIELD_POS | PADFIELD_ROOM, &pad);

	rooms[0] = pad.room;
	rooms[1] = -1;

	return chrSetChrPresetToChrNearPos(checktype, chr, distance, &pad.pos, rooms);
}

bool chrSetChrPresetToChrNearPos(u8 checktype, struct chrdata *chr, f32 distance, struct coord *pos, RoomNum *rooms)
{
	s32 i;
	s16 *propnumptr;
	s16 propnums[256];
	RoomNum allrooms[21];
	f32 xmin = pos->x - distance;
	f32 xmax = pos->x + distance;
	f32 ymin = pos->y - distance;
	f32 ymax = pos->y + distance;
	f32 zmin = pos->z - distance;
	f32 zmax = pos->z + distance;
	RoomNum neighbours[11];

	roomsCopy(rooms, allrooms);

	for (i = 0; rooms[i] != -1; i++) {
		bgRoomGetNeighbours(rooms[i], neighbours, 10);
		roomsAppend(neighbours, allrooms, 20);
	}

	roomGetProps(allrooms, propnums, 256);

	propnumptr = propnums;

	while (*propnumptr >= 0) {
		struct prop *prop = &g_Vars.props[*propnumptr];

		if (prop->type == PROPTYPE_CHR || prop->type == PROPTYPE_PLAYER) {
			struct chrdata *loopchr = prop->chr;

			if (loopchr->chrnum != chr->chrnum
					&& !chrIsDead(loopchr)
					&& prop->pos.x >= xmin
					&& prop->pos.x <= xmax
					&& prop->pos.y >= ymin
					&& prop->pos.y <= ymax
					&& prop->pos.z >= zmin
					&& prop->pos.z <= zmax
					&& chrCompareTeams(loopchr, chr, checktype)) {
				chr->chrpreset1 = loopchr->chrnum;
				return true;
			}
		}

		*propnumptr++;
	}

	return false;
}

bool chrCompareTeams(struct chrdata *chr1, struct chrdata *chr2, u8 checktype)
{
	if (chr1 && chr1->prop) {
		if (checktype == COMPARE_ANY) {
			return true;
		}

		if (checktype == COMPARE_FRIENDS) { // Return true if chrs are friends
			if (g_Vars.normmplayerisrunning) {
				if ((g_MpSetup.options & MPOPTION_TEAMSENABLED) && chr2->team == chr1->team) {
					return true;
				}
			} else {
				if (g_Vars.bond && g_Vars.bond->prop) {
					struct chrdata *playerchr = g_Vars.bond->prop->chr;

					// @bug: This makes Jon an ally in Duel
					if ((chr2 == playerchr && chr1->headnum == HEAD_JONATHAN) ||
							(chr1 == playerchr && chr2->headnum == HEAD_JONATHAN)) {
						return true;
					}
				}

				if ((chr2->team & chr1->team) != 0) {
					return true;
				}
			}
		} else if (checktype == COMPARE_ENEMIES) { // Return true if chrs are enemies
			if (g_Vars.normmplayerisrunning) {
				if ((g_MpSetup.options & MPOPTION_TEAMSENABLED) == 0 || chr2->team != chr1->team) {
					return true;
				}
			} else {
				if (g_Vars.bond && g_Vars.bond->prop) {
					struct chrdata *playerchr = g_Vars.bond->prop->chr;

					// @bug: This makes Jon an ally in Duel
					if ((chr2 == playerchr && chr1->headnum == HEAD_JONATHAN) ||
							(chr1 == playerchr && chr2->headnum == HEAD_JONATHAN)) {
						return false;
					}
				}

				if ((chr2->team & chr1->team) == 0) {
					return true;
				}
			}
		}

		return false;
	}

	return false;
}

void chrSetChrPreset(struct chrdata *chr, s32 chrpreset)
{
	chr->chrpreset1 = chrResolveId(chr, chrpreset);
}

void chrSetChrPresetByChrnum(struct chrdata *basechr, s32 chrnum, s32 chrpreset)
{
	struct chrdata *chr = chrFindById(basechr, chrnum);

	if (chr) {
		chr->chrpreset1 = chrResolveId(basechr, chrpreset);
	}
}

void chrSetPadPreset(struct chrdata *chr, s32 pad_id)
{
	chr->padpreset1 = chrResolvePadId(chr, pad_id);
}

void chrSetPadPresetByChrnum(struct chrdata *basechr, s32 chrnum, s32 pad_id)
{
	struct chrdata *chr = chrFindById(basechr, chrnum);

	if (chr) {
		chr->padpreset1 = chrResolvePadId(basechr, pad_id);
	}
}

/**
 * Check if chr has line of sight to their target. If not, find a route
 * to them then iterate the route backwards (from target to chr).
 * Find the first waypoint in view of both the chr and their target
 * and store it as the chr's pad preset.
 */
bool chrSetPadPresetToPadOnRouteToTarget(struct chrdata *chr)
{
	struct prop *target = chrGetTargetProp(chr);
	struct prop *prop = chr->prop;
	struct waypoint *fromwp;
	struct waypoint *towp;
	struct waypoint *waypoints[5];
	s32 numwaypoints;
	s32 i;
	struct pad pad;

	if (target->type != PROPTYPE_PLAYER || g_Vars.bondvisible) {
		if (cdTestLos04(&prop->pos, prop->rooms, &target->pos, CDTYPE_BG)) {
			return false;
		}

		fromwp = waypointFindClosestToPos(&prop->pos, prop->rooms);
		towp = waypointFindClosestToPos(&target->pos, target->rooms);

		if (fromwp && towp) {
			// Note from/to are swapped here, so the route is from target to chr
			navSetSeed(CHRNAVSEED(chr), CHRNAVSEED(chr));
			numwaypoints = navFindRoute(towp, fromwp, waypoints, 5);
			navSetSeed(0, 0);

			if (numwaypoints >= 3) {
				for (i = 0; waypoints[i] != NULL; i++) {
					struct waypoint *wp = waypoints[i];

					padUnpack(wp->padnum, PADFIELD_POS, &pad);

					if (cdTestLos04(&target->pos, target->rooms, &pad.pos, CDTYPE_BG)) {
						if (cdTestLos04(&prop->pos, prop->rooms, &pad.pos, CDTYPE_BG)) {
							chr->padpreset1 = wp->padnum;
							return true;
						}
					}
				}
			}
		}
	}

	return false;
}

bool chrIsPosOffScreen(struct coord *pos, RoomNum *rooms)
{
	bool offscreen = true;
	s32 i;

	if (envIsPosInFogMaxDistance(pos, 0)) {
		for (i = 0; rooms[i] != -1; i++) {
			if (bgRoomIsOnscreen(rooms[i])) {
				break;
			}
		}

		if (rooms[i] != -1) {
			// Room is visible by player
			offscreen = !camIsPosInScreenBox(pos, 200, bgGetRoomDrawSlot(rooms[i]));
		}
	}

	return offscreen;
}

/**
 * Test if a chr can be spawned into or nearby the given position, taking into
 * consideration collision checks.
 *
 * If the spawn can happen, the position and rooms are updated with the actual
 * position to be used and the function returns true.
 *
 * If the spawn cannot happen, the function return false.
 */
#if VERSION >= VERSION_NTSC_1_0
bool chrAdjustPosForSpawn(f32 chrradius, struct coord *pos, RoomNum *rooms, f32 angle, bool allowonscreen, bool force, bool onlysurrounding)
{
	struct coord testpos;
	s32 i;
	u32 types;
	RoomNum testrooms[8];
	f32 ymin;
	f32 ymax = 200;
	f32 curangle = angle;
	f32 ground;

	if (force) {
		types = CDTYPE_ALL & ~CDTYPE_BG;
		allowonscreen = true;
	} else {
		types = CDTYPE_ALL;
	}

	if (onlysurrounding) {
		// Skip testing the given pos, and just do the surrounding checks below.
		// Add 45 degrees to the angle here, but this isn't necessary.
		curangle += 0.7852731347084f;

		if (curangle >= M_BADTAU) {
			curangle -= M_BADTAU;
		}
	} else {
		// Check that the chr isn't being spawned out of bounds, and do a volume
		// test reaching 200cm above and below the chr's feet... unless there is
		// ground under the chr (highly likely), in which case reduce the volume
		// to be tested to the ground Y value. I'm not sure why this is useful,
		// because if the chr was being spawned on top of another chr or object
		// then the calculated ground value would be raised.
		ymin = -200;
		ground = cdFindGroundAtCyl(pos, chrradius, rooms, NULL, NULL);

		if (ground > -100000 && ground - pos->y < -200) {
			ymin = ground - pos->y;
		}

		if (cdTestVolume(pos, chrradius, rooms, types, CHECKVERTICAL_YES, ymax, ymin) != CDRESULT_COLLISION
				&& (allowonscreen || chrIsPosOffScreen(pos, rooms))) {
			return true;
		}
	}

#ifdef PLATFORM_N64
	// Try 60cm in 8 directions
	for (i = 0; i < 8; i++) {
		testpos.x = pos->x + sinf(curangle) * 60;
		testpos.y = pos->y;
		testpos.z = pos->z + cosf(curangle) * 60;
#else
	// On Defection some floating point precision issues result in P2 being placed in the way of P1's cutscene animation,
	// which makes P1 start stuck in P2 in coop, so the distance is increased to 80 to avoid that
	const f32 distance = (g_Vars.stagenum == STAGE_DEFECTION) ? 80.f : 60.f;
	for (i = 0; i < 8; i++) {
		testpos.x = pos->x + sinf(curangle) * distance;
		testpos.y = pos->y;
		testpos.z = pos->z + cosf(curangle) * distance;
#endif
		if ((onlysurrounding && cdTestCylMove04(pos, rooms, &testpos, testrooms, CDTYPE_ALL & ~CDTYPE_PLAYERS, 1, ymax, -200) != CDRESULT_COLLISION)
				|| (!onlysurrounding && cdTestLos11(pos, rooms, &testpos, testrooms, CDTYPE_BG))) {
			chr0f021fa8(NULL, &testpos, testrooms);
			ground = cdFindGroundAtCyl(&testpos, chrradius, testrooms, 0, 0);
			ymin = -200;

			if (ground > -100000 && ground - pos->y < -200) {
				ymin = ground - pos->y;
			}

			if (cdTestVolume(&testpos, chrradius, testrooms, CDTYPE_ALL, CHECKVERTICAL_YES, ymax, ymin) != CDRESULT_COLLISION
					&& (allowonscreen || chrIsPosOffScreen(&testpos, testrooms))
					&& (!onlysurrounding || ground > -100000)) {
				pos->x = testpos.x;
				pos->y = testpos.y;
				pos->z = testpos.z;
				roomsCopy(testrooms, rooms);
				return true;
			}
		}

		curangle += 0.7852731347084f;

		if (curangle >= M_BADTAU) {
			curangle -= M_BADTAU;
		}
	}

	return false;
}
#else
/**
 * ntsc-beta's version of this function doesn't have the onlysurrounding argument
 * nor out of bounds checking, and lacks the reduction for the volume test.
 */
bool chrAdjustPosForSpawn(f32 chrradius, struct coord *pos, RoomNum *rooms, f32 angle, bool allowonscreen, bool force)
{
	struct coord testpos;
	s32 i;
	u32 types;
	RoomNum testrooms[8];
	f32 curangle = angle;

	if (force) {
		types = CDTYPE_ALL & ~CDTYPE_BG;
		allowonscreen = true;
	} else {
		types = CDTYPE_ALL;
	}

	if (cdTestVolume(pos, chrradius, rooms, types, CHECKVERTICAL_YES, 200, -200) != CDRESULT_COLLISION
			&& (allowonscreen || chrIsPosOffScreen(pos, rooms))) {
		return true;
	}

	for (i = 0; i < 8; i++) {
		testpos.x = sinf(curangle) * 60 + pos->x;
		testpos.y = pos->y;
		testpos.z = cosf(curangle) * 60 + pos->z;

		if (cdTestLos11(pos, rooms, &testpos, testrooms, CDTYPE_BG)
				&& cdTestVolume(&testpos, chrradius, testrooms, CDTYPE_ALL, CHECKVERTICAL_YES, 200, -200.0f) != CDRESULT_COLLISION
				&& (allowonscreen || chrIsPosOffScreen(&testpos, testrooms))) {
			pos->x = testpos.x;
			pos->y = testpos.y;
			pos->z = testpos.z;
			roomsCopy(testrooms, rooms);
			return true;
		}

		curangle += 0.7852731347084f;

		if (curangle >= M_BADTAU) {
			curangle -= M_BADTAU;
		}
	}

	return false;
}
#endif

/**
 * Attempts to spawn a chr at the given coordinates.
 *
 * In low memory conditions, the function will iterate all existing chrs in
 * search of a corpse that can be reaped. If one is found then the reap will be
 * triggered, but the function will not attempt to spawn the chr until the next
 * time it's called.
 */
struct prop *chrSpawnAtCoord(s32 bodynum, s32 headnum, struct coord *pos, RoomNum *rooms, f32 angle, u8 *ailist, u32 spawnflags)
{
	struct prop *prop;
	struct coord pos2;
	RoomNum rooms2[8];
	s32 stack;

	if (chrsGetNumFree() > 1) {
		if (headnum < 0) {
#ifndef PLATFORM_N64
			// Bodies with a BUILT-IN head (skedar, Elvis, Dr Caroll — the
			// unk00_01 bit) must spawn headless: the setup path
			// (bodyAllocateChr) skips head selection for them entirely, and
			// bolting a human head on makes the model build fail (chaos
			// pd.spawn_body of a Skedar King returned NULL here). Keep the
			// negative headnum so body0f02ce8c takes the no-head path.
			if (!g_HeadsAndBodies[bodynum].unk00_01) {
				headnum = bodyChooseHead(bodynum);
			}
#else
			headnum = bodyChooseHead(bodynum);
#endif
		}

		pos2.x = pos->x;
		pos2.y = pos->y;
		pos2.z = pos->z;
		roomsCopy(rooms, rooms2);

#if VERSION >= VERSION_NTSC_1_0
		if (chrAdjustPosForSpawn(20, &pos2, rooms2, angle, (spawnflags & SPAWNFLAG_ALLOWONSCREEN) != 0, false, false))
#else
		if (chrAdjustPosForSpawn(20, &pos2, rooms2, angle, (spawnflags & SPAWNFLAG_ALLOWONSCREEN) != 0, false))
#endif
		{
			struct model *model = bodyAllocateModel(bodynum, headnum, spawnflags);
			struct chrdata *chr;

			if (model) {
				prop = chrAllocate(model, &pos2, rooms2, angle, ailist);

				if (prop) {
					propActivateThisFrame(prop);
					propEnable(prop);

					chr = prop->chr;
					chr->headnum = headnum;
					chr->bodynum = bodynum;
					chr->race = bodyGetRace(chr->bodynum);
					chr->flags = 0;
					chr->flags2 = 0;
#if VERSION >= VERSION_NTSC_1_0
					chr->hidden2 |= CHRH2FLAG_SPAWNED;
#endif

					if (spawnflags & SPAWNFLAG_NOBLOOD) {
						chr->noblood = true;
					}

#ifndef PLATFORM_N64
					// Campaign co-op: this is a RUNTIME chr spawn (reinforcement,
					// scripted guard, clone) that only the host runs — the client's
					// NPC AI/scripts are gated off, so it never creates this chr and
					// the guard is invisible there. Broadcast it so the client makes
					// a matching shell with this server-assigned syncid; the chr-state
					// broadcast then drives its pose/anim/weapons/HP. Gated to an
					// in-progress game so setup-time spawns (deterministic positional
					// syncids, created identically on both sides) aren't re-sent.
					if (g_NetMode == NETMODE_SERVER && g_NetCoopChrLifecycle
							&& g_Vars.coopplayernum >= 0
							&& prop->syncid && g_NetLocalClient
							&& g_NetLocalClient->state == CLSTATE_GAME) {
						netServerBroadcastChrSpawn(prop, angle, spawnflags);
					}
#endif

					return prop;
				}

				modelmgrFreeModel(model);
			}
		}
	}

	// Low memory - find a corpse to reap
	if (chrsGetNumFree() < 4) {
#if VERSION >= VERSION_NTSC_1_0
		s32 stack2;
		struct chrdata *replacechr;
		s32 startindex;
		s32 index;
#else
		s32 startindex;
		struct chrdata *replacechr;
		s32 index;
#endif

		replacechr = NULL;
		startindex = rngRandom() % g_NumChrSlots;
		index = startindex;

#if VERSION < VERSION_NTSC_1_0
		if (startindex);
#endif

		do {
			if (g_ChrSlots[index].chrnum >= 0 && g_ChrSlots[index].model && g_ChrSlots[index].prop) {
				if (g_ChrSlots[index].actiontype == ACT_DEAD
#if VERSION >= VERSION_NTSC_1_0
						|| (g_ChrSlots[index].actiontype == ACT_DRUGGEDKO && (g_ChrSlots[index].chrflags & CHRCFLAG_KEEPCORPSEKO) == 0)
#endif
						) {
					// If we've found a chr that's ready to be reaped, great.
					// Bail out of the loop.
					if (g_ChrSlots[index].act_dead.invistimer60 >= TICKS(120)) {
						replacechr = &g_ChrSlots[index];
						break;
					}

					// Otherwise, this chr is dead/KO'ed and can be reaped as a
					// last resort, so store them and keep looping in search of
					// a better chr.
					if (replacechr == NULL) {
						replacechr = &g_ChrSlots[index];
					}
				}
			}

			index = (index + 1) % g_NumChrSlots;
		} while (index != startindex);

		if (replacechr) {
			replacechr->act_dead.fadewheninvis = true;
			replacechr->act_dead.fadenow = true;
		}
	}

	return NULL;
}

struct prop *chrSpawnAtPad(struct chrdata *basechr, s32 body, s32 head, s32 pad_id, u8 *ailist, u32 spawnflags)
{
	s32 resolved_pad_id = chrResolvePadId(basechr, pad_id);
	struct pad pad;
	RoomNum room[2];
	f32 fvalue;
	padUnpack(resolved_pad_id, PADFIELD_POS | PADFIELD_LOOK | PADFIELD_ROOM, &pad);
	fvalue = atan2f(pad.look.x, pad.look.z);
	room[0] = pad.room;
	room[1] = -1;

	return chrSpawnAtCoord(body, head, &pad.pos, &room[0], fvalue, ailist, spawnflags);
}

struct prop *chrSpawnAtChr(struct chrdata *basechr, s32 body, s32 head, u32 chrnum, u8 *ailist, u32 spawnflags)
{
	struct chrdata *chr = chrFindById(basechr, chrnum);
	f32 fvalue;

	if (1) {
		fvalue = chrGetInverseTheta(chr);
	}

	return chrSpawnAtCoord(body, head, &chr->prop->pos, chr->prop->rooms, fvalue, ailist, spawnflags);
}

bool chrIsPropPresetBlockingSightToTarget(struct chrdata *chr)
{
	bool result = false;
	struct prop *prop = chr->prop;
	struct prop *target = chrGetTargetProp(chr);

	if (chr->proppreset1 >= 0) {
		chrSetPerimEnabled(chr, false);
		propSetPerimEnabled(target, false);

		if (!cdTestLos04(&prop->pos, prop->rooms, &target->pos,
					CDTYPE_OBJS | CDTYPE_DOORS | CDTYPE_PATHBLOCKER | CDTYPE_BG)) {
			struct prop *obstacle = cdGetObstacleProp();

			if (obstacle && obstacle->type == PROPTYPE_OBJ
					&& chr->proppreset1 == (s16)(obstacle - g_Vars.props)) {
				result = true;
			}
		}

		chrSetPerimEnabled(chr, true);
		propSetPerimEnabled(target, true);
	}

	return result;
}

void func0f04b740(void)
{
	// empty
}

bool chrSetPos(struct chrdata *chr, struct coord *pos, RoomNum *rooms, f32 theta, bool findground)
{
	// Net-snap entry point (CSP teleport / force-move). Unlike its sibling
	// chrMoveToPos this is reached directly from wire-driven paths, so guard the
	// pointers it unconditionally dereferences below (chr->prop->rooms,
	// chr->model->...) — a spectator / mid-spawn remote slot can momentarily have
	// a NULL model or prop. See docs/netplay-code-review-2026.md (M-1).
	if (chr == NULL || chr->prop == NULL || chr->model == NULL || pos == NULL || rooms == NULL) {
		return false;
	}

	const f32 angle = BADDEG2RAD(360.f - theta);

	if (findground) {
		const u32 oldhidden = chr->hidden;
		chr->hidden |= CHRHFLAG_WARPONSCREEN;
		const bool ret = chrMoveToPos(chr, pos, rooms, angle, true);
		if ((oldhidden & CHRHFLAG_WARPONSCREEN) == 0) {
			chr->hidden &= ~CHRHFLAG_WARPONSCREEN;
		}
	}

	bool newrooms = false;
	for (s32 i = 0; i < ARRAYCOUNT(chr->prop->rooms) && rooms[i] >= 0; ++i) {
		if (chr->prop->rooms[i] != rooms[i]) {
			newrooms = true;
			break;
		}
	}

	propSetPerimEnabled(chr->prop, false);

	chr->prop->pos = *pos;

	const f32 ground = cdFindGroundInfoAtCyl(pos, chr->radius, rooms, &chr->floorcol,
			&chr->floortype, NULL, &chr->floorroom, NULL, NULL);

	chr->ground = ground;
	chr->manground = ground;
	chr->sumground = ground * (PAL ? 8.4175090789795f : 9.999998f);

	if (newrooms) {
		propDeregisterRooms(chr->prop);
		roomsCopy(rooms, chr->prop->rooms);
		chr0f0220ac(chr);
	}

	modelSetRootPosition(chr->model, pos);

	const u16 nodetype = chr->model->definition->rootnode->type;

	if ((nodetype & 0xff) == MODELNODETYPE_CHRINFO) {
		union modelrwdata *rwdata = modelGetNodeRwData(chr->model, chr->model->definition->rootnode);
		rwdata->chrinfo.ground = ground;
	}

	chrSetLookAngle(chr, angle);

	if (chr->prop->type == PROPTYPE_PLAYER) {
		struct player *player = g_Vars.players[playermgrGetPlayerNumByProp(chr->prop)];
		player->vv_manground = ground;
		player->vv_ground = ground;
		player->vv_theta = theta;
		player->unk1c64 = 1;
	}

	propSetPerimEnabled(chr->prop, true);

	return true;
}

bool chrMoveToPos(struct chrdata *chr, struct coord *pos, RoomNum *rooms, f32 angle, bool force)
{
	struct coord pos2;
	RoomNum rooms2[8];
	bool result = false;
	u32 nodetype;
	union modelrwdata *rwdata;
	struct player *player = NULL;
	f32 ground;

	pos2.x = pos->x;
	pos2.y = pos->y;
	pos2.z = pos->z;

	roomsCopy(rooms, rooms2);
	propSetPerimEnabled(chr->prop, false);

#if VERSION >= VERSION_NTSC_1_0
	if (chrAdjustPosForSpawn(chr->radius, &pos2, rooms2, angle, (chr->hidden & CHRHFLAG_WARPONSCREEN) != 0, force, (chr->hidden & CHRHFLAG_SPAWNONLYSURROUNDING) != 0))
#else
	if (chrAdjustPosForSpawn(chr->radius, &pos2, rooms2, angle, (chr->hidden & CHRHFLAG_WARPONSCREEN) != 0, force))
#endif
	{
		ground = cdFindGroundInfoAtCyl(&pos2, chr->radius, rooms2, &chr->floorcol,
				&chr->floortype, NULL, &chr->floorroom, NULL, NULL);

		chr->ground = ground;
		chr->manground = ground;
		chr->sumground = ground * (PAL ? 8.4175090789795f : 9.999998f);
		chr->prop->pos.x = pos2.x;
		chr->prop->pos.y = pos2.y;
		chr->prop->pos.z = pos2.z;

		propDeregisterRooms(chr->prop);
		roomsCopy(rooms2, chr->prop->rooms);
		chr0f0220ac(chr);
		modelSetRootPosition(chr->model, &pos2);

		nodetype = chr->model->definition->rootnode->type;

		if ((nodetype & 0xff) == MODELNODETYPE_CHRINFO) {
			rwdata = modelGetNodeRwData(chr->model, chr->model->definition->rootnode);
			rwdata->chrinfo.ground = ground;
		}

		chr->chrflags |= CHRCFLAG_FORCETOGROUND;
		chrSetLookAngle(chr, angle);

		if (chr->prop->type == PROPTYPE_PLAYER) {
			player = g_Vars.players[playermgrGetPlayerNumByProp(chr->prop)];
			player->vv_manground = ground;
			player->vv_ground = ground;
			player->vv_theta = ((M_BADTAU - angle) * 360.0f) / M_BADTAU;
			player->vv_verta = 0;
			player->unk1c64 = 1;
		}

#ifndef PLATFORM_N64
		if (g_NetMode == NETMODE_SERVER && player) {
			player->ucmd |= UCMD_FL_FORCEPOS | UCMD_FL_FORCEANGLE | UCMD_FL_FORCEGROUND;
		}
#endif

		result = true;
	}

	propSetPerimEnabled(chr->prop, true);

	return result;
}

bool chrCheckCoverOutOfSight(struct chrdata *chr, s32 covernum, bool soft)
{
	struct cover cover;
	struct prop *target;
	bool targetcanseecover;

	// @bug: Should be >= coverGetCount()
	if (covernum < 0 || covernum > coverGetCount() || !coverUnpack(covernum, &cover)) {
		return false;
	}

	target = chrGetTargetProp(chr);

	if (!target) {
		return false;
	}

	if (soft) {
		targetcanseecover = cdTestLos03(&target->pos, target->rooms, cover.pos,
				CDTYPE_OBJS | CDTYPE_DOORS | CDTYPE_BG,
				GEOFLAG_BLOCK_SIGHT);
	} else {
		targetcanseecover = cdIsNearlyInSight(&target->pos, target->rooms, cover.pos, 50.0f,
				CDTYPE_OBJS | CDTYPE_DOORS | CDTYPE_BG);
	}

	if (!targetcanseecover != false) {
		// Target can't see cover
		coverSetOutOfSight(covernum, true);
		return true;
	}

	coverSetOutOfSight(covernum, false);
	return false;
}

s32 chrAssignCoverByCriteria(struct chrdata *chr, u16 criteria, s32 refdist)
{
	RoomNum rooms[8];
	struct cover cover;
	struct covercandidate tmp;
	s32 oldcover;
	s32 i;
	struct prop *roomprop;
	s32 numcovers = coverGetCount();
	s32 numcandidates = 0;
	struct prop *target = chrGetTargetProp(chr);
	bool userandomdist = false;
	bool changed;
	f32 sqdist;
	f32 y = chr->prop->pos.y + 170;
	s32 currefdist = refdist;
	struct prop *gotoprop;

	if (criteria & COVERCRITERIA_DISTTOFETCHPROP) {
		if (!chr->aibot || !chr->aibot->gotoprop) {
			return -1;
		}

		gotoprop = chr->aibot->gotoprop;
	}

	if (chr == NULL) {
		return 0;
	}

	oldcover = chr->cover;
	refdist *= refdist;

	// Iterate all cover, filter them by criteria and store them in g_CoverCandidates
	for (i = 0; i < numcovers; i++) {
		if (coverUnpack(i, &cover)
				&& !coverIsSpecial(&cover)
				&& ((criteria & COVERCRITERIA_2000) == 0 || (cover.flags & COVERFLAG_OMNIDIRECTIONAL))
				&& ((criteria & COVERCRITERIA_1000) || (cover.flags & COVERFLAG_AIMDIFFROOM) == 0 || !arrayIntersects(cover.rooms, target->rooms))) {
			userandomdist = false;

			if ((criteria & COVERCRITERIA_0001) && (criteria & COVERCRITERIA_FURTHEREST)) {
				userandomdist = true;
				criteria &= ~(COVERCRITERIA_0001 | COVERCRITERIA_FURTHEREST);
			}

			if (((criteria & COVERCRITERIA_FORCENEWCOVER) == 0 || i != oldcover)
					&& ((criteria & COVERCRITERIA_2000) || !(coverIsInUse(i) || cover.pos->y > y))) {
				if (criteria & COVERCRITERIA_ROOMSFROMME) {
					roomprop = chr->prop;
				} else if (criteria & COVERCRITERIA_ROOMSFROMTARGET) {
					roomprop = target;
				} else if (criteria & COVERCRITERIA_DISTTOTARGET) {
					roomprop = target;
				} else {
					roomprop = chr->prop;
				}

				rooms[0] = roomprop->rooms[0];
				rooms[1] = -1;

				if (criteria & COVERCRITERIA_ALLOWNEIGHBOURINGROOMS) {
					bgRoomGetNeighbours(roomprop->rooms[0], &rooms[1], 6);
				} else if (criteria & COVERCRITERIA_ONLYNEIGHBOURINGROOMS) {
					bgRoomGetNeighbours(roomprop->rooms[0], &rooms[0], 7);
				}

				if (((criteria & COVERCRITERIA_0040) == 0 || !arrayIntersects(cover.rooms, rooms))
						&& ((criteria & COVERCRITERIA_0020) == 0 || arrayIntersects(cover.rooms, rooms))
						&& (rooms[1] == -1
							|| chr->oldrooms[0] == -1
							|| (criteria & COVERCRITERIA_0200) == 0
							|| !arrayIntersects(cover.rooms, chr->oldrooms))) {
					if (criteria & COVERCRITERIA_DISTTOME) {
						sqdist = chrGetSquaredDistanceToCoord(chr, cover.pos);
					} else if (criteria & COVERCRITERIA_DISTTOTARGET) {
						sqdist = coordGetSquaredDistanceToCoord(&target->pos, cover.pos);
					} else if (criteria & COVERCRITERIA_DISTTOFETCHPROP) {
						sqdist = coordGetSquaredDistanceToCoord(&gotoprop->pos, cover.pos);
					} else if (userandomdist) {
						sqdist = rngRandom() % 0xf000;
					} else {
						sqdist = 0;
						currefdist = 0;
					}

					if (!currefdist
							|| (currefdist < 0 && sqdist > refdist)
							|| (currefdist > 0 && sqdist < refdist)) {
						g_CoverCandidates[numcandidates].sqdist = sqdist;
						g_CoverCandidates[numcandidates].covernum = i;
						numcandidates++;
					}
				}
			}
		}
	}

	// Sort candidates by distance ascending, or descending if using COVERCRITERIA_FURTHEREST
	if (numcandidates >= 2) {
		do {
			changed = false;

			for (i = 0; i < numcandidates - 1; i++) {
				if ((((criteria & COVERCRITERIA_0001) || (criteria & COVERCRITERIA_DISTTOFETCHPROP) || userandomdist) && (g_CoverCandidates[i].sqdist > g_CoverCandidates[i + 1].sqdist))
						|| ((criteria & COVERCRITERIA_FURTHEREST) && g_CoverCandidates[i].sqdist < g_CoverCandidates[i + 1].sqdist)) {
					changed = true;

					tmp = g_CoverCandidates[i];
					g_CoverCandidates[i] = g_CoverCandidates[i + 1];
					g_CoverCandidates[i + 1] = tmp;
				}
			}
		} while (changed);
	}

	// Assign the first out of sight cover
	for (i = 0; i < numcandidates; i++) {
		if (1 && chrCheckCoverOutOfSight(chr, g_CoverCandidates[i].covernum, criteria & COVERCRITERIA_ALLOWSOFT)) {
			chr->cover = g_CoverCandidates[i].covernum;

			if (oldcover != -1) {
				coverSetInUse(oldcover, false);
			}

			coverSetInUse(chr->cover, true);

			return g_CoverCandidates[i].covernum;
		}
	}

	return -1;
}

/**
 * Find cover in the opposite direction (?) of the chr's runfrompos and assign
 * it to the chr. The distance from the runfrompos to the cover must be between
 * mindist and maxdist along the X/Z plane. The chr will not choose cover more
 * than 170cm higher than their current elevation, but strangely there is no
 * lower Y limit.
 *
 * Testing with this function logic results in the following, where:
 * R = run from pos
 * C = chr's pos
 * A = available cover
 * . = unavailable cover
 *
 *  A A A A A A A A A A A A A A . . . . . .
 *  A A A A A A A A A A A A A A . . . . . .
 *  A A A A A A A A A A A A A A . . . . . .
 *  A A A A A A A A A A A A A A . . . . . .
 *  A A A A A A A A A A A A A A . . . . . .
 *  A A A A A A A R A A A A A A . . . . . .
 *  A A A A A A A A A A A A A A . . . . . .
 *  A A A A A A A A A A A A A A . . . . . .
 *  A A A A A A A A A A A A A A . . . . . .
 *  A A A A A A A A A A A A A A . . . . . .
 *  A A A A A A A A A A A A A A . . . . . .
 *  A A A A A A A A A A A A A A . . . . . .
 *  A A A A A A A A A A A A A A . . . . . .
 *  A A A A A A A A A A A A A A . . . . . .
 *  A A A A A A A A A A A A A A . . . . . A
 *  A A A A A A A A A A A A A A . . . A A A
 *  A A A A A A A A A A A A A A . A A A A A
 *  A A A A A A A A A A A A A C A A A A A A
 *  . . . . . . . . . . . A A A A A A A A A
 *  . . . . . . . . . A A A A A A A A A A A
 *
 * The block of available cover around the runfrompos might be a bug, but if
 * this function is called with mindist = chr - runfrompos or higher then it
 * will work as expected.
 *
 * Preference is given to cover which is the "most opposite", meaning ones
 * which are directly behind the chr from the perspective of runfrompos.
 */
s32 chrAssignCoverAwayFromDanger(struct chrdata *chr, s32 mindist, s32 maxdist)
{
	s32 i;
	f32 vecfromdanger[2];
	f32 vectocover[2];
	f32 y;
	f32 ymax;
	f32 bestsqdist;
	f32 sqdist;
	s32 numcovers;
	s32 prevcover;
	s32 newcover;
	f32 coversqdistfrompos;
	struct cover cover;

	ymax = chr->prop->pos.y + 170;
	y = 0;
	bestsqdist = 0;
	newcover = -1;

	numcovers = coverGetCount();
	prevcover = chr->cover;

	mindist *= mindist;
	maxdist *= maxdist;

	if (mindist);
	if (maxdist);

	vecfromdanger[0] = chr->prop->pos.x - chr->runfrompos.x;
	vecfromdanger[1] = chr->prop->pos.z - chr->runfrompos.z;

	guNormalize(&vecfromdanger[0], &y, &vecfromdanger[1]);

	for (i = 0; i < numcovers; i++) {
		if (coverUnpack(i, &cover) && !coverIsInUse(i) && !(cover.pos->y > ymax) && !coverIsSpecial(&cover)) {
			coversqdistfrompos = coordGetSquaredDistanceToCoord(&chr->runfrompos, cover.pos);

			if (!(coversqdistfrompos < mindist) && !(coversqdistfrompos > maxdist)) {
				vectocover[0] = cover.pos->x - chr->prop->pos.x;
				vectocover[1] = cover.pos->z - chr->prop->pos.z;

				guNormalize(&vectocover[0], &y, &vectocover[1]);

				sqdist = vecfromdanger[0] * vectocover[0] + vecfromdanger[1] * vectocover[1];

				if (!(sqdist < 0) && sqdist > bestsqdist) {
					bestsqdist = sqdist;
					newcover = i;
					if (1);
				}
			}
		}

		if (numcovers);
	}

	chr->cover = newcover;

	if (newcover != -1) {
		if (prevcover != -1) {
			coverSetInUse(prevcover, false);
		}

		coverSetInUse(chr->cover, true);
	}

	return newcover;
}

s16 chrGoToCover(struct chrdata *chr, u8 speed)
{
	struct cover cover;

	if (!chr) {
		return 0;
	}

	if (chrIsReadyForOrders(chr) && chr->cover != -1 && coverUnpack(chr->cover, &cover)) {
		chrGoToRoomPos(chr, cover.pos, &cover.rooms[0], speed);
		return chr->cover;
	}

	return -1;
}

bool chrRunFromPos(struct chrdata *chr, u32 goposflags, f32 rundist, struct coord *frompos)
{
	f32 curdistfrompos;
	struct coord delta;
	RoomNum rooms[8];

	if (chrIsReadyForOrders(chr)) {
		delta.x = chr->prop->pos.x - frompos->x;
		delta.y = chr->prop->pos.y;
		delta.z = chr->prop->pos.z - frompos->z;

		// @bug: This check should be &&. The runfrompos will fail if the
		// frompos is on the same X or Z axis as the chr, which is unlikely
		// because it's a floating point number.
		if (delta.f[0] == 0.0f || delta.f[2] == 0.0f) {
			return false;
		}

		curdistfrompos = sqrtf(delta.f[0] * delta.f[0] + delta.f[2] * delta.f[2]);
		delta.x *= rundist / curdistfrompos;
		delta.z *= rundist / curdistfrompos;

		chrSetPerimEnabled(chr, false);

		if (cdExamLos08(&chr->prop->pos, chr->prop->rooms, &delta, CDTYPE_ALL, GEOFLAG_WALL) == CDRESULT_COLLISION) {
#if VERSION >= VERSION_JPN_FINAL
			cdGetPos(&delta, 18592, "chr/chraction.c");
#elif VERSION >= VERSION_PAL_FINAL
			cdGetPos(&delta, 18555, "chr/chraction.c");
#elif VERSION >= VERSION_PAL_BETA
			cdGetPos(&delta, 18550, "chraction.c");
#elif VERSION >= VERSION_NTSC_1_0
			cdGetPos(&delta, 18547, "chraction.c");
#else
			cdGetPos(&delta, 18277, "chraction.c");
#endif
		}

		chrSetPerimEnabled(chr, true);

		func0f065e74(&chr->prop->pos, chr->prop->rooms, &delta, rooms);

		return chrGoToRoomPos(chr, &delta, rooms, goposflags);
	}

	return false;
}

void chrAddTargetToBdlist(struct chrdata *chr)
{
	if (chr->prop) {
		struct prop *target = chrGetTargetProp(chr);
		s32 i;

		if (target) {
			for (i = 0; i < g_Vars.lvupdate60; i++) {
				chr->bdlist[chr->bdstart] = target->pos.x - chr->prop->pos.x;
				chr->bdstart++;
				chr->bdstart %= 60;

				chr->bdlist[chr->bdstart] = target->pos.z - chr->prop->pos.z;
				chr->bdstart++;
				chr->bdstart %= 60;
			}
		}
	}
}

s32 chrGetDistanceLostToTargetInLastSecond(struct chrdata *chr)
{
	s32 *bdlist = &chr->bdlist[0];
	s32 index = chr->bdstart;
	u32 stack[2];

	s32 x1 = bdlist[(index + 1) % 60];
	s32 z1 = bdlist[index];
	s32 olddist = sqrtf(x1 * x1 + z1 * z1);

	s32 x2 = bdlist[(index + 59) % 60];
	s32 z2 = bdlist[(index + 58) % 60];
	s32 curdist = sqrtf(x2 * x2 + z2 * z2);

	return curdist - olddist;
}

bool chrIsTargetNearlyInSight(struct chrdata *chr, u32 distance)
{
	struct prop *target = chrGetTargetProp(chr);

	return cdIsNearlyInSight(&chr->prop->pos, chr->prop->rooms, &target->pos, distance, CDTYPE_BG);
}

bool chrIsNearlyInTargetsSight(struct chrdata *chr, u32 distance)
{
	struct prop *target = chrGetTargetProp(chr);

	return cdIsNearlyInSight(&target->pos, target->rooms, &chr->prop->pos, distance, CDTYPE_BG);
}

f32 func0f04c784(struct chrdata *chr)
{
	f32 targetfacingangle = 0;
	u32 stack;
	struct prop *target = chrGetTargetProp(chr);
	f32 angletotarget;
	f32 result;

	if (target->type == PROPTYPE_CHR) {
		targetfacingangle = chrGetInverseTheta(target->chr);
	} else if (target->type == PROPTYPE_PLAYER) {
		s32 playernum = playermgrGetPlayerNumByProp(target);
		targetfacingangle = g_Vars.players[playernum]->vv_theta;
	}

	angletotarget = atan2f(target->pos.z - chr->prop->pos.z, target->pos.x - chr->prop->pos.x);
	result = (angletotarget * 360 / M_BADTAU - targetfacingangle) + 90;

	if (result > 180) {
		result -= 360;
	}

	return result;
}

bool chr0f04c874(struct chrdata *chr, u32 angle360, struct coord *pos, u8 arg3, u8 arg4)
{
	struct prop *target = chrGetTargetProp(chr);
	f32 sqdist = 0;
	f32 f24 = func0f04c784(chr);
	f32 cosine;
	f32 sine;
	struct coord chrpos;
	f32 xdiff;
	f32 ydiff;
	f32 zdiff;
	f32 scale;
	bool again;
	struct coord saved;
	s32 result;
	f32 ymax;
	f32 ymin;
	f32 radius;

	chrpos.x = chr->prop->pos.x;
	chrpos.y = chr->prop->pos.y;
	chrpos.z = chr->prop->pos.z;

	do {
		f32 angle360f = angle360;

		if (angle360f > 180) {
			angle360f -= 360;
		}

		angle360f -= f24;
		angle360f *= 0.017450513318181f;

		cosine = cosf(angle360f);
		sine = sinf(angle360f);
		xdiff = chrpos.x - target->pos.x;
		zdiff = chrpos.z - target->pos.z;

		pos->x = target->pos.x + (xdiff * cosine - zdiff * sine);
		pos->y = chrpos.y;
		pos->z = target->pos.z + (xdiff * sine + zdiff * cosine);

		chrGetBbox(chr->prop, &radius, &ymax, &ymin);

		result = cdExamCylMove03(&chrpos, chr->prop->rooms, pos,
				CDTYPE_BG | CDTYPE_OBJS | CDTYPE_DOORS, 1,
				ymax - chrpos.f[1],
				ymin - chrpos.f[1]);

		if (result == CDRESULT_COLLISION) {
			f32 xdiff;
			f32 zdiff;
			f32 tmp;

#if VERSION >= VERSION_JPN_FINAL
			cdGetPos(pos, 18731, "chr/chraction.c");
#elif VERSION >= VERSION_PAL_FINAL
			cdGetPos(pos, 18694, "chr/chraction.c");
#elif VERSION >= VERSION_PAL_BETA
			cdGetPos(pos, 18689, "chraction.c");
#elif VERSION >= VERSION_NTSC_1_0
			cdGetPos(pos, 18686, "chraction.c");
#else
			cdGetPos(pos, 18416, "chraction.c");
#endif

			xdiff = pos->x - chrpos.x;
			zdiff = pos->z - chrpos.z;
			tmp = sqrtf(xdiff * xdiff + zdiff * zdiff);
			scale = (tmp - 50.0f) / tmp;

			if (scale < 0) {
				pos->x = chrpos.x;
				pos->y = chrpos.y;
				pos->z = chrpos.z;
			} else {
				xdiff *= scale;
				zdiff *= scale;

				pos->x = chrpos.x + xdiff;
				pos->y = chrpos.y;
				pos->z = chrpos.z + zdiff;
			}
		}

		if (arg3) {
			xdiff = chrpos.x - pos->x;
			ydiff = chrpos.y - pos->y;
			zdiff = chrpos.z - pos->z;

			sqdist = xdiff * xdiff + ydiff * ydiff + zdiff * zdiff;

			saved.x = pos->x;
			saved.y = pos->y;
			saved.z = pos->z;

			angle360 = 360 - angle360;
			again = true;
			arg3 = 0;
		} else {
			again = false;
		}
	} while (again);

	if (sqdist != 0) {
		f32 sqdist2;
		xdiff = chrpos.x - pos->x;
		ydiff = chrpos.y - pos->y;
		zdiff = chrpos.z - pos->z;

		sqdist2 = xdiff * xdiff + ydiff * ydiff + zdiff * zdiff;

		if (sqdist2 < sqdist) {
			pos->x = saved.f[0];
			pos->y = saved.f[1];
			pos->z = saved.f[2];
		}
	}

	chrGoToPos(chr, pos, arg4);

	return true;
}

/**
 * Chrs are assigned to teams, and they can be assigned to more than one.
 * The team assignments determine if a chr considers another chr to be friendly
 * or not.
 *
 * The chr->team value is a bitmask of which teams they belong to. There are 8
 * teams total.
 *
 * The team list is an array of 264 shorts. The first 7 are indexes into
 * the same list which mark the start of each team. Team 0 does not have
 * an entry in this list because it always starts at offset 7.
 *
 * Elements 7 onwards are chrnums. Each team is terminated with -2.
 */
void rebuildTeams(void)
{
	s32 numchrs = chrsGetNumSlots();
	s16 index = 7;
	s32 team;
	s32 i;
	struct chrdata *chr;
	u8 teammasks[] = { 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80 };

	for (team = 0; team < ARRAYCOUNT(teammasks); team++) {
		if (team != 0) {
			g_TeamList[team - 1] = index;
		}

		for (i = 0; i < numchrs; i++) {
			chr = &g_ChrSlots[i];

			if (chr->chrnum >= 0 && (chr->team & teammasks[team])) {
				g_TeamList[index] = chr->chrnum;
				index++;
			}
		}

		g_TeamList[index] = -2;
		index++;

		if (index >= (MAX_CHRSPERTEAM + 1) * MAX_TEAMS) {
			break;
		}
	}
}

/**
 * Chrs are partitioned into squadrons for AI scripting purposes, where their
 * squadron number is in the range 0-15.
 *
 * The squadron list is an array of 272 shorts. The first 15 are indexes into
 * the same list which mark the start of each squadron. Squadron 0 does not have
 * an entry in this list because it always starts at offset 15.
 *
 * Elements 15 onwards are chrnums. Each squadron is terminated with -2.
 */
void rebuildSquadrons(void)
{
	s32 numchrs = chrsGetNumSlots();
	s16 index = 15;
	s32 squadron;
	s32 i;

	for (squadron = 0; squadron < MAX_SQUADRONS; squadron++) {
		if (squadron != 0) {
			g_SquadronList[squadron - 1] = index;
		}

		for (i = 0; i < numchrs; i++) {
			struct chrdata *chr = &g_ChrSlots[i];

			if (chr->chrnum >= 0 && chr->squadron == squadron) {
				if (chr->prop == NULL || chr->prop->type != PROPTYPE_PLAYER) {
					g_SquadronList[index] = chr->chrnum;
					index++;
				}
			}
		}

		g_SquadronList[index] = -2;
		index++;

		if (index >= (MAX_CHRSPERSQUADRON + 1) * MAX_SQUADRONS) {
			break;
		}
	}
}

s16 *teamGetChrIds(s32 team_id)
{
	s32 i;
	u8 lookup[8] = {0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80};

	for (i = 0; i < MAX_TEAMS; i++) {
		if (lookup[i] == team_id) {
			team_id = i;
			break;
		}
	}

	if (team_id < 0 || team_id >= MAX_TEAMS) {
		return NULL;
	}

	if (team_id != 0) {
		return &g_TeamList[g_TeamList[team_id - 1]];
	}

	return &g_TeamList[MAX_TEAMS - 1];
}

s16 *squadronGetChrIds(s32 squadron_id)
{
	if (squadron_id < 0 || squadron_id >= MAX_SQUADRONS) {
		return NULL;
	}

	if (squadron_id != 0) {
		return &g_SquadronList[g_SquadronList[squadron_id - 1]];
	}

	return &g_SquadronList[MAX_SQUADRONS - 1];
}

void audioMarkAsRecentlyPlayed(s16 audioid)
{
	g_RecentQuipsPlayed[g_RecentQuipsIndex++] = audioid;

	if (g_RecentQuipsIndex >= ARRAYCOUNT(g_RecentQuipsPlayed)) {
		g_RecentQuipsIndex = 0;
	}
}

bool audioWasNotPlayedRecently(s16 audioid)
{
	u8 i;

	for (i = 0; i < ARRAYCOUNT(g_RecentQuipsPlayed); i++) {
		if (g_RecentQuipsPlayed[i] == audioid) {
			return false;
		}
	}

	return true;
}

#if VERSION >= VERSION_NTSC_1_0
Gfx *chrsRenderChrStats(Gfx *gdl, RoomNum *rooms)
{
	return gdl;
}
#else
Gfx *chrsRenderChrStats(Gfx *gdl, RoomNum *rooms)
{
	s32 x;
	s32 y;
	s32 textwidth = 0;
	s32 textheight = 0;
	s32 x2;
	s32 y2;
	struct coord sp20c;
	f32 sp204[2];
	char fullbuffer[256];
	char aibotbuffer[120];
	u8 aibot = 0;
	s32 i;
	s32 numchrs = chrsGetNumSlots();

	gdl = text0f153628(gdl);

	for (i = 0; i < numchrs; i++) {
		struct chrdata *chr = &g_ChrSlots[i];

		if (chr && chr->prop && arrayIntersects(chr->prop->rooms, rooms)) {
			sp20c.x = chr->prop->pos.x;
			sp20c.y = chr->ground + chr->height - 30;
			sp20c.z = chr->prop->pos.z;

			mtx4TransformVecInPlace(g_Vars.currentplayer->worldtoscreenmtx, &sp20c);

			if (sp20c.z < -100 && sp20c.z > -1000) {
				cam0f0b4eb8(&sp20c, sp204, g_Vars.currentplayer->c_perspfovy, g_Vars.currentplayer->c_perspaspect);
				x = sp204[0];
				y = sp204[1];

				if (chr->aibot) {
					if (g_MpSetup.options & MPOPTION_TEAMSENABLED) {
						aibot = 1;
						sprintf(aibotbuffer, "\nTEAM %d: Cmd: %s", chr->team, botGetCommandName(chr->aibot->command));
					}
				}

				sprintf(fullbuffer, "CHR[%d]\n%s\n%s\n%s%s", chr->chrnum,
						chr->myaction >= ARRAYCOUNT(g_ChrMyActionNames) ? "myaction?" : g_ChrMyActionNames[chr->myaction],
						chr->actiontype >= ARRAYCOUNT(g_ChrActionNames) || chr->actiontype < 0 ? "ACT_?" : g_ChrActionNames2[chr->actiontype],
						chr->actiontype == ACT_GOPOS || chr->actiontype == ACT_PATROL ? g_ChrLiftActionNames[chr->liftaction] : g_ChrLiftActionNames[0],
						aibot ? aibotbuffer : "");

				textMeasure(&textheight, &textwidth, fullbuffer, g_CharsHandelGothicXs, g_FontHandelGothicXs, 0);

				x2 = x + textwidth;
				y2 = y + textheight;

				gdl = text0f153858(gdl, &x, &y, &x2, &y2);
				gdl = textRender(gdl, &x, &y, fullbuffer,
						g_CharsHandelGothicXs, g_FontHandelGothicXs,
						0xff8800aa, 0x00000088, viGetWidth(), viGetHeight(), 0, 0);
			}
		}
	}

	gdl = text0f153780(gdl);

	return gdl;
}
#endif

void chrToggleModelPart(struct chrdata *chr, s32 partnum)
{
	if (chr && chr->model && chr->model->definition) {
		struct modelnode *node = modelGetPart(chr->model->definition, partnum);
		union modelrwdata *rwdata = NULL;

		if (node) {
			rwdata = modelGetNodeRwData(chr->model, node);
		}

		if (rwdata) {
			bool visible = rwdata->toggle.visible;
			rwdata->toggle.visible = !visible;
		}
	}
}

/**
 * Make the chr do an avoiding animation, as if they are avoiding some object
 * being thrown at them.
 *
 * The chosen animation is based on the chr's relative angle to their target.
 *
 * Collision checks are done, and no animation will be done if there would be a
 * collision.
 */
void chrAvoid(struct chrdata *chr)
{
	// The first 4 items here are animation numbers
	// and the second 4 are their corresponding end frames.
	s32 anims[] = {
		ANIM_0064, ANIM_0065, ANIM_0066, ANIM_0067,
		48, 58, 35, 35
	};

	s32 animindex;
	struct prop *target = chrGetTargetProp(chr);
	f32 relangle = chrGetAngleToPos(chr, &target->pos) / M_BADTAU * 360;
	u32 chranimflags = 0;
	f32 ymax;
	f32 ymin;
	f32 radius;
	s32 cdresult = CDRESULT_NOCOLLISION;
	f32 xdiff;
	f32 zdiff;
	f32 halfchrradius;
	f32 chrangle = modelGetChrRotY(chr->model);
	RoomNum dstrooms[8];
	struct coord dstpos;

	// @bug: This shouldn't be here, and the perim is not enabled again
	// if the chr is not ready for orders.
	chrSetPerimEnabled(chr, false);

	if (chrIsReadyForOrders(chr)) {
		chrSetPerimEnabled(chr, false);

		if (relangle > 45 && relangle <= 135) {
			animindex = 3;
			chranimflags = CHRANIMFLAG_FLIP;
			chrangle += M_PI;
		} else if (relangle > 135 && relangle < 225) {
			animindex = 2;

			if ((rngRandom() % 255) >= 2) {
				cdresult = CDRESULT_COLLISION;
				chr->chrflags &= ~CHRCFLAG_AVOIDING;
			}
		} else if (relangle > 225 && relangle < 315) {
			animindex = 3;
			chrangle += M_PI;
		} else {
			animindex = (rngRandom() % 100 < 50) ? 0 : 1;

			if (animindex == 0) {
				chrangle -= 1.5707963705063f;
			} else {
				chrangle += 1.5707963705063f;
			}
		}

		if (cdresult != CDRESULT_COLLISION) {
			dstpos.x = chr->prop->pos.x - sinf(chrangle) * 100;
			dstpos.y = chr->prop->pos.y;
			dstpos.z = chr->prop->pos.z + cosf(chrangle) * 100;

			chrGetBbox(chr->prop, &radius, &ymax, &ymin);

			halfchrradius = radius * 0.5f;

			func0f065e74(&chr->prop->pos, chr->prop->rooms, &dstpos, dstrooms);
			chr0f021fa8(chr, &dstpos, dstrooms);

			xdiff = dstpos.x - chr->prop->pos.x;
			zdiff = dstpos.z - chr->prop->pos.z;

			if (xdiff > halfchrradius || zdiff > halfchrradius || xdiff < -halfchrradius || zdiff < -halfchrradius) {
				cdresult = cdExamCylMove05(&chr->prop->pos, chr->prop->rooms, &dstpos, dstrooms, CDTYPE_ALL, true, ymax - chr->prop->pos.y, ymin - chr->prop->pos.y);
			}

			if (cdresult == CDRESULT_ERROR) {
				chr->chrflags &= ~CHRCFLAG_AVOIDING;
			} else if (cdresult == CDRESULT_NOCOLLISION) {
				chrStartAnim(chr, anims[animindex], 0, anims[4 + animindex], chranimflags, 2, 0.6f);
			} else {
				chr->chrflags &= ~CHRCFLAG_AVOIDING;
			}
		}

		chrSetPerimEnabled(chr, true);
	}
}

/**
 * Checks if the chr is doing an avoiding animation.
 *
 * These animations are possibly unused. In each one, the chr jumps backwards or
 * to the side as if avoiding something, then looks at whatever it was that just
 * went past.
 */
bool chrIsAvoiding(struct chrdata *chr)
{
	s32 anim = modelGetAnimNum(chr->model);
	chr->chrflags &= ~CHRCFLAG_AVOIDING;

	// Possible @bug or just sloppy code: The flag check below can never pass
	// because that flag was just turned off above.
	if (anim == ANIM_0064
			|| anim == ANIM_0065
			|| anim == ANIM_0066
			|| anim == ANIM_0067
			|| (chr->chrflags & CHRCFLAG_AVOIDING)) {
		return true;
	}

	return false;
}

void chrDrCarollEmitSparks(struct chrdata *chr)
{
	if (chr && chr->prop) {
		psCreate(0, chr->prop, SFX_SHIELD_DAMAGE, -1, -1, 0, 0, PSTYPE_NONE, 0, -1, 0, -1, -1, -1, -1);
		sparksCreate(chr->prop->rooms[0], chr->prop, &chr->prop->pos, NULL, 0, SPARKTYPE_ELECTRICAL);
	}
}
