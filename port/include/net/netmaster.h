#ifndef _IN_NETMASTER_H
#define _IN_NETMASTER_H

// Master-server registration + in-game server browser (port-only).
//
// The master server is an external UDP tracker (the project's VPS). Running
// game servers (listen and dedicated) periodically announce themselves to it
// with a HEARTBEAT; browsing clients ask it for the directory with a
// LIST_REQUEST and receive one or more LIST_RESPONSE packets. The browser then
// queries each listed server *directly* over the existing PDQM protocol to
// measure true client->server ping and (on the Details view) pull a live
// scoreboard. The master is therefore a thin directory; it never relays game
// traffic and never sees join passwords (only the NET_QF_PASSWORD flag).
//
// Wire format for the master protocol is documented in docs/PORT_MASTER_SERVER.md
// (the implementation contract for the VPS side). This header is intentionally
// free of ENet types so menu code can include it without pulling in enet.h —
// the parsed ENetAddress + per-entry ping timing live privately in netmaster.c.

#include "net/net.h"
#include "net/netmsg.h" // NET_QF_* flag bits shared with the query summary

#define NET_MASTER_MAGIC "PDMS\x01"
#define NET_MASTER_DEFAULT_PORT 27100 // same number as the game port — one port to open/remember

// Compile-time default master host — the project's VPS. Used unless the user
// overrides it via Net.Master.Addr in pd.ini (empty there = use this) or the
// --master CLI flag. This baked-in value is intentionally NOT written into the
// ini; only a user-supplied override is. An unresolvable value yields an empty
// browser and no advertising (no crash).
#define NET_MASTER_DEFAULT_ADDR "204.152.192.106"

// Master message type byte (follows the 5-byte magic).
#define NET_MASTER_MSG_HEARTBEAT     0x01 // server -> master: advertise/keep-alive
#define NET_MASTER_MSG_UNREGISTER    0x02 // server -> master: clean shutdown
#define NET_MASTER_MSG_LIST_REQUEST  0x03 // client -> master: ask for directory
#define NET_MASTER_MSG_LIST_RESPONSE 0x04 // master -> client: directory page
#define NET_MASTER_MSG_REGISTER_ACK  0x05 // master -> server: optional, reports public addr
// "Host Online Game" extension: the master spawns a dedicated instance for the
// requester and hands back its address + a one-off admin token. Old masters
// silently drop these opcodes (the request just times out with a clear message).
#define NET_MASTER_MSG_HOST_REQUEST  0x06 // client -> master: u32 proto, str name, u8 maxplayers, str join_password
#define NET_MASTER_MSG_HOST_GRANT    0x07 // master -> client: str addr ("ip:port"), str admin_token
#define NET_MASTER_MSG_HOST_DENY     0x08 // master -> client: str reason

// Browser list capacity and per-entry name length. Client-side storage only (the
// master feeds entries one at a time, capped by a >= NET_BROWSER_MAX guard), so
// this is free to grow - more listed servers, no wire-format impact.
#define NET_BROWSER_MAX        256
#define NET_BROWSER_NAME_LEN   64

// Sentinel ping (ms) meaning "queried, no reply yet / timed out".
#define NET_PING_PENDING 0xFFFF

// Browser state (drives the menu status line).
#define NETBROWSER_IDLE       0 // socket closed
#define NETBROWSER_REQUESTING 1 // LIST_REQUEST sent, awaiting first response
#define NETBROWSER_LISTED     2 // have a (possibly partial) list; pinging servers
#define NETBROWSER_ERROR      3 // socket create / master resolve failed

// Host-request state (drives the "Host Online Game" wait dialog).
#define NETHOSTREQ_IDLE       0 // socket closed
#define NETHOSTREQ_REQUESTING 1 // HOST_REQUEST sent, retransmitting until a reply
#define NETHOSTREQ_GRANTED    2 // grant received; addr + token below are valid
#define NETHOSTREQ_DENIED     3 // master refused; reason below
#define NETHOSTREQ_ERROR      4 // socket create / resolve failed, or timed out

#define NET_HOSTREQ_REASON_LEN 64

// One browser list row. Display + connect fields only; the parsed address and
// ping-timing state are kept privately in netmaster.c (parallel arrays).
struct netserverentry {
	char addr[NET_MAX_ADDR + 1]; // "ip:port" — used to connect and to direct-query
	char name[NET_BROWSER_NAME_LEN];
	u8  flags;                   // NET_QF_*
	u8  num_clients;
	u8  max_clients;
	u8  num_sims;
	u8  stagenum;
	u8  scenario;
	u16 ping;                    // ms, or NET_PING_PENDING
};

// Live "Details" view, filled from a direct NET_QUERYTYPE_DETAILS query.
struct netserverdetailplayer {
	char name[NET_MAX_NAME];
	u16 ping;
	u8  team;
	s16 score;
	s16 deaths;
};

struct netserverdetailsim {
	char name[NET_MAX_NAME];
	u8  team;
	u8  difficulty;
	s16 score;
};

struct netserverdetails {
	u8  valid;                   // 0 until the first details reply lands
	char addr[NET_MAX_ADDR + 1]; // which server these details are for
	char name[NET_BROWSER_NAME_LEN];
	u8  flags;
	u8  stagenum;
	u8  scenario;
	u8  scorelimit;
	u8  timelimit;
	u16 teamscorelimit;
	u8  num_players;
	struct netserverdetailplayer players[NET_MAX_CLIENTS];
	u8  num_sims;
	struct netserverdetailsim sims[NET_MAX_BOTS];
	// Server's mod dir basename from the query summary ("" = vanilla), plus
	// whether it matches ours (join auth rejects a mod-dir mismatch, so the
	// Details view can warn before the player tries to connect). The match is
	// computed at parse time in netmaster.c — netmenu.c can't safely include
	// netmsg.h for netModDirName (the bool/include-order trap).
	char mod[64];
	u8  modmatch;
};

// Runtime config (registered in netmaster.c). g_NetMasterAddr empty disables
// both advertising and browsing.
extern char g_NetMasterAddr[NET_MAX_ADDR + 1];
extern u32  g_NetMasterPort;
extern s32  g_NetMasterAdvertise;

// Browser state, populated list, and the current Details payload.
extern s32 g_NetBrowserState;
extern struct netserverentry g_NetServerList[NET_BROWSER_MAX];
extern s32 g_NetServerCount;
extern struct netserverdetails g_NetServerDetails;

/* server side (host / dedicated) */

// Per-frame heartbeat pump. Call from netEndFrame on the server path. Sends a
// HEARTBEAT to the master every NET_MASTER_HEARTBEAT_MS and immediately on a
// state change, when Net.Master.Advertise is set and a master is configured.
void netMasterTick(void);

// Best-effort UNREGISTER on server shutdown. Call from netDisconnect.
void netMasterUnregister(void);

// Handle a connectionless reply from the master (e.g. REGISTER_ACK), dispatched
// from netServerConnectionlessPacket after the PDMS magic matches.
void netMasterHandlePacket(const u8 *data, s32 len);

/* client side (browser) */

// Open the browser: create the standalone non-blocking UDP socket and send a
// LIST_REQUEST to the master. Safe to call when already open (no-op).
void netBrowserOpen(void);

// Close the browser: destroy the socket and clear transient state. The parsed
// list is retained so a re-open is instant; call from the dialog's close path.
void netBrowserClose(void);

// Per-frame poll. Drains the socket (master list pages + direct query replies),
// updates pings, and re-pings listed servers periodically. Call once per frame
// while the browser or details dialog is open.
void netBrowserTick(void);

// Re-send the LIST_REQUEST and re-ping every known server ("Refresh" button).
void netBrowserRefresh(void);

// Fire a direct NET_QUERYTYPE_DETAILS query at list entry `index`. The reply
// fills g_NetServerDetails (valid flips to 1). Returns 0 on success, -1 if the
// index is out of range or the socket is not open. The details dialog calls
// this on open and ~1 Hz thereafter for a live scoreboard.
s32 netBrowserQueryDetails(s32 index);

/* client side (Host Online Game request) */

// State machine + results for the host request (see NETHOSTREQ_*). On GRANTED,
// addr is the instance to netStartClient() into and token is the one-off admin
// password for the auto `/admin login`. On DENIED/ERROR, reason is displayable.
extern s32 g_NetHostRequestState;
extern char g_NetHostGrantAddr[NET_MAX_ADDR + 1];
extern char g_NetHostGrantToken[NET_MAX_PASSWORD];
extern char g_NetHostDenyReason[NET_HOSTREQ_REASON_LEN];

// Open a standalone socket and send a HOST_REQUEST to the master (the browser
// socket pattern). name/maxplayers/password describe the wanted server; the
// request retransmits every ~3s until a reply or the ~20s timeout.
void netHostRequestOpen(const char *name, s32 maxplayers, const char *password);

// Per-frame poll while the wait dialog is open: drains the socket, parses
// GRANT/DENY, retransmits, and times out into NETHOSTREQ_ERROR.
void netHostRequestTick(void);

// Destroy the socket. The GRANTED addr/token (and DENIED reason) survive the
// close so the caller can act on them afterwards.
void netHostRequestClose(void);

#endif // _IN_NETMASTER_H
