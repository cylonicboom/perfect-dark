# Client-Hosted UPnP Port Forwarding

When a player hosts a listen server (Combat Simulator → Network Game → Host,
`--host`, or `/coop` hosting) from behind a home router, remote players can't
reach them unless the server's UDP port is forwarded. `port/src/net/netupnp.c`
does that automatically by speaking the UPnP IGD (Internet Gateway Device)
protocol to the router. No new dependency: the whole exchange runs over the
bundled ENet socket layer (UDP datagrams for discovery, TCP streams for the
HTTP/SOAP round trips).

This complements — not replaces — **Host Online Game** (the master spawns a
VPS instance, `PORT_HOSTED_SERVER.md`): UPnP is for hosting on your own
machine with your own bandwidth/latency, which is also the only mode where
the host plays with zero self-ping.

## How it works

Non-blocking state machine, pumped once per frame from `schedEndFrame`
(`netUpnpTick`) — the standalone-socket pattern of the server browser
(`netmaster.c`). Nothing here can hitch the frame; a missing or slow gateway
just times out quietly in the background.

```
netStartServer ──▶ DISCOVER ──▶ DESCRIBE ──▶ MAPPING ──▶ GETIP ──▶ MAPPED
                      │             │           │                    │ (renew @ lease/2)
                      └────────── FAILED ◀──────┘                    │
netDisconnect ────────────────────────────────────────▶ DELETING ──▶ IDLE
```

1. **DISCOVER** — SSDP: an HTTP-ish `M-SEARCH` datagram is multicast to
   `239.255.255.250:1900` (plus a `255.255.255.255` broadcast hedge for
   stacks where v4-mapped multicast doesn't route) asking for
   `InternetGatewayDevice:1` and `:2`. The router unicasts back a response
   whose `LOCATION` header is the URL of its description document. 3 rounds
   of ~2.5s before giving up.
2. **DESCRIBE** — HTTP GET of that URL. The XML lists the device's services;
   we take the control URL of `WANIPConnection` (preferred) or
   `WANPPPConnection` (DSL gateways), either IGD generation. The local
   address of the connected TCP socket (getsockname) is captured here — that
   is exactly the LAN IP the router should forward to.
3. **MAPPING** — SOAP `AddPortMapping`: external UDP port = internal UDP
   port = the game port, internal client = our LAN IP, description
   "PerfectDark netplay", lease 1 hour. If the gateway answers UPnP error
   725 (`OnlyPermanentLeasesSupported`) the request is retried once with
   lease 0 (permanent).
4. **GETIP** — SOAP `GetExternalIPAddress`, informational: the success line
   in the console includes a ready-to-share `join address ip:port`. Failure
   here doesn't matter — the mapping is already live.
5. **MAPPED** — timed leases are re-added at half-life so the mapping
   outlives long sessions; permanent leases aren't renewed.
6. **DELETING** — on `netDisconnect` (stop hosting) the mapping is removed
   with SOAP `DeletePortMapping`, still async: the tick keeps pumping after
   the session ends. On process exit, `netUpnpShutdown` (called from
   `main.c`'s `cleanup`) finishes a pending delete with a bounded blocking
   pump (~2s worst case, and only when a mapping actually exists) so
   quitting doesn't leave a stale forward on the router.

Re-hosting in the same session skips discovery — the control endpoint is
cached; if the cached endpoint has gone stale (router rebooted), the SOAP
failure clears the cache and rediscovers once.

HTTP is spoken as **HTTP/1.0** with `Connection: close` on purpose: 1.0
forbids chunked transfer encoding, so responses are just "headers + body
until Content-Length or EOF" and no chunked decoder is needed. Responses are
parsed with plain string scanning; the flat subset of XML that IGD
descriptions use doesn't justify a parser.

The master-hosted VPS instances also run this path (they call
`netStartServer` too) — there's no gateway to find in a datacenter, so
discovery just times out. `--no-upnp` in the spawn command line would skip
even that.

## Config / CLI / console

```
Net.UPnP.Enabled     # pd.ini, default 1 — forward the server port when hosting
--no-upnp            # CLI: disable for this session
/upnp                # console: status (state, gateway, mapping, external IP)
/upnp on|off         # toggle; off deletes an active mapping
/upnp retry          # full rediscovery + remap (needs a running server)
```

Log lines are prefixed `NET: UPnP:`. Success is a `LOG_CHAT` line
(`forwarded UDP port 27100 -> 192.168.1.10 (join address 203.0.113.7:27100)`);
everything else is `LOG_NOTE`. Failure is soft by design — hosting continues,
LAN players are unaffected, and manual port forwarding still works, so
nothing louder than a note is warranted.

## Limitations

- The gateway must have UPnP/IGD enabled (consumer routers usually ship it
  on; some ISP boxes don't).
- Error 718 (`ConflictInMappingEntry` — the external port is already mapped
  to a *different* machine) is reported and given up on rather than retried
  on another port: the game port is baked into the master heartbeat / join
  flow, so silently moving the external port would do more harm than good.
  Change `Net.Server.Port` instead.
- CGNAT (ISP-level NAT) can't be traversed by UPnP at all — the router's WAN
  IP isn't public. The `GetExternalIPAddress` line makes this diagnosable:
  if the reported external IP is in 100.64/10 or another private range,
  UPnP succeeded but direct hosting still won't work; use Host Online Game.

## Testing

Exercised against a scripted fake gateway (SSDP responder + description +
SOAP endpoints, including the 725 fallback) driving the real state machine on
localhost, plus the no-gateway timeout path. The harness lives outside the
repo (scratch); the quickest manual smoke test on a real network is:
host a match, `/upnp` for status, check the router's port-forward table,
stop hosting, check the entry is gone.
