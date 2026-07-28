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

// Options need 64-bit values — port-only options live in bits 32-63 of
// g_MpSetup.options (e.g. MPOPTION_NODOORS at bit 32) — so they use their own
// table rather than the s32-id `namedid` shared by stages/scenarios/bot-diffs.
struct namedoption {
	const char *name;
	u64 bit;
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
	// Port-only scenarios. Without these, `scenario = ZONES` in a playlist file
	// fell through parseScenario's strtol fallback to 0 = Combat, silently.
	{ "GRAFFITI",   MPSCENARIO_PAINTROOM },
	{ "PAINTROOM",  MPSCENARIO_PAINTROOM },
	{ "PAINT",      MPSCENARIO_PAINTROOM },
	{ "ZONES",      MPSCENARIO_ZONES },
	{ "RACE",       MPSCENARIO_RACE },
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

static const struct namedoption s_options[] = {
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
	// NOCULL / NOOMLIMIT were retired (replaced by the /octree commands);
	// old playlists naming them now warn as unknown options.
	{ "GOLDENEYE",          MPOPTION_GOLDENEYE },
	{ "NODOORS",            MPOPTION_NODOORS }, // port-only, high word (bit 32)
	// Classic options (GE Style broken into pieces) — port-only, high word
	// (bits 34-45). GOLDENEYE is the master; each is active master OR own bit.
	{ "CLASSIC_SNAPLEAN",    MPOPTION_CLASSIC_SNAPLEAN },
	{ "CLASSIC_NOCROUCHACC", MPOPTION_CLASSIC_NOCROUCHACC },
	{ "CLASSIC_RELOAD",      MPOPTION_CLASSIC_RELOAD },
	{ "CLASSIC_LEDGEWALL",   MPOPTION_CLASSIC_LEDGEWALL },
	{ "CLASSIC_SIGHT",       MPOPTION_CLASSIC_SIGHT },
	{ "CLASSIC_HIDESIGHT",   MPOPTION_CLASSIC_HIDESIGHT },
	{ "CLASSIC_GEHUD",       MPOPTION_CLASSIC_GEHUD },
	{ "CLASSIC_NOSECONDARY", MPOPTION_CLASSIC_NOSECONDARY },
	{ "CLASSIC_NOMIDCROUCH", MPOPTION_CLASSIC_NOMIDCROUCH },
	{ "CLASSIC_NODUALWIELD", MPOPTION_CLASSIC_NODUALWIELD },
	{ "CLASSIC_IFRAMES",     MPOPTION_CLASSIC_IFRAMES },
	{ "CLASSIC_NOBLUR",      MPOPTION_CLASSIC_NOBLUR },
	// MPOPTION_HOSTSPECTATOR intentionally not exposed — set by netStartServer in dedicated.
	{ NULL, 0 }
};

// Weapon tokens for the [server] `banned=` key → MPWEAPON_* indices (the
// g_MpWeapons roster, NOT WEAPON_* nums — bans filter g_MpSetup.weapons[]
// which holds these). First entry per id is the canonical name used when
// logging; later entries are accepted aliases.
static const struct namedid s_mpweapons[] = {
	{ "FALCON2",          MPWEAPON_FALCON2 },
	{ "FALCON2SILENCED",  MPWEAPON_FALCON2_SILENCER },
	{ "FALCON2SCOPE",     MPWEAPON_FALCON2_SCOPE },
	{ "MAGSEC4",          MPWEAPON_MAGSEC4 },
	{ "MAULER",           MPWEAPON_MAULER },
	{ "PHOENIX",          MPWEAPON_PHOENIX },
	{ "DY357MAGNUM",      MPWEAPON_DY357MAGNUM },
	{ "MAGNUM",           MPWEAPON_DY357MAGNUM },
	{ "DY357LX",          MPWEAPON_DY357LX },
	{ "CMP150",           MPWEAPON_CMP150 },
	{ "CYCLONE",          MPWEAPON_CYCLONE },
	{ "CALLISTO",         MPWEAPON_CALLISTO },
	{ "RCP120",           MPWEAPON_RCP120 },
	{ "LAPTOPGUN",        MPWEAPON_LAPTOPGUN },
	{ "LAPTOP",           MPWEAPON_LAPTOPGUN },
	{ "DRAGON",           MPWEAPON_DRAGON },
	{ "K7AVENGER",        MPWEAPON_K7AVENGER },
	{ "K7",               MPWEAPON_K7AVENGER },
	{ "AR34",             MPWEAPON_AR34 },
	{ "SUPERDRAGON",      MPWEAPON_SUPERDRAGON },
	{ "SHOTGUN",          MPWEAPON_SHOTGUN },
	{ "REAPER",           MPWEAPON_REAPER },
	{ "SNIPERRIFLE",      MPWEAPON_SNIPERRIFLE },
	{ "SNIPER",           MPWEAPON_SNIPERRIFLE },
	{ "FARSIGHT",         MPWEAPON_FARSIGHT },
	{ "DEVASTATOR",       MPWEAPON_DEVASTATOR },
	{ "ROCKETLAUNCHER",   MPWEAPON_ROCKETLAUNCHER },
	{ "ROCKET",           MPWEAPON_ROCKETLAUNCHER },
	{ "SLAYER",           MPWEAPON_SLAYER },
	{ "COMBATKNIFE",      MPWEAPON_COMBATKNIFE },
	{ "KNIFE",            MPWEAPON_COMBATKNIFE },
	{ "CROSSBOW",         MPWEAPON_CROSSBOW },
	{ "TRANQUILIZER",     MPWEAPON_TRANQUILIZER },
	{ "TRANQ",            MPWEAPON_TRANQUILIZER },
	{ "GRENADE",          MPWEAPON_GRENADE },
	{ "NBOMB",            MPWEAPON_NBOMB },
	{ "TIMEDMINE",        MPWEAPON_TIMEDMINE },
	{ "PROXIMITYMINE",    MPWEAPON_PROXIMITYMINE },
	{ "PROXYMINE",        MPWEAPON_PROXIMITYMINE },
	{ "REMOTEMINE",       MPWEAPON_REMOTEMINE },
	{ "LASER",            MPWEAPON_LASER },
	{ "XRAYSCANNER",      MPWEAPON_XRAYSCANNER },
	{ "XRAY",             MPWEAPON_XRAYSCANNER },
	{ "NIGHTVISION",      MPWEAPON_NIGHTVISION },
	{ "IRSCANNER",        MPWEAPON_IRSCANNER },
	{ "CLOAKINGDEVICE",   MPWEAPON_CLOAKINGDEVICE },
	{ "CLOAK",            MPWEAPON_CLOAKINGDEVICE },
	{ "COMBATBOOST",      MPWEAPON_COMBATBOOST },
	{ "BOOST",            MPWEAPON_COMBATBOOST },
	{ "SPEEDPILL",        MPWEAPON_COMBATBOOST },
	{ "PP9I",             MPWEAPON_PP9I },
	{ "CC13",             MPWEAPON_CC13 },
	{ "KL01313",          MPWEAPON_KL01313 },
	{ "KF7SPECIAL",       MPWEAPON_KF7SPECIAL },
	{ "KF7",              MPWEAPON_KF7SPECIAL },
	{ "ZZT",              MPWEAPON_ZZT },
	{ "DMC",              MPWEAPON_DMC },
	{ "AR53",             MPWEAPON_AR53 },
	{ "RCP45",            MPWEAPON_RCP45 },
	{ "SHIELD",           MPWEAPON_SHIELD },
	{ NULL, 0 }
};

// Convenience groups for `banned=` — expand to several MPWEAPON_* ids with
// the same suffix applied to every member.
static const s32 s_banGroupGadgets[] = {
	MPWEAPON_XRAYSCANNER, MPWEAPON_NIGHTVISION, MPWEAPON_IRSCANNER,
	MPWEAPON_CLOAKINGDEVICE, -1
};
static const s32 s_banGroupMines[] = {
	MPWEAPON_TIMEDMINE, MPWEAPON_PROXIMITYMINE, MPWEAPON_REMOTEMINE, -1
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

// Option lookup over the 64-bit option table. Returns the option's bit, or 0 if
// the name is unknown (every real option bit is nonzero).
static u64 lookupOptionBit(const char *name)
{
	for (const struct namedoption *p = s_options; p->name; ++p) {
		if (ieq(p->name, name)) return p->bit;
	}
	return 0;
}

// Public name->id lookups, reusing the parser tables above. Used by the admin
// `set` command (net.c) to accept human-readable stage/scenario/option/bot-diff
// names. Stage/scenario/bot-diff return -1 on no match; option returns the
// MPOPTION_* bit or 0.
s32 playlistLookupStage(const char *name)    { return lookupNamedId(s_stages, name, -1); }
s32 playlistLookupScenario(const char *name) { return lookupNamedId(s_scenarios, name, -1); }
s32 playlistLookupBotDiff(const char *name)  { return lookupNamedId(s_botdiffs, name, -1); }
u64 playlistLookupOption(const char *name)   { return lookupOptionBit(name); }

u64 playlistAllOptionBits(void)
{
	u64 m = 0;
	for (const struct namedoption *p = s_options; p->name; ++p) {
		m |= p->bit;
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
		case MPSCENARIO_PAINTROOM: return "GRAFFITI";
		case MPSCENARIO_ZONES: return "ZONES";
		case MPSCENARIO_RACE: return "RACE";
		default: return "?";
	}
}

// Reverse lookup for the bot-difficulty token, for serialization. All of the
// above scenarioName/stageName outputs parse back via the case-insensitive
// table lookups, so the file round-trips.
static const char *botDiffName(u8 d)
{
	for (const struct namedid *p = s_botdiffs; p->name; ++p) {
		if ((u8)p->id == d) { return p->name; }
	}
	return "NORMAL";
}

// Absolute path the playlist was last loaded from, captured in playlistLoad so
// playlistAppendEntryToFile can append admin-saved entries to the same file.
static char s_loadedPath[FS_MAXPATH + 1] = { 0 };

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
static u64 parseOptionList(const char *list)
{
	u64 bits = 0;
	char buf[256];
	strncpy(buf, list, sizeof(buf) - 1);
	buf[sizeof(buf) - 1] = '\0';

	char *save = NULL;
	for (char *tok = strtok(buf, ", \t"); tok; tok = strtok(NULL, ", \t")) {
		const u64 bit = lookupOptionBit(tok);
		if (bit == 0) {
			sysLogPrintf(LOG_WARNING, "playlist: unknown option `%s`", tok);
		} else {
			bits |= bit;
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

// Canonical (first-listed) name for an MPWEAPON_* id, for logging.
static const char *mpweaponBanName(s32 id)
{
	for (const struct namedid *p = s_mpweapons; p->name; ++p) {
		if (p->id == id) return p->name;
	}
	return "?";
}

static void banApplyBits(struct playlist *pl, s32 id, u8 bits)
{
	if (id >= 0 && id < (s32)sizeof(pl->weapon_bans)) {
		pl->weapon_bans[id] |= bits;
	}
}

// Render the ban table as "NAME, NAME:pri, NAME:sec, ..." into buf. Returns
// the number of banned weapons (0 = no bans, buf untouched).
static s32 playlistFormatBans(const struct playlist *pl, char *buf, size_t bufsize)
{
	s32 count = 0;
	size_t len = 0;

	buf[0] = '\0';

	for (s32 id = 0; id < (s32)sizeof(pl->weapon_bans); ++id) {
		const u8 b = pl->weapon_bans[id];
		if (!b) continue;
		// both functions banned == whole ban (matches the apply logic)
		const char *suffix = ((b & PLAYLIST_BAN_WEAPON)
				|| (b & (PLAYLIST_BAN_PRI | PLAYLIST_BAN_SEC)) == (PLAYLIST_BAN_PRI | PLAYLIST_BAN_SEC)) ? ""
				: (b & PLAYLIST_BAN_PRI) ? ":pri" : ":sec";
		const int n = snprintf(buf + len, bufsize - len, "%s%s%s",
				count ? ", " : "", mpweaponBanName(id), suffix);
		if (n < 0 || (size_t)n >= bufsize - len) break; // truncated, stop
		len += (size_t)n;
		++count;
	}

	return count;
}

// Parse one [server] `banned=` value: comma-separated weapon tokens, each
// with an optional :pri / :sec function suffix (no suffix = whole-weapon
// ban). GADGETS / MINES group tokens expand to their members. Multiple
// banned= lines OR together. Modifies `val` in place (strtok-style).
static void parseBannedList(struct playlist *pl, char *val)
{
	char *tok = val;

	while (tok && *tok) {
		char *next = strchr(tok, ',');
		if (next) *next++ = '\0';
		tok = trim(tok);
		if (!*tok) { tok = next; continue; }

		// optional :pri / :sec suffix
		u8 bits = PLAYLIST_BAN_WEAPON;
		char *colon = strchr(tok, ':');
		if (colon) {
			*colon = '\0';
			char *suffix = trim(colon + 1);
			if (ieq(suffix, "PRI") || ieq(suffix, "PRIMARY")) {
				bits = PLAYLIST_BAN_PRI;
			} else if (ieq(suffix, "SEC") || ieq(suffix, "SECONDARY")) {
				bits = PLAYLIST_BAN_SEC;
			} else {
				sysLogPrintf(LOG_WARNING, "playlist: unknown ban suffix `:%s` on `%s`, banning whole weapon", suffix, tok);
			}
			tok = trim(tok);
		}

		if (ieq(tok, "GADGETS")) {
			for (const s32 *id = s_banGroupGadgets; *id >= 0; ++id) banApplyBits(pl, *id, bits);
		} else if (ieq(tok, "MINES")) {
			for (const s32 *id = s_banGroupMines; *id >= 0; ++id) banApplyBits(pl, *id, bits);
		} else {
			const s32 id = lookupNamedId(s_mpweapons, tok, -1);
			if (id < 0) {
				sysLogPrintf(LOG_WARNING, "playlist: unknown weapon `%s` in banned=", tok);
			} else {
				banApplyBits(pl, id, bits);
			}
		}

		tok = next;
	}
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

	// Remember the resolved path so admin `saverotation` can append to it.
	strncpy(s_loadedPath, resolved_path, sizeof(s_loadedPath) - 1);
	s_loadedPath[sizeof(s_loadedPath) - 1] = '\0';

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
			} else if (ieq(key, "banned")) {
				parseBannedList(pl, val);
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
				const u64 bits = parseOptionList(val);
				cur->mp_options |= bits;
				cur->mp_options_mask |= bits;
			} else if (ieq(key, "options_clear")) {
				const u64 bits = parseOptionList(val);
				cur->mp_options &= ~bits;
				cur->mp_options_mask |= bits;
			} else if (ieq(key, "bots")) {
				const s32 v = (s32)strtol(val, NULL, 0);
				cur->bot_count = (u8)(v < 0 ? 0 : v > NET_MAX_BOTS ? NET_MAX_BOTS : v);
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

	{
		char bansbuf[256];
		if (playlistFormatBans(pl, bansbuf, sizeof(bansbuf))) {
			sysLogPrintf(LOG_NOTE, "playlist: banned: %s", bansbuf);
		}
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

s32 playlistPickBallot(const struct playlist *pl, u64 *rng_state, s32 n, s16 *out_indices)
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
		out_indices[written++] = (s16)idx;
	}

	if (randslot >= 0) {
		out_indices[written++] = (s16)-1; // RANDOM sentinel
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
		// MPSCENARIO_COUNT, not a hardcoded 6 — RANDOM could never pick
		// Graffiti/Zones/Race.
		out->scenario = (s8)(((u32)sysGetMicroseconds()) % (u32)MPSCENARIO_COUNT);
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
	// port-only flags like MPOPTION_GOLDENEYE / the high-word
	// MPOPTION_CLASSIC_* set that a previous menu-driven Combat Sim session
	// may have saved into mpsetups.bin — from silently leaking into dedicated-
	// server matches. `options_clear=` remains parsed for back-compat but
	// is now redundant: any bit not in `options=` is already 0.
	//
	// MPOPTION_HOSTSPECTATOR is preserved across applies because it's a
	// host-session flag (set by netStartServer in dedicated mode, by
	// menuhandlerHostStart in host-and-play with the Host Spectator menu
	// option) — not a per-match toggle. Clearing it here would break
	// SVC_LOBBY_STATE / SVC_STAGE_START's spectator-status broadcast. Everything
	// else — including the high-word port options like MPOPTION_NODOORS (bit 32) —
	// is authoritative from the playlist: bits listed in `options=` are ON, the
	// rest OFF. resolved->mp_options is a full 64-bit value.
	const u64 sticky = MPOPTION_HOSTSPECTATOR;
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
	for (s32 i = 0; i < NET_MAX_BOTS; ++i) {
		g_MpSetup.chrslots &= ~MPCHRSLOT(i + MAX_PLAYERS);
		g_BotConfigsArray[i].base.name[0] = '\0';
	}
	const u8 prof = (resolved->bot_difficulty <= BOTDIFF_DARK)
			? resolved->bot_difficulty : (u8)BOTDIFF_NORMAL;
	for (s32 i = 0; i < resolved->bot_count && i < NET_MAX_BOTS; ++i) {
		mpCreateBotFromProfile(i, prof);
	}
	g_BotCount = resolved->bot_count;
}

void playlistApplyWeaponBans(void)
{
	const struct playlist *pl = &g_NetPlaylist;

	// Bans are a host/server policy: never run on a client (its slots are
	// wire-authoritative from SVC_STAGE_START) and never offline.
	if (g_NetMode != NETMODE_SERVER) {
		return;
	}

	for (s32 i = 0; i < NUM_MPWEAPONSLOTS; ++i) {
		const u8 mw = g_MpSetup.weapons[i];
		u8 b;

		if (mw >= sizeof(pl->weapon_bans)) {
			continue;
		}
		b = pl->weapon_bans[mw];
		if (!b) {
			continue;
		}

		// Both functions banned leaves the weapon unusable — promote to a
		// whole ban (the engine's fn-flag hooks assume never-both, see
		// PORT_WEAPON_PRESETS.md).
		if ((b & PLAYLIST_BAN_WEAPON)
				|| (b & (PLAYLIST_BAN_PRI | PLAYLIST_BAN_SEC)) == (PLAYLIST_BAN_PRI | PLAYLIST_BAN_SEC)) {
			sysLogPrintf(LOG_NOTE, "playlist: banned weapon %s removed from slot %d",
					mpweaponBanName(mw), i);
			g_MpSetup.weapons[i] = MPWEAPON_NONE;
			g_MpSlotFnFlags[i] = 0;
		} else {
			// Function ban: rides the preset fn-flag system. The low ban bits
			// equal FNFLAG_PRIMARY/SECONDARY_DISABLED by definition.
			g_MpSlotFnFlags[i] |= (u8)(b & (PLAYLIST_BAN_PRI | PLAYLIST_BAN_SEC));
			sysLogPrintf(LOG_NOTE, "playlist: banned %s function on %s (slot %d)",
					(b & PLAYLIST_BAN_PRI) ? "primary" : "secondary",
					mpweaponBanName(mw), i);
		}
	}
}

// ---------- serialize (admin saverotation) ----------

s32 playlistAppendEntryToFile(const struct playlistentry *e)
{
	if (!e) {
		return -1;
	}
	if (s_loadedPath[0] == '\0') {
		sysLogPrintf(LOG_WARNING, "playlist: no loaded file to append to");
		return -1;
	}

	FILE *f = fopen(s_loadedPath, "a");
	if (!f) {
		sysLogPrintf(LOG_WARNING, "playlist: cannot append to `%s`", s_loadedPath);
		return -1;
	}

	// Section id is cosmetic (the name= line is authoritative); use a unique
	// suffix so re-saves don't collide.
	fprintf(f, "\n[entry.admin_%llu]\n", (unsigned long long)sysGetMicroseconds());
	fprintf(f, "name        = \"%s\"\n", e->name);
	if (e->stagenum == PLAYLIST_RANDOM_STAGE) {
		fprintf(f, "stage       = RANDOM\n");
	} else {
		fprintf(f, "stage       = %s\n", stageName(e->stagenum));
	}
	if (e->scenario == PLAYLIST_RANDOM_SCENARIO) {
		fprintf(f, "scenario    = RANDOM\n");
	} else {
		fprintf(f, "scenario    = %s\n", scenarioName(e->scenario));
	}
	if (e->preset_name[0]) {
		fprintf(f, "preset      = \"%s\"\n", e->preset_name);
	}
	fprintf(f, "timelimit   = %d\n", (s32)e->timelimit);
	fprintf(f, "scorelimit  = %d\n", (s32)e->scorelimit);
	if (e->teamscorelimit) {
		fprintf(f, "teamscorelimit = %d\n", (s32)e->teamscorelimit);
	}

	// Emit the options that are forced ON (set in both options and mask).
	char opts[256];
	opts[0] = '\0';
	for (const struct namedoption *p = s_options; p->name; ++p) {
		if ((e->mp_options & e->mp_options_mask) & p->bit) {
			if (opts[0]) {
				strncat(opts, ",", sizeof(opts) - strlen(opts) - 1);
			}
			strncat(opts, p->name, sizeof(opts) - strlen(opts) - 1);
		}
	}
	if (opts[0]) {
		fprintf(f, "options     = %s\n", opts);
	}

	fprintf(f, "bots        = %d\n", (s32)e->bot_count);
	fprintf(f, "bot_diff    = %s\n", botDiffName(e->bot_difficulty));
	fprintf(f, "weight      = %d\n", (s32)(e->weight ? e->weight : 1));

	fclose(f);
	sysLogPrintf(LOG_NOTE, "playlist: appended `%s` to %s", e->name, s_loadedPath);
	return 0;
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

	{
		char bansbuf[256];
		if (playlistFormatBans(&g_NetPlaylist, bansbuf, sizeof(bansbuf))) {
			if (g_NetMode) {
				netChatPrintf(NULL, "banned: %s", bansbuf);
			} else {
				sysLogPrintf(LOG_NOTE, "  banned: %s", bansbuf);
			}
		}
	}
}
