#ifndef _IN_NETMSG_H
#define _IN_NETMSG_H

#include <PR/ultratypes.h>
#include "net/net.h"
#include "net/netbuf.h"

#define SVC_BAD           0x00 // trash
#define SVC_NOP           0x01 // does nothing
#define SVC_AUTH          0x02 // auth response, sent in response to CLC_AUTH
#define SVC_CHAT          0x03 // chat message
#define SVC_STAGE_START   0x10 // start level
#define SVC_STAGE_END     0x11 // end level

// SVC_STAGE_START mode byte (campaign co-op vs Combat Sim).
#define NETSTAGEMODE_COMBAT 0 // Combat Sim (g_MpSetup follows)
#define NETSTAGEMODE_COOP   1 // campaign co-op (difficulty byte follows; solo load)
#define SVC_PLAYER_MOVE   0x20 // player movement and inputs
#define SVC_PLAYER_GUNS   0x21 // player gun state
#define SVC_PLAYER_STATS  0x22 // player stats (health etc)
#define SVC_PROP_MOVE     0x30 // prop movement
#define SVC_PROP_SPAWN    0x31 // new prop spawned
#define SVC_PROP_DAMAGE   0x32 // prop was damaged
#define SVC_PROP_PICKUP   0x33 // prop was picked up
#define SVC_PROP_USE      0x34 // door/lift/etc was used
#define SVC_PROP_DOOR     0x35 // door state changed
#define SVC_PROP_LIFT     0x36 // lift state changed
#define SVC_PROP_FREE     0x37 // networked prop destroyed/freed server-side (detonated mine/projectile) — client removes its copy
#define SVC_PROP_RECONCILE 0x38 // periodic active weapon/obj syncid set; client removes ghosts the host already freed
#define SVC_CHR_DAMAGE    0x42 // chr was damaged
#define SVC_CHR_DISARM    0x43 // chr's weapons were dropped
#define SVC_CHR_FIRE      0x44 // sim chr fired its weapon (sound + animation cue)
#define SVC_KILL          0x45 // kill-feed entry ("Shooter > Victim")
#define SVC_SCORE         0x46 // server-authoritative scoreboard deltas
#define SVC_KOH_STATE    0x47 // King of the Hill authoritative hill position/state
#define SVC_EXPLOSION    0x48 // explosion visual effect (for timer-detonated networked props)
#define SVC_LOBBY_STATE  0x49 // lobby info broadcast to clients waiting for game start
#define SVC_VOTE_OPEN    0x4a // open a vote-for-next-map ballot at end-of-round
#define SVC_VOTE_RESULTS 0x4b // close the vote: winning index + per-candidate tally
#define SVC_ADMIN        0x4c // admin command response (one text line to the admin)

#define CLC_BAD      0x00 // trash
#define CLC_NOP      0x01 // does nothing
#define CLC_AUTH     0x02 // auth request, sent immediately after connecting
#define CLC_CHAT     0x03 // chat message
#define CLC_MOVE     0x04 // player input
#define CLC_SETTINGS 0x05 // player settings changed
#define CLC_HIT      0x06 // client-reported chr hit; server validates and applies damage
#define CLC_VOTE     0x07 // client's vote-for-next-map ballot choice
#define CLC_ADMIN    0x08 // admin command line (text), server-executed if authorized
#define CLC_ADMIN_SETUP 0x09 // admin pushes a full g_MpSetup + bot config; server starts the match
#define CLC_PROP_HIT 0x0a // client-reported destructible-prop/glass hit; server validates + applies

// Server status query (port-only server browser + master server). The "flags"
// byte is shared by the direct PDQM query summary and the master HEARTBEAT.
#define NET_QF_INPROGRESS (1 << 0) // a match is in progress (not in lobby)
#define NET_QF_PASSWORD   (1 << 1) // server requires a join password
#define NET_QF_DEDICATED  (1 << 2) // headless / windowed dedicated server
#define NET_QF_CHALLENGE  (1 << 3) // running a Combat Sim challenge

// Direct server query type — optional trailing byte after NET_QUERY_MAGIC.
// Absent (legacy 5-byte request) is treated as SUMMARY.
#define NET_QUERYTYPE_SUMMARY 0 // browser-list row only
#define NET_QUERYTYPE_DETAILS 1 // summary + live scoreboard (players/sims)

// Server status payload builders. The summary block is reused verbatim by both
// the direct query response and the master heartbeat; details appends the live
// scoreboard. See netmsg.c.
u32 netmsgQuerySummaryWrite(struct netbuf *dst);
u32 netmsgQueryDetailsWrite(struct netbuf *dst);

u32 netmsgClcAuthWrite(struct netbuf *dst);
u32 netmsgClcAuthRead(struct netbuf *src, struct netclient *srccl);
u32 netmsgClcChatWrite(struct netbuf *dst, const char *str);
u32 netmsgClcChatRead(struct netbuf *src, struct netclient *srccl);
u32 netmsgClcMoveWrite(struct netbuf *dst);
u32 netmsgClcMoveRead(struct netbuf *src, struct netclient *srccl);
u32 netmsgClcSettingsWrite(struct netbuf *dst);
u32 netmsgClcSettingsRead(struct netbuf *src, struct netclient *srccl);
u32 netmsgClcHitWrite(struct netbuf *dst, struct chrdata *chr, f32 damage, struct coord *vector, struct gset *gset, s16 hitpart, s16 side, s16 *arg10);
u32 netmsgClcHitRead(struct netbuf *src, struct netclient *srccl);
// Client -> server: our local player's gunfire hit a destructible prop (glass /
// object). The server can't re-simulate a remote shooter's shot (partial weapon
// gset), so we report it like a chr hit; the server validates, applies objDamage,
// and broadcasts SVC_PROP_DAMAGE. Wire: { propptr, damage:f32, pos:coord, weaponnum:s8 }.
u32 netmsgClcPropHitWrite(struct netbuf *dst, struct prop *prop, f32 damage, struct coord *pos, s32 weaponnum);
u32 netmsgClcPropHitRead(struct netbuf *src, struct netclient *srccl);

u32 netmsgSvcAuthWrite(struct netbuf *dst, struct netclient *authcl);
u32 netmsgSvcAuthRead(struct netbuf *src, struct netclient *srccl);
u32 netmsgSvcChatWrite(struct netbuf *dst, const char *str);
u32 netmsgSvcChatRead(struct netbuf *src, struct netclient *srccl);
u32 netmsgSvcStageStartWrite(struct netbuf *dst);
u32 netmsgSvcStageStartRead(struct netbuf *src, struct netclient *srccl);
u32 netmsgSvcStageEndWrite(struct netbuf *dst);
u32 netmsgSvcStageEndRead(struct netbuf *src, struct netclient *srccl);
u32 netmsgSvcPlayerMoveWrite(struct netbuf *dst, struct netclient *movecl);
u32 netmsgSvcPlayerMoveRead(struct netbuf *src, struct netclient *srccl);
u32 netmsgSvcPlayerStatsWrite(struct netbuf *dst, struct netclient *actcl);
u32 netmsgSvcPlayerStatsRead(struct netbuf *src, struct netclient *srccl);
u32 netmsgSvcPropSpawnWrite(struct netbuf *dst, struct prop *prop);
u32 netmsgSvcPropSpawnRead(struct netbuf *src, struct netclient *srccl);
u32 netmsgSvcPropMoveWrite(struct netbuf *dst, struct prop *prop, struct coord *initrot);
u32 netmsgSvcPropMoveRead(struct netbuf *src, struct netclient *srccl);
u32 netmsgSvcPropDamageWrite(struct netbuf *dst, struct prop *prop, f32 damage, struct coord *pos, s32 weaponnum, s32 playernum);
u32 netmsgSvcPropDamageRead(struct netbuf *src, struct netclient *srccl);
u32 netmsgSvcPropPickupWrite(struct netbuf *dst, struct netclient *actcl, struct prop *prop, const s32 tickop);
u32 netmsgSvcPropPickupRead(struct netbuf *src, struct netclient *srccl);
u32 netmsgSvcPropUseWrite(struct netbuf *dst, struct prop *prop, struct netclient *usercl, const s32 tickop);
u32 netmsgSvcPropUseRead(struct netbuf *src, struct netclient *srccl);
u32 netmsgSvcPropDoorWrite(struct netbuf *dst, struct prop *prop, struct netclient *usercl);
u32 netmsgSvcPropDoorRead(struct netbuf *src, struct netclient *srccl);
u32 netmsgSvcPropLiftWrite(struct netbuf *dst, struct prop *prop);
u32 netmsgSvcPropLiftRead(struct netbuf *src, struct netclient *srccl);
u32 netmsgSvcPropFreeWrite(struct netbuf *dst, struct prop *prop);
u32 netmsgSvcPropFreeRead(struct netbuf *src, struct netclient *srccl);
u32 netmsgSvcPropReconcileWrite(struct netbuf *dst);
u32 netmsgSvcPropReconcileRead(struct netbuf *src, struct netclient *srccl);
u32 netmsgSvcChrDamageWrite(struct netbuf *dst, struct chrdata *chr, f32 damage, struct coord *vector, struct gset *gset, struct prop *aprop, s32 hitpart, bool damageshield, struct prop *prop2, s32 side, s16 *arg11, bool explosion, struct coord *explosionpos);
u32 netmsgSvcChrDamageRead(struct netbuf *src, struct netclient *srccl);
u32 netmsgSvcChrDisarmWrite(struct netbuf *dst, struct chrdata *chr, struct prop *attacker, u8 weaponnum, f32 wpndamage, struct coord *wpnpos);
u32 netmsgSvcChrDisarmRead(struct netbuf *src, struct netclient *srccl);
u32 netmsgSvcChrFireWrite(struct netbuf *dst, struct chrdata *chr, u8 handnum, u16 soundnum);
u32 netmsgSvcChrFireRead(struct netbuf *src, struct netclient *srccl);
u32 netmsgSvcKillWrite(struct netbuf *dst, const char *shooter, const char *victim, u8 shooter_team, u8 victim_team);
u32 netmsgSvcKillRead(struct netbuf *src, struct netclient *srccl);
u32 netmsgSvcScoreWrite(struct netbuf *dst, const s32 *mpchrindexes, s32 count);
u32 netmsgSvcScoreRead(struct netbuf *src, struct netclient *srccl);
u32 netmsgSvcKohStateWrite(struct netbuf *dst);
u32 netmsgSvcKohStateRead(struct netbuf *src, struct netclient *srccl);
u32 netmsgSvcExplosionWrite(struct netbuf *dst, s32 exptype, const struct coord *pos, const RoomNum *rooms);
u32 netmsgSvcExplosionRead(struct netbuf *src, struct netclient *srccl);
u32 netmsgSvcLobbyStateWrite(struct netbuf *dst);
u32 netmsgSvcLobbyStateRead(struct netbuf *src, struct netclient *srccl);

// Vote system (port-only, dedicated server). Wire layout:
//   SVC_VOTE_OPEN:
//     u8 num_candidates   1..6
//     u8 vote_seconds     deadline in seconds
//     per candidate:
//       u8  playlist_index  0..N-1 or 0xFF for the RANDOM slot
//       u8  stagenum        STAGE_MP_* (pre-resolved by server)
//       u8  scenario        MPSCENARIO_*
//       u8  preset_index    g_MpWeaponPresets index, or 0xFF if default
//       u8  bot_count
//       u8  timelimit       minutes
//       u8  scorelimit
//       str name            null-terminated, <= 32 chars
//   SVC_VOTE_RESULTS:
//     u8 winning_index    0..num_candidates-1
//     u8 winner_was_random 1 if the winner was the RANDOM slot
//     u8 tally_count      mirrors num_candidates for parsing convenience
//     per candidate: u8 votes
//   CLC_VOTE:
//     u8 candidate_index  0..num_candidates-1, or 0xFF for abstain
u32 netmsgSvcVoteOpenWrite(struct netbuf *dst);
u32 netmsgSvcVoteOpenRead(struct netbuf *src, struct netclient *srccl);
u32 netmsgSvcVoteResultsWrite(struct netbuf *dst);
u32 netmsgSvcVoteResultsRead(struct netbuf *src, struct netclient *srccl);
u32 netmsgClcVoteWrite(struct netbuf *dst, u8 candidate_index);
u32 netmsgClcVoteRead(struct netbuf *src, struct netclient *srccl);

// Admin remote control. CLC_ADMIN carries a single text command line
// (client -> server); the server runs it through netServerAdminCommand after
// auth/permission checks. SVC_ADMIN carries one text response line back to the
// admin (server -> client), printed to that client's console.
//   CLC_ADMIN:  str line
//   SVC_ADMIN:  str line
u32 netmsgClcAdminWrite(struct netbuf *dst, const char *line);
u32 netmsgClcAdminRead(struct netbuf *src, struct netclient *srccl);
u32 netmsgSvcAdminWrite(struct netbuf *dst, const char *line);
u32 netmsgSvcAdminRead(struct netbuf *src, struct netclient *srccl);

// Admin setup push (client -> server). Serializes the admin client's locally-
// configured g_MpSetup + bot configs (same block layout as SVC_STAGE_START,
// minus the server-authoritative per-client manifest). The server reads into
// temporaries, and only if the sender is the in-control admin does it commit to
// its own g_MpSetup/g_BotConfigsArray and mpStartMatch() — which broadcasts
// SVC_STAGE_START to all clients as normal.
u32 netmsgClcAdminSetupWrite(struct netbuf *dst);
u32 netmsgClcAdminSetupRead(struct netbuf *src, struct netclient *srccl);

#endif
