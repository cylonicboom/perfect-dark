#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <PR/ultratypes.h>
#include "platform.h"
#include "types.h"
#include "config.h"
#include "system.h"
#include "net/netenet.h"
#include "net/net.h"
#include "net/netbuf.h"
#include "net/netmsg.h"
#include "net/netmaster.h"

// Glue implemented in net.c. Declared here (not in the ENet-free net.h) because
// the signatures use ENet types. netSendConnectionless sends a connectionless
// datagram out of the *server's* ENet socket, so the master sees the same
// public ip:port that clients connect to (NAT-friendly). netParseAddr splits an
// "ip[:port]" string into an ENetAddress.
extern void netSendConnectionless(const ENetAddress *addr, const void *data, u32 len);
extern s32 netParseAddr(ENetAddress *out, const char *str);

/* config (registered below) */
char g_NetMasterAddr[NET_MAX_ADDR + 1] = NET_MASTER_DEFAULT_ADDR;
u32  g_NetMasterPort = NET_MASTER_DEFAULT_PORT;
s32  g_NetMasterAdvertise = 1;

/* public browser state (read by the menu UI) */
s32 g_NetBrowserState = NETBROWSER_IDLE;
struct netserverentry g_NetServerList[NET_BROWSER_MAX];
s32 g_NetServerCount = 0;
struct netserverdetails g_NetServerDetails;

/* tunables */
#define NET_MASTER_HEARTBEAT_MS 15000u // re-announce cadence; master should expire after ~3 missed (~45s)
#define NET_BROWSER_PING_MS      5000u // re-ping each listed server this often for a live ping/count
#define NET_BROWSER_RETRY_MS     3000u // resend LIST_REQUEST while still empty

/* shared resolved master address */
static ENetAddress s_masterAddr;
static u8 s_masterResolved = 0;

/* server heartbeat state */
static u32 s_hbLastMs = 0;

/* browser transport state */
static ENetSocket s_browserSock = ENET_SOCKET_NULL;
static u32 s_listReqMs = 0;
// Per-entry direct-query bookkeeping, parallel to g_NetServerList[].
static ENetAddress s_entryAddr[NET_BROWSER_MAX];
static u8  s_entryAddrValid[NET_BROWSER_MAX];
static u32 s_entryQueryMs[NET_BROWSER_MAX];
// Which server the Details view is currently bound to (so stray details replies
// from a previously-selected server don't overwrite the active one).
static ENetAddress s_detailsAddr;
static u8 s_detailsAddrValid = 0;

PD_CONSTRUCTOR static void netMasterConfigInit(void)
{
	configRegisterString("Net.Master.Addr", g_NetMasterAddr, sizeof(g_NetMasterAddr) - 1);
	configRegisterUInt("Net.Master.Port", &g_NetMasterPort, 0, 0xFFFF);
	configRegisterInt("Net.Master.Advertise", &g_NetMasterAdvertise, 0, 1);
}

/* helpers */

// Same 16-bit checksum the PDQM query response uses (net.c). Master-protocol
// packets carry no app-layer checksum (UDP's is relied on); only the direct
// query reply is checksummed, and this validates it.
static u16 netQueryCrc(const u8 *data, u32 len)
{
	u16 crc = 0xFFFF;
	u16 x;
	for (u32 i = 0; i < len; ++i) {
		x = crc >> 8 ^ data[i];
		x ^= x >> 4;
		crc += (crc << 8) ^ (x << 12) ^ (x << 5) ^ x;
	}
	return crc;
}

static s32 netAddrEqual(const ENetAddress *a, const ENetAddress *b)
{
	return a->port == b->port && memcmp(&a->ipv6, &b->ipv6, sizeof(a->ipv6)) == 0;
}

// Resolve g_NetMasterAddr:g_NetMasterPort into s_masterAddr once. Blocking DNS,
// but only on the first use per session. Returns 1 on success.
static u8 netMasterResolve(void)
{
	if (s_masterResolved) {
		return 1;
	}
	if (!g_NetMasterAddr[0]) {
		return 0;
	}
	memset(&s_masterAddr, 0, sizeof(s_masterAddr));
	if (enet_address_set_hostname(&s_masterAddr, g_NetMasterAddr) != 0) {
		sysLogPrintf(LOG_WARNING, "NET: could not resolve master server '%s'", g_NetMasterAddr);
		return 0;
	}
	s_masterAddr.port = (u16)g_NetMasterPort;
	s_masterResolved = 1;
	return 1;
}

/* server side: advertise to the master */

void netMasterTick(void)
{
	if (g_NetMode != NETMODE_SERVER) {
		return;
	}
	if (!g_NetMasterAdvertise || !g_NetMasterAddr[0]) {
		return;
	}

	// Gate on the heartbeat cadence BEFORE resolving. Stamp the attempt time
	// even on failure so an unresolvable master (bad/placeholder addr, no
	// network) retries at most once per interval — not a blocking getaddrinfo
	// every frame.
	const u32 now = enet_time_get();
	if (s_hbLastMs != 0 && (now - s_hbLastMs) < NET_MASTER_HEARTBEAT_MS) {
		return;
	}
	s_hbLastMs = now ? now : 1u;

	if (!netMasterResolve()) {
		return;
	}

	u8 data[512];
	struct netbuf buf = { .data = data, .size = sizeof(data) };
	netbufStartWrite(&buf);
	netbufWriteData(&buf, NET_MASTER_MAGIC, sizeof(NET_MASTER_MAGIC) - 1);
	netbufWriteU8(&buf, NET_MASTER_MSG_HEARTBEAT);
	netmsgQuerySummaryWrite(&buf); // proto, flags, counts, game type, map, names
	netbufWriteU16(&buf, g_NetServerActualPort ? g_NetServerActualPort : (u16)g_NetServerPort);
	if (!buf.error) {
		netSendConnectionless(&s_masterAddr, buf.data, buf.wp);
	}
}

void netMasterUnregister(void)
{
	if (g_NetMode != NETMODE_SERVER || !s_masterResolved) {
		return;
	}
	if (!g_NetMasterAdvertise || !g_NetMasterAddr[0]) {
		return;
	}

	u8 data[16];
	struct netbuf buf = { .data = data, .size = sizeof(data) };
	netbufStartWrite(&buf);
	netbufWriteData(&buf, NET_MASTER_MAGIC, sizeof(NET_MASTER_MAGIC) - 1);
	netbufWriteU8(&buf, NET_MASTER_MSG_UNREGISTER);
	netbufWriteU16(&buf, g_NetServerActualPort ? g_NetServerActualPort : (u16)g_NetServerPort);
	if (!buf.error) {
		netSendConnectionless(&s_masterAddr, buf.data, buf.wp);
	}

	s_hbLastMs = 0; // a fresh server in this process re-registers immediately
}

void netMasterHandlePacket(const u8 *data, s32 len)
{
	if (len < 6 || memcmp(data, NET_MASTER_MAGIC, sizeof(NET_MASTER_MAGIC) - 1) != 0) {
		return;
	}
	if (data[5] == NET_MASTER_MSG_REGISTER_ACK) {
		struct netbuf buf;
		netbufStartReadData(&buf, data, len);
		netbufReadSkip(&buf, 6); // magic + type
		const char *seen = netbufReadStr(&buf);
		if (!buf.error && seen && seen[0]) {
			sysLogPrintf(LOG_NOTE, "NET: master sees this server as %s", seen);
		}
	}
}

/* client side: server browser */

static void netBrowserResetList(void)
{
	memset(g_NetServerList, 0, sizeof(g_NetServerList));
	memset(s_entryAddrValid, 0, sizeof(s_entryAddrValid));
	memset(s_entryQueryMs, 0, sizeof(s_entryQueryMs));
	g_NetServerCount = 0;
}

static void netBrowserSendQuery(const ENetAddress *addr, u8 querytype)
{
	u8 pkt[6];
	memcpy(pkt, NET_QUERY_MAGIC, sizeof(NET_QUERY_MAGIC) - 1);
	pkt[5] = querytype;
	ENetBuffer eb;
	eb.data = pkt;
	eb.dataLength = sizeof(pkt);
	enet_socket_send(s_browserSock, addr, &eb, 1);
}

static void netBrowserSendListRequest(void)
{
	if (!netMasterResolve()) {
		g_NetBrowserState = NETBROWSER_ERROR;
		return;
	}

	u8 pkt[16];
	struct netbuf buf = { .data = pkt, .size = sizeof(pkt) };
	netbufStartWrite(&buf);
	netbufWriteData(&buf, NET_MASTER_MAGIC, sizeof(NET_MASTER_MAGIC) - 1);
	netbufWriteU8(&buf, NET_MASTER_MSG_LIST_REQUEST);
	netbufWriteU32(&buf, NET_PROTOCOL_VER);

	ENetBuffer eb;
	eb.data = buf.data;
	eb.dataLength = buf.wp;
	enet_socket_send(s_browserSock, &s_masterAddr, &eb, 1);

	s_listReqMs = enet_time_get();
	if (g_NetBrowserState != NETBROWSER_LISTED) {
		g_NetBrowserState = NETBROWSER_REQUESTING;
	}
}

static s32 netBrowserFindEntry(const ENetAddress *from)
{
	for (s32 i = 0; i < g_NetServerCount; i++) {
		if (s_entryAddrValid[i] && netAddrEqual(&s_entryAddr[i], from)) {
			return i;
		}
	}
	return -1;
}

static void netBrowserAddOrUpdate(const char *addr, u8 flags, u8 nc, u8 mc, u8 ns,
		u8 stage, u8 scen, const char *name)
{
	if (!addr || !addr[0]) {
		return;
	}

	s32 idx = -1;
	for (s32 i = 0; i < g_NetServerCount; i++) {
		if (strcmp(g_NetServerList[i].addr, addr) == 0) {
			idx = i;
			break;
		}
	}

	if (idx < 0) {
		if (g_NetServerCount >= NET_BROWSER_MAX) {
			return;
		}
		idx = g_NetServerCount++;
		memset(&g_NetServerList[idx], 0, sizeof(g_NetServerList[idx]));
		strncpy(g_NetServerList[idx].addr, addr, NET_MAX_ADDR);
		g_NetServerList[idx].addr[NET_MAX_ADDR] = '\0';
		g_NetServerList[idx].ping = NET_PING_PENDING;
		s_entryAddrValid[idx] = (u8)(netParseAddr(&s_entryAddr[idx], addr) ? 1 : 0);
		s_entryQueryMs[idx] = 0; // ping ASAP on the next tick
	}

	// Adopt the master's snapshot only until a direct reply gives live data
	// (a direct reply sets ping != NET_PING_PENDING).
	if (g_NetServerList[idx].ping == NET_PING_PENDING) {
		struct netserverentry *e = &g_NetServerList[idx];
		e->flags = flags;
		e->num_clients = nc;
		e->max_clients = mc;
		e->num_sims = ns;
		e->stagenum = stage;
		e->scenario = scen;
		strncpy(e->name, name ? name : "", NET_BROWSER_NAME_LEN - 1);
		e->name[NET_BROWSER_NAME_LEN - 1] = '\0';
	}
}

static void netBrowserParseList(const u8 *data, s32 len)
{
	struct netbuf buf;
	netbufStartReadData(&buf, data, len);
	netbufReadSkip(&buf, 6); // magic + type
	(void)netbufReadU16(&buf); // total (informational)
	(void)netbufReadU8(&buf);  // pkt_index
	(void)netbufReadU8(&buf);  // pkt_count
	const u8 n = netbufReadU8(&buf);

	for (u8 i = 0; i < n; i++) {
		const char *addr = netbufReadStr(&buf);
		const u8 flags = netbufReadU8(&buf);
		const u8 nc = netbufReadU8(&buf);
		const u8 mc = netbufReadU8(&buf);
		const u8 ns = netbufReadU8(&buf);
		const u8 stage = netbufReadU8(&buf);
		const u8 scen = netbufReadU8(&buf);
		const char *name = netbufReadStr(&buf);
		if (buf.error) {
			break;
		}
		netBrowserAddOrUpdate(addr, flags, nc, mc, ns, stage, scen, name);
	}

	if (g_NetBrowserState == NETBROWSER_REQUESTING) {
		g_NetBrowserState = NETBROWSER_LISTED;
	}
}

static void netBrowserParseQuery(const u8 *data, s32 len, const ENetAddress *from)
{
	const u16 size = (u16)(data[5] | (data[6] << 8));
	if (size < 9 || (s32)size > len) {
		return;
	}
	const u16 crcStored = (u16)(data[size - 2] | (data[size - 1] << 8));
	if (netQueryCrc(data, (u32)(size - 2)) != crcStored) {
		return;
	}

	struct netbuf buf;
	netbufStartReadData(&buf, data, (u32)(size - 2)); // readable region excludes the checksum
	netbufReadSkip(&buf, 5);    // magic
	(void)netbufReadU16(&buf);  // size field
	(void)netbufReadU32(&buf);  // protocol_ver (join enforces the match)
	const u8 flags = netbufReadU8(&buf);
	const u8 nc = netbufReadU8(&buf);
	const u8 mc = netbufReadU8(&buf);
	const u8 ns = netbufReadU8(&buf);
	const u8 stage = netbufReadU8(&buf);
	const u8 scen = netbufReadU8(&buf);
	const char *name = netbufReadStr(&buf);
	(void)netbufReadStr(&buf); // rom name
	(void)netbufReadStr(&buf); // mod dir
	if (buf.error) {
		return;
	}

	const u32 now = enet_time_get();
	const s32 idx = netBrowserFindEntry(from);
	if (idx >= 0) {
		struct netserverentry *e = &g_NetServerList[idx];
		e->flags = flags;
		e->num_clients = nc;
		e->max_clients = mc;
		e->num_sims = ns;
		e->stagenum = stage;
		e->scenario = scen;
		strncpy(e->name, name ? name : "", NET_BROWSER_NAME_LEN - 1);
		e->name[NET_BROWSER_NAME_LEN - 1] = '\0';
		const u32 sent = s_entryQueryMs[idx];
		const u32 rtt = (sent && now >= sent) ? (now - sent) : 0u;
		e->ping = (rtt >= NET_PING_PENDING) ? (u16)(NET_PING_PENDING - 1) : (u16)rtt;
	}

	// A details block follows the summary only when we asked for it (querytype
	// DETAILS), which we only do for the Details view's selected server.
	if (netbufReadLeft(&buf) <= 0) {
		return;
	}

	const u8 committed = (u8)(s_detailsAddrValid && netAddrEqual(from, &s_detailsAddr));

	struct netserverdetails d;
	memset(&d, 0, sizeof(d));
	d.valid = 1;
	if (idx >= 0) {
		strncpy(d.addr, g_NetServerList[idx].addr, NET_MAX_ADDR);
	}
	strncpy(d.name, name ? name : "", NET_BROWSER_NAME_LEN - 1);
	d.flags = flags;
	d.stagenum = stage;
	d.scenario = scen;
	d.scorelimit = netbufReadU8(&buf);
	d.timelimit = netbufReadU8(&buf);
	d.teamscorelimit = netbufReadU16(&buf);

	const u8 np = netbufReadU8(&buf);
	for (u8 i = 0; i < np; i++) {
		const char *pn = netbufReadStr(&buf);
		const u16 png = netbufReadU16(&buf);
		const u8 tm = netbufReadU8(&buf);
		const s16 sc = netbufReadS16(&buf);
		const s16 dt = netbufReadS16(&buf);
		if (buf.error) {
			return;
		}
		if (i < NET_MAX_CLIENTS) {
			strncpy(d.players[i].name, pn ? pn : "", NET_MAX_NAME - 1);
			d.players[i].ping = png;
			d.players[i].team = tm;
			d.players[i].score = sc;
			d.players[i].deaths = dt;
		}
	}
	d.num_players = (np < NET_MAX_CLIENTS) ? np : (u8)NET_MAX_CLIENTS;

	const u8 nb = netbufReadU8(&buf);
	for (u8 i = 0; i < nb; i++) {
		const char *bn = netbufReadStr(&buf);
		const u8 tm = netbufReadU8(&buf);
		const u8 df = netbufReadU8(&buf);
		const s16 sc = netbufReadS16(&buf);
		if (buf.error) {
			return;
		}
		if (i < MAX_BOTS) {
			strncpy(d.sims[i].name, bn ? bn : "", NET_MAX_NAME - 1);
			d.sims[i].team = tm;
			d.sims[i].difficulty = df;
			d.sims[i].score = sc;
		}
	}
	d.num_sims = (nb < MAX_BOTS) ? nb : (u8)MAX_BOTS;

	if (!buf.error && committed) {
		g_NetServerDetails = d;
	}
}

void netBrowserOpen(void)
{
	if (s_browserSock != ENET_SOCKET_NULL) {
		return; // already open
	}

	s_browserSock = enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM);
	if (s_browserSock == ENET_SOCKET_NULL) {
		sysLogPrintf(LOG_ERROR, "NET: server browser socket create failed");
		g_NetBrowserState = NETBROWSER_ERROR;
		return;
	}
	// enet_socket_create() makes a PF_INET6 socket; clear V6ONLY (as
	// enet_host_create does) so we can reach IPv4 servers / master via
	// v4-mapped addresses. Without this, sends to IPv4 hosts silently fail.
	enet_socket_set_option(s_browserSock, ENET_SOCKOPT_IPV6_V6ONLY, 0);
	enet_socket_set_option(s_browserSock, ENET_SOCKOPT_NONBLOCK, 1);
	enet_socket_set_option(s_browserSock, ENET_SOCKOPT_BROADCAST, 1);

	s_masterResolved = 0; // re-resolve in case Net.Master.Addr changed
	s_detailsAddrValid = 0;
	g_NetServerDetails.valid = 0;
	g_NetBrowserState = NETBROWSER_IDLE;

	netBrowserResetList();
	netBrowserSendListRequest();
}

void netBrowserClose(void)
{
	if (s_browserSock != ENET_SOCKET_NULL) {
		enet_socket_destroy(s_browserSock);
		s_browserSock = ENET_SOCKET_NULL;
	}
	s_detailsAddrValid = 0;
	g_NetServerDetails.valid = 0;
	g_NetBrowserState = NETBROWSER_IDLE;
}

void netBrowserRefresh(void)
{
	if (s_browserSock == ENET_SOCKET_NULL) {
		return;
	}
	netBrowserResetList();
	g_NetServerDetails.valid = 0;
	g_NetBrowserState = NETBROWSER_REQUESTING;
	netBrowserSendListRequest();
}

s32 netBrowserQueryDetails(s32 index)
{
	if (index < 0 || index >= g_NetServerCount || s_browserSock == ENET_SOCKET_NULL) {
		return -1;
	}
	if (!s_entryAddrValid[index]) {
		return -1;
	}
	s_detailsAddr = s_entryAddr[index];
	s_detailsAddrValid = 1;
	// Stamp the query time so the reply (which carries the summary prefix too)
	// yields a correct ping for this entry, not one measured against the older
	// background summary ping.
	s_entryQueryMs[index] = enet_time_get();
	netBrowserSendQuery(&s_entryAddr[index], NET_QUERYTYPE_DETAILS);
	return 0;
}

void netBrowserTick(void)
{
	if (s_browserSock == ENET_SOCKET_NULL) {
		return;
	}

	const u32 now = enet_time_get();

	// 1) drain all pending datagrams (master list pages + direct query replies)
	for (;;) {
		static u8 rxbuf[1024];
		ENetAddress from;
		ENetBuffer eb;
		memset(&from, 0, sizeof(from));
		eb.data = rxbuf;
		eb.dataLength = sizeof(rxbuf);
		const int r = enet_socket_receive(s_browserSock, &from, &eb, 1);
		if (r <= 0) {
			break;
		}
		if (r >= 6 && memcmp(rxbuf, NET_MASTER_MAGIC, sizeof(NET_MASTER_MAGIC) - 1) == 0) {
			if (rxbuf[5] == NET_MASTER_MSG_LIST_RESPONSE) {
				netBrowserParseList(rxbuf, r);
			}
		} else if (r >= 9 && memcmp(rxbuf, NET_QUERY_MAGIC, sizeof(NET_QUERY_MAGIC) - 1) == 0) {
			netBrowserParseQuery(rxbuf, r, &from);
		}
	}

	// 2) keep asking the master until we have a list
	if (g_NetBrowserState == NETBROWSER_REQUESTING && (now - s_listReqMs) >= NET_BROWSER_RETRY_MS) {
		netBrowserSendListRequest();
	}

	// 3) direct-ping each listed server on a cadence (gives true client ping +
	//    live slot counts independent of the master's snapshot)
	for (s32 i = 0; i < g_NetServerCount; i++) {
		if (!s_entryAddrValid[i]) {
			continue;
		}
		if (s_entryQueryMs[i] == 0 || (now - s_entryQueryMs[i]) >= NET_BROWSER_PING_MS) {
			netBrowserSendQuery(&s_entryAddr[i], NET_QUERYTYPE_SUMMARY);
			s_entryQueryMs[i] = now ? now : 1u;
		}
	}
}
