#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <PR/ultratypes.h>
#include <PR/ultrasched.h>
#include <PR/os_message.h>

#include "lib/main.h"
#include "bss.h"
#include "data.h"

#include "video.h"
#include "audio.h"
#include "input.h"
#include "fs.h"
#include "romdata.h"
#include "config.h"
#include "mod.h"
#include "system.h"
#include "console.h"
#include "utils.h"
#include "net/net.h"

u32 g_OsMemSize = 0;
s32 g_OsMemSizeMb = 16;
u8 g_Is4Mb = 0;
s8 g_Resetting = false;
OSSched g_Sched;

OSMesgQueue g_MainMesgQueue;
OSMesg g_MainMesgBuf[32];

u8 *g_MempHeap = NULL;
u32 g_MempHeapSize = 0;

u32 g_VmNumTlbMisses = 0;
u32 g_VmNumPageMisses = 0;
u32 g_VmNumPageReplaces = 0;
u8 g_VmShowStats = 0;

s32 g_TickRateDiv = 1;
s32 g_TickExtraSleep = true;

s32 g_SkipIntro = false;

s32 g_FileAutoSelect = -1;

// Saved Combat Sim player profile name. Stored in pd.ini so we can look up
// the player's pak file by name when entering Combat Simulator each session.
char g_MpProfileName[MAX_PLAYERNAME] = "";

// Cached head/body from the loaded Combat Sim profile. Refreshed whenever
// a profile is loaded (mpProfileLoadFromPak) or explicitly selected via the
// Combat Simulator "Load Player" dialog (mpProfileSave). Used to swap the
// default Joanna disguise model on the inventory menu with the player's
// chosen character. -1 means "no profile loaded — fall back to defaults".
s32 g_MpProfileHead = -1;
s32 g_MpProfileBody = -1;

extern s32 g_StageNum;

s32 bootGetMemSize(void)
{
	return (s32)g_OsMemSize;
}

void *bootAllocateStack(s32 threadid, s32 size)
{
	static u8 bruh[0x1000];
	return bruh;
}

void bootCreateSched(void)
{
	osCreateMesgQueue(&g_MainMesgQueue, g_MainMesgBuf, ARRAYCOUNT(g_MainMesgBuf));
	if (osTvType == OS_TV_MPAL) {
		osCreateScheduler(&g_Sched, NULL, OS_VI_MPAL_LAN1, 1);
	} else {
		osCreateScheduler(&g_Sched, NULL, OS_VI_NTSC_LAN1, 1);
	}
}

static void gameInit(void)
{
	osMemSize = g_OsMemSizeMb * 1024 * 1024;

	for (s32 i = 0; i < MAX_LOCAL_PLAYERS; ++i) {
		struct extplayerconfig *cfg = g_PlayerExtCfg + i;
		cfg->fovzoommult = cfg->fovzoom ? cfg->fovy / 60.0f : 1.0f;
	}

	if (g_HudCenter == HUDCENTER_NORMAL) {
		g_HudAlignModeL = G_ASPECT_CENTER_EXT;
		g_HudAlignModeR = G_ASPECT_CENTER_EXT;
	} else if (g_HudCenter == HUDCENTER_WIDE) {
		g_HudAlignModeL = G_ASPECT_LEFT_EXT | G_ASPECT_WIDE_EXT;
		g_HudAlignModeR = G_ASPECT_RIGHT_EXT | G_ASPECT_WIDE_EXT;
	}
}

static void cleanup(void)
{
	sysLogPrintf(LOG_NOTE, "shutdown");
	netDisconnect();
	inputSaveBinds();
	configSave(CONFIG_PATH);
	videoShutdown();
	crashShutdown();
	// TODO: actually shut down all subsystems
}

int main(int argc, const char **argv)
{
	sysInitArgs(argc, argv);

	if (!sysArgCheck("--no-crash-handler")) {
		crashInit();
	}

	conInit();
	sysInit();
	fsInit();
	configInit();

	// Parse --dedicated / --dedicated-windowed before videoInit / audioInit so
	// they can skip SDL window + audio device creation. The actual CLI parsing
	// happens in netInit (below), but we peek at the flags directly here since
	// netInit runs after videoInit by design (ENet doesn't need video). The
	// duplication is intentional and cheap — sysArgCheck is a linear arg scan.
	if (sysArgCheck("--dedicated")) {
		g_NetDedicatedMode = 1;
	} else if (sysArgCheck("--dedicated-windowed")) {
		g_NetDedicatedMode = 2;
	}
	if (g_NetDedicatedMode) {
		sysLogPrintf(LOG_NOTE, "starting dedicated server (mode %d: %s)",
				g_NetDedicatedMode,
				g_NetDedicatedMode == 1 ? "headless" : "windowed");
		// Headless mode: silence the audio mixer before audioInit so the
		// per-frame audio path in schedAudioFrame short-circuits. The
		// --no-sound CLI flag normally toggles this *after* audioInit;
		// dedicated needs it before.
		if (g_NetDedicatedMode == 1) {
			g_SndDisabled = true;
		}
	}

	videoInit();
	inputInit();
	audioInit();
	romdataInit();
	netInit();

	g_ValidGbcRomFound = romdataCheckGbcRom();

	gameInit();

	if (fsGetModDir()) {
		modConfigLoad(MOD_CONFIG_FNAME);
	}

	atexit(cleanup);

	bootCreateSched();

	g_OsMemSize = osGetMemSize();

	g_MempHeapSize = g_OsMemSize;
	g_MempHeap = sysMemZeroAlloc(g_MempHeapSize);
	if (!g_MempHeap) {
		sysFatalError("Could not alloc %u bytes for memp heap.", g_MempHeapSize);
	}

	sysLogPrintf(LOG_NOTE, "memp heap at %p - %p", g_MempHeap, g_MempHeap + g_MempHeapSize);
	sysLogPrintf(LOG_NOTE, "rom  file at %p - %p", g_RomFile, g_RomFile + g_RomFileSize);

	g_SndDisabled = sysArgCheck("--no-sound");

	g_StageNum = sysArgGetInt("--boot-stage", STAGE_TITLE);

	g_FileAutoSelect = sysArgGetInt("--profile", -1);

	// --mpprofile <name> overrides the Combat Sim profile name loaded from
	// pd.ini. The actual pak lookup runs from menudialogMainMenu MENUOP_OPEN
	// (and the Combat Sim hook as a fallback), where mema is initialised and
	// the pak system is online.
	{
		const char *mpprofile = sysArgGetString("--mpprofile");
		if (mpprofile != NULL && mpprofile[0]) {
			strncpy(g_MpProfileName, mpprofile, MAX_PLAYERNAME - 1);
			g_MpProfileName[MAX_PLAYERNAME - 1] = '\0';
			sysLogPrintf(LOG_NOTE, "mp profile override: %s", g_MpProfileName);
		}
	}

	if (g_StageNum == STAGE_TITLE && (sysArgCheck("--skip-intro") || g_SkipIntro)) {
		// shorthand for --boot-stage 0x26
		g_StageNum = STAGE_CITRAINING;
	} else if (g_StageNum < 0x01 || g_StageNum > 0x5d) {
		// stage num out of range
		g_StageNum = STAGE_TITLE;
	}

	if (g_NetJoinLatch || g_NetHostLatch) {
		if (g_FileAutoSelect < 0) {
			// default to profile 0 if going into a net game
			g_FileAutoSelect = 0;
		}
		// skip the intro if going into a net game
		g_StageNum = STAGE_CITRAINING;
	}

	if (g_StageNum != STAGE_TITLE) {
		sysLogPrintf(LOG_NOTE, "boot stage set to 0x%02x", g_StageNum);
	}

	if (g_FileAutoSelect >= 0) {
		sysLogPrintf(LOG_NOTE, "player profile set to %d", g_FileAutoSelect);
	}

	mainProc();

	return 0;
}

PD_CONSTRUCTOR static void gameConfigInit(void)
{
	configRegisterInt("Game.MemorySize", &g_OsMemSizeMb, 4, 2048);
	configRegisterInt("Game.CenterHUD", &g_HudCenter, 0, 2);
	configRegisterInt("Game.MenuMouseControl", &g_MenuMouseControl, 0, 1);
	configRegisterFloat("Game.ScreenShakeIntensity", &g_ViShakeIntensityMult, 0.f, 10.f);
	configRegisterInt("Game.TickRateDivisor", &g_TickRateDiv, 0, 10);
	configRegisterInt("Game.ExtraSleep", &g_TickExtraSleep, 0, 1);
	configRegisterInt("Game.SkipIntro", &g_SkipIntro, 0, 1);
	configRegisterInt("Game.DisableMpDeathMusic", &g_MusicDisableMpDeath, 0, 1);
	configRegisterInt("Game.GEMuzzleFlashes", &g_BgunGeMuzzleFlashes, 0, 1);
	for (s32 j = 0; j < MAX_LOCAL_PLAYERS; ++j) {
		const s32 i = j + 1;
		configRegisterFloat(strFmt("Game.Player%d.FovY", i), &g_PlayerExtCfg[j].fovy, 5.f, 175.f);
		configRegisterInt(strFmt("Game.Player%d.FovAffectsZoom", i), &g_PlayerExtCfg[j].fovzoom, 0, 1);
		configRegisterInt(strFmt("Game.Player%d.MouseAimMode", i), &g_PlayerExtCfg[j].mouseaimmode, 0, 1);
		configRegisterFloat(strFmt("Game.Player%d.MouseAimSpeedX", i), &g_PlayerExtCfg[j].mouseaimspeedx, 0.f, 10.f);
		configRegisterFloat(strFmt("Game.Player%d.MouseAimSpeedY", i), &g_PlayerExtCfg[j].mouseaimspeedy, 0.f, 10.f);
		configRegisterFloat(strFmt("Game.Player%d.RadialMenuSpeed", i), &g_PlayerExtCfg[j].radialmenuspeed, 0.f, 10.f);
		configRegisterFloat(strFmt("Game.Player%d.CrosshairSway", i), &g_PlayerExtCfg[j].crosshairsway, 0.f, 10.f);
		configRegisterInt(strFmt("Game.Player%d.CrouchMode", i), &g_PlayerExtCfg[j].crouchmode, 0, CROUCHMODE_TOGGLE_ANALOG);
		configRegisterInt(strFmt("Game.Player%d.ExtendedControls", i), &g_PlayerExtCfg[j].extcontrols, 0, 1);
		configRegisterUInt(strFmt("Game.Player%d.CrosshairColour", i), &g_PlayerExtCfg[j].crosshaircolour, 0, 0xFFFFFFFF);
		configRegisterUInt(strFmt("Game.Player%d.CrosshairSize", i), &g_PlayerExtCfg[j].crosshairsize, 0, 4);
		configRegisterInt(strFmt("Game.Player%d.CrosshairHealth", i), &g_PlayerExtCfg[j].crosshairhealth, 0, CROSSHAIR_HEALTH_ON_WHITE);
		configRegisterInt(strFmt("Game.Player%d.CrosshairForceClassic", i), &g_PlayerExtCfg[j].crosshairforceclassic, 0, 1);
		configRegisterInt(strFmt("Game.Player%d.CrosshairHideUnlessAiming", i), &g_PlayerExtCfg[j].crosshairhideunlessaiming, 0, 1);
		configRegisterInt(strFmt("Game.Player%d.UseKeyReloads", i), &g_PlayerExtCfg[j].usereloads, 0, false);
	}

	// Combat Sim player profile name — used to look up the player's pak file
	// on the next session so they don't have to re-select their character.
	configRegisterString("MP.Profile.Name", g_MpProfileName, MAX_PLAYERNAME);

	// Cached head/body from the last loaded profile, persisted so the CI
	// training "title screen" sequence can render the player's chosen
	// character on the first frame — before mpProfileLoadFromPak runs at
	// main-menu open. -1 means "no profile saved yet, use default Joanna".
	configRegisterInt("MP.Profile.Head", &g_MpProfileHead, -1, 255);
	configRegisterInt("MP.Profile.Body", &g_MpProfileBody, -1, 255);

	// When set, mpGenerateBotNames picks from a fixed dictionary of fun
	// first names ("BobSim", "AliceSim", ...) instead of the profile-based
	// "MeatSim:N" scheme. Set to 0 in pd.ini to restore the original
	// behaviour. The name is broadcast in SVC_STAGE_START's bot config
	// block, so clients see whatever the host has configured.
	extern s32 g_MpAutoRenameSims;
	configRegisterInt("MP.AutoRenameSims", &g_MpAutoRenameSims, 0, 1);
}
