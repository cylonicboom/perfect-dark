/**
 * Archipelago transport bridge (port-only).
 *
 * Moves BYTES <-> JSON TEXT only: a non-blocking TCP socket + a minimal
 * WebSocket client (handshake + frame masking/reassembly). All Archipelago
 * protocol logic and JSON parsing live in Lua (scripts/ap/client.lua +
 * json.lua); this file is deliberately ignorant of the AP message schema.
 *
 * Lives entirely in C statics so the connection survives the per-stage
 * lua_State teardown (luaaiReset -> lua_close), exactly like g_ApUnlocks and
 * the persist KV. The pump (apTransportTick) is driven from luaTick(), which
 * runs every frame including in menus, so you can connect from the
 * mission-select screen and stay serviced across stage loads.
 *
 * Lua surface (registered into the `pd` table):
 *   pd.ap_connect(url)   "ws://host:port[/path]"  -> bool   (non-blocking)
 *   pd.ap_status()       -> "disconnected"|"connecting"|"handshaking"|
 *                           "connected"|"error"
 *   pd.ap_send(text)     -> bool      queue one text frame
 *   pd.ap_poll()         -> string|nil   next decoded inbound text message
 *   pd.ap_disconnect()
 *
 * P1a: ws:// only. wss:// (mbedTLS) is layered in behind this same byte API in
 * P1b -- see docs/archipelago_blueprint.md §6.5.
 */
#include <ultra64.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "constants.h"
#include "types.h"
#include "game/luaai.h"
#include "net/netenet.h"
#include "lua.h"
#include "lauxlib.h"

#ifndef PLATFORM_N64

/* ------------------------------------------------------------------------- *
 * Growable byte buffer
 * ------------------------------------------------------------------------- */
struct apbuf {
	u8 *data;
	u32 len;
	u32 cap;
};

static void apbufEnsure(struct apbuf *b, u32 extra)
{
	if (b->len + extra <= b->cap) {
		return;
	}
	u32 ncap = b->cap ? b->cap : 1024;
	while (ncap < b->len + extra) {
		ncap *= 2;
	}
	b->data = realloc(b->data, ncap);
	b->cap = ncap;
}

static void apbufAppend(struct apbuf *b, const void *p, u32 n)
{
	if (!n) {
		return;
	}
	apbufEnsure(b, n);
	memcpy(b->data + b->len, p, n);
	b->len += n;
}

/* Drop the first n bytes, shifting the remainder down. */
static void apbufConsume(struct apbuf *b, u32 n)
{
	if (n >= b->len) {
		b->len = 0;
		return;
	}
	memmove(b->data, b->data + n, b->len - n);
	b->len -= n;
}

static void apbufFree(struct apbuf *b)
{
	free(b->data);
	b->data = NULL;
	b->len = b->cap = 0;
}

/* ------------------------------------------------------------------------- *
 * Transport state (all C statics -- survive the lua_State teardown)
 * ------------------------------------------------------------------------- */
enum {
	AP_DISCONNECTED = 0,
	AP_CONNECTING,   /* TCP connect in flight */
	AP_HANDSHAKING,  /* HTTP upgrade sent, awaiting 101 */
	AP_CONNECTED,    /* WebSocket open */
	AP_ERROR
};

#define AP_MSGQ_MAX 256          /* pending decoded inbound messages */
#define AP_RX_CHUNK 8192         /* per-recv read size */
#define AP_MSG_LIMIT (4 * 1024 * 1024) /* sanity cap on one assembled message */

static ENetSocket s_sock = ENET_SOCKET_NULL;
static ENetAddress s_apaddr;
static s32 s_state = AP_DISCONNECTED;

static struct apbuf s_rx;        /* raw bytes from socket not yet parsed */
static struct apbuf s_asm;       /* current (possibly fragmented) message */
static struct apbuf s_tx;        /* pending outbound bytes */
static s32 s_fragopcode = -1;    /* opcode of an in-progress fragmented msg */

static char *s_msgq[AP_MSGQ_MAX];
static s32 s_msgqhead;
static s32 s_msgqtail;

static char s_aphost[256];
static char s_path[256];

static u32 s_rng = 0x9e3779b9u;

static u8 apRandByte(void)
{
	s_rng = s_rng * 1103515245u + 12345u;
	return (u8)(s_rng >> 16);
}

/* ------------------------------------------------------------------------- *
 * base64 (encode only -- for Sec-WebSocket-Key)
 * ------------------------------------------------------------------------- */
static void apBase64(const u8 *in, u32 n, char *out)
{
	static const char tbl[] =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	u32 i, o = 0;
	for (i = 0; i + 2 < n; i += 3) {
		u32 v = (in[i] << 16) | (in[i + 1] << 8) | in[i + 2];
		out[o++] = tbl[(v >> 18) & 0x3f];
		out[o++] = tbl[(v >> 12) & 0x3f];
		out[o++] = tbl[(v >> 6) & 0x3f];
		out[o++] = tbl[v & 0x3f];
	}
	if (i < n) {
		u32 v = in[i] << 16;
		if (i + 1 < n) {
			v |= in[i + 1] << 8;
		}
		out[o++] = tbl[(v >> 18) & 0x3f];
		out[o++] = tbl[(v >> 12) & 0x3f];
		out[o++] = (i + 1 < n) ? tbl[(v >> 6) & 0x3f] : '=';
		out[o++] = '=';
	}
	out[o] = '\0';
}

/* ------------------------------------------------------------------------- *
 * Message queue
 * ------------------------------------------------------------------------- */
static void apMsgPush(const u8 *p, u32 n)
{
	s32 next = (s_msgqtail + 1) % AP_MSGQ_MAX;
	if (next == s_msgqhead) {
		/* Queue full -- drop oldest to keep newest (better for liveness). */
		free(s_msgq[s_msgqhead]);
		s_msgq[s_msgqhead] = NULL;
		s_msgqhead = (s_msgqhead + 1) % AP_MSGQ_MAX;
	}
	char *s = malloc(n + 1);
	if (!s) {
		return;
	}
	memcpy(s, p, n);
	s[n] = '\0';
	s_msgq[s_msgqtail] = s;
	s_msgqtail = next;
}

static char *apMsgPop(void)
{
	if (s_msgqhead == s_msgqtail) {
		return NULL;
	}
	char *s = s_msgq[s_msgqhead];
	s_msgq[s_msgqhead] = NULL;
	s_msgqhead = (s_msgqhead + 1) % AP_MSGQ_MAX;
	return s;
}

static void apMsgClear(void)
{
	while (s_msgqhead != s_msgqtail) {
		free(s_msgq[s_msgqhead]);
		s_msgq[s_msgqhead] = NULL;
		s_msgqhead = (s_msgqhead + 1) % AP_MSGQ_MAX;
	}
	s_msgqhead = s_msgqtail = 0;
}

/* ------------------------------------------------------------------------- *
 * Teardown
 * ------------------------------------------------------------------------- */
static void apReset(s32 newstate)
{
	if (s_sock != ENET_SOCKET_NULL) {
		enet_socket_destroy(s_sock);
		s_sock = ENET_SOCKET_NULL;
	}
	s_rx.len = 0;
	s_asm.len = 0;
	s_tx.len = 0;
	s_fragopcode = -1;
	apMsgClear();
	s_state = newstate;
}

/* ------------------------------------------------------------------------- *
 * WebSocket frame I/O
 * ------------------------------------------------------------------------- */
/* Append a masked client frame carrying (op, payload) to the tx buffer. */
static void apWsSendFrame(u8 opcode, const u8 *payload, u32 n)
{
	u8 hdr[14];
	u32 h = 0;
	u8 mask[4];
	u32 i;

	hdr[h++] = 0x80 | (opcode & 0x0f); /* FIN + opcode */

	if (n < 126) {
		hdr[h++] = 0x80 | (u8)n;       /* MASK + len */
	} else if (n <= 0xffff) {
		hdr[h++] = 0x80 | 126;
		hdr[h++] = (u8)(n >> 8);
		hdr[h++] = (u8)n;
	} else {
		hdr[h++] = 0x80 | 127;
		hdr[h++] = 0; hdr[h++] = 0; hdr[h++] = 0; hdr[h++] = 0;
		hdr[h++] = (u8)(n >> 24);
		hdr[h++] = (u8)(n >> 16);
		hdr[h++] = (u8)(n >> 8);
		hdr[h++] = (u8)n;
	}

	mask[0] = apRandByte();
	mask[1] = apRandByte();
	mask[2] = apRandByte();
	mask[3] = apRandByte();
	hdr[h++] = mask[0];
	hdr[h++] = mask[1];
	hdr[h++] = mask[2];
	hdr[h++] = mask[3];

	apbufAppend(&s_tx, hdr, h);

	/* Masked payload. */
	apbufEnsure(&s_tx, n);
	for (i = 0; i < n; i++) {
		s_tx.data[s_tx.len + i] = payload[i] ^ mask[i & 3];
	}
	s_tx.len += n;
}

/* Try to parse one complete server frame out of s_rx. Returns 1 if a frame
 * was consumed (caller should loop), 0 if more bytes are needed, -1 on a
 * protocol error / close. */
static s32 apWsParseOne(void)
{
	u8 *d = s_rx.data;
	u32 avail = s_rx.len;
	u32 hdr;
	u64 plen;
	u8 op, masked;

	if (avail < 2) {
		return 0;
	}

	op = d[0] & 0x0f;
	masked = (d[1] & 0x80) ? 1 : 0;
	plen = d[1] & 0x7f;
	hdr = 2;

	if (plen == 126) {
		if (avail < 4) {
			return 0;
		}
		plen = ((u64)d[2] << 8) | d[3];
		hdr = 4;
	} else if (plen == 127) {
		if (avail < 10) {
			return 0;
		}
		plen = 0;
		for (s32 k = 0; k < 8; k++) {
			plen = (plen << 8) | d[2 + k];
		}
		hdr = 10;
	}

	if (masked) {
		hdr += 4; /* servers shouldn't mask, but tolerate */
	}

	if (plen > AP_MSG_LIMIT) {
		return -1;
	}
	if (avail < hdr + plen) {
		return 0; /* wait for the rest of the frame */
	}

	u8 *pl = d + hdr;
	if (masked) {
		u8 *mk = d + hdr - 4;
		for (u64 k = 0; k < plen; k++) {
			pl[k] ^= mk[k & 3];
		}
	}

	switch (op) {
	case 0x0: /* continuation */
	case 0x1: /* text */
	case 0x2: /* binary */
		if (op != 0x0) {
			s_asm.len = 0;
			s_fragopcode = op;
		}
		if (s_asm.len + (u32)plen > AP_MSG_LIMIT) {
			return -1;
		}
		apbufAppend(&s_asm, pl, (u32)plen);
		if (d[0] & 0x80) { /* FIN */
			apMsgPush(s_asm.data, s_asm.len);
			s_asm.len = 0;
			s_fragopcode = -1;
		}
		break;
	case 0x8: /* close */
		apbufConsume(&s_rx, hdr + (u32)plen);
		return -1;
	case 0x9: /* ping -> pong (echo payload) */
		apWsSendFrame(0xA, pl, (u32)plen);
		break;
	case 0xA: /* pong */
		break;
	default:
		break;
	}

	apbufConsume(&s_rx, hdr + (u32)plen);
	return 1;
}

/* ------------------------------------------------------------------------- *
 * Socket helpers
 * ------------------------------------------------------------------------- */
/* Non-blocking: pull whatever is readable into s_rx. Returns -1 if the peer
 * closed or errored, 0 otherwise. */
static s32 apSockDrainInto(void)
{
	u8 chunk[AP_RX_CHUNK];

	for (;;) {
		u32 cond = ENET_SOCKET_WAIT_RECEIVE;
		if (enet_socket_wait(s_sock, &cond, 0) != 0) {
			return -1;
		}
		if (!(cond & ENET_SOCKET_WAIT_RECEIVE)) {
			return 0; /* nothing more to read this frame */
		}

		ENetBuffer eb;
		eb.data = chunk;
		eb.dataLength = sizeof(chunk);
		int r = enet_socket_receive(s_sock, NULL, &eb, 1);
		if (r > 0) {
			apbufAppend(&s_rx, chunk, (u32)r);
			continue;
		}
		/* r == 0 with RECEIVE ready means orderly close (EOF); r < 0 is an
		 * error. enet_socket_receive returns 0 for BOTH EWOULDBLOCK and EOF,
		 * but we only get here with RECEIVE set, so 0 == EOF. */
		return -1;
	}
}

/* Flush pending tx; returns -1 on error. */
static s32 apSockFlushTx(void)
{
	while (s_tx.len > 0) {
		ENetBuffer eb;
		eb.data = s_tx.data;
		eb.dataLength = s_tx.len;
		int r = enet_socket_send(s_sock, NULL, &eb, 1);
		if (r > 0) {
			apbufConsume(&s_tx, (u32)r);
			continue;
		}
		if (r == 0) {
			return 0; /* EWOULDBLOCK -- try again next frame */
		}
		return -1;
	}
	return 0;
}

static void apSendHttpUpgrade(void)
{
	u8 keyraw[16];
	char keyb64[32];
	char req[640];
	s32 i;

	for (i = 0; i < 16; i++) {
		keyraw[i] = apRandByte();
	}
	apBase64(keyraw, 16, keyb64);

	snprintf(req, sizeof(req),
		"GET %s HTTP/1.1\r\n"
		"Host: %s\r\n"
		"Upgrade: websocket\r\n"
		"Connection: Upgrade\r\n"
		"Sec-WebSocket-Key: %s\r\n"
		"Sec-WebSocket-Version: 13\r\n"
		"\r\n",
		s_path, s_aphost, keyb64);

	apbufAppend(&s_tx, req, (u32)strlen(req));
}

/* ------------------------------------------------------------------------- *
 * Per-frame pump (called from luaTick)
 * ------------------------------------------------------------------------- */
void apTransportTick(void)
{
	if (s_state == AP_DISCONNECTED || s_state == AP_ERROR) {
		return;
	}

	if (s_state == AP_CONNECTING) {
		u32 cond = ENET_SOCKET_WAIT_SEND;
		if (enet_socket_wait(s_sock, &cond, 0) != 0) {
			apReset(AP_ERROR);
			return;
		}
		if (!(cond & ENET_SOCKET_WAIT_SEND)) {
			return; /* still connecting */
		}
		int err = 0;
		if (enet_socket_get_option(s_sock, ENET_SOCKOPT_ERROR, &err) != 0 || err != 0) {
			apReset(AP_ERROR);
			return;
		}
		apSendHttpUpgrade();
		s_state = AP_HANDSHAKING;
	}

	if (apSockFlushTx() < 0) {
		apReset(AP_ERROR);
		return;
	}

	if (apSockDrainInto() < 0) {
		apReset(AP_ERROR);
		return;
	}

	if (s_state == AP_HANDSHAKING) {
		/* Look for the end of the HTTP response headers. */
		if (s_rx.len >= 4) {
			u32 i;
			for (i = 0; i + 3 < s_rx.len; i++) {
				if (s_rx.data[i] == '\r' && s_rx.data[i + 1] == '\n' &&
						s_rx.data[i + 2] == '\r' && s_rx.data[i + 3] == '\n') {
					/* Status line must be "HTTP/1.1 101". */
					s32 ok = (s_rx.len >= 12 &&
							memcmp(s_rx.data, "HTTP/1.1 101", 12) == 0);
					apbufConsume(&s_rx, i + 4);
					if (!ok) {
						apReset(AP_ERROR);
						return;
					}
					s_state = AP_CONNECTED;
					break;
				}
			}
		}
	}

	if (s_state == AP_CONNECTED) {
		s32 r;
		while ((r = apWsParseOne()) == 1) {
			/* keep draining frames */
		}
		if (r < 0) {
			apReset(AP_ERROR);
			return;
		}
	}
}

/* ------------------------------------------------------------------------- *
 * Lua API
 * ------------------------------------------------------------------------- */
/* pd.ap_connect("ws://host:port[/path]") -> bool */
static int l_pd_ap_connect(lua_State *L)
{
	const char *url = luaL_checkstring(L, 1);
	const char *p = url;
	const char *hoststart, *portstart, *pathstart;
	char portbuf[8];
	s32 port = 0;
	s32 i;

	apReset(AP_DISCONNECTED);

	/* scheme */
	if (strncmp(p, "ws://", 5) == 0) {
		p += 5;
	} else if (strncmp(p, "wss://", 6) == 0) {
		/* P1b: TLS not wired yet. */
		fprintf(stderr, "[ap] ap_connect: wss:// not supported yet (P1b); use ws://\n");
		lua_pushboolean(L, 0);
		return 1;
	} else {
		fprintf(stderr, "[ap] ap_connect: url must start with ws:// or wss://\n");
		lua_pushboolean(L, 0);
		return 1;
	}

	hoststart = p;
	while (*p && *p != ':' && *p != '/') {
		p++;
	}
	i = (s32)(p - hoststart);
	if (i <= 0 || i >= (s32)sizeof(s_aphost)) {
		lua_pushboolean(L, 0);
		return 1;
	}
	memcpy(s_aphost, hoststart, i);
	s_aphost[i] = '\0';

	if (*p == ':') {
		p++;
		portstart = p;
		while (*p >= '0' && *p <= '9') {
			p++;
		}
		i = (s32)(p - portstart);
		if (i <= 0 || i >= (s32)sizeof(portbuf)) {
			lua_pushboolean(L, 0);
			return 1;
		}
		memcpy(portbuf, portstart, i);
		portbuf[i] = '\0';
		port = atoi(portbuf);
	} else {
		port = 80;
	}

	pathstart = p;
	if (*pathstart == '\0') {
		strcpy(s_path, "/");
	} else {
		snprintf(s_path, sizeof(s_path), "%s", pathstart);
	}

	/* Resolve + connect (DNS resolve is blocking, but one-shot at menu time). */
	if (enet_address_set_hostname(&s_apaddr, s_aphost) != 0) {
		fprintf(stderr, "[ap] ap_connect: could not resolve host '%s'\n", s_aphost);
		lua_pushboolean(L, 0);
		return 1;
	}
	s_apaddr.port = (u16)port;

	s_sock = enet_socket_create(ENET_SOCKET_TYPE_STREAM);
	if (s_sock == ENET_SOCKET_NULL) {
		lua_pushboolean(L, 0);
		return 1;
	}
	enet_socket_set_option(s_sock, ENET_SOCKOPT_IPV6_V6ONLY, 0);
	enet_socket_set_option(s_sock, ENET_SOCKOPT_NONBLOCK, 1);

	s_rng ^= (u32)(uintptr_t)&s_rng;

	if (enet_socket_connect(s_sock, &s_apaddr) != 0) {
		apReset(AP_ERROR);
		lua_pushboolean(L, 0);
		return 1;
	}
	s_state = AP_CONNECTING;
	lua_pushboolean(L, 1);
	return 1;
}

static int l_pd_ap_status(lua_State *L)
{
	const char *s;
	switch (s_state) {
	case AP_CONNECTING:  s = "connecting";  break;
	case AP_HANDSHAKING: s = "handshaking"; break;
	case AP_CONNECTED:   s = "connected";   break;
	case AP_ERROR:       s = "error";       break;
	default:             s = "disconnected"; break;
	}
	lua_pushstring(L, s);
	return 1;
}

static int l_pd_ap_send(lua_State *L)
{
	size_t n = 0;
	const char *text = luaL_checklstring(L, 1, &n);
	if (s_state != AP_CONNECTED) {
		lua_pushboolean(L, 0);
		return 1;
	}
	apWsSendFrame(0x1, (const u8 *)text, (u32)n);
	lua_pushboolean(L, 1);
	return 1;
}

static int l_pd_ap_poll(lua_State *L)
{
	char *s = apMsgPop();
	if (!s) {
		lua_pushnil(L);
		return 1;
	}
	lua_pushstring(L, s);
	free(s);
	return 1;
}

static int l_pd_ap_disconnect(lua_State *L)
{
	if (s_state == AP_CONNECTED) {
		u8 code[2] = { 0x03, 0xe8 }; /* 1000 normal */
		apWsSendFrame(0x8, code, 2);
		apSockFlushTx();
	}
	apReset(AP_DISCONNECTED);
	(void)L;
	return 0;
}

void luaApiRegisterAp(lua_State *L)
{
	/* pd table is on top of the stack (see luaApiRegister). */
	lua_pushcfunction(L, l_pd_ap_connect);    lua_setfield(L, -2, "ap_connect");
	lua_pushcfunction(L, l_pd_ap_status);     lua_setfield(L, -2, "ap_status");
	lua_pushcfunction(L, l_pd_ap_send);       lua_setfield(L, -2, "ap_send");
	lua_pushcfunction(L, l_pd_ap_poll);       lua_setfield(L, -2, "ap_poll");
	lua_pushcfunction(L, l_pd_ap_disconnect); lua_setfield(L, -2, "ap_disconnect");
}

#endif /* !PLATFORM_N64 */
