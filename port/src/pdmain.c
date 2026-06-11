#include <stdlib.h>

#include <ultra64.h>
#include <PR/ultrasched.h>
#include "lib/sched.h"
#include "lib/vars.h"
#include "constants.h"
#include "game/camdraw.h"
#include "game/cheats.h"
#include "game/luaai.h"
#include "game/debug.h"
#include "game/file.h"
#include "game/lang.h"
#include "game/race.h"
#include "game/body.h"
#include "game/stubs/game_000840.h"
#include "game/stubs/game_000850.h"
#include "game/stubs/game_000860.h"
#include "game/stubs/game_000870.h"
#include "game/smoke.h"
#include "game/stubs/game_0008e0.h"
#include "game/stubs/game_0008f0.h"
#include "game/stubs/game_000900.h"
#include "game/stubs/game_000910.h"
#include "game/tex.h"
#include "game/stubs/game_00b180.h"
#include "game/stubs/game_00b200.h"
#include "game/challenge.h"
#include "game/title.h"
#include "game/pdmode.h"
#include "game/objectives.h"
#include "game/endscreen.h"
#include "game/playermgr.h"
#include "game/player.h"
#include "game/game_1531a0.h"
#include "game/gfxmemory.h"
#include "game/lang.h"
#include "game/lv.h"
#include "game/timing.h"
#include "game/music.h"
#include "game/stubs/game_175f50.h"
#include "game/game_175f90.h"
#include "game/zbuf.h"
#include "game/game_1a78b0.h"
#include "game/mplayer/mplayer.h"
#include "game/pak.h"
#include "game/splat.h"
#include "game/utils.h"
#include "bss.h"
#include "lib/audiomgr.h"
#include "lib/args.h"
#include "lib/boot.h"
#include "lib/vm.h"
#include "lib/rzip.h"
#include "lib/vi.h"
#include "lib/fault.h"
#include "lib/crash.h"
#include "lib/dma.h"
#include "lib/joy.h"
#include "lib/main.h"
#include "lib/snd.h"
#include "lib/memp.h"
#include "lib/mema.h"
#include "lib/model.h"
#include "lib/profile.h"
#include "lib/videbug.h"
#include "lib/debughud.h"
#include "lib/anim.h"
#include "lib/rdp.h"
#include "lib/lib_34d0.h"
#include "lib/lib_2f490.h"
#include "lib/rmon.h"
#include "lib/rng.h"
#include "lib/str.h"
#include "data.h"
#include "types.h"
#include "system.h"
#include "console.h"
#include "net/net.h"
#include "spectator.h"
#include "net/netmsg.h"
#include "headless.h"
#include "game/prop.h"
#include "game/mplayer/scenarios.h"
#include "game/bg.h"
#include "game/bondgun.h"
#include "lib/mtx.h"
#include "video.h"
#include "input.h"

// bg.c global (not in bg.h): the room bgTickPortals starts its portal walk
// from. bgTick normally copies currentplayer->cam_room into it; the headless
// Tier 2 visibility pass calls bgTickPortals directly (skipping bgTickRooms'
// room-graphics load/unload work, which is render-tier) so it sets this
// itself per combatant.
extern s32 g_CamRoom;

extern u8 *g_MempHeap;
extern u32 g_MempHeapSize;
extern bool gfx_external_textures_enabled;

void rngSetSeed(u32 seed);
void rngCosmeticSetSeed(u64 seed); // cosmetic RNG stream (rngcosmetic_c.c)

bool var8005d9b0 = false;
s32 g_StageNum = STAGE_TITLE;
u32 g_MainMemaHeapSize = 1024 * 300;
bool var8005d9bc = false;
s32 var8005d9c0 = 0;
s32 var8005d9c4 = 0;
bool g_MainGameLogicEnabled = true;
u32 g_MainNumGfxTasks = 0;
bool g_MainIsEndscreen = false;
s32 g_DoBootPakMenu = 0;

u32 var8005dd3c = 0x00000000;
u32 var8005dd40 = 0x00000000;
u32 var8005dd44 = 0x00000000;
u32 var8005dd48 = 0x00000000;
u32 var8005dd4c = 0x00000000;
u32 var8005dd50 = 0x00000000;
s32 g_MainChangeToStageNum = -1;
bool g_MainIsDebugMenuOpen = false;

struct stageallocation g_StageAllocations8Mb[] = {
	{ STAGE_CITRAINING,    "-ml0 -me0 -mgfx120 -mvtx98 -ma400"             },
	{ STAGE_DEFECTION,     "-ml0 -me0 -mgfx110 -mgfxtra80 -mvtx100 -ma700" },
	{ STAGE_INVESTIGATION, "-ml0 -me0 -mgfx110 -mgfxtra80 -mvtx100 -ma700" },
	{ STAGE_EXTRACTION,    "-ml0 -me0 -mgfx110 -mgfxtra80 -mvtx100 -ma700" },
	{ STAGE_CHICAGO,       "-ml0 -me0 -mgfx110 -mgfxtra80 -mvtx100 -ma700" },
	{ STAGE_G5BUILDING,    "-ml0 -me0 -mgfx110 -mgfxtra80 -mvtx100 -ma700" },
	{ STAGE_VILLA,         "-ml0 -me0 -mgfx110 -mgfxtra80 -mvtx100 -ma600" },
	{ STAGE_INFILTRATION,  "-ml0 -me0 -mgfx110 -mgfxtra80 -mvtx100 -ma500" },
	{ STAGE_RESCUE,        "-ml0 -me0 -mgfx110 -mgfxtra80 -mvtx100 -ma500" },
	{ STAGE_ESCAPE,        "-ml0 -me0 -mgfx110 -mgfxtra80 -mvtx100 -ma500" },
	{ STAGE_AIRBASE,       "-ml0 -me0 -mgfx110 -mgfxtra80 -mvtx100 -ma700" },
	{ STAGE_AIRFORCEONE,   "-ml0 -me0 -mgfx110 -mgfxtra80 -mvtx100 -ma700" },
	{ STAGE_CRASHSITE,     "-ml0 -me0 -mgfx110 -mgfxtra80 -mvtx100 -ma700" },
	{ STAGE_PELAGIC,       "-ml0 -me0 -mgfx110 -mgfxtra80 -mvtx100 -ma700" },
	{ STAGE_DEEPSEA,       "-ml0 -me0 -mgfx110 -mgfxtra80 -mvtx100 -ma700" },
	{ STAGE_DEFENSE,       "-ml0 -me0 -mgfx110 -mgfxtra80 -mvtx100 -ma700" },
	{ STAGE_ATTACKSHIP,    "-ml0 -me0 -mgfx110 -mgfxtra80 -mvtx100 -ma700" },
	{ STAGE_SKEDARRUINS,   "-ml0 -me0 -mgfx110 -mgfxtra80 -mvtx100 -ma700" },
	{ STAGE_MP_SKEDAR,     "-ml0 -me0 -mgfx200 -mvtx200 -ma400"            },
	{ STAGE_MP_RAVINE,     "-ml0 -me0 -mgfx200 -mvtx200 -ma400"            },
	{ STAGE_MP_PIPES,      "-ml0 -me0 -mgfx200 -mvtx200 -ma400"            },
	{ STAGE_MP_G5BUILDING, "-ml0 -me0 -mgfx200 -mvtx200 -ma400"            },
	{ STAGE_MP_SEWERS,     "-ml0 -me0 -mgfx200 -mvtx200 -ma400"            },
	{ STAGE_MP_WAREHOUSE,  "-ml0 -me0 -mgfx200 -mvtx200 -ma400"            },
	{ STAGE_MP_BASE,       "-ml0 -me0 -mgfx200 -mvtx200 -ma400"            },
	{ STAGE_MP_COMPLEX,    "-ml0 -me0 -mgfx200 -mvtx200 -ma400"            },
	{ STAGE_MP_TEMPLE,     "-ml0 -me0 -mgfx200 -mvtx200 -ma400"            },
	{ STAGE_MP_FELICITY,   "-ml0 -me0 -mgfx200 -mvtx200 -ma400"            },
	{ STAGE_MP_AREA52,     "-ml0 -me0 -mgfx200 -mvtx200 -ma400"            },
	{ STAGE_MP_GRID,       "-ml0 -me0 -mgfx200 -mvtx200 -ma400"            },
	{ STAGE_MP_CARPARK,    "-ml0 -me0 -mgfx200 -mvtx200 -ma400"            },
	{ STAGE_MP_RUINS,      "-ml0 -me0 -mgfx200 -mvtx200 -ma400"            },
	{ STAGE_MP_FORTRESS,   "-ml0 -me0 -mgfx200 -mvtx200 -ma400"            },
	{ STAGE_MP_VILLA,      "-ml0 -me0 -mgfx200 -mvtx200 -ma400"            },
	{ STAGE_TEST_RUN,      "-ml0 -me0 -mgfx200 -mvtx200 -ma400"            },
	{ STAGE_TEST_MP2,      "-ml0 -me0 -mgfx200 -mvtx200 -ma400"            },
	{ STAGE_TEST_MP6,      "-ml0 -me0 -mgfx200 -mvtx200 -ma400"            },
	{ STAGE_TEST_MP7,      "-ml0 -me0 -mgfx200 -mvtx200 -ma400"            },
	{ STAGE_TEST_MP8,      "-ml0 -me0 -mgfx200 -mvtx200 -ma400"            },
	{ STAGE_TEST_MP14,     "-ml0 -me0 -mgfx200 -mvtx200 -ma400"            },
	{ STAGE_TEST_MP16,     "-ml0 -me0 -mgfx200 -mvtx200 -ma400"            },
	{ STAGE_TEST_MP17,     "-ml0 -me0 -mgfx200 -mvtx200 -ma400"            },
	{ STAGE_TEST_MP18,     "-ml0 -me0 -mgfx200 -mvtx200 -ma400"            },
	{ STAGE_TEST_MP19,     "-ml0 -me0 -mgfx200 -mvtx200 -ma400"            },
	{ STAGE_TEST_MP20,     "-ml0 -me0 -mgfx200 -mvtx200 -ma400"            },
	{ STAGE_TEST_ASH,      "-ml0 -me0 -mgfx120 -mvtx98 -ma400"             },
	{ STAGE_28,            "-ml0 -me0 -mgfx120 -mvtx98 -ma400"             },
	{ STAGE_MBR,           "-ml0 -me0 -mgfx120 -mvtx100 -ma700"            },
	{ STAGE_TEST_SILO,     "-ml0 -me0 -mgfx200 -mvtx200 -ma400"            },
	{ STAGE_24,            "-ml0 -me0 -mgfx120 -mvtx98 -ma400"             },
	{ STAGE_MAIANSOS,      "-ml0 -me0 -mgfx120 -mvtx100 -ma500"            },
	{ STAGE_RETAKING,      "-ml0 -me0 -mgfx120 -mvtx98 -ma400"             },
	{ STAGE_TEST_DEST,     "-ml0 -me0 -mgfx120 -mvtx98 -ma400"             },
	{ STAGE_2B,            "-ml0 -me0 -mgfx120 -mvtx98 -ma400"             },
	{ STAGE_WAR,           "-ml0 -me0 -mgfx120 -mvtx98 -ma400"             },
	{ STAGE_TEST_UFF,      "-ml0 -me0 -mgfx120 -mvtx98 -ma400"             },
	{ STAGE_TEST_OLD,      "-ml0 -me0 -mgfx120 -mvtx98 -ma400"             },
	{ STAGE_DUEL,          "-ml0 -me0 -mgfx120 -mvtx100 -ma700"            },
	{ STAGE_TEST_LAM,      "-ml0 -me0 -mgfx120 -mvtx98 -ma400"             },
	{ STAGE_TEST_ARCH,     "-ml0 -me0 -mgfx200 -mvtx200 -ma400"            },
	{ STAGE_TEST_LEN,      "-ml0 -me0 -mgfx120 -mvtx98 -ma300"             },
	{ STAGE_TITLE,         "-ml0 -me0 -mgfx80 -mvtx20 -ma001"              },
#ifndef PLATFORM_N64
	// GoldenEye X Mod
	{ STAGE_EXTRA1,        "-ml0 -me0 -mgfx200 -mvtx200 -ma400"            }, // Frigate
	{ STAGE_EXTRA2,        "-ml0 -me0 -mgfx200 -mvtx200 -ma400"            }, // Complex
	{ STAGE_EXTRA3,        "-ml0 -me0 -mgfx200 -mvtx200 -ma400"            }, // Train
	{ STAGE_EXTRA4,        "-ml0 -me0 -mgfx200 -mvtx200 -ma400"            }, // Archives
	{ STAGE_EXTRA5,        "-ml0 -me0 -mgfx200 -mvtx200 -ma400"            }, // Aztec
	{ STAGE_EXTRA6,        "-ml0 -me0 -mgfx200 -mvtx200 -ma400"            }, // Tample
	{ STAGE_EXTRA7,        "-ml0 -me0 -mgfx200 -mvtx200 -ma400"            }, // Icicle Pyramid
	{ STAGE_EXTRA8,        "-ml0 -me0 -mgfx200 -mvtx200 -ma400"            }, // Caves
	{ STAGE_EXTRA9,        "-ml0 -me0 -mgfx200 -mvtx200 -ma400"            }, // Library
	{ STAGE_EXTRA10,       "-ml0 -me0 -mgfx200 -mvtx200 -ma400"            }, // Facility
	{ STAGE_EXTRA11,       "-ml0 -me0 -mgfx200 -mvtx200 -ma400"            }, // Bunker
	{ STAGE_EXTRA12,       "-ml0 -me0 -mgfx200 -mvtx200 -ma400"            }, // Caverns
	{ STAGE_EXTRA13,       "-ml0 -me0 -mgfx200 -mvtx200 -ma400"            }, // Basement
	{ STAGE_EXTRA14,       "-ml0 -me0 -mgfx200 -mvtx200 -ma400"            }, // Egyptian
	{ STAGE_EXTRA15,       "-ml0 -me0 -mgfx200 -mvtx200 -ma400"            }, // Stack
	{ STAGE_EXTRA16,       "-ml0 -me0 -mgfx110 -mgfxtra80 -mvtx100 -ma700" }, // Runway
	{ STAGE_EXTRA17,       "-ml0 -me0 -mgfx110 -mgfxtra80 -mvtx100 -ma700" }, // Control
	// Kakariko Village Mod
	{ STAGE_EXTRA18,       "-ml0 -me0 -mgfx200 -mvtx200 -ma400"            }, // Tawfret Ruins
	{ STAGE_EXTRA19,       "-ml0 -me0 -mgfx200 -mvtx200 -ma400"            }, // Targitzan's Temple
	// Goldfinger 64 Mod
	{ STAGE_EXTRA20,       "-ml0 -me0 -mgfx200 -mvtx200 -ma400"            }, // Junkyard
	{ STAGE_EXTRA21,       "-ml0 -me0 -mgfx200 -mvtx200 -ma400"            }, // Steel Mill
	{ STAGE_EXTRA22,       "-ml0 -me0 -mgfx200 -mvtx200 -ma400"            }, // Mall
	{ STAGE_EXTRA23,       "-ml0 -me0 -mgfx200 -mvtx200 -ma400"            }, // Tunnels
	// Additional
	{ STAGE_EXTRA24,       "-ml0 -me0 -mgfx200 -mvtx200 -ma400"            }, // Rogue
	{ STAGE_EXTRA25,       "-ml0 -me0 -mgfx120 -mvtx200 -ma400"            }, // Paradox
	{ STAGE_EXTRA26,       "-ml0 -me0 -mgfx200 -mvtx200 -ma400"            }, // War Colors
#endif
	{ 0,                   "-ml0 -me0 -mgfx120 -mvtx98 -ma300"             },
};

struct stageallocation g_StageAllocations4Mb[] = {
	{ STAGE_MP_SKEDAR,     "-ml0 -me0 -mgfx96 -mvtx96 -ma140"              },
	{ STAGE_MP_PIPES,      "-ml0 -me0 -mgfx96 -mvtx96 -ma140"              },
	{ STAGE_MP_AREA52,     "-ml0 -me0 -mgfx96 -mvtx96 -ma140"              },
	{ STAGE_MP_RAVINE,     "-ml0 -me0 -mgfx96 -mvtx96 -ma140"              },
	{ STAGE_MP_G5BUILDING, "-ml0 -me0 -mgfx96 -mvtx96 -ma140"              },
	{ STAGE_MP_SEWERS,     "-ml0 -me0 -mgfx96 -mvtx96 -ma140"              },
	{ STAGE_MP_WAREHOUSE,  "-ml0 -me0 -mgfx96 -mvtx96 -ma140"              },
	{ STAGE_MP_BASE,       "-ml0 -me0 -mgfx96 -mvtx96 -ma140"              },
	{ STAGE_MP_COMPLEX,    "-ml0 -me0 -mgfx96 -mvtx96 -ma140"              },
	{ STAGE_MP_TEMPLE,     "-ml0 -me0 -mgfx96 -mvtx96 -ma140"              },
	{ STAGE_MP_FELICITY,   "-ml0 -me0 -mgfx96 -mvtx96 -ma140"              },
	{ STAGE_MP_GRID,       "-ml0 -me0 -mgfx96 -mvtx96 -ma140"              },
	{ STAGE_TEST_RUN,      "-ml0 -me0 -mgfx96 -mvtx96 -ma140"              },
	{ STAGE_MP_CARPARK,    "-ml0 -me0 -mgfx96 -mvtx96 -ma140"              },
	{ STAGE_MP_RUINS,      "-ml0 -me0 -mgfx96 -mvtx96 -ma140"              },
	{ STAGE_MP_FORTRESS,   "-ml0 -me0 -mgfx96 -mvtx96 -ma130"              },
	{ STAGE_MP_VILLA,      "-ml0 -me0 -mgfx96 -mvtx96 -ma140"              },
	{ STAGE_TEST_MP2,      "-ml0 -me0 -mgfx96 -mvtx96 -ma115"              },
	{ STAGE_TEST_MP6,      "-ml0 -me0 -mgfx96 -mvtx96 -ma115"              },
	{ STAGE_TEST_MP7,      "-ml0 -me0 -mgfx96 -mvtx96 -ma115"              },
	{ STAGE_TEST_MP8,      "-ml0 -me0 -mgfx96 -mvtx96 -ma115"              },
	{ STAGE_TEST_MP14,     "-ml0 -me0 -mgfx96 -mvtx96 -ma115"              },
	{ STAGE_TEST_MP16,     "-ml0 -me0 -mgfx96 -mvtx96 -ma115"              },
	{ STAGE_TEST_MP17,     "-ml0 -me0 -mgfx96 -mvtx96 -ma115"              },
	{ STAGE_TEST_MP18,     "-ml0 -me0 -mgfx96 -mvtx96 -ma115"              },
	{ STAGE_TEST_MP19,     "-ml0 -me0 -mgfx96 -mvtx96 -ma115"              },
	{ STAGE_TEST_MP20,     "-ml0 -me0 -mgfx96 -mvtx96 -ma115"              },
	{ STAGE_TEST_LEN,      "-ml0 -me0 -mgfx100 -mvtx96 -ma120"             },
	{ STAGE_4MBMENU,       "-mgfx100 -mvtx50 -ma50"                        },
	{ STAGE_TITLE,         "-ml0 -me0 -mgfx80 -mvtx20 -ma001"              },
	{ 0,                   "-ml0 -me0 -mgfx100 -mvtx96 -ma300"             },
};

Gfx var8005dcc8[] = {
	gsSPSegment(0x00, 0x00000000),
	gsSPDisplayList(&var800613a0),
	gsSPDisplayList(&var80061380),
	gsDPFullSync(),
	gsSPEndDisplayList(),
};

s32 g_MainIsBooting = 1;

void mainInit(void)
{
	s32 x;
	s32 i;
	s32 j;
	u32 addr;

	faultInit();
	dmaInit();
	amgrInit();
	varsInit();
	mempInit();
	memaInit();
	joyInit();
	joyReset();

	var8005d9b0 = rmonIsDisabled();

	g_Is4Mb = (osGetMemSize() <= 0x400000);
	g_VmShowStats = 0;

	// no copyright screen
	viSetMode(VIMODE_HI);
	viConfigureForLegal();
	viBlack(true);
	viUpdateMode();

	filesInit();

	if (var8005d9b0) {
		argSetString("          -ml0 -me0 -mgfx100 -mvtx50 -mt700 -ma400");
	}

	mempSetHeap(g_MempHeap, g_MempHeapSize);

	mempResetPool(MEMPOOL_8);
	mempResetPool(MEMPOOL_PERMANENT);
	crashReset();
	challengesInit();
	utilsInit();
	texInit();
	langInit();
	lvInit();
	cheatsInit();
	textInit();
	dhudInit();
	playermgrInit();
	frametimeInit();
	profileInit();
	smokesInit();
	mpInit(true);
	pheadInit();
	paksInit();
	pheadInit2();
	animsInit();
	racesInit();
	bodiesInit();
	titleInit();

	modelSetDistanceChecksDisabled(true); // don't use LODs

	g_MainIsBooting = 0;
}

void mainProc(void)
{
	mainInit();
	rdpInit();
	sndInit();

	while (true) {
		mainLoop();
	}
}

/**
 * It's suspected that this function would have allowed developers to override
 * the value of variables while the game is running in order to view their
 * effects immediately rather than having to recompile the game each time.
 *
 * The developers would have used rmon to create a table of name/value pairs,
 * then this function would have looked up the given variable name in the table
 * and written the new value to the variable's address.
 */
void mainOverrideVariable(char *name, void *value)
{
	// empty
}

/**
 * This function enters an infinite loop which iterates once per stage load.
 * Within this loop is an inner loop which runs very frequently and decides
 * whether to run mainTick on each iteration.
 *
 * NTSC beta checks two shorts at an offset 64MB into the development board
 * and refuses to continue if they are not any of the allowed values.
 * Decomp patches these reads in its build system so it can be played
 * without the development board.
 */
void mainLoop(void)
{
	s32 ending = false;
	s32 index;
	s32 numplayers;
	u32 stack;

	func0f175f98();

	var8005d9c4 = 0;
	argGetLevel(&g_StageNum);

	if (g_DoBootPakMenu) {
		g_Vars.pakstocheck = 0xfd;
		g_StageNum = STAGE_BOOTPAKMENU;
	}

	if (g_StageNum != STAGE_TITLE) {
		titleSetNextStage(g_StageNum);

		if (g_StageNum < STAGE_TITLE) {
			func0f01b148(0);

			if (argFindByPrefix(1, "-hard")) {
				lvSetDifficulty(argFindByPrefix(1, "-hard")[0] - '0');
			}
		}
	}

	if (g_StageNum == STAGE_CITRAINING && IS4MB()) {
		g_StageNum = STAGE_4MBMENU;
	}

	rngSetSeed(osGetCount());
	rngCosmeticSetSeed(osGetCount()); // cosmetic stream: unsynced, may diverge

	// Outer loop - this is infinite because ending is never changed
	while (!ending) {
		g_MainNumGfxTasks = 0;
		g_MainGameLogicEnabled = true;
		g_MainIsEndscreen = false;

		if (var8005d9b0 && var8005d9c4 == 0) {
			index = -1;

			if (IS4MB()) {
				if (g_StageNum < STAGE_TITLE && getNumPlayers() >= 2) {
					index = 0; \
					while (g_StageAllocations4Mb[index].stagenum) { \
						if (g_StageAllocations4Mb[index].stagenum == g_StageNum + 400) { \
							break; \
						} \
						index++;
					}

					if (g_StageAllocations4Mb[index].stagenum == 0) {
						index = -1;
					}
				}

				if (index);

				if (index < 0) {
					index = 0;
					while (g_StageAllocations4Mb[index].stagenum) {
						if (g_StageNum == g_StageAllocations4Mb[index].stagenum) {
							break;
						}

						index++;
					}
				}

				argSetString(g_StageAllocations4Mb[index].string);
			} else {
				// 8MB
				if (g_StageNum < STAGE_TITLE && getNumPlayers() >= 2) {
					index = 0; \
					while (g_StageAllocations8Mb[index].stagenum) { \
						if (g_StageNum + 400 == g_StageAllocations8Mb[index].stagenum) { \
							break; \
						} \
						index++;
					}

					if (g_StageAllocations8Mb[index].stagenum == 0) {
						index = -1;
					}
				}

				if (index < 0) {
					index = 0;

					while (g_StageAllocations8Mb[index].stagenum) {
						if (g_StageNum == g_StageAllocations8Mb[index].stagenum) {
							break;
						}

						index++;
					}
				}

				argSetString(g_StageAllocations8Mb[index].string);
			}
		}

		var8005d9c4 = 0;

		mempResetPool(MEMPOOL_7);
		mempResetPool(MEMPOOL_STAGE);
		filesStop(4);

		if (argFindByPrefix(1, "-ma")) {
			g_MainMemaHeapSize = strtol(argFindByPrefix(1, "-ma"), NULL, 0) * 1024;
			if (g_NetMode && g_NetMaxClients > MAX_LOCAL_PLAYERS) {
				g_MainMemaHeapSize *= MAX_PLAYERS / MAX_LOCAL_PLAYERS;
			}
		}

		memaReset(mempAlloc(g_MainMemaHeapSize, MEMPOOL_STAGE), g_MainMemaHeapSize);
		langReset(g_StageNum);
		playermgrReset();

		if (g_StageNum >= STAGE_TITLE) {
			numplayers = 0;
		} else {
			if (argFindByPrefix(1, "-play")) {
				numplayers = strtol(argFindByPrefix(1, "-play"), NULL, 0);
			} else {
				numplayers = 1;
			}

			if (getNumPlayers() >= 2) {
				numplayers = getNumPlayers();
			}

			// Host spectator mode: allocate enough struct player slots for the
			// remote combatants AND the host's 1-4 panel viewports. Combatants
			// keep the low slot indices [0..combatants-1] so cl->playernum maps
			// straight to g_Vars.players[cl->playernum] (netPlayersAllocate
			// stays unchanged). Panels go at high slot indices
			// [combatants..combatants+panels-1] and spectatorAllocatePanels
			// reorders g_Vars.playerorder so they render first in lvRender's
			// per-player loop. Without this room, a connecting remote has no
			// struct player on the host (cl->player would land on a panel slot
			// and get nulled by the spectator guard) — they'd be invisible /
			// unspectatable in the world. Source of truth is
			// g_NetLocalClient->is_spectator because g_MpSetup is wiped by
			// mpsetupLoadCurrentFile. Excluded on STAGE_CITRAINING (Combat Sim
			// setup menu) because playermgr.c also excludes it from
			// spectatorAllocatePanels.
			if (g_NetMode == NETMODE_SERVER && g_NetLocalClient && g_NetLocalClient->is_spectator
					&& g_StageNum != STAGE_CITRAINING) {
				const s32 combatants = (getNumPlayers() > 0) ? getNumPlayers() : 0;
				s32 panels;
				if (g_NetDedicatedMode) {
					// Dedicated: 0 panels, numplayers = combatants only. No
					// phantom slot to spawn a ghost chr/prop into the world.
					panels = 0;
				} else {
					panels = g_SpectatorPanelCount;
					if (panels < 1) panels = 1;
					if (panels > SPEC_MAX_PANELS) panels = SPEC_MAX_PANELS;
				}
				numplayers = combatants + panels;
				if (numplayers > MAX_PLAYERS) numplayers = MAX_PLAYERS;
			}
		}

		if (numplayers < 2) {
			g_Vars.bondplayernum = 0;
			g_Vars.coopplayernum = -1;
			g_Vars.antiplayernum = -1;
		} else if (argFindByPrefix(1, "-coop")) {
			g_Vars.bondplayernum = 0;
			g_Vars.coopplayernum = 1;
			g_Vars.antiplayernum = -1;
		} else if (argFindByPrefix(1, "-anti")) {
			g_Vars.bondplayernum = 0;
			g_Vars.coopplayernum = -1;
			g_Vars.antiplayernum = 1;
		}

		playermgrAllocatePlayers(numplayers);

		if (argFindByPrefix(1, "-mpbots")) {
			g_Vars.lvmpbotlevel = 1;
		}

		if (g_Vars.coopplayernum >= 0 || g_Vars.antiplayernum >= 0) {
#ifdef PLATFORM_N64
			g_MpSetup.chrslots = 0x03;
#else
			if (g_Vars.antiplayernum < 0) {
				// Counter-Operative now uses a different approach which allows more than 2 players.
				// Co-Operative: one chr slot per player (host + remote partners), up
				// to MAX_PLAYERS. netCoopEnterStage set the player count via
				// setNumPlayers(N), so getNumPlayers()/numplayers == N here; both
				// host and client derive the same N (host: g_NetNumClients; client:
				// the SVC_STAGE_START co-op manifest count), keeping the deterministic
				// spawn-pad allocation in setup.c identical on both ends.
				s32 ncoop = (numplayers >= 2 && numplayers <= MAX_PLAYERS) ? numplayers : 2;
				if (g_MpSetup.chrslots & (MPCHRSLOTS_BOTS_MASK | (MPCHRSLOTS_PLAYERS_MASK & ~0xfu))) {
					g_MpSetup.storedbotbits = g_MpSetup.chrslots & (MPCHRSLOTS_BOTS_MASK | (MPCHRSLOTS_PLAYERS_MASK & ~0xfu));
				}
				g_MpSetup.chrslots = MPCHRSLOT(ncoop) - 1;
			}
#endif
			mpReset();
		} else if (g_Vars.perfectbuddynum) {
			mpReset();
		} else if (g_Vars.mplayerisrunning == false
				&& (numplayers >= 2 || g_Vars.lvmpbotlevel || argFindByPrefix(1, "-play"))) {
#ifndef PLATFORM_N64
			// Spectator host: mpStartMatch already populated chrslots with just
			// the combatant bits (host excluded, remotes assigned slots from 0
			// upward). The numplayers we have here is combatants + panels (the
			// spectator inflation a few lines above), so the unconditional
			// rebuild below would mark every panel slot as a combatant — and
			// that chrslots gets sent to clients in SVC_STAGE_START, causing
			// them to allocate ghost player slots, and locally would make
			// setup.c walk panel slot indices looking for spawn pads. Leave
			// chrslots as mpStartMatch wrote it and only run mpReset.
			if (g_NetMode == NETMODE_SERVER && g_NetLocalClient && g_NetLocalClient->is_spectator
					&& g_StageNum != STAGE_CITRAINING) {
				g_MpSetup.stagenum = g_StageNum;
				mpReset();
			} else
#endif
			{
				g_MpSetup.chrslots = 1;

				for (s32 i = 1; i < numplayers; ++i) {
					g_MpSetup.chrslots |= MPCHRSLOT(i);
				}

				g_MpSetup.stagenum = g_StageNum;
				mpReset();
			}
		}

		// Per-subsystem reset trail. lvReset is the heaviest (loads stage
		// geometry, pads, props, scenarios) and the most likely candidate
		// for a Skedar-specific crash; the others are quick struct resets.
		// Each step gets its own log so we can isolate which one blows up.
		netDiagLogf("ml_init_pre", "stage=%u", (u32)g_StageNum);
		gfxReset();
		joyReset();
		dhudReset();
		zbufReset(g_StageNum);
		netDiagLogf("ml_lvreset_pre", "stage=%u", (u32)g_StageNum);
		lvReset(g_StageNum);
		netDiagLogf("ml_lvreset_post", "stage=%u", (u32)g_StageNum);
		viReset(g_StageNum);
		frametimeCalculate();
		profileReset();
		netDiagLogf("ml_init_post", "stage=%u", (u32)g_StageNum);

		// Outer loop start: stage init has run (memaReset / lvReset etc).
		// Bracket the outer game loop with diag logs so a crash during the
		// FIRST mainTick on a freshly-loaded stage is visible — the crash
		// pattern "stage_start_post then nothing" we keep seeing on Skedar
		// is consistent with the very first render frame on the new stage
		// hitting something un-loaded (Skedar BG model, etc.).
		netDiagLogf("ml_loop_enter", "stage=%u", (u32)g_StageNum);

		{
			// Per-call bracket inside the per-frame loop so we can tell
			// exactly which of schedStartFrame / mainTick / schedEndFrame
			// crashed on the first iteration. Capped at 10 to avoid
			// flooding the diag log past the initial-frame visibility.
			u32 ml_inner_logged = 0;
			while (g_MainChangeToStageNum < 0) {
				const s32 cycles = osGetCount() - g_Vars.thisframestartt;
				if (!g_Vars.mininc60 || (cycles >= g_Vars.mininc60 * CYCLES_PER_FRAME - CYCLES_PER_FRAME / 2)) {
					const bool ml_log = (ml_inner_logged < 10u);
					if (ml_log) { netDiagLogf("ml_sched_start_pre", ""); }
					schedStartFrame(&g_Sched);
					if (ml_log) { netDiagLogf("ml_sched_start_post", ""); }
					if (ml_log) { netDiagLogf("ml_main_tick_pre", ""); }
					mainTick();
					if (ml_log) { netDiagLogf("ml_main_tick_post", ""); }
					if (ml_log) { netDiagLogf("ml_sched_end_pre", ""); }
					schedEndFrame(&g_Sched);
					if (ml_log) { netDiagLogf("ml_sched_end_post", ""); ml_inner_logged++; }
				}
				if (g_TickExtraSleep) {
					sysSleep(EXTRA_SLEEP_TIME);
				}
			}
		}

		// Stage change requested. Trail through cleanup so a crash inside
		// lvStop / memp pool tear-down / file close becomes locatable.
		netDiagLogf("ml_loop_exit", "from=%u to=%d", (u32)g_StageNum, g_MainChangeToStageNum);

		lvStop();
		netDiagLogf("ml_lvstop_done", "");
		mempDisablePool(MEMPOOL_STAGE);
		mempDisablePool(MEMPOOL_7);
		filesStop(4);
		netDiagLogf("ml_cleanup_done", "");
		viBlack(true);
		pak0f116994();

		g_StageNum = g_MainChangeToStageNum;
		g_MainChangeToStageNum = -1;
	}
}

void mainTick(void)
{
	// Crash-hunt: mt_entry0 BEFORE any local variable declarations / function
	// prologue work so a crash in the prologue itself is visible. Placed
	// before the OSScMsg struct init in case that's the trigger. Static
	// counter capped at 10 to keep the diag log readable.
	{
		static u32 entry0_logged = 0;
		if (entry0_logged < 10u) {
			netDiagLogf("mt_entry0", "stagechg=%d gle=%d numbots=%d numchrs=%d",
				g_MainChangeToStageNum, (s32)g_MainGameLogicEnabled,
				(s32)g_BotCount, (s32)g_MpNumChrs);
			entry0_logged++;
		}
	}

	Gfx *gdl = NULL;
	Gfx *gdlstart = NULL;
	OSScMsg msg = {OS_SC_DONE_MSG};
	s32 i;

	// Crash-hunt: mainTick entered. Cap at 10 fires so the diag log stays
	// readable. If we see ml_loop_enter but no mt_entry, the crash is in
	// schedStartFrame / videoStartFrame between the loop entry and here.
	{
		static u32 entry_logged = 0;
		if (entry_logged < 10u) {
			netDiagLogf("mt_entry", "stagechg=%d gle=%d",
				g_MainChangeToStageNum, (s32)g_MainGameLogicEnabled);
			entry_logged++;
		}
	}

	if (g_MainChangeToStageNum < 0) {
		if (inputKeyJustPressed(VK_F2) && inputGetKeyModState() & KM_SHIFT) {
			bool enabled = videoGetExternalTextures();
			videoSetExternalTextures(!enabled);
		}

		frametimeCalculate();
		profileReset();
		profileSetMarker(PROFILE_MAINTICK_START);
		joyDebugJoy();
		schedSetCrashEnable2(false);

		if (g_MainGameLogicEnabled) {
			// Headless dedicated server: skip the renderer setup. gfxGetMasterDisplayList
			// returns into g_GfxBuffers and the gDPSetTile prologue writes there.
			// Without a real renderer that buffer is never shipped to the GPU, but
			// the per-frame gfxAllocate / gfxSwapBuffers reset cycle still needs to
			// run so chr render-prep allocations (matrices) don't overflow the pool.
			const bool headless = (g_NetDedicatedMode == 1);

			if (!headless) {
				gdl = gdlstart = gfxGetMasterDisplayList();

				gDPSetTile(gdl++, G_IM_FMT_RGBA, G_IM_SIZ_16b, 0, 0x0000, G_TX_LOADTILE, 0, G_TX_NOMIRROR | G_TX_WRAP, G_TX_NOMASK, G_TX_NOLOD, G_TX_NOMIRROR | G_TX_WRAP, G_TX_NOMASK, G_TX_NOLOD);
				gDPSetTile(gdl++, G_IM_FMT_RGBA, G_IM_SIZ_4b, 0, 0x0100, 6, 0, G_TX_NOMIRROR | G_TX_WRAP, G_TX_NOMASK, G_TX_NOLOD, G_TX_NOMIRROR | G_TX_WRAP, G_TX_NOMASK, G_TX_NOLOD);
			} else {
				// Get the buffer head so any stray gdl writes have a target,
				// but don't ship it. (Most chr render-prep paths skip cleanly
				// when PLAYERCOUNT() == 0, but the master gdl pointer is read
				// by some helpers via gfxGetMasterDisplayList directly.)
				gdl = gdlstart = gfxGetMasterDisplayList();
			}

			// First-mainTick crash-hunt trail. The sims-in-Skedar crash
			// pattern points at the per-player propsTickPlayer pass: lvTick
			// resets per-frame state, the per-player loop runs lvTickPlayer
			// (physics + handsTickAttack), then lvRender does the actual
			// propsTickPlayer that ticks sim chrs via chrTick on the
			// client (per the bot/chrtick routing in prop.c). One log per
			// subsystem call so the last line tells you which one died.
			// Each one is capped at 10 fires so the diag log doesn't
			// flood after the first match of gameplay.
			static u32 mt_logged = 0;
			const bool mt_log = (mt_logged < 10u);
			if (mt_log) { netDiagLogf("mt_lvtick_pre", ""); }
			lvTick();
			if (mt_log) { netDiagLogf("mt_lvtick_post", ""); }
			playermgrShuffle();
			if (mt_log) { netDiagLogf("mt_shuffle_post", ""); }

			if (g_StageNum < STAGE_TITLE) {
				// Spectator input runs once per frame (not per panel) — it
				// only modifies the active panel's freecam state. Cheap no-op
				// when the host isn't spectating.
				spectatorReadInput();
				// Lua possession (controllable cube) freecam input — once per
				// frame; no-op unless pd.possess_spawn is active.
				luaPossessReadInput();

				for (i = 0; i < PLAYERCOUNT(); i++) {
					setCurrentPlayerNum(playermgrGetPlayerAtOrder(i));

					if (g_StageNum != STAGE_TEST_OLD || !titleIsKeepingMode()) {
						viSetViewPosition(g_Vars.currentplayer->viewleft, g_Vars.currentplayer->viewtop);
						viSetFovAspectAndSize(
								g_Vars.currentplayer->fovy, g_Vars.currentplayer->aspect,
								g_Vars.currentplayer->viewwidth, g_Vars.currentplayer->viewheight);
					}

					if (mt_log) { netDiagLogf("mt_lvtickplayer_pre", "i=%d cp=%d", i, g_Vars.currentplayernum); }
					if (g_Vars.currentplayer && g_Vars.currentplayer->is_spectator) {
						// Spectator panels have no prop / no mpchr — lvTickPlayer
						// would deref prop->pos and crash. spectatorTickPanel
						// runs the minimum needed: cam pose + matrices for
						// lvRender to read this frame.
						spectatorTickPanel(g_Vars.currentplayer->spectator_panel);
					} else if (g_Vars.currentplayer && !g_Vars.currentplayer->client
							&& g_Vars.currentplayer->isremote && g_NetMode == NETMODE_SERVER) {
						// Orphaned *remote* combatant slot after mid-stage disconnect:
						// netClientReset cleared player->client; netPlayersAllocate
						// won't re-bind until the next stage transition. The
						// chr is still in g_Vars.players[] but its bgun /
						// matrices / lookingatprop state hasn't been ticked
						// since disconnect — lvTickPlayer's playerTick chain
						// dereferences chr / prop / aibot links that are in a
						// half-cleaned state and crashes inside the sim AI
						// path (chrIsRoomOffScreen seen via addr2line).
						// Skipping the tick entirely keeps the orphan inert
						// until the next round's playermgrAllocatePlayers
						// re-binds via netPlayersAllocate.
						//
						// The `isremote` guard is essential: the local HOST's own
						// player[0]->client also goes NULL after a round transition
						// (root cause still open — it isn't re-bound for the local
						// client), and without this guard the skip fired on the host
						// too, bypassing playerTick's viewport setup and freezing the
						// host view at the 100x100 init (the "top-left corner" bug).
						// The host is never a half-cleaned orphan, so it must tick.
						if (mt_log) { netDiagLogf("mt_lvtp_skip_orphan", "i=%d", i); }
					} else {
						lvTickPlayer();
					}
					// Possession: after the body ticks, override this player's
					// camera to follow the controllable cube's fly pose. No-op
					// unless possession is active (and only for the local player).
					luaPossessApplyCamera();
					if (mt_log) { netDiagLogf("mt_lvtickplayer_post", "i=%d", i); }
				}
			}

			// Headless: lvRender owns the per-player propsTickPlayer +
			// scenarioTickChr + propsSort calls — gameplay state, not just
			// rendering. With lvRender skipped, sim AI never advances and
			// the server broadcasts stale positions. Run the gameplay-tier
			// subset here for each non-spectator slot before bailing out
			// of the render path. (bgTick / lightsTick / autoaimTick are
			// render-tier and stay skipped.)
			if (headless && g_StageNum < STAGE_TITLE) {
				// CRITICAL: propsTickPlayer's foreground gate at prop.c:2006
				// adds g_Vars.alwaystick to the per-prop score. When non-zero,
				// every active prop is treated as foreground and its tick
				// (objTickPlayer for pickups, botTick for sim AI, chrTick
				// for animation) runs unconditionally. The codebase has
				// dedicated support for this; it's just never set in normal
				// play. In headless we need it on because rendering doesn't
				// populate PROPFLAG_ONANYSCREENPREVTICK — without it most
				// props silently skip their gameplay tick.
				g_Vars.alwaystick = 1;

				s32 lastcombatant = -1;
				for (s32 j = 0; j < PLAYERCOUNT(); j++) {
					struct player *pl = g_Vars.players[playermgrGetPlayerAtOrder(j)];
					if (pl && !pl->is_spectator) {
						lastcombatant = j;
					}
				}
				for (s32 j = 0; j < PLAYERCOUNT(); j++) {
					setCurrentPlayerNum(playermgrGetPlayerAtOrder(j));
					if (g_Vars.currentplayer && g_Vars.currentplayer->is_spectator) {
						continue;
					}
					// Skip orphaned combatant slots. netClientReset nukes
					// player->client on disconnect, and the bidirectional
					// link only gets restored when netPlayersAllocate
					// runs (at the next stage transition). Between a
					// mid-match disconnect and the next round, the chr
					// stays in the world but nothing controls it. Without
					// this skip propsTickPlayer keeps firing pickup
					// detection for the abandoned chr — items get
					// "consumed" server-side but no SVC_PROP_PICKUP goes
					// out (gate at propobj.c requires currentplayer->client),
					// so the items disappear silently from every client's
					// view. Sim AI hits the orphan via chrIsRoomOffScreen
					// and crashes deref'ing the stale chain.
					if (g_Vars.currentplayer && !g_Vars.currentplayer->client) {
						continue;
					}

					// Tier 2: synthetic per-player visibility
					// (docs/PORT_HEADLESS_BLIND_SERVER.md §9). Mirrors lvRender's
					// per-player camera setup (lv.c:1404-1450) minus the
					// framebuffer work. playerTick (tick path) already maintains
					// cam_pos/cam_look/cam_up/cam_room per remote pawn, and
					// bmoveProcessRemoteInput keeps fovy tracking the client's
					// real (zoomed) FOV — so this builds REAL camera matrices:
					// vi0000b1d0 computes the perspective matrix and stashes it
					// via camSetMtxF1754 (must precede playerAllocateMatrices —
					// the spectatorRenderPanel ordering trap), then
					// playerAllocateMatrices sets the world-to-screen /
					// projection matrices the portal flood projects through.
					// bgTickPortals then recomputes ROOMFLAG_ONSCREEN for THIS
					// player and bgChooseRoomsToLoad ORs the slot's bits into
					// g_MpRoomVisibility — restoring spawn-pad visibility
					// avoidance and chrIsRoomOffScreen AI LOD with no wire
					// change and no client trust (pose-derived only). The gdl
					// writes land in the throwaway master display list.
					bool cam_primed = false;
					{
						struct player *pl_vis = g_Vars.currentplayer;
						if (pl_vis && pl_vis->prop && pl_vis->cam_room >= 1
								&& pl_vis->cam_room < g_Vars.roomcount) {
							viSetViewPosition(pl_vis->viewleft, pl_vis->viewtop);
							viSetFovAspectAndSize(pl_vis->fovy, pl_vis->aspect,
									pl_vis->viewwidth, pl_vis->viewheight);
							mtx00016748(g_Vars.currentplayerstats->scale_bg2gfx);
							gdl = vi0000b1d0(gdl);
							playerAllocateMatrices(&pl_vis->cam_pos,
									&pl_vis->cam_look, &pl_vis->cam_up);
							g_CamRoom = pl_vis->cam_room;
							bgTickPortals();
							cam_primed = true;
						}
					}

					propsTickPlayer(j == lastcombatant);
					scenarioTickChr(NULL);
					propsSort();

					// Attack dispatch (PORT_HEADLESS_BLIND_SERVER.md §6.1).
					// handsTickAttack's ONLY caller is lvRender (lv.c:1456) —
					// on a listen host it runs per player including remotes,
					// spawning their thrown/fired projectiles (grenades,
					// rockets, mines: HANDATTACKTYPE_THROW/SHOOTPROJECTILE)
					// server-side, where dynamic prop spawns are host-owned
					// (SVC_PROP_SPAWN). Skipped headless, a client's grenade
					// existed only on the throwing client. Mirroring it here
					// is listen-host parity: the remote-shooter rails already
					// exist on that path — chrHit is skipped for remote
					// shooters (CLC_HIT applies the damage; the server trace
					// only feeds hit validation), melee early-returns in
					// handInflictMeleeDamage (each machine handles its own
					// local melee), and the explosive-shell (Phoenix) trace
					// explosion NEEDS to run here to exist at all. Gated on
					// cam_primed: the trace projects through this player's
					// matrices (objHit derefs camGetProjectionMtxF without a
					// NULL guard), so it must not run on an unprimed slot.
					// Gun-load kick (§6.1 follow-up, the "client shoots nothing"
					// root — 2026-06-11 srvhand diag: memown=2 mls=0 loaded=0
					// forever). bgunLoadAll's ONLY caller is lvRender
					// (lv.c:1424-1429) — render-tier — so headless it never ran,
					// gunctrl.loadall stayed true (bgunReset sets it), and
					// bgunTickGameplay2 skips bgunTickLoad while loadall is set:
					// the master-load never claimed the gunmem from
					// GUNMEMOWNER_CHRBODY, bgunIsLoaded() stayed false, and the
					// fire state machine (bgun0f09bf44 -> HANDSTATE_ATTACK ->
					// hand->firing -> handTickAttack) never ran for remote pawns.
					// Hitscan never noticed (damage rides CLC_HIT), but every
					// projectile weapon (rocket/grenade/mine/laptop) silently
					// no-opped for every client. Mirror the lvRender call here;
					// lvRender's extra gates (var80075d60 debug default, menu bg,
					// third-person/eyespy camera) don't apply to a headless
					// remote pawn.
					if (g_Vars.currentplayer && g_Vars.currentplayer->prop
							&& g_Vars.currentplayer->gunctrl.loadall) {
						g_Vars.currentplayer->gunctrl.loadall = bgunLoadAll();
					}

					// bgunTickGameplay2 is ALSO render-tier — its only caller is
					// playerRenderHud (player.c:5142) — yet it owns the gun-load
					// ticker (bgunTickLoad -> bgunTickMasterLoad: claims the
					// gunmem from GUNMEMOWNER_CHRBODY and advances
					// masterloadstate to LOADED) plus the per-hand housekeeping
					// (bgun0f0a5550: muzzlepos/posmtx updates the projectile
					// spawn position reads). Without it the loadall kick above
					// clears the flag but nothing ever loads — memown stayed 2
					// forever (second srvhand diag run, 2026-06-11 18:55).
					// Run it inside the cam prime so camGetProjectionMtxF is
					// THIS pawn's matrices (bgun0f0a5550 transforms muzzlepos
					// through it); mirror the playerRenderHud camera gates.
					if (cam_primed
							&& g_Vars.currentplayer->cameramode != CAMERAMODE_THIRDPERSON
							&& g_Vars.currentplayer->cameramode != CAMERAMODE_EYESPY) {
						bgunTickGameplay2();
					}

					if (cam_primed) {
						handsTickAttack();
					}

					// projdiag (temporary): name the gate blocking remote-pawn
					// projectile fire on the dedicated server ("client shoots
					// nothing", 2026-06-11). One line per second per remote pawn
					// dumps every gate on the fire chain: trigger -> bgun0f09bf44
					// (bgunIsLoaded: gunmem owner + masterload) -> HANDSTATE_ATTACK
					// -> hand->firing -> handTickAttack -> bgunCreateFiredProjectile.
					{
						struct player *pl_fd = g_Vars.currentplayer;
						static u32 s_fireDiagFrame = 0;
						if (pl_fd && pl_fd->isremote && pl_fd->prop
								&& g_Vars.lvframe60 - s_fireDiagFrame > 60) {
							s_fireDiagFrame = g_Vars.lvframe60;
							struct hand *h0 = &pl_fd->hands[HAND_RIGHT];
							sysLogPrintf(LOG_WARNING,
									"projdiag: srvhand pl=%d cam=%d weap=%d state=%d firing=%d atk=%d loaded=%d memtype=%d memown=%d mls=%d switchto=%d gset=%d/%d",
									g_Vars.currentplayernum, cam_primed,
									pl_fd->gunctrl.weaponnum, h0->state, h0->firing,
									h0->attacktype, bgunIsLoaded(),
									pl_fd->gunctrl.gunmemtype, pl_fd->gunctrl.gunmemowner,
									pl_fd->gunctrl.masterloadstate,
									pl_fd->gunctrl.switchtoweaponnum,
									h0->gset.weaponnum, h0->gset.weaponfunc);
						}
					}

					// Pickup detection. propsTestForPickup is normally called
					// from lvRender's per-player loop (lv.c:1416) — the function
					// iterates props near the current player and routes weapon
					// / ammo / case pickups through objTestForPickup, which then
					// calls propPickupByPlayer + writes SVC_PROP_PICKUP. Without
					// it, players walk over pickups with no effect on the
					// dedicated server.
					propsTestForPickup();

					// Door / lift / pickup-by-activate. lvRender at lv.c:1391
					// calls currentPlayerInteract(false) when the player's
					// activate input bit is set. Mirror that here; the bit is
					// driven by UCMD_ACTIVATE on remote players.
					if (g_Vars.currentplayer
							&& (g_Vars.currentplayer->bondactivateorreload & JO_ACTION_ACTIVATE)) {
						currentPlayerInteract(false);
					}

					// Death-state advancement for headless. The death state
					// machine normally runs inside playerRenderHud (player.c
					// ~4966): isdead 1 -> 2, deathanimfinished, redbloodfinished,
					// colourfadetimemax60 all get advanced as the animation
					// plays out. Without rendering, none of this runs.
					//
					// lvTickPlayer (lv.c:2358) counts any dead player whose
					// flags haven't advanced as "numdying", and lv.c:2402 only
					// fires mainEndStage() when numdying == 0. Result in
					// headless: hit g_MpScoreLimit -> g_NumReasonsToEndMpMatch
					// goes positive -> player dies -> deathanimfinished stays
					// false -> mainEndStage never fires -> match never ends ->
					// next round never starts -> player can't respawn (the
					// respawn gate at player.c:5151 also requires
					// g_NumReasonsToEndMpMatch == 0, which never clears).
					//
					// No anim to wait for in headless, so advance the
					// terminal state directly the moment we see isdead set.
					// On respawn (playerStartNewLife) all three reset to 0/-1
					// via playerResetDefaults, so this won't re-fire.
					{
						struct player *pl_ds = g_Vars.currentplayer;
						if (pl_ds && pl_ds->isremote && pl_ds->isdead) {
							if (pl_ds->isdead == 1) {
								pl_ds->isdead = 2;
							}
							pl_ds->deathanimfinished = true;
							pl_ds->redbloodfinished = true;
							if (pl_ds->colourfadetimemax60 >= 0) {
								pl_ds->colourfadetimemax60 = -1;
							}
						}
					}

					// Respawn handling for headless. Two render-tier functions
					// drive respawn normally: playerRenderHud sets
					// dostartnewlife=true on UCMD_RESPAWN; lvRender's
					// per-player loop reads it and calls playerStartNewLife.
					// Both skipped in headless. Mirror the detect + consume.
					struct player *p = g_Vars.currentplayer;
					// Diagnostic: log every dead remote player's state once a
					// second so we can see why the respawn gate isn't firing.
					// Throttled to keep the log readable.
					if (p && p->isremote && (g_NetTick % 60u) == 0u) {
						const struct netclient *cl_ = p->client;
						const u32 ucmd = cl_ ? cl_->inmove[cl_->inmove_head].ucmd : 0u;
						netDiagLogf("respawn_dead_state",
								"pnum=%d isdead=%d client=%p paused=%d endmatch=%d ucmd=0x%08x dostart=%d",
								g_Vars.currentplayernum,
								(s32)p->isdead, (void *)p->client,
								(s32)mpIsPaused(), g_NumReasonsToEndMpMatch,
								(unsigned)ucmd, (s32)p->dostartnewlife);
					}
					if (p && p->isremote && p->isdead && p->client && !mpIsPaused()
							&& g_NumReasonsToEndMpMatch == 0) {
						const struct netclient *cl_ = p->client;
						if (cl_->inmove[cl_->inmove_head].ucmd & UCMD_RESPAWN) {
							netDiagLogf("respawn_ucmd_seen",
									"pnum=%d cl=%u isdead=%d dostartnewlife=%d",
									g_Vars.currentplayernum, (unsigned)cl_->id,
									(s32)p->isdead, (s32)p->dostartnewlife);
							p->dostartnewlife = true;
						}
					}
					if (p && p->dostartnewlife) {
						netDiagLogf("respawn_invoke", "pnum=%d", g_Vars.currentplayernum);
						playerStartNewLife();
					}
				}
			}

			if (!headless) {
				if (mt_log) { netDiagLogf("mt_lvrender_pre", ""); }
				gdl = lvRender(gdl);
				if (mt_log) {
					netDiagLogf("mt_lvrender_post", "");
					mt_logged++;
				}

				if (debugGetProfileMode() >= 2) {
					gdl = profileRender(gdl);
				}

				gdl = conRender(gdl);
				gdl = luaHudRender(gdl); /* declared in game/luaai.h */
				gdl = netKillFeedRender(gdl);
				gdl = netDebugRender(gdl);

				gDPFullSync(gdl++);
				gSPEndDisplayList(gdl++);
			} else if (mt_log) {
				netDiagLogf("mt_headless_skiprender", "");
				mt_logged++;
			}
		}

		if (g_MainGameLogicEnabled) {
			// gfxSwapBuffers resets the per-frame allocator pool — must run
			// even in headless or g_GfxMemPos grows until it overflows.
			gfxSwapBuffers();
			if (g_NetDedicatedMode != 1) {
				viUpdateMode();
			}
		}

		if (g_NetDedicatedMode != 1) {
			rdpCreateTask(gdlstart, gdl, 0, (uintptr_t) &msg);
		}
		memaPrint();
		profileSetMarker(PROFILE_MAINTICK_END);
	}

	if (g_NetDedicatedMode == 1) {
		// No vsync sleep in headless — pace the loop to 60 Hz so the server
		// doesn't peg a core. g_NetTick advances at 60 Hz inside netStartFrame,
		// so anything faster wastes CPU without helping clients.
		headlessPace(60);
	}
}

void mainEndStage(void)
{
	sndStopNosedive();

	// Stop any active spectate before the stage tears down: the spectated chr
	// and its player slot are freed during this transition, so the spectate
	// redirect (lvRender) and camera (netSpectateApply) must not keep pointing
	// at a dangling chr into the next render. Reached on both host and client
	// for any stage end (round over, "end match" menu, disconnect). No-op when
	// not spectating; also unhides our own body.
	netSpectateStop();

	if (!g_MainIsEndscreen) {
		pak0f11c6d0();
		joyDisableTemporarily();

		if (g_Vars.coopplayernum >= 0) {
			s32 prevplayernum = g_Vars.currentplayernum;
			s32 i;

			for (i = 0; i < LOCALPLAYERCOUNT(); i++) {
				setCurrentPlayerNum(i);
				endscreenPushCoop();
			}

			setCurrentPlayerNum(prevplayernum);
			musicStartMenu();
		} else if (g_Vars.antiplayernum >= 0) {
			s32 prevplayernum = g_Vars.currentplayernum;
			s32 i;

			for (i = 0; i < LOCALPLAYERCOUNT(); i++) {
				setCurrentPlayerNum(i);
				endscreenPushAnti();
			}

			setCurrentPlayerNum(prevplayernum);
			musicStartMenu();
		} else if (g_Vars.normmplayerisrunning) {
			mpEndMatch();
		} else {
			endscreenPrepare();
			musicStartMenu();
		}

		netServerStageEnd();
	}

	g_MainIsEndscreen = true;
}

/**
 * Change to the given stage at the end of the current frame.
 */
void mainChangeToStage(s32 stagenum)
{
	pak0f11c6d0();

	g_MainChangeToStageNum = stagenum;
}

s32 mainGetStageNum(void)
{
	return g_StageNum;
}

void func0000e990(void)
{
	objectivesCheckAll();
	objectivesDisableChecking();

	// Co-op mission end is HOST-AUTHORITATIVE. A CLIENT only NOTIFIES the host and
	// then waits for SVC_STAGE_END — it must NOT run mainEndStage itself. Doing so
	// dumped the client straight to the debrief out of sync with (and ahead of) the
	// host's outro: the client's local sim can reach this choke point at a different
	// time than the host's, so each machine ended independently. Instead the host
	// drives the end — its script plays the outro (synced to the client via
	// SVC_CUTSCENE) and then runs mainEndStage -> netServerStageEnd -> SVC_STAGE_END,
	// which the client receives and ends on. The host falls through to mainEndStage
	// below.
	//
	// BUT only a GENUINE completion may notify: this choke point is also
	// reached by the death/abort fade path (player.c, var8007074c — a button
	// press during the fade-out), so the unconditional notify let a client
	// ABORTING or dying out of the mission end it for the host and everyone
	// else ("client leaves -> host forced to quit"). objectivesCheckAll just
	// ran, so the statuses are fresh: incomplete objectives = the client is
	// bailing, not finishing — leave the session (the host's disconnect path
	// parks our pawn for reclaim) and end the stage locally, which is plain
	// solo semantics once offline.
	if (g_NetMode == NETMODE_CLIENT && g_Vars.coopplayernum >= 0) {
		if (objectiveIsAllComplete()) {
			netClientStageComplete();
			return;
		}
		netDisconnect();
		// fall through: ends OUR stage only (we're offline now)
	}

	mainEndStage();
}
