#ifndef IN_GAME_DATA_020DF0_H
#define IN_GAME_DATA_020DF0_H
#include <ultra64.h>

extern struct menudialog g_2PMissionInventoryHMenuDialog;
extern struct menudialog g_2PMissionInventoryVMenuDialog;
extern struct menudialog g_MpWeaponsMenuDialog;
extern struct menudialog g_MpPlayerOptionsMenuDialog;
extern struct menudialog g_MpControlMenuDialog;
extern struct menudialog g_MpPlayerStatsMenuDialog;
extern struct menudialog g_MpPlayerNameMenuDialog;
extern struct menudialog g_MpLoadSettingsMenuDialog;
extern struct menudialog g_MpLoadPresetMenuDialog;
extern struct menudialog g_MpLoadPlayerMenuDialog;
extern struct menudialog g_MpArenaMenuDialog;
extern struct menudialog g_MpLimitsMenuDialog;
extern struct menudialog g_MpHandicapsMenuDialog;
extern struct menudialog g_MpReadyMenuDialog;
extern struct menudialog g_MpSimulantsMenuDialog;
extern struct menudialog g_MpTeamsMenuDialog;
extern struct menudialog g_MpChallengeListOrDetailsMenuDialog;
extern struct menudialog g_MpScenarioMenuDialog;
extern struct menudialog g_MpQuickTeamMenuDialog;

extern s32 g_Difficulty;

extern s16 g_FadeNumFrames;
extern f32 g_FadeFrac;
extern u32 g_FadePrevColour;
extern u32 g_FadeColour;
extern s16 g_FadeDelay;

extern s32 var80087260;
extern struct stagethinglist stagethinglist_20e10;
extern struct stagethinglist stagethinglist_20e3c;
extern struct stagethinglist stagethinglist_20e48;
extern struct stagethinglist stagethinglist_20e54;
extern struct stagethinglist stagethinglist_20e60;
extern struct stagethinglist stagethinglist_20e6c;
extern struct stagethinglist stagethinglist_20e80;
extern struct stagethinglist stagethinglist_20e94;
extern struct stagethinglist stagethinglist_20ea4;
extern struct stagethinglist stagethinglist_20ec8;
extern struct stagethinglist stagethinglist_20edc;
extern struct stagethinglist stagethinglist_20eec;
extern struct stagethinglist stagethinglist_20ef8;
extern struct stagethinglist stagethinglist_20f0c;
extern struct stagethinglist stagethinglist_20f18;
extern struct stagethinglist stagethinglist_20f24;
extern struct stagethinglist stagethinglist_20f50;
extern struct stagethinglist stagethinglist_20f5c;
extern struct stagethinglist stagethinglist_20f74;
extern struct stagethinglist stagethinglist_20f84;
extern struct stagethinglist stagethinglist_20fb8;
extern struct stagethinglist stagethinglist_20fd8;
extern struct stagethinglist stagethinglist_20fe8;
extern struct stagethinglist stagethinglist_20ff8;
extern struct modelstate g_ModelStates[441];
extern u8 propexplosiontypes[];
extern struct stagethinglist stagethinglist_2208c;
extern struct stagethinglist stagethinglist_220d0;
extern struct stagethinglist stagethinglist_220a4;
extern struct stagethinglist stagethinglist_2100c;
extern struct stagethinglist stagethinglist_21018;
extern struct stagethinglist stagethinglist_21024;
extern struct stagethinglist stagethinglist_21034;
extern struct stagethinglist stagethinglist_21084;
extern u32 var8007c0c0;
extern struct stagethinglist stagethinglist_221a4;
extern struct stagethinglist stagethinglist_221b4;
extern struct stagethinglist stagethinglist_221cc;
extern struct stagethinglist stagethinglist_221e4;
extern struct stagethinglist stagethinglist_22200;
extern struct stagethinglist stagethinglist_22220;
extern struct stagethinglist stagethinglist_2224c;
extern struct stagethinglist stagethinglist_222b4;
extern struct stagethinglist stagethinglist_22318;
extern struct stagethinglist stagethinglist_22374;
extern struct stagethinglist stagethinglist_223d4;
extern struct stagethinglist stagethinglist_22424;
extern struct stagethinglist stagethinglist_2247c;
extern struct stagethinglist stagethinglist_224d8;
extern struct stagethinglist stagethinglist_22538;
extern struct stagethinglist stagethinglist_2258c;
extern struct stagethinglist stagethinglist_225d8;
extern struct stagethinglist stagethinglist_22630;
extern struct stagethinglist stagethinglist_22698;
extern struct stagethinglist stagethinglist_226e8;
extern struct stagethinglist stagethinglist_22754;
extern struct stagethinglist stagethinglist_227ac;
extern struct stagethinglist stagethinglist_22804;
extern struct stagethinglist stagethinglist_2285c;
extern struct stagethinglist stagethinglist_228b4;
extern struct stagethinglist stagethinglist_2291c;
extern struct stagethinglist stagethinglist_22970;
extern struct stagethinglist stagethinglist_229c4;
extern struct stagethinglist stagethinglist_22a20;
extern struct stagethinglist stagethinglist_22a78;
extern struct stagethinglist stagethinglist_22adc;
extern struct stagethinglist stagethinglist_22b28;
extern struct stagethinglist stagethinglist_22b80;
extern struct stagethinglist stagethinglist_22be0;
extern struct stagethinglist stagethinglist_22c3c;
extern struct stagethinglist stagethinglist_22c54;
extern struct stagethinglist stagethinglist_22ca0;
extern struct stagethinglist stagethinglist_22cf0;
extern struct stagethinglist stagethinglist_22d40;
extern struct stagethinglist stagethinglist_22d90;
extern struct stagethinglist stagethinglist_22de0;
extern struct stagethinglist stagethinglist_22e34;
extern struct stagethinglist stagethinglist_22e60;
extern struct stagethinglist stagethinglist_22eb8;
extern struct stagethinglist stagethinglist_22ec8;
extern struct stagethinglist stagethinglist_22f0c;
extern struct stagethinglist stagethinglist_22f1c;
extern struct body g_Bodies[];
extern u32 var8007dae4;
extern f32 var8007db80;
extern f32 var8007db84;
extern u32 var8007db88;
extern u32 var8007db94;
extern u32 var8007dba0;
extern u32 var8007dbb8;
extern u32 var8007dbd0;
extern u32 var8007dbe8;
extern u32 var8007dbf4;
extern u32 var8007dc00;
extern u32 var8007dc10;
extern struct var8007e3d0 var8007e3d0[4];
extern u32 var8007e4a0;
extern u32 var8007e4a4;
extern f32 var8007e4a8;
extern struct explosiontype g_ExplosionTypes[NUM_EXPLOSIONTYPES];
extern struct smoketype g_SmokeTypes[NUM_SMOKETYPES];
extern struct sparktype g_SparkTypes[];
extern s32 g_SparksAreActive;
extern struct weatherdata *g_WeatherData;
extern s32 var8007f0c4[4];
extern u32 g_RainSpeedExtra;
extern u32 g_SnowSpeed;
extern u32 g_SnowSpeedExtra;
extern u32 var8007f0e0;
extern u32 var8007f0e4;
extern u32 var8007f0e8;
extern u32 var8007f0ec;
extern u32 var8007f0f0;
extern u32 var8007f0f4;
extern u32 var8007f0f8;
extern u32 var8007f0fc;
extern u32 var8007f104;
extern u32 var8007f108;
extern u32 var8007f10c;
extern u32 var8007f110;
extern u32 var8007f120;
extern u32 var8007f124;
extern u32 var8007f130;
extern bool g_CreditsAltTitleRequested;
extern struct creditthing var8007f13c[];
extern u32 var8007f3cc;
extern u32 var8007f410;
extern u32 var8007f450;
extern u32 var8007f468;
extern u32 var8007f46c;
extern struct credit g_Credits[];
extern u32 var8007f6c4;
extern u32 var8007f6d4;
extern u32 var8007f6e0;
extern u32 var8007f6e4;
extern u32 var8007f6e8;
extern u32 var8007f6ec;
extern u32 var8007f6f0;
extern u32 var8007f6f4;
extern u32 var8007f6f8;
extern u32 var8007f6fc;
extern u32 var8007f700;
extern u32 var8007f740;
extern u32 var8007f748;
extern u32 var8007f750;
extern u32 var8007f7b0;
extern s32 var8007f840;
extern u8 var8007f844;
extern u8 var8007f848;
extern u32 var8007f860;
extern u32 var8007f864;
extern u32 var8007f868;
extern u32 var8007f86c;
extern u32 var8007f870;
extern u32 var8007f874;
extern u8 var8007f878;
extern u32 var8007f87c;
extern f32 var8007f8a8;
extern u32 var8007f8dc;
extern u32 var8007f8e8;
extern u32 var8007f8ec;
extern u32 var8007f8f0;
extern u32 var8007f8f4;
extern u32 var8007f8f8;
extern u32 var8007f8fc;
extern u32 var8007f900;
extern u32 var8007f904;
extern u32 var8007f9d0;
extern u32 var8007f9d8;
extern u32 var8007f9fc;
extern u32 var8007fa60;
extern u32 var8007fa80;
extern bool g_ShardsActive;
extern u8 g_InGameSubtitles;
extern u8 g_CutsceneSubtitles;
extern s32 g_ScreenSize;
extern s32 g_ScreenRatio;
extern u8 g_ScreenSplit;
extern s32 g_ScaleX;
extern u32 var8007fac4;
extern u32 var8007fac8;
extern u32 var8007facc;
extern u32 var8007fad0;
extern u32 var8007fad4;
extern u32 var8007fad8;
extern u32 var8007fadc;
extern u32 var8007fae0;
extern u32 var8007fae4;
extern u32 var8007fae8;
extern u32 var8007faec;
extern u32 var8007faf0;
extern struct font *g_FontTahoma2;
extern struct font2a4 *g_FontTahoma1;
extern struct font *g_FontNumeric2;
extern struct font2a4 *g_FontNumeric1;
extern struct font *g_FontHandelGothicXs2;
extern struct font2a4 *g_FontHandelGothicXs1;
extern struct font *g_FontHandelGothicSm2;
extern struct font2a4 *g_FontHandelGothicSm1;
extern struct font *g_FontHandelGothicMd2;
extern struct font2a4 *g_FontHandelGothicMd1;
extern struct font *g_FontHandelGothicLg2;
extern struct font2a4 *g_FontHandelGothicLg1;
extern u32 var8007fb24;
extern u32 var8007fb28;
extern u32 var8007fb2c;
extern u32 var8007fb30;
extern u32 var8007fb34;
extern u32 var8007fb38;
extern u32 var8007fb3c;
extern u32 var8007fb5c;
extern u32 var8007fb9c;
extern u32 var8007fbac;
extern u32 var8007fbb0;
extern u32 var8007fbb4;
extern u32 var8007fbb8;
extern u32 var8007fbbc;
extern u32 var8007fbc0;
extern u32 var8007fbc4;
extern u32 var8007fbc8;
extern u32 var8007fbcc;
extern u32 var8007fbdc;
extern u32 var8007fbe8;
extern u32 var8007fbec;
extern u32 g_StageIndex;
extern s16 var8007fc0c;
extern u16 var8007fc10;
extern s32 var8007fc14;
extern s32 g_CamRoom;
extern u32 var8007fc24;
extern u32 var8007fc28;
extern u32 var8007fc2c;
extern s32 var8007fc30;
extern s32 var8007fc34;
extern u32 var8007fc38;
extern u16 var8007fc3c;
extern s32 g_NumPortalThings;
extern u32 var8007fc44;
extern u32 var8007fc4c;
extern u32 var8007fc54;
extern bool g_PortalStack[20];
extern s32 g_PortalStackIndex;
extern u32 g_PortalMode;
extern u32 var8007fcb0;
extern f32 var8007fcb4;
extern struct stagetableentry g_Stages[61];
extern u32 var80081018;
extern u32 var80081058;
extern struct smallsky smallskies[];
extern s32 var80082050;
extern void *filetable[];
extern u32 g_GfxSizesByPlayerCount[];
extern u32 g_VtxSizesByPlayerCount[];
extern u32 g_GfxNumSwaps;
extern u32 var80084010;
extern bool var80084014;
extern f32 var80084018;
extern s32 g_StageTimeElapsed60;
extern s32 g_MpTimeLimit60;
extern s32 g_MpScoreLimit;
extern s32 g_MpTeamScoreLimit;
extern struct audiohandle *g_MiscAudioHandle;
extern s32 g_NumReasonsToEndMpMatch;
extern f32 g_StageTimeElapsed1f;
extern bool var80084040;
extern u32 g_MiscSfxSounds[3];
extern s32 var80084050;
extern u32 var80084068;
extern u32 var80084078;
extern u32 var80084088;
extern struct audiohandle *g_CutsceneStaticAudioHandle;
extern s32 g_CutsceneStaticTimer;
extern u8 g_CutsceneStaticActive;
extern u32 g_CutsceneTime240_60;

#endif
