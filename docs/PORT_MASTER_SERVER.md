# Master Server, Server Browser & Join Password (port-net-predict)

Port-only server discovery for netplay. Lets testers find running games (listen
*and* dedicated) from an in-game **Server Browser** instead of typing IPs, and
lets hosts gate a game with a password.

This document is the **wire-protocol contract** for the external master server
(the project VPS). The game side lives in:

- `port/src/net/netmaster.c` / `port/include/net/netmaster.h` — heartbeat sender,
  browser socket, list/query parsing, browser state.
- `port/src/net/netmsg.c` — `netmsgQuerySummaryWrite` / `netmsgQueryDetailsWrite`
  (the shared status payloads).
- `port/src/net/net.c` — `netServerQueryResponse` (direct query), `netSendConnectionless`,
  `netServerConnectionlessPacket` (recognises the master magic for REGISTER_ACK).
- `port/src/net/netmenu.c` — the browser/details/password menus.

All multi-byte integers are **little-endian**. Strings are `u16 length`
(including the NUL) followed by `length` bytes ending in `\0` — the
`netbufWriteStr`/`netbufReadStr` format.

---

## Architecture

```
host / dedicated  --HEARTBEAT (every 15s)-->  MASTER (VPS)
   (g_NetMode=SERVER)  <--REGISTER_ACK (opt)--   directory
                                                     ^
   browser client (g_NetMode=NONE) --LIST_REQUEST----+
        |              <--LIST_RESPONSE (directory)---
        |
        +-- PDQM summary query --> each listed server   (true ping + live counts)
        +-- PDQM details query --> selected server       (live scoreboard)
```

The master is a **thin directory**: it never relays game traffic and never sees
join passwords (only the `passworded` flag). The browser measures ping and pulls
live details by querying each game server **directly** over the existing PDQM
protocol, so ping is the tester's own client→server RTT.

The host sends its heartbeat out of the **game server's own UDP socket**
(`netSendConnectionless` → `g_NetHost->socket`), so the heartbeat's source
`ip:port` is exactly what clients connect to. The master should pair the
heartbeat **source IP** with the announced `game_port`.

---

## Shared flags byte

Used in the query summary and the master HEARTBEAT/LIST_RESPONSE:

| bit | mask | meaning |
|-----|------|---------|
| 0 | 0x01 | match in progress (not in lobby) |
| 1 | 0x02 | passworded (join requires a password) |
| 2 | 0x04 | dedicated server |
| 3 | 0x08 | running a Combat Sim challenge |

---

## A. Master protocol — UDP, magic `"PDMS\x01"` (5 bytes), default port **27100**

Every packet is `magic[5]`, `msgtype[1]`, then the payload below. **No app-layer
checksum** (UDP's is relied on) — keep the master simple.

### 0x01 HEARTBEAT  (server → master)

Sent every ~15 s while `Net.Master.Advertise` is set. Payload = the **summary
block** (identical bytes to the direct-query summary, section B) followed by the
game port:

```
u32 protocol_ver
u8  flags
u8  num_clients
u8  max_clients
u8  num_sims
u8  stagenum
u8  scenario
str server_name
str rom_name
str mod_dir
u16 game_port        // the server's listen port; pair with the source IP
```

Master behaviour: upsert keyed by `(source_ip, game_port)`; store the summary
fields; **expire after ~45 s** (3 missed heartbeats). Optionally reply with a
REGISTER_ACK.

### 0x02 UNREGISTER  (server → master)

Best-effort on clean shutdown:

```
u16 game_port
```

Master removes `(source_ip, game_port)`.

### 0x03 LIST_REQUEST  (client → master)

```
u32 protocol_ver
```

Master replies with one or more LIST_RESPONSE packets. The client retransmits
this every ~3 s until it gets a response. You may filter by `protocol_ver` if
you wish (the game does not strictly require it; mismatched servers just fail to
join with a clear message).

### 0x04 LIST_RESPONSE  (master → client)

Keep each datagram under ~1200 bytes; split a large directory across several
packets (the client assembles by address, de-duping).

```
u16 total            // total servers across all packets (informational)
u8  pkt_index        // 0-based
u8  pkt_count        // number of packets in this response
u8  n                // entries in THIS packet
n × {
    str addr         // "ip:port" the client will connect to / direct-query
    u8  flags
    u8  num_clients
    u8  max_clients
    u8  num_sims
    u8  stagenum
    u8  scenario
    str server_name
}
```

`addr` is the connect string the client passes straight to `netStartClient`
(e.g. `"203.0.113.7:27100"` or `"[2001:db8::1]:27100"`). Build it from the
heartbeat source IP + announced `game_port`.

### 0x05 REGISTER_ACK  (master → server, optional)

```
str seen_public_addr   // e.g. "203.0.113.7:27100"
```

The host logs this (handy for the operator to confirm reachability). Sent to the
heartbeat's source, so it arrives on the game socket and is dispatched by
`netServerConnectionlessPacket` → `netMasterHandlePacket`.

---

## B. Direct server query — PDQM, magic `"PDQM\x01"`, on the **game port**

The master does not need to implement this (the game does, both ends), but it is
documented here because it shares the summary block and the flags byte.

**Request** (client → server): the 5-byte magic, optionally followed by one
`u8 querytype` (`0` = summary, `1` = details). A bare 5-byte request = summary
(legacy-compatible).

**Response** (server → client):

```
"PDQM\x01"          // 5 bytes
u16 size             // total packet length incl. magic, this field, and checksum
--- summary block (netmsgQuerySummaryWrite) ---
u32 protocol_ver
u8  flags
u8  num_clients
u8  max_clients
u8  num_sims
u8  stagenum
u8  scenario
str server_name
str rom_name
str mod_dir
--- details block, only when querytype == 1 (netmsgQueryDetailsWrite) ---
u8  scorelimit
u8  timelimit
u16 teamscorelimit
u8  num_players
num_players × { str name; u16 ping; u8 team; s16 score; s16 deaths }
u8  num_sims
num_sims × { str name; u8 team; u8 difficulty; s16 score }
--- ---
u16 checksum         // 16-bit CRC over all preceding bytes (size-2 of them)
```

The checksum is the same routine as before (see `netServerQueryResponse` in
`net.c` and `checksum()` in `tools/query.py`). `tools/query.py [--details] <addr>`
exercises both forms.

---

## C. Join password

A passworded server (`Server.Password` non-empty, or `--password`) advertises
`NET_QF_PASSWORD` but never sends the password. The client supplies it in
`CLC_AUTH` (a trailing `str password`); the server string-compares and rejects a
mismatch with `DISCONNECT_PASSWORD` ("Incorrect password"). The browser prompts
for the password before connecting to a flagged server. ENet is unencrypted, so
this gates access — it is not strong security.

`NET_PROTOCOL_VER` is bumped to **32** for the CLC_AUTH change + new disconnect
reason. The master should advertise/serve only servers reporting the matching
protocol version to its clients.

---

## Configuration (game side, `pd.ini`)

```
Net.Master.Addr        user override; empty (default) = use the baked-in NET_MASTER_DEFAULT_ADDR
Net.Master.Port        master UDP port (default 27100, same as the game port)
Net.Master.Advertise   server registers with the master (0/1, default 1; 0 = don't advertise)
Server.Password        host join password (default empty = open)
```

CLI: `--master <addr>`, `--no-advertise`, `--password <pw>` (plus the existing
`--server-name`, `--port`, `--dedicated`).

> `NET_MASTER_DEFAULT_ADDR` in `port/include/net/netmaster.h` is the baked-in VPS
> IP (`204.152.192.106`). It is **not** written into the ini — only a user
> override is. Testers can repoint to a different master by setting
> `Net.Master.Addr` in `pd.ini`; leaving it blank uses the baked-in IP.

## Out of scope (current version)

- **NAT punch-through / relay** — hosts still need a forwarded/open UDP port.
- **Master anti-spoof / auth** — the master trusts heartbeats; harden VPS-side.
- **TLS** — none; the password gates access only.
