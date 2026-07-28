#ifndef _IN_NET_PLAYLIST_H
#define _IN_NET_PLAYLIST_H

#include <PR/ultratypes.h>

// Maximum entries the playlist can hold. Keeps the menu / vote ballot bounded.
// Port-only file (no N64 constraint); the array it sizes is heap/global, not a
// wire format, so this is just generous headroom for long map rotations.
#define PLAYLIST_MAX_ENTRIES 256
#define PLAYLIST_NAME_MAXLEN 32
#define PLAYLIST_PRESET_MAXLEN 18

// Sentinel used in stagenum / scenario / weaponpreset slots to mean "pick at
// random at resolve time". Disk format uses the literal string "RANDOM"; the
// parser turns that into these values, and playlistResolveRandoms replaces
// them with concrete picks just before the entry is applied to g_MpSetup.
#define PLAYLIST_RANDOM_STAGE     ((s16)-1)
#define PLAYLIST_RANDOM_SCENARIO  ((s8)-1)
#define PLAYLIST_RANDOM_PRESET    ((s8)-1)

// Per-weapon ban bits for the [server] `banned=` key, indexed by MPWEAPON_*.
// The low two bits intentionally equal FNFLAG_PRIMARY_DISABLED /
// FNFLAG_SECONDARY_DISABLED so they OR straight into g_MpSlotFnFlags[].
#define PLAYLIST_BAN_PRI    0x01 // weapon's primary function disabled
#define PLAYLIST_BAN_SEC    0x02 // weapon's secondary function disabled
#define PLAYLIST_BAN_WEAPON 0x80 // weapon removed from the loadout entirely

// Sized to cover NUM_MPWEAPONS (0x31 on the port) with headroom; playlist.c
// bounds-checks ids against this at parse time.
#define PLAYLIST_WEAPONBAN_SLOTS 64

// One match in the rotation. mp_options applies only the bits listed in
// mp_options_mask — a 1 bit in the mask means "force this MPOPTION to whatever
// mp_options says"; a 0 bit in the mask means "leave the existing g_MpSetup
// bit alone". This lets entries opt in or out of specific options without
// having to enumerate every flag.
struct playlistentry {
	s16 stagenum;            // STAGE_MP_* or PLAYLIST_RANDOM_STAGE
	s8  scenario;            // MPSCENARIO_* or PLAYLIST_RANDOM_SCENARIO
	s8  weaponpreset;        // g_MpWeaponPresets index, -2 = built-in by name lookup
	u64 mp_options;          // override bits (64-bit; high bits 32-63 are port-only options like MPOPTION_NODOORS)
	u64 mp_options_mask;     // which mp_options bits are authoritative
	u8  scorelimit;
	u8  timelimit;
	u16 teamscorelimit;
	u8  bot_count;           // 0..NET_MAX_BOTS
	u8  bot_difficulty;      // BOTDIFF_*
	u8  weight;              // 1..255, vote-pool weighting
	char name[PLAYLIST_NAME_MAXLEN];
	char preset_name[PLAYLIST_PRESET_MAXLEN]; // resolved at apply time
};

struct playlist {
	struct playlistentry entries[PLAYLIST_MAX_ENTRIES];
	// s16, not u8: PLAYLIST_MAX_ENTRIES is 256, so `count >= 256` on a u8 is
	// always false — the "too many entries" guard was unreachable, count++
	// wrapped 255 -> 0, and a long playlist silently overwrote its own start.
	s16 count;

	// Server header settings — read from the [server] section of the
	// playlist file. vote_candidates is clamped to [2, num_entries]; if the
	// file omits it, defaults below apply.
	char server_name[64];
	u8 vote_seconds;          // default 20
	u8 vote_candidates;       // default 3
	u8 random_in_pool;        // 0/1, default 1
	// Minimum connected human clients before the dedicated-server auto-start
	// gate fires (and before a post-vote round advance happens). Below this,
	// the server idles in the Combat Sim lobby. 0 means "never wait" (the
	// pre-existing behavior); default 1.
	u8 min_humans_to_start;

	// Server-wide weapon / weapon-function ban list, indexed by MPWEAPON_*
	// (PLAYLIST_BAN_* bits). Read from the [server] `banned=` key; applied to
	// the final g_MpSetup.weapons / g_MpSlotFnFlags at match start by
	// playlistApplyWeaponBans. All-zero = no bans.
	u8 weapon_bans[PLAYLIST_WEAPONBAN_SLOTS];
};

// Load from disk. Returns 1 on success, 0 if the file is missing or empty.
// Errors are logged via sysLogPrintf; on parse error the load continues with
// whatever entries were valid so the server can still start.
s32 playlistLoad(struct playlist *pl, const char *path);

// Reset to empty (count = 0, defaults restored).
void playlistFree(struct playlist *pl);

// Pick a random entry using the per-entry weights. rng_in is advanced.
// Returns -1 if the playlist is empty. The returned index is into pl->entries.
s32 playlistPick(const struct playlist *pl, u64 *rng_state);

// Pick N distinct entries for a vote ballot. Writes indices into out_indices;
// returns the number written (may be < n if the playlist is small). Honors
// per-entry weights but guarantees no duplicates. If pl->random_in_pool is
// set and n > 1, the last slot is reserved for a sentinel RANDOM choice that
// the caller resolves at apply time.
s32 playlistPickBallot(const struct playlist *pl, u64 *rng_state, s32 n, s16 *out_indices);

// Resolve RANDOM sentinels in the entry into concrete picks (stage, scenario,
// preset). The output is a copy of the input with sentinels replaced; the
// input is left untouched. RANDOM stage uses mpChooseRandomStage (honors
// challenge unlocks). RANDOM scenario picks uniformly from 0..5. RANDOM
// preset picks from saved g_MpWeaponPresets[] OR returns -1 (apply-time
// fallback to default Combat Sim weapon set).
void playlistResolveRandoms(const struct playlistentry *in, struct playlistentry *out);

// Apply an already-resolved entry to g_MpSetup + g_BotCount + g_BotConfigsArray.
// Server-side only. Caller is responsible for calling netServerStageStart
// after this to broadcast the new setup to clients.
void playlistApply(const struct playlistentry *resolved);

// The single global playlist used by the dedicated-server path.
extern struct playlist g_NetPlaylist;

// Print the current playlist via netChatPrintf (or sysLogPrintf if not in
// a net session yet). For /playlist list console command.
void playlistDumpToChat(void);

// Name -> id lookups over the playlist parser's tables, for the admin `set`
// command. Stage/scenario/bot-difficulty return -1 if the name is unknown;
// option returns the MPOPTION_* bit (0 if unknown — every real option bit is
// nonzero). The option bit is 64-bit so high-word options (e.g. MPOPTION_NODOORS
// at bit 32) are representable. playlistAllOptionBits returns the OR of every
// settable option bit.
s32 playlistLookupStage(const char *name);
s32 playlistLookupScenario(const char *name);
s32 playlistLookupBotDiff(const char *name);
u64 playlistLookupOption(const char *name);
u64 playlistAllOptionBits(void);

// Append one entry to the playlist file the server last loaded (for the admin
// `saverotation` command). Returns 0 on success, -1 if no file is known or it
// can't be written. The in-memory g_NetPlaylist is updated separately by the
// caller; this only persists to disk so the entry survives a restart.
s32 playlistAppendEntryToFile(const struct playlistentry *e);

// Enforce g_NetPlaylist.weapon_bans on the active setup: banned weapons in
// g_MpSetup.weapons[] become MPWEAPON_NONE ("Nothing" — the pad never spawns),
// function bans OR FNFLAG_* bits into g_MpSlotFnFlags[]. Server-only no-op
// otherwise; idempotent. Called from mpStartMatch right before the loadout is
// locked in (after preset picks / re-rolls / admin pushes), so the filtered
// slots + fn-flags are what SVC_STAGE_START broadcasts.
void playlistApplyWeaponBans(void);

#endif
