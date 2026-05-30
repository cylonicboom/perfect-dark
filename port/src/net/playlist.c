#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "platform.h"
#include <PR/ultratypes.h>
#include "types.h"
#include "constants.h"
#include "bss.h"
#include "data.h"
#include "system.h"
#include "fs.h"
#include "mpsetups.h"
#include "net/net.h"
#include "net/playlist.h"
#include "game/mplayer/mplayer.h"

struct playlist g_NetPlaylist;

// Forward decl from setup.c; lifts the existing weighted RNG used for the
// in-game Random Map menu so playlist random-stage picks honor the same
// challenge-unlock gating.
extern struct mparena g_MpArenas[];
extern s16 mpChooseRandomStage(void);
extern s32 challengeIsFeatureUnlocked(s32 feature);

// ---------- name → id lookup tables ----------

struct namedid {
	const char *name;
	s32 id;
};

static const struct namedid s_stages[] = {
	{ "SKEDAR",     STAGE_MP_SKEDAR },
	{ "PIPES",      STAGE_MP_PIPES },
	{ "RAVINE",     STAGE_MP_RAVINE },
	{ "G5BUILDING", STAGE_MP_G5BUILDING },
	{ "G5",         STAGE_MP_G5BUILDING },
	{ "SEWERS",     STAGE_MP_SEWERS },
	{ "WAREHOUSE",  STAGE_MP_WAREHOUSE },
	{ "GRID",       STAGE_MP_GRID },
	{ "RUINS",      STAGE_MP_RUINS },
	{ "AREA52",     STAGE_MP_AREA52 },
	{ "BASE",       STAGE_MP_BASE },
	{ "FORTRESS",   STAGE_MP_FORTRESS },
	{ "VILLA",      STAGE_MP_VILLA },
	{ "CARPARK",    STAGE_MP_CARPARK },
	{ "TEMPLE",     STAGE_MP_TEMPLE },
	{ "COMPLEX",    STAGE_MP_COMPLEX },
	{ "FELICITY",   STAGE_MP_FELICITY },
	{ NULL, 0 }
};

static const struct namedid s_scenarios[] = {
	{ "COMBAT",     MPSCENARIO_COMBAT },
	{ "DEATHMATCH", MPSCENARIO_COMBAT },
	{ "HTB",        MPSCENARIO_HOLDTHEBRIEFCASE },
	{ "HOLDTHEBRIEFCASE", MPSCENARIO_HOLDTHEBRIEFCASE },
	{ "HTM",        MPSCENARIO_HACKERCENTRAL },
	{ "HACKERCENTRAL", MPSCENARIO_HACKERCENTRAL },
	{ "HACKTHATMAC",   MPSCENARIO_HACKERCENTRAL },
	{ "POPACAP",    MPSCENARIO_POPACAP },
	{ "PAC",        MPSCENARIO_POPACAP },
	{ "KOH",        MPSCENARIO_KINGOFTHEHILL },
	{ "KINGOFTHEHILL", MPSCENARIO_KINGOFTHEHILL },
	{ "CTC",        MPSCENARIO_CAPTURETHECASE },
	{ "CAPTURETHECASE", MPSCENARIO_CAPTURETHECASE },
	{ NULL, 0 }
};

static const struct namedid s_botdiffs[] = {
	{ "MEAT",    BOTDIFF_MEAT },
	{ "EASY",    BOTDIFF_EASY },
	{ "NORMAL",  BOTDIFF_NORMAL },
	{ "HARD",    BOTDIFF_HARD },
	{ "PERFECT", BOTDIFF_PERFECT },
	{ "DARK",    BOTDIFF_DARK },
	{ NULL, 0 }
};

static const struct namedid s_options[] = {
	{ "ONEHITKILLS",   MPOPTION_ONEHITKILLS },
	{ "TEAMS",         MPOPTION_TEAMSENABLED },
	{ "NORADAR",       MPOPTION_NORADAR },
	{ "NOAUTOAIM",     MPOPTION_NOAUTOAIM },
	{ "NOPLAYERHIGHLIGHT",  MPOPTION_NOPLAYERHIGHLIGHT },
	{ "NOPICKUPHIGHLIGHT",  MPOPTION_NOPICKUPHIGHLIGHT },
	{ "SLOWMOTION_ON",      MPOPTION_SLOWMOTION_ON },
	{ "SLOWMOTION_SMART",   MPOPTION_SLOWMOTION_SMART },
	{ "FASTMOVEMENT",       MPOPTION_FASTMOVEMENT },
	{ "DISPLAYTEAM",        MPOPTION_DISPLAYTEAM },
	{ "KILLSSCORE",         MPOPTION_KILLSSCORE },
	{ "SPAWNWITHWEAPON",    MPOPTION_SPAWNWITHWEAPON },
	{ "NODRUGBLUR",         MPOPTION_NODRUGBLUR },
	{ "AUTORANDOMWEAPON_START", MPOPTION_AUTORANDOMWEAPON_START },
	{ "AUTORANDOMWEAPON_END",   MPOPTION_AUTORANDOMWEAPON_END },
	{ "FRIENDLYFIRE",       MPOPTION_FRIENDLYFIRE },
	{ "NOPLAYERONRADAR",    MPOPTION_NOPLAYERONRADAR },
	{ "CONTROLLERS_ONLY",   MPOPTION_CONTROLLERS_ONLY },
	{ "NOCULL",             MPOPTION_NOCULL },
	{ "NOOMLIMIT",          MPOPTION_NOOMLIMIT },
	{ "GOLDENEYE",          MPOPTION_GOLDENEYE },
	// MPOPTION_HOSTSPECTATOR intentionally not exposed — set by netStartServer in dedicated.
	{ NULL, 0 }
};

// Case-insensitive string compare. The disk format accepts mixed case.
static int ieq(const char *a, const char *b)
{
	for (; *a && *b; ++a, ++b) {
		const int ca = toupper((unsigned char)*a);
		const int cb = toupper((unsigned char)*b);
		if (ca != cb) return 0;
	}
	return *a == *b;
}

static s32 lookupNamedId(const struct namedid *tbl, const char *name, s32 fallback)
{
	for (; tbl->name; ++tbl) {
		if (ieq(tbl->name, name)) return tbl->id;
	}
	return fallback;
}

// Public name->id lookups, reusing the parser tables above. Used by the admin
// `set` command (net.c) to accept human-readable stage/scenario/option/bot-diff
// names. Stage/scenario/bot-diff return -1 on no match; option returns the
// MPOPTION_* bit or 0.
s32 playlistLookupStage(const char *name)    { return lookupNamedId(s_stages, name, -1); }
s32 playlistLookupScenario(const char *name) { return lookupNamedId(s_scenarios, name, -1); }
s32 playlistLookupBotDiff(const char *name)  { return lookupNamedId(s_botdiffs, name, -1); }
u32 playlistLookupOption(const char *name)   { return (u32)lookupNamedId(s_options, name, 0); }

u32 playlistAllOptionBits(void)
{
	u32 m = 0;
	for (const struct namedid *p = s_options; p->name; ++p) {
		m |= (u32)p->id;
	}
	return m;
}

static const char *stageName(s32 stagenum)
{
	for (const struct namedid *p = s_stages; p->name; ++p) {
		if (p->id == stagenum) return p->name;
	}
	return "?";
}

static const char *scenarioName(s32 sc)
{
	switch (sc) {
		case MPSCENARIO_COMBAT: return "Combat";
		case MPSCENARIO_HOLDTHEBRIEFCASE: return "HTB";
		case MPSCENARIO_HACKERCENTRAL: return "HTM";
		case MPSCENARIO_POPACAP: return "PAC";
		case MPSCENARIO_KINGOFTHEHILL: return "KOH";
		case MPSCENARIO_CAPTURETHECASE: return "CTC";
		default: return "?";
	}
}

// ---------- parser helpers ----------

// In-place trim of leading + trailing whitespace and a trailing '\r'/'\n'.
static char *trim(char *s)
{
	while (*s && isspace((unsigned char)*s)) ++s;
	char *end = s + strlen(s);
	while (end > s && (isspace((unsigned char)end[-1]) || end[-1] == '\r' || end[-1] == '\n')) --end;
	*end = '\0';
	return s;
}

// Strip surrounding double-quotes from a value. Modifies in place.
static char *unquote(char *s)
{
	const size_t n = strlen(s);
	if (n >= 2 && s[0] == '"' && s[n - 1] == '"') {
		s[n - 1] = '\0';
		return s + 1;
	}
	return s;
}

// Strip ;-style INI comments. Quotes are not honored — playlist values don't
// contain ; in practice.
static void stripComment(char *s)
{
	for (char *p = s; *p; ++p) {
		if (*p == ';' || (*p == '/' && p[1] == '/')) {
			*p = '\0';
			return;
		}
	}
}

// Parse the comma-separated MPOPTION list into a bitmask. Recognised names
// from s_options; unknown names are logged.
static u32 parseOptionList(const char *list)
{
	u32 bits = 0;
	char buf[256];
	strncpy(buf, list, sizeof(buf) - 1);
	buf[sizeof(buf) - 1] = '\0';

	char *save = NULL;
	for (char *tok = strtok(buf, ", \t"); tok; tok = strtok(NULL, ", \t")) {
		const s32 v = lookupNamedId(s_options, tok, -1);
		if (v < 0) {
			sysLogPrintf(LOG_WARNING, "playlist: unknown option `%s`", tok);
		} else {
			bits |= (u32)v;
		}
	}
	(void)save;
	return bits;
}

// Resolve a stage/scenario/preset token. "RANDOM" maps to the sentinel;
// otherwise look up by name, or fall back to strtol for raw IDs.
static s16 parseStage(const char *tok)
{
	if (ieq(tok, "RANDOM")) return PLAYLIST_RANDOM_STAGE;
	const s32 v = lookupNamedId(s_stages, tok, -1);
	if (v >= 0) return (s16)v;
	return (s16)strtol(tok, NULL, 0); // raw hex/decimal stage id
}

static s8 parseScenario(const char *tok)
{
	if (ieq(tok, "RANDOM")) return PLAYLIST_RANDOM_SCENARIO;
	const s32 v = lookupNamedId(s_scenarios, tok, -1);
	if (v >= 0) return (s8)v;
	return (s8)strtol(tok, NULL, 0);
}

static u8 parseBotDiff(const char *tok)
{
	const s32 v = lookupNamedId(s_botdiffs, tok, -1);
	if (v >= 0) return (u8)v;
	return BOTDIFF_NORMAL;
}

// ---------- top-level parser ----------

static void resetEntry(struct playlistentry *e)
{
	memset(e, 0, sizeof(*e));
	e->stagenum = PLAYLIST_RANDOM_STAGE;
	e->scenario = PLAYLIST_RANDOM_SCENARIO;
	e->weaponpreset = PLAYLIST_RANDOM_PRESET;
	e->scorelimit = 10;
	e->timelimit = 10;
	e->bot_count = 0;
	e->bot_difficulty = BOTDIFF_NORMAL;
	e->weight = 1;
	strncpy(e->name, "Entry", sizeof(e->name) - 1);
}

void playlistFree(struct playlist *pl)
{
	memset(pl, 0, sizeof(*pl));
	pl->vote_seconds = 20;
	pl->vote_candidates = 3;
	pl->random_in_pool = 1;
	pl->min_humans_to_start = 1;
	strncpy(pl->server_name, "Perfect Dark Dedicated", sizeof(pl->server_name) - 1);
}

s32 playlistLoad(struct playlist *pl, const char *path)
{
	playlistFree(pl);

	// Try a sequence of candidate paths so the user's file is found whether
	// it sits next to pd.ini (save dir), the exe, the working directory, or
	// the data dir. Bare names (no `/`, no `\`, no `$`, no drive letter)
	// expand to all four candidates; explicit paths (./foo, $S/foo, C:/foo,
	// /foo) are used as-is via fsFullPath.
	FILE *f = NULL;
	char resolved_path[FS_MAXPATH + 1] = { 0 };

	const bool is_explicit = (path[0] == '$' || path[0] == '/' || path[0] == '\\'
			|| (path[0] == '.' && (path[1] == '/' || path[1] == '\\' || path[1] == '.'))
			|| (path[0] && path[1] == ':'));

	if (is_explicit) {
		const char *resolved = fsFullPath(path);
		strncpy(resolved_path, resolved ? resolved : path, FS_MAXPATH);
		f = fsFileOpenRead(path);
		if (!f) {
			sysLogPrintf(LOG_WARNING, "playlist: cannot open `%s` (resolved to `%s`)",
					path, resolved_path);
			return 0;
		}
	} else {
		// Strip any leading directory component just to be safe — the
		// candidates below all prepend their own prefix.
		const char *base = strrchr(path, '/');
		if (!base) base = strrchr(path, '\\');
		base = base ? base + 1 : path;

		static const char *prefixes[] = { "$S", "$E", ".", "$B" };
		char candidate[FS_MAXPATH + 1];
		for (s32 i = 0; i < (s32)(sizeof(prefixes) / sizeof(prefixes[0])); ++i) {
			snprintf(candidate, sizeof(candidate), "%s/%s", prefixes[i], base);
			f = fsFileOpenRead(candidate);
			const char *resolved = fsFullPath(candidate);
			if (f) {
				strncpy(resolved_path, resolved ? resolved : candidate, FS_MAXPATH);
				break;
			}
			sysLogPrintf(LOG_NOTE, "playlist: tried `%s` (`%s`) — not found",
					candidate, resolved ? resolved : "?");
		}
		if (!f) {
			sysLogPrintf(LOG_WARNING, "playlist: `%s` not found in any of $S/$E/./$B", base);
			return 0;
		}
	}

	sysLogPrintf(LOG_NOTE, "playlist: loading `%s`", resolved_path);

	char line[1024];
	enum { SEC_NONE, SEC_SERVER, SEC_ENTRY } section = SEC_NONE;
	struct playlistentry *cur = NULL;

	while (fgets(line, sizeof(line), f)) {
		stripComment(line);
		char *l = trim(line);
		if (!*l) continue;

		// [section] line
		if (l[0] == '[') {
			char *rb = strchr(l, ']');
			if (!rb) {
				sysLogPrintf(LOG_WARNING, "playlist: malformed section `%s`", l);
				continue;
			}
			*rb = '\0';
			char *sec = trim(l + 1);

			if (ieq(sec, "server")) {
				section = SEC_SERVER;
				cur = NULL;
			} else if (strncmp(sec, "entry.", 6) == 0 || strncmp(sec, "Entry.", 6) == 0) {
				if (pl->count >= PLAYLIST_MAX_ENTRIES) {
					sysLogPrintf(LOG_WARNING, "playlist: too many entries, dropping `%s`", sec);
					section = SEC_NONE;
					cur = NULL;
					continue;
				}
				cur = &pl->entries[pl->count++];
				resetEntry(cur);
				// Stamp the section name as the default display name.
				strncpy(cur->name, sec + 6, sizeof(cur->name) - 1);
				section = SEC_ENTRY;
			} else {
				sysLogPrintf(LOG_WARNING, "playlist: unknown section `[%s]`", sec);
				section = SEC_NONE;
			}
			continue;
		}

		// key = value
		char *eq = strchr(l, '=');
		if (!eq) {
			sysLogPrintf(LOG_WARNING, "playlist: malformed line `%s`", l);
			continue;
		}
		*eq = '\0';
		char *key = trim(l);
		char *val = unquote(trim(eq + 1));

		if (section == SEC_SERVER) {
			if (ieq(key, "name")) {
				strncpy(pl->server_name, val, sizeof(pl->server_name) - 1);
			} else if (ieq(key, "vote_seconds")) {
				const s32 v = (s32)strtol(val, NULL, 0);
				pl->vote_seconds = (u8)(v < 5 ? 5 : v > 120 ? 120 : v);
			} else if (ieq(key, "vote_candidates")) {
				const s32 v = (s32)strtol(val, NULL, 0);
				pl->vote_candidates = (u8)(v < 2 ? 2 : v > 6 ? 6 : v);
			} else if (ieq(key, "random_in_pool")) {
				pl->random_in_pool = (ieq(val, "true") || strtol(val, NULL, 0)) ? 1 : 0;
			} else if (ieq(key, "min_humans_to_start")) {
				const s32 v = (s32)strtol(val, NULL, 0);
				pl->min_humans_to_start = (u8)(v < 0 ? 0 : v > NET_MAX_CLIENTS ? NET_MAX_CLIENTS : v);
			} else {
				sysLogPrintf(LOG_WARNING, "playlist: unknown server key `%s`", key);
			}
		} else if (section == SEC_ENTRY && cur) {
			if (ieq(key, "name")) {
				strncpy(cur->name, val, sizeof(cur->name) - 1);
				cur->name[sizeof(cur->name) - 1] = '\0';
			} else if (ieq(key, "stage")) {
				cur->stagenum = parseStage(val);
			} else if (ieq(key, "scenario")) {
				cur->scenario = parseScenario(val);
			} else if (ieq(key, "preset")) {
				if (ieq(val, "RANDOM")) {
					cur->weaponpreset = PLAYLIST_RANDOM_PRESET;
					cur->preset_name[0] = '\0';
				} else {
					strncpy(cur->preset_name, val, sizeof(cur->preset_name) - 1);
					cur->preset_name[sizeof(cur->preset_name) - 1] = '\0';
					// Resolve by name now if the preset table is populated.
					// If the named preset is added later (reload), playlistApply
					// will re-resolve.
					cur->weaponpreset = (s8)mpWeaponPresetFind(cur->preset_name);
				}
			} else if (ieq(key, "timelimit")) {
				cur->timelimit = (u8)strtol(val, NULL, 0);
			} else if (ieq(key, "scorelimit")) {
				cur->scorelimit = (u8)strtol(val, NULL, 0);
			} else if (ieq(key, "teamscorelimit")) {
				cur->teamscorelimit = (u16)strtol(val, NULL, 0);
			} else if (ieq(key, "options")) {
				const u32 bits = parseOptionList(val);
				cur->mp_options |= bits;
				cur->mp_options_mask |= bits;
			} else if (ieq(key, "options_clear")) {
				const u32 bits = parseOptionList(val);
				cur->mp_options &= ~bits;
				cur->mp_options_mask |= bits;
			} else if (ieq(key, "bots")) {
				const s32 v = (s32)strtol(val, NULL, 0);
				cur->bot_count = (u8)(v < 0 ? 0 : v > MAX_BOTS ? MAX_BOTS : v);
			} else if (ieq(key, "bot_diff") || ieq(key, "bot_difficulty")) {
				cur->bot_difficulty = parseBotDiff(val);
			} else if (ieq(key, "weight")) {
				const s32 v = (s32)strtol(val, NULL, 0);
				cur->weight = (u8)(v < 1 ? 1 : v > 255 ? 255 : v);
			} else {
				sysLogPrintf(LOG_WARNING, "playlist: unknown entry key `%s`", key);
			}
		}
	}

	fsFileFree(f);

	if (pl->vote_candidates > pl->count && pl->count > 0) {
		pl->vote_candidates = pl->count;
	}

	sysLogPrintf(LOG_NOTE, "playlist: loaded %d entries from %s", pl->count, path);
	for (s32 i = 0; i < pl->count; ++i) {
		const struct playlistentry *e = &pl->entries[i];
		sysLogPrintf(LOG_NOTE, "  [%d] %s: stage=%s scenario=%s bots=%d/%s weight=%d",
				i, e->name,
				e->stagenum == PLAYLIST_RANDOM_STAGE ? "RANDOM" : stageName(e->stagenum),
				e->scenario == PLAYLIST_RANDOM_SCENARIO ? "RANDOM" : scenarioName(e->scenario),
				(s32)e->bot_count, "?", (s32)e->weight);
	}

	return pl->count > 0;
}

// ---------- selection ----------

// Simple xorshift64 advancer so callers can pass a stable seed and get
// repeatable picks (useful for the vote tally tie-breaker).
static u32 rngStep(u64 *state)
{
	u64 x = *state ? *state : 0x9e3779b97f4a7c15ULL;
	x ^= x << 13;
	x ^= x >> 7;
	x ^= x << 17;
	*state = x;
	return (u32)(x & 0xFFFFFFFFu);
}

s32 playlistPick(const struct playlist *pl, u64 *rng_state)
{
	if (!pl || pl->count == 0) return -1;

	u32 total = 0;
	for (s32 i = 0; i < pl->count; ++i) {
		total += pl->entries[i].weight ? pl->entries[i].weight : 1;
	}
	if (total == 0) return 0;

	u32 r = rngStep(rng_state) % total;
	for (s32 i = 0; i < pl->count; ++i) {
		const u32 w = pl->entries[i].weight ? pl->entries[i].weight : 1;
		if (r < w) return i;
		r -= w;
	}
	return pl->count - 1;
}

s32 playlistPickBallot(const struct playlist *pl, u64 *rng_state, s32 n, s8 *out_indices)
{
	if (!pl || pl->count == 0 || n <= 0) return 0;

	// Reserve last slot for RANDOM sentinel if requested + n > 1.
	const s32 randslot = (pl->random_in_pool && n > 1) ? (n - 1) : -1;
	const s32 picks_needed = (randslot >= 0) ? (n - 1) : n;

	s32 written = 0;
	u8 used[PLAYLIST_MAX_ENTRIES] = { 0 };
	const s32 cap = picks_needed > pl->count ? pl->count : picks_needed;

	while (written < cap) {
		const s32 idx = playlistPick(pl, rng_state);
		if (idx < 0) break;
		if (used[idx]) continue;
		used[idx] = 1;
		out_indices[written++] = (s8)idx;
	}

	if (randslot >= 0) {
		out_indices[written++] = (s8)-1; // RANDOM sentinel
	}

	return written;
}

void playlistResolveRandoms(const struct playlistentry *in, struct playlistentry *out)
{
	*out = *in;

	if (out->stagenum == PLAYLIST_RANDOM_STAGE) {
		out->stagenum = mpChooseRandomStage();
	}
	if (out->scenario == PLAYLIST_RANDOM_SCENARIO) {
		// Pick uniform from 0..5 using lib RNG (server-authoritative — this
		// runs before SVC_STAGE_START so seeds aren't yet synced).
		out->scenario = (s8)(((u32)sysGetMicroseconds()) % 6u);
	}
	if (out->weaponpreset == PLAYLIST_RANDOM_PRESET) {
		if (g_MpWeaponPresetCount > 0) {
			out->weaponpreset = (s8)(((u32)sysGetMicroseconds()) % g_MpWeaponPresetCount);
		} else {
			out->weaponpreset = -1; // apply-time fallback to default loadout
		}
	} else if (out->preset_name[0]) {
		// Re-resolve by name in case presets were reloaded since parse.
		const s32 idx = mpWeaponPresetFind(out->preset_name);
		if (idx >= 0) out->weaponpreset = (s8)idx;
	}
}

// ---------- apply ----------

void playlistApply(const struct playlistentry *resolved)
{
	if (!resolved) return;

	g_MpSetup.stagenum = (u8)resolved->stagenum;
	g_MpSetup.scenario = (u8)resolved->scenario;
	g_MpSetup.timelimit = resolved->timelimit;
	g_MpSetup.scorelimit = resolved->scorelimit;
	g_MpSetup.teamscorelimit = resolved->teamscorelimit;

	// Playlist is authoritative for g_MpSetup.options. Bits listed in
	// `options=` are ON; everything else is OFF. This stops bits that
	// mpsetupLoadCurrentFile picked up from disk — particularly the
	// port-only upper-byte flags like MPOPTION_GOLDENEYE / MPOPTION_NOCULL /
	// MPOPTION_NOOMLIMIT that a previous menu-driven Combat Sim session may
	// have saved into mpsetups.bin — from silently leaking into dedicated-
	// server matches. `options_clear=` remains parsed for back-compat but
	// is now redundant: any bit not in `options=` is already 0.
	//
	// MPOPTION_HOSTSPECTATOR is preserved across applies because it's a
	// host-session flag (set by netStartServer in dedicated mode, by
	// menuhandlerHostStart in host-and-play with the Host Spectator menu
	// option) — not a per-match toggle. Clearing it here would break
	// SVC_LOBBY_STATE / SVC_STAGE_START's spectator-status broadcast.
	const u32 sticky = MPOPTION_HOSTSPECTATOR;
	g_MpSetup.options = (g_MpSetup.options & sticky)
			| (resolved->mp_options & ~sticky);

	// Weapons: if a valid preset index, copy its weapons + slotfnflags into
	// the active setup. Otherwise leave g_MpSetup.weapons alone (last-applied
	// default). The default Random preset rotation in mpApplyWeaponSet still
	// works because we don't touch g_MpWeaponSetNum.
	if (resolved->weaponpreset >= 0 && resolved->weaponpreset < g_MpWeaponPresetCount) {
		const struct mpweaponpreset *p = &g_MpWeaponPresets[resolved->weaponpreset];
		for (s32 i = 0; i < NUM_MPWEAPONSLOTS; ++i) {
			g_MpSetup.weapons[i] = p->weapons[i];
			g_MpSlotFnFlags[i] = p->slotfnflags[i];
		}
	}

	// Bot fill: clear any existing bot slots, then create N bots at the
	// requested difficulty. Profile indices 0..5 in g_BotProfiles map 1:1 to
	// difficulties MEAT..DARK (BOTTYPE_GENERAL for all six), so use
	// difficulty as the profile index directly. mpCreateBotFromProfile sets
	// chrslots bit (botnum + MAX_PLAYERS), type, difficulty, name, head,
	// body, team. g_BotCount is derived elsewhere from chrslots popcount;
	// we set it here so mpHasSimulants returns true in mpReset.
	for (s32 i = 0; i < MAX_BOTS; ++i) {
		g_MpSetup.chrslots &= ~(1u << (i + MAX_PLAYERS));
		g_BotConfigsArray[i].base.name[0] = '\0';
	}
	const u8 prof = (resolved->bot_difficulty <= BOTDIFF_DARK)
			? resolved->bot_difficulty : (u8)BOTDIFF_NORMAL;
	for (s32 i = 0; i < resolved->bot_count && i < MAX_BOTS; ++i) {
		mpCreateBotFromProfile(i, prof);
	}
	g_BotCount = resolved->bot_count;
}

// ---------- debug print ----------

void playlistDumpToChat(void)
{
	void netChatPrintf(struct netclient *dst, const char *fmt, ...);

	if (g_NetPlaylist.count == 0) {
		if (g_NetMode) netChatPrintf(NULL, "playlist: empty");
		else sysLogPrintf(LOG_NOTE, "playlist: empty");
		return;
	}

	for (s32 i = 0; i < g_NetPlaylist.count; ++i) {
		const struct playlistentry *e = &g_NetPlaylist.entries[i];
		const char *stage = (e->stagenum == PLAYLIST_RANDOM_STAGE) ? "RANDOM" : stageName(e->stagenum);
		const char *scen  = (e->scenario == PLAYLIST_RANDOM_SCENARIO) ? "RANDOM" : scenarioName(e->scenario);
		if (g_NetMode) {
			netChatPrintf(NULL, "[%d] %s: %s/%s bots=%d wt=%d",
					i, e->name, stage, scen, (s32)e->bot_count, (s32)e->weight);
		} else {
			sysLogPrintf(LOG_NOTE, "  [%d] %s: %s/%s bots=%d wt=%d",
					i, e->name, stage, scen, (s32)e->bot_count, (s32)e->weight);
		}
	}
}
