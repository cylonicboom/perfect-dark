#ifndef _PORT_NET_DEMO_H
#define _PORT_NET_DEMO_H

// Demo recording (Phase 2 of the killcam/demo work — see docs notes &
// ~/.claude/plans/composed-hatching-book.md). Brute-force capture of a whole
// match to disk: the same per-tick world pose the killcam ring records
// (netKillcamCaptureLiveFrame), streamed to a file instead of a 240-tick ring,
// for later free-camera playback.
//
// MVP scope (this pass): players + sims only (the killcam record set). Props
// (weapons/projectiles/doors/lifts) are deferred to a follow-up — the file
// format is versioned so that extension can bump DEMO_VERSION.
//
// Client-side + local; no wire format change (it records data the machine
// already has). DEMO_VERSION is independent of NET_PROTOCOL_VER (we store the
// latter in the header for diagnostics / future cross-build playback gating).

#include "types.h"

#define DEMO_MAGIC      0x4d444450u // 'PDDM'
#define DEMO_VERSION    2u // 2: netkillcamentry gained islocalplayer + heldweapon[2]

// File header, written once at record start. Everything needed to reconstruct
// the stage for playback (the SVC_STAGE_START payload, essentially): the full
// mp setup (carries stagenum + scenario + options), the bot configs, and the
// RNG seeds (so the deterministic sim/prop allocation matches at playback).
struct demoheader {
	u32 magic;            // DEMO_MAGIC
	u32 version;          // DEMO_VERSION
	u32 protover;         // NET_PROTOCOL_VER at record time (diagnostic)
	u32 framewidth;       // combatant slots per frame (MAX_MPCHRS)
	u32 botcount;         // g_BotCount at record start (sims in play)
	u32 pad;              // keep 8-byte alignment for rngseeds
	u64 rngseeds[2];      // g_NetRngSeeds at record start
	struct mpsetup setup; // stage / scenario / options / chrslots
	struct mpbotconfig bots[NET_MAX_BOTS]; // sim configs (head/body/team/name)
};

// One frame on disk = a struct netkillcamframe (frame number + per-combatant
// pose). Appended once per logical 60Hz tick after the header.

void netDemoRecordTick(void);  // capture + append one frame (called from lvTick)
void netDemoStop(void);        // close any open recording (stage end / disconnect)
s32  netDemoIsRecording(void); // 1 while a recording file is open

// --- Playback (/demoplay <file>) ---
// netDemoPlayTick advances the playback cursor (reads the next frame) each tick;
// netDemoRenderBegin/End bracket lvRender (the killcam pattern): puppet all
// combatants from the current frame, render, then restore live poses.
void netDemoPlayTick(void);    // called from lvTick alongside netDemoRecordTick
s32  netDemoRenderBegin(void); // 1 if puppeting this frame (caller must RenderEnd)
void netDemoRenderEnd(void);
s32  netDemoIsPlaying(void);   // 1 while playing back (lv.c forces a single fullscreen viewport)
struct chrdata *netDemoFollowedChr(void); // the combatant whose eye we render from (NULL if none)

// Console entry: /demorec start [name]|stop|status, /demoplay <file>|stop.
// Returns 1 if handled.
s32  netDemoConsoleCommand(const char *cmd, const char *arg);

#endif
