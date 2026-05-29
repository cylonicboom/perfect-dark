#ifndef _IN_NET_PLAYLIST_H
#define _IN_NET_PLAYLIST_H

#include <PR/ultratypes.h>

// Maximum entries the playlist can hold. Keeps the menu / vote ballot bounded.
#define PLAYLIST_MAX_ENTRIES 64
#define PLAYLIST_NAME_MAXLEN 32
#define PLAYLIST_PRESET_MAXLEN 18

// Sentinel used in stagenum / scenario / weaponpreset slots to mean "pick at
// random at resolve time". Disk format uses the literal string "RANDOM"; the
// parser turns that into these values, and playlistResolveRandoms replaces
// them with concrete picks just before the entry is applied to g_MpSetup.
#define PLAYLIST_RANDOM_STAGE     ((s16)-1)
#define PLAYLIST_RANDOM_SCENARIO  ((s8)-1)
#define PLAYLIST_RANDOM_PRESET    ((s8)-1)

// One match in the rotation. mp_options applies only the bits listed in
// mp_options_mask — a 1 bit in the mask means "force this MPOPTION to whatever
// mp_options says"; a 0 bit in the mask means "leave the existing g_MpSetup
// bit alone". This lets entries opt in or out of specific options without
// having to enumerate every flag.
struct playlistentry {
	s16 stagenum;            // STAGE_MP_* or PLAYLIST_RANDOM_STAGE
	s8  scenario;            // MPSCENARIO_* or PLAYLIST_RANDOM_SCENARIO
	s8  weaponpreset;        // g_MpWeaponPresets index, -2 = built-in by name lookup
	u32 mp_options;          // override bits
	u32 mp_options_mask;     // which mp_options bits are authoritative
	u8  scorelimit;
	u8  timelimit;
	u16 teamscorelimit;
	u8  bot_count;           // 0..MAX_BOTS
	u8  bot_difficulty;      // BOTDIFF_*
	u8  weight;              // 1..255, vote-pool weighting
	char name[PLAYLIST_NAME_MAXLEN];
	char preset_name[PLAYLIST_PRESET_MAXLEN]; // resolved at apply time
};

struct playlist {
	struct playlistentry entries[PLAYLIST_MAX_ENTRIES];
	u8 count;

	// Server header settings — read from the [server] section of the
	// playlist file. vote_candidates is clamped to [2, num_entries]; if the
	// file omits it, defaults below apply.
	char server_name[64];
	u8 vote_seconds;          // default 20
	u8 vote_candidates;       // default 3
	u8 random_in_pool;        // 0/1, default 1
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
s32 playlistPickBallot(const struct playlist *pl, u64 *rng_state, s32 n, s8 *out_indices);

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

#endif
