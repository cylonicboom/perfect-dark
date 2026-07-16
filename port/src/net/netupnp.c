#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
// netenet.h must precede types.h: types.h does `#define bool s32`, and
// netenet.h `#undef bool` afterwards. Because types.h is include-guarded, its
// definition only runs once — so netenet.h has to come before the first time
// types.h is pulled in (here, via net.h). This mirrors net.c/netmaster.c.
#include "platform.h"
#include "net/netenet.h"
#include "net/net.h"
#include "net/netupnp.h"
#include "types.h"
#include "config.h"
#include "system.h"

// Client-hosted UPnP port forwarding. See netupnp.h for the overview. The
// protocol against the router is three ordinary exchanges:
//
//   1. DISCOVER — SSDP: multicast an HTTP-ish "M-SEARCH" datagram to
//      239.255.255.250:1900 asking for an InternetGatewayDevice; the router
//      unicasts back a response whose LOCATION header is the URL of its
//      device description document.
//   2. DESCRIBE — plain HTTP GET of that URL; the XML lists the device's
//      services, and we want the control URL of WANIPConnection (or
//      WANPPPConnection on DSL gateways).
//   3. MAPPING / GETIP / DELETING — SOAP POSTs to that control URL:
//      AddPortMapping (external UDP port -> our LAN IP), GetExternalIPAddress
//      (informational), DeletePortMapping (cleanup on server stop / quit).
//
// All of it is pumped non-blocking from netUpnpTick so a missing/slow gateway
// costs nothing but a few quiet log lines. HTTP is spoken as HTTP/1.0 with
// Connection: close on purpose: 1.0 forbids chunked transfer encoding, so the
// response is simply "headers + body until Content-Length or EOF" and no
// chunked decoder is needed (miniupnpd, AVM, Broadcom and friends all accept
// 1.0). Responses are parsed with plain string scanning — the subset of XML
// that IGD descriptions use is flat enough that a real parser buys nothing.

/* config (registered below) + public state */
s32 g_NetUpnpEnabled = 1;
s32 g_NetUpnpState = NETUPNP_IDLE;
char g_NetUpnpExternalIP[46] = "";

/* tunables */
#define NETUPNP_SSDP_ADDR       "239.255.255.250" // SSDP well-known multicast group
#define NETUPNP_SSDP_PORT       1900
#define NETUPNP_SSDP_ROUND_MS   2500u // per M-SEARCH round: MX is 2s, +0.5s slack
#define NETUPNP_SSDP_ROUNDS     3     // rounds before giving up (no gateway)
#define NETUPNP_HTTP_TIMEOUT_MS 4000u // per HTTP/SOAP round trip
#define NETUPNP_LEASE_SECS      3600u // requested mapping lease
#define NETUPNP_RENEW_MS        (NETUPNP_LEASE_SECS * 1000u / 2u) // re-add at half-life
#define NETUPNP_MAX_TRIED       4     // description URLs to try per discovery

/* gateway endpoints (filled by DISCOVER / DESCRIBE) */
static char s_descHost[64];  // description document host (dotted IP)
static u16  s_descPort;
static char s_descPath[192];
static char s_ctrlHost[64];  // WAN*Connection control endpoint
static u16  s_ctrlPort;
static char s_ctrlPath[256]; // fits any extracted <controlURL> (224) + "/" prefix
static char s_serviceType[80]; // e.g. "urn:schemas-upnp-org:service:WANIPConnection:1"
static char s_lanIP[46];       // our address on the gateway's subnet (getsockname
                               // on the connected DESCRIBE socket) = InternalClient

/* mapping state */
static u16 s_mapPort;        // UDP port being mapped (external == internal)
static u8  s_leaseZero;      // gateway rejected timed leases (SOAP 725); using 0
static u8  s_mapAttempted;   // an AddPortMapping POST went out — delete on stop
static u8  s_mapActive;      // AddPortMapping confirmed OK
static u8  s_usedCachedCtrl; // skipped discovery via cached control URL — on
                             // failure, clear the cache and rediscover once
static u8  s_renewing;       // current MAPPING pass is a lease renewal
static u32 s_renewAtMs;

/* SSDP transport */
static ENetSocket s_ssdpSock = ENET_SOCKET_NULL;
static u32 s_ssdpSentMs;
static s32 s_ssdpRound;
static char s_triedLoc[NETUPNP_MAX_TRIED][224]; // description URLs already tried
static s32 s_triedCount;

/* HTTP transport (one request in flight at a time) */
#define NETUPNP_HTTP_OFF        0
#define NETUPNP_HTTP_CONNECTING 1
#define NETUPNP_HTTP_SENDING    2
#define NETUPNP_HTTP_RECEIVING  3

static ENetSocket s_httpSock = ENET_SOCKET_NULL;
static u8  s_httpPhase = NETUPNP_HTTP_OFF;
static u32 s_httpStartMs;
static char s_httpReq[2048];
static u32 s_httpReqLen, s_httpReqSent;
// Response accumulator. IGD description documents are usually a few KB but
// some vendors (AVM Fritz!Box) ship tens of KB — 64K covers everything seen
// in the wild; a longer response is truncated, which at worst fails the
// service scan and reads as "no gateway".
static char s_httpBuf[65536];
static u32 s_httpLen;

PD_CONSTRUCTOR static void netUpnpConfigInit(void)
{
	configRegisterInt("Net.UPnP.Enabled", &g_NetUpnpEnabled, 0, 1);
}

/* string helpers */

// Case-insensitive strstr (HTTP header names are case-insensitive and routers
// genuinely vary: "LOCATION:", "Location:"). Not locale-sensitive.
static const char *netUpnpStrCaseStr(const char *hay, const char *needle)
{
	const size_t nlen = strlen(needle);
	if (!nlen) {
		return hay;
	}
	for (; *hay; ++hay) {
		size_t i = 0;
		while (i < nlen && hay[i]
				&& tolower((unsigned char)hay[i]) == tolower((unsigned char)needle[i])) {
			++i;
		}
		if (i == nlen) {
			return hay;
		}
	}
	return NULL;
}

// Split "http://host[:port]/path" into components. Returns 1 on success.
static s32 netUpnpParseUrl(const char *url, char *host, u32 hostlen, u16 *port, char *path, u32 pathlen)
{
	if (strncmp(url, "http://", 7) != 0) {
		return 0;
	}
	const char *h = url + 7;
	const char *pend = h;
	while (*pend && *pend != ':' && *pend != '/') {
		++pend;
	}
	if ((u32)(pend - h) == 0 || (u32)(pend - h) >= hostlen) {
		return 0;
	}
	memcpy(host, h, pend - h);
	host[pend - h] = '\0';

	u32 p = 80;
	if (*pend == ':') {
		p = (u32)atoi(pend + 1);
		if (p == 0 || p > 0xFFFF) {
			return 0;
		}
		while (*pend && *pend != '/') {
			++pend;
		}
	}
	*port = (u16)p;

	if (*pend == '/') {
		strncpy(path, pend, pathlen - 1);
		path[pathlen - 1] = '\0';
	} else {
		strcpy(path, "/");
	}
	return 1;
}

// Extract the text between `tag` open/close within [block, blockend). The tag
// match is on the bare name so attributes on the open tag are tolerated.
// Returns 1 and NUL-terminates into out on success.
static s32 netUpnpXmlText(const char *block, const char *blockend, const char *tag, char *out, u32 outlen)
{
	char open[48];
	snprintf(open, sizeof(open), "<%s", tag);
	const char *p = block;
	while ((p = strstr(p, open)) != NULL && p < blockend) {
		const char *q = p + strlen(open);
		if (*q != '>' && *q != ' ') { // partial tag-name match (e.g. <serviceTypeFoo)
			p = q;
			continue;
		}
		q = strchr(q, '>');
		if (!q || q >= blockend) {
			return 0;
		}
		++q;
		const char *e = strstr(q, "</");
		if (!e || e > blockend) {
			return 0;
		}
		u32 len = (u32)(e - q);
		if (len >= outlen) {
			len = outlen - 1;
		}
		memcpy(out, q, len);
		out[len] = '\0';
		return 1;
	}
	return 0;
}

/* HTTP transport */

static void netUpnpHttpAbort(void)
{
	if (s_httpSock != ENET_SOCKET_NULL) {
		enet_socket_destroy(s_httpSock);
		s_httpSock = ENET_SOCKET_NULL;
	}
	s_httpPhase = NETUPNP_HTTP_OFF;
}

// Kick off a request to host:port. The full request text (headers + body) is
// already in s_httpReq. Returns 1 on success (pump with netUpnpHttpTick).
static s32 netUpnpHttpBegin(const char *host, u16 port)
{
	netUpnpHttpAbort();

	ENetAddress addr;
	memset(&addr, 0, sizeof(addr));
	if (enet_address_set_hostname(&addr, host) != 0) { // dotted IP in practice — no DNS stall
		return 0;
	}
	addr.port = port;

	s_httpSock = enet_socket_create(ENET_SOCKET_TYPE_STREAM);
	if (s_httpSock == ENET_SOCKET_NULL) {
		return 0;
	}
	enet_socket_set_option(s_httpSock, ENET_SOCKOPT_NONBLOCK, 1);

	if (enet_socket_connect(s_httpSock, &addr) < 0) { // in-progress returns 0
		netUpnpHttpAbort();
		return 0;
	}

	s_httpReqLen = (u32)strlen(s_httpReq);
	s_httpReqSent = 0;
	s_httpLen = 0;
	s_httpBuf[0] = '\0';
	s_httpPhase = NETUPNP_HTTP_CONNECTING;
	s_httpStartMs = enet_time_get();
	return 1;
}

// Response complete? Trust Content-Length when present (some routers keep the
// connection open a beat after the body); otherwise wait for EOF.
static s32 netUpnpHttpComplete(void)
{
	const char *hdrend = strstr(s_httpBuf, "\r\n\r\n");
	if (!hdrend) {
		return 0;
	}
	const char *cl = netUpnpStrCaseStr(s_httpBuf, "content-length:");
	if (cl && cl < hdrend) {
		const u32 bodylen = (u32)atoi(cl + 15);
		return (s_httpLen - (u32)(hdrend + 4 - s_httpBuf)) >= bodylen;
	}
	return 0;
}

// Pump the in-flight request. Returns 0 = still working, 1 = response complete
// (in s_httpBuf, NUL-terminated), -1 = failed / timed out.
static s32 netUpnpHttpTick(void)
{
	if (s_httpPhase == NETUPNP_HTTP_OFF) {
		return -1;
	}
	if (enet_time_get() - s_httpStartMs > NETUPNP_HTTP_TIMEOUT_MS) {
		netUpnpHttpAbort();
		return -1;
	}

	if (s_httpPhase == NETUPNP_HTTP_CONNECTING) {
		u32 cond = ENET_SOCKET_WAIT_SEND;
		if (enet_socket_wait(s_httpSock, &cond, 0) != 0) {
			netUpnpHttpAbort();
			return -1;
		}
		if (!(cond & ENET_SOCKET_WAIT_SEND)) {
			return 0;
		}
		int err = 0;
		if (enet_socket_get_option(s_httpSock, ENET_SOCKOPT_ERROR, &err) != 0 || err != 0) {
			netUpnpHttpAbort();
			return -1;
		}
		// Connected. Remember our own address on this route — for the gateway
		// description fetch this is exactly the LAN IP the router should
		// forward to (InternalClient in AddPortMapping).
		ENetAddress local;
		if (enet_socket_get_address(s_httpSock, &local) == 0) {
			enet_address_get_ip(&local, s_lanIP, sizeof(s_lanIP));
		}
		s_httpPhase = NETUPNP_HTTP_SENDING;
	}

	if (s_httpPhase == NETUPNP_HTTP_SENDING) {
		while (s_httpReqSent < s_httpReqLen) {
			ENetBuffer eb;
			eb.data = s_httpReq + s_httpReqSent;
			eb.dataLength = s_httpReqLen - s_httpReqSent;
			const int r = enet_socket_send(s_httpSock, NULL, &eb, 1);
			if (r < 0) {
				netUpnpHttpAbort();
				return -1;
			}
			if (r == 0) { // would block — resume next tick
				return 0;
			}
			s_httpReqSent += (u32)r;
		}
		s_httpPhase = NETUPNP_HTTP_RECEIVING;
	}

	// RECEIVING. enet_socket_receive returns 0 both for would-block and for
	// orderly close, so only read when poll/select says readable — then a
	// 0-byte read really is EOF.
	for (;;) {
		u32 cond = ENET_SOCKET_WAIT_RECEIVE;
		if (enet_socket_wait(s_httpSock, &cond, 0) != 0) {
			netUpnpHttpAbort();
			return -1;
		}
		if (!(cond & ENET_SOCKET_WAIT_RECEIVE)) {
			return 0;
		}
		if (s_httpLen >= sizeof(s_httpBuf) - 1) { // full: treat as complete
			netUpnpHttpAbort();
			return 1;
		}
		ENetBuffer eb;
		eb.data = s_httpBuf + s_httpLen;
		eb.dataLength = sizeof(s_httpBuf) - 1 - s_httpLen;
		const int r = enet_socket_receive(s_httpSock, NULL, &eb, 1);
		if (r < 0) {
			netUpnpHttpAbort();
			return -1;
		}
		if (r == 0) { // readable + 0 bytes = peer closed
			netUpnpHttpAbort();
			s_httpBuf[s_httpLen] = '\0';
			return s_httpLen ? 1 : -1;
		}
		s_httpLen += (u32)r;
		s_httpBuf[s_httpLen] = '\0';
		if (netUpnpHttpComplete()) {
			netUpnpHttpAbort();
			return 1;
		}
	}
}

// HTTP status code of the response in s_httpBuf ("HTTP/1.x NNN ..."), 0 if unparsable.
static s32 netUpnpHttpStatus(void)
{
	if (strncmp(s_httpBuf, "HTTP/", 5) != 0) {
		return 0;
	}
	const char *sp = strchr(s_httpBuf, ' ');
	return sp ? atoi(sp + 1) : 0;
}

/* SOAP */

// Build a SOAP POST into s_httpReq. `args` is the pre-rendered inner argument
// XML (no escaping needed — every value we send is numeric, an IP, or a
// constant). The SOAPAction service type must be the one the gateway declared
// (WANIPConnection:1/:2 or WANPPPConnection:1), not a hardcoded string.
static void netUpnpSoapBuild(const char *action, const char *args)
{
	char body[1024];
	snprintf(body, sizeof(body),
			"<?xml version=\"1.0\"?>\r\n"
			"<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\""
			" s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
			"<s:Body><u:%s xmlns:u=\"%s\">%s</u:%s></s:Body></s:Envelope>\r\n",
			action, s_serviceType, args, action);

	snprintf(s_httpReq, sizeof(s_httpReq),
			"POST %s HTTP/1.0\r\n"
			"Host: %s:%u\r\n"
			"User-Agent: PerfectDark UPnP/1.0\r\n"
			"Content-Type: text/xml; charset=\"utf-8\"\r\n"
			"SOAPAction: \"%s#%s\"\r\n"
			"Content-Length: %u\r\n"
			"Connection: close\r\n\r\n%s",
			s_ctrlPath, s_ctrlHost, s_ctrlPort, s_serviceType, action,
			(u32)strlen(body), body);
}

static s32 netUpnpSoapBeginAddMapping(void)
{
	char args[512];
	snprintf(args, sizeof(args),
			"<NewRemoteHost></NewRemoteHost>"
			"<NewExternalPort>%u</NewExternalPort>"
			"<NewProtocol>UDP</NewProtocol>"
			"<NewInternalPort>%u</NewInternalPort>"
			"<NewInternalClient>%s</NewInternalClient>"
			"<NewEnabled>1</NewEnabled>"
			"<NewPortMappingDescription>PerfectDark netplay</NewPortMappingDescription>"
			"<NewLeaseDuration>%u</NewLeaseDuration>",
			s_mapPort, s_mapPort, s_lanIP, s_leaseZero ? 0u : NETUPNP_LEASE_SECS);
	netUpnpSoapBuild("AddPortMapping", args);
	return netUpnpHttpBegin(s_ctrlHost, s_ctrlPort);
}

static s32 netUpnpSoapBeginDeleteMapping(void)
{
	char args[192];
	snprintf(args, sizeof(args),
			"<NewRemoteHost></NewRemoteHost>"
			"<NewExternalPort>%u</NewExternalPort>"
			"<NewProtocol>UDP</NewProtocol>",
			s_mapPort);
	netUpnpSoapBuild("DeletePortMapping", args);
	return netUpnpHttpBegin(s_ctrlHost, s_ctrlPort);
}

static s32 netUpnpSoapBeginGetExternalIP(void)
{
	netUpnpSoapBuild("GetExternalIPAddress", "");
	return netUpnpHttpBegin(s_ctrlHost, s_ctrlPort);
}

// UPnP SOAP fault code (<errorCode> in a 500 response), 0 if none found.
static s32 netUpnpSoapErrorCode(void)
{
	const char *p = strstr(s_httpBuf, "<errorCode>");
	return p ? atoi(p + 11) : 0;
}

/* SSDP discovery */

static void netUpnpSsdpClose(void)
{
	if (s_ssdpSock != ENET_SOCKET_NULL) {
		enet_socket_destroy(s_ssdpSock);
		s_ssdpSock = ENET_SOCKET_NULL;
	}
}

static void netUpnpSsdpSearch(void)
{
	// Search for both IGD generations — a v2-only gateway need not answer a
	// v1 ST. Each is also sent to the limited-broadcast address as a hedge
	// for stacks where v4-mapped multicast doesn't route (the router's SSDP
	// listener commonly hears 255.255.255.255:1900 too; a no-op otherwise).
	static const char *const sts[2] = {
		"urn:schemas-upnp-org:device:InternetGatewayDevice:1",
		"urn:schemas-upnp-org:device:InternetGatewayDevice:2",
	};

	ENetAddress mcast, bcast;
	memset(&mcast, 0, sizeof(mcast));
	memset(&bcast, 0, sizeof(bcast));
	enet_address_set_hostname(&mcast, NETUPNP_SSDP_ADDR);
	enet_address_set_hostname(&bcast, "255.255.255.255");
	mcast.port = NETUPNP_SSDP_PORT;
	bcast.port = NETUPNP_SSDP_PORT;

	for (s32 i = 0; i < 2; ++i) {
		char pkt[256];
		const int len = snprintf(pkt, sizeof(pkt),
				"M-SEARCH * HTTP/1.1\r\n"
				"HOST: %s:%u\r\n"
				"MAN: \"ssdp:discover\"\r\n"
				"MX: 2\r\n"
				"ST: %s\r\n\r\n",
				NETUPNP_SSDP_ADDR, NETUPNP_SSDP_PORT, sts[i]);
		ENetBuffer eb;
		eb.data = pkt;
		eb.dataLength = (size_t)len;
		enet_socket_send(s_ssdpSock, &mcast, &eb, 1);
		enet_socket_send(s_ssdpSock, &bcast, &eb, 1);
	}

	s_ssdpSentMs = enet_time_get();
	++s_ssdpRound;
}

static s32 netUpnpSsdpOpen(void)
{
	if (s_ssdpSock != ENET_SOCKET_NULL) {
		return 1;
	}
	s_ssdpSock = enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM);
	if (s_ssdpSock == ENET_SOCKET_NULL) {
		return 0;
	}
	// Same options as the browser socket: v4-mapped sends need V6ONLY off,
	// never block the frame, and allow the broadcast hedge above.
	enet_socket_set_option(s_ssdpSock, ENET_SOCKOPT_IPV6_V6ONLY, 0);
	enet_socket_set_option(s_ssdpSock, ENET_SOCKOPT_NONBLOCK, 1);
	enet_socket_set_option(s_ssdpSock, ENET_SOCKOPT_BROADCAST, 1);
	s_ssdpRound = 0;
	s_triedCount = 0;
	return 1;
}

// Drain SSDP replies; returns 1 when a NEW (untried) description URL has been
// parsed into s_descHost/Port/Path.
static s32 netUpnpSsdpPoll(void)
{
	for (;;) {
		static char rx[1500];
		ENetAddress from;
		ENetBuffer eb;
		memset(&from, 0, sizeof(from));
		eb.data = rx;
		eb.dataLength = sizeof(rx) - 1;
		const int r = enet_socket_receive(s_ssdpSock, &from, &eb, 1);
		if (r <= 0) {
			return 0;
		}
		rx[r] = '\0';

		const char *loc = netUpnpStrCaseStr(rx, "location:");
		if (!loc) {
			continue;
		}
		loc += 9;
		while (*loc == ' ' || *loc == '\t') {
			++loc;
		}
		char url[224];
		u32 n = 0;
		while (loc[n] && loc[n] != '\r' && loc[n] != '\n' && n < sizeof(url) - 1) {
			url[n] = loc[n];
			++n;
		}
		url[n] = '\0';

		s32 tried = 0;
		for (s32 i = 0; i < s_triedCount; ++i) {
			if (strcmp(s_triedLoc[i], url) == 0) {
				tried = 1;
				break;
			}
		}
		if (tried || s_triedCount >= NETUPNP_MAX_TRIED) {
			continue;
		}
		if (!netUpnpParseUrl(url, s_descHost, sizeof(s_descHost), &s_descPort,
				s_descPath, sizeof(s_descPath))) {
			continue;
		}
		strcpy(s_triedLoc[s_triedCount++], url);
		return 1;
	}
}

/* description document -> control URL */

static s32 netUpnpParseDescription(const char *xml)
{
	// Walk every <service> block; prefer WANIPConnection (Ethernet/fibre
	// gateways) over WANPPPConnection (DSL), and take the first hit of each.
	const char *bestblock = NULL, *bestend = NULL;
	char besttype[80] = "";
	s32 bestisip = 0;

	const char *p = xml;
	while ((p = strstr(p, "<service>")) != NULL) {
		const char *end = strstr(p, "</service>");
		if (!end) {
			break;
		}
		char svctype[80];
		if (netUpnpXmlText(p, end, "serviceType", svctype, sizeof(svctype))) {
			const s32 isip = strstr(svctype, ":WANIPConnection:") != NULL;
			const s32 isppp = strstr(svctype, ":WANPPPConnection:") != NULL;
			if ((isip || isppp) && (!bestblock || (isip && !bestisip))) {
				bestblock = p;
				bestend = end;
				bestisip = isip;
				strcpy(besttype, svctype);
			}
		}
		p = end + 10;
	}

	if (!bestblock) {
		return 0;
	}

	char ctrl[224];
	if (!netUpnpXmlText(bestblock, bestend, "controlURL", ctrl, sizeof(ctrl))) {
		return 0;
	}

	strcpy(s_serviceType, besttype);

	if (strncmp(ctrl, "http://", 7) == 0) {
		return netUpnpParseUrl(ctrl, s_ctrlHost, sizeof(s_ctrlHost), &s_ctrlPort,
				s_ctrlPath, sizeof(s_ctrlPath));
	}

	// Relative control URL: same host/port as the description document.
	strcpy(s_ctrlHost, s_descHost);
	s_ctrlPort = s_descPort;
	if (ctrl[0] == '/') {
		strncpy(s_ctrlPath, ctrl, sizeof(s_ctrlPath) - 1);
	} else {
		snprintf(s_ctrlPath, sizeof(s_ctrlPath), "/%s", ctrl);
	}
	s_ctrlPath[sizeof(s_ctrlPath) - 1] = '\0';
	return 1;
}

/* state machine */

static void netUpnpFail(const char *why)
{
	netUpnpSsdpClose();
	netUpnpHttpAbort();
	g_NetUpnpState = NETUPNP_FAILED;
	sysLogPrintf(LOG_NOTE, "NET: UPnP: %s — port %u must be forwarded manually if joiners can't connect",
			why, s_mapPort);
}

static void netUpnpBeginDescribe(void)
{
	snprintf(s_httpReq, sizeof(s_httpReq),
			"GET %s HTTP/1.0\r\n"
			"Host: %s:%u\r\n"
			"User-Agent: PerfectDark UPnP/1.0\r\n"
			"Connection: close\r\n\r\n",
			s_descPath, s_descHost, s_descPort);
	if (netUpnpHttpBegin(s_descHost, s_descPort)) {
		g_NetUpnpState = NETUPNP_DESCRIBE;
	}
	// On failure stay in DISCOVER — the round timer / other replies decide.
}

static void netUpnpBeginMapping(s32 renewing)
{
	s_renewing = (u8)renewing;
	s_mapAttempted = 1;
	if (netUpnpSoapBeginAddMapping()) {
		g_NetUpnpState = NETUPNP_MAPPING;
	} else {
		netUpnpFail("could not contact gateway control endpoint");
	}
}

void netUpnpStart(u16 port)
{
	if (!g_NetUpnpEnabled || port == 0) {
		return;
	}

	// Hard-reset any leftover work (a DELETING in flight is abandoned: the
	// remap below re-adds the same external port, superseding the old entry).
	netUpnpSsdpClose();
	netUpnpHttpAbort();

	s_mapPort = port;
	s_mapAttempted = 0;
	s_mapActive = 0;
	s_leaseZero = 0;
	g_NetUpnpExternalIP[0] = '\0';

	// Control endpoint already known from an earlier mapping this session?
	// Skip discovery — re-hosting maps instantly. If the gateway went away
	// the SOAP failure path clears the cache and rediscovers once.
	if (s_ctrlPath[0] && s_lanIP[0]) {
		s_usedCachedCtrl = 1;
		netUpnpBeginMapping(0);
		return;
	}
	s_usedCachedCtrl = 0;

	if (!netUpnpSsdpOpen()) {
		netUpnpFail("SSDP socket create failed");
		return;
	}
	g_NetUpnpState = NETUPNP_DISCOVER;
	sysLogPrintf(LOG_NOTE, "NET: UPnP: searching for a gateway to forward UDP port %u...", port);
	netUpnpSsdpSearch();
}

void netUpnpStop(void)
{
	netUpnpSsdpClose();

	if (s_mapAttempted && s_ctrlPath[0]) {
		// Delete even if the ADD was only in flight when we stopped — deleting
		// a mapping that never landed is a harmless SOAP fault (714).
		netUpnpHttpAbort();
		if (netUpnpSoapBeginDeleteMapping()) {
			g_NetUpnpState = NETUPNP_DELETING;
			return;
		}
	}

	netUpnpHttpAbort();
	if (g_NetUpnpState != NETUPNP_FAILED) {
		g_NetUpnpState = NETUPNP_IDLE;
	}
}

void netUpnpTick(void)
{
	switch (g_NetUpnpState) {
	case NETUPNP_IDLE:
	case NETUPNP_FAILED:
		return;

	case NETUPNP_DISCOVER: {
		if (netUpnpSsdpPoll()) {
			netUpnpBeginDescribe();
			return;
		}
		if (enet_time_get() - s_ssdpSentMs >= NETUPNP_SSDP_ROUND_MS) {
			if (s_ssdpRound >= NETUPNP_SSDP_ROUNDS) {
				netUpnpFail("no UPnP gateway found");
			} else {
				netUpnpSsdpSearch();
			}
		}
		return;
	}

	case NETUPNP_DESCRIBE: {
		const s32 r = netUpnpHttpTick();
		if (r == 0) {
			return;
		}
		if (r > 0 && netUpnpHttpStatus() == 200 && netUpnpParseDescription(s_httpBuf)) {
			netUpnpSsdpClose();
			sysLogPrintf(LOG_NOTE, "NET: UPnP: gateway found at %s:%u (%s)",
					s_ctrlHost, s_ctrlPort, s_serviceType);
			netUpnpBeginMapping(0);
			return;
		}
		// This candidate was a dud — fall back to discovery for another reply
		// (the round/timeout logic decides when to give up for real).
		g_NetUpnpState = NETUPNP_DISCOVER;
		return;
	}

	case NETUPNP_MAPPING: {
		const s32 r = netUpnpHttpTick();
		if (r == 0) {
			return;
		}
		if (r > 0 && netUpnpHttpStatus() == 200) {
			s_mapActive = 1;
			s_renewAtMs = enet_time_get() + NETUPNP_RENEW_MS;
			if (s_renewing) { // quiet on renewals — nothing changed
				g_NetUpnpState = NETUPNP_MAPPED;
			} else if (netUpnpSoapBeginGetExternalIP()) {
				g_NetUpnpState = NETUPNP_GETIP;
			} else {
				sysLogPrintf(LOG_NOTE, "NET: UPnP: forwarded UDP port %u -> %s", s_mapPort, s_lanIP);
				g_NetUpnpState = NETUPNP_MAPPED;
			}
			return;
		}
		const s32 err = (r > 0) ? netUpnpSoapErrorCode() : 0;
		if (err == 725 && !s_leaseZero) {
			// OnlyPermanentLeasesSupported — retry once with LeaseDuration 0.
			s_leaseZero = 1;
			netUpnpBeginMapping(s_renewing);
			return;
		}
		if (s_usedCachedCtrl) {
			// Cached endpoint from a previous session segment is stale
			// (router rebooted / changed) — forget it and rediscover once.
			s_usedCachedCtrl = 0;
			s_ctrlPath[0] = '\0';
			if (netUpnpSsdpOpen()) {
				g_NetUpnpState = NETUPNP_DISCOVER;
				netUpnpSsdpSearch();
				return;
			}
		}
		if (err) {
			char why[64];
			snprintf(why, sizeof(why), "gateway refused the mapping (error %d)", err);
			netUpnpFail(why);
		} else {
			netUpnpFail("AddPortMapping request failed");
		}
		return;
	}

	case NETUPNP_GETIP: {
		const s32 r = netUpnpHttpTick();
		if (r == 0) {
			return;
		}
		if (r > 0 && netUpnpHttpStatus() == 200) {
			char ip[46];
			if (netUpnpXmlText(s_httpBuf, s_httpBuf + s_httpLen,
					"NewExternalIPAddress", ip, sizeof(ip)) && ip[0]) {
				strcpy(g_NetUpnpExternalIP, ip);
			}
		}
		// Informational only — the mapping is live either way.
		if (g_NetUpnpExternalIP[0]) {
			sysLogPrintf(LOG_CHAT, "NET: UPnP: forwarded UDP port %u -> %s (join address %s:%u)",
					s_mapPort, s_lanIP, g_NetUpnpExternalIP, s_mapPort);
		} else {
			sysLogPrintf(LOG_CHAT, "NET: UPnP: forwarded UDP port %u -> %s", s_mapPort, s_lanIP);
		}
		g_NetUpnpState = NETUPNP_MAPPED;
		return;
	}

	case NETUPNP_MAPPED: {
		// Timed leases are re-added at half-life so the mapping outlives long
		// sessions; permanent (lease-0) mappings are cleaned up on stop/quit.
		if (!s_leaseZero && (s32)(enet_time_get() - s_renewAtMs) >= 0) {
			netUpnpBeginMapping(1);
		}
		return;
	}

	case NETUPNP_DELETING: {
		const s32 r = netUpnpHttpTick();
		if (r == 0) {
			return;
		}
		if (r > 0 && netUpnpHttpStatus() == 200) {
			sysLogPrintf(LOG_NOTE, "NET: UPnP: removed the UDP port %u forward", s_mapPort);
		} else {
			sysLogPrintf(LOG_NOTE, "NET: UPnP: could not remove the port %u forward (router will expire it)", s_mapPort);
		}
		s_mapAttempted = 0;
		s_mapActive = 0;
		g_NetUpnpState = NETUPNP_IDLE;
		return;
	}
	}
}

void netUpnpShutdown(void)
{
	// Nothing mapped (or the async delete already finished): nothing to do.
	if (g_NetUpnpState != NETUPNP_DELETING) {
		if (!s_mapAttempted || !s_ctrlPath[0]) {
			return;
		}
		netUpnpStop(); // arms DELETING when possible
		if (g_NetUpnpState != NETUPNP_DELETING) {
			return;
		}
	}

	// Pump the delete to completion with a hard deadline. The socket wait
	// doubles as the sleep so this isn't a busy loop; worst case ~2s once at
	// process exit, and only when a mapping actually exists.
	const u32 deadline = enet_time_get() + 2000u;
	while (g_NetUpnpState == NETUPNP_DELETING && (s32)(deadline - enet_time_get()) > 0) {
		netUpnpTick();
		if (g_NetUpnpState != NETUPNP_DELETING || s_httpSock == ENET_SOCKET_NULL) {
			break;
		}
		u32 cond = ENET_SOCKET_WAIT_SEND | ENET_SOCKET_WAIT_RECEIVE;
		if (enet_socket_wait(s_httpSock, &cond, 50) != 0) {
			break;
		}
	}
	netUpnpHttpAbort();
}

/* console: /upnp [status|on|off|retry] */

static const char *netUpnpStateName(void)
{
	switch (g_NetUpnpState) {
	case NETUPNP_IDLE:     return "idle";
	case NETUPNP_DISCOVER: return "searching for gateway";
	case NETUPNP_DESCRIBE: return "reading gateway description";
	case NETUPNP_MAPPING:  return "requesting port mapping";
	case NETUPNP_GETIP:    return "querying external IP";
	case NETUPNP_MAPPED:   return "mapped";
	case NETUPNP_DELETING: return "removing mapping";
	case NETUPNP_FAILED:   return "failed";
	default:               return "?";
	}
}

s32 netUpnpConsoleCommand(const char *arg)
{
	if (strcmp(arg, "on") == 0 || strcmp(arg, "off") == 0) {
		const s32 on = (arg[1] == 'n');
		g_NetUpnpEnabled = on;
		if (!on) {
			netUpnpStop(); // deletes an active mapping / aborts work in flight
		} else if (g_NetMode == NETMODE_SERVER && g_NetUpnpState == NETUPNP_IDLE) {
			netUpnpStart(g_NetServerActualPort ? g_NetServerActualPort : (u16)g_NetServerPort);
		}
		sysLogPrintf(LOG_CHAT, "NET: UPnP %s", on ? "enabled" : "disabled");
		return 1;
	}

	if (strcmp(arg, "retry") == 0) {
		if (!g_NetUpnpEnabled) {
			sysLogPrintf(LOG_CHAT, "NET: UPnP is disabled (/upnp on first)");
		} else if (g_NetMode != NETMODE_SERVER) {
			sysLogPrintf(LOG_CHAT, "NET: UPnP retry needs a running server (host first)");
		} else {
			s_ctrlPath[0] = '\0'; // full rediscovery, not the cached endpoint
			netUpnpStart(g_NetServerActualPort ? g_NetServerActualPort : (u16)g_NetServerPort);
		}
		return 1;
	}

	// no arg / "status"
	sysLogPrintf(LOG_CHAT, "NET: UPnP: %s, state: %s",
			g_NetUpnpEnabled ? "enabled" : "disabled", netUpnpStateName());
	if (s_ctrlPath[0]) {
		sysLogPrintf(LOG_CHAT, "NET: UPnP: gateway %s:%u (%s)", s_ctrlHost, s_ctrlPort, s_serviceType);
	}
	if (s_mapActive) {
		sysLogPrintf(LOG_CHAT, "NET: UPnP: UDP %u -> %s:%u (%s lease)%s%s",
				s_mapPort, s_lanIP, s_mapPort,
				s_leaseZero ? "permanent" : "1h renewing",
				g_NetUpnpExternalIP[0] ? ", external IP " : "",
				g_NetUpnpExternalIP);
	}
	return 1;
}
