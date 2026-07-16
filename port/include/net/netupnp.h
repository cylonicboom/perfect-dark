#ifndef _IN_NETUPNP_H
#define _IN_NETUPNP_H

// Client-hosted UPnP port forwarding (port-only).
//
// When a player hosts a listen server from behind a home router, remote
// clients can't reach them unless the server UDP port is forwarded. This
// module talks the UPnP IGD (Internet Gateway Device) protocol to the router
// to open that port automatically: SSDP multicast discovery finds the gateway,
// an HTTP GET of its description document locates the WAN*Connection control
// endpoint, and SOAP AddPortMapping / DeletePortMapping manage the mapping.
//
// Everything runs as a non-blocking state machine pumped once per frame from
// schedEndFrame (netUpnpTick) — the same standalone-socket pattern as the
// server browser in netmaster.c — so hosting never hitches on a slow or
// absent gateway. The whole exchange uses the bundled ENet socket layer
// (UDP datagram for SSDP, TCP stream for the HTTP/SOAP round trips), so
// there is no new dependency and no platform-specific socket code here.
//
// Like netmaster.h, this header is deliberately free of ENet types so any
// caller (menus, main.c) can include it.

#include "types.h"

// State machine phases (g_NetUpnpState, for /upnp status + logging).
#define NETUPNP_IDLE     0 // nothing to do (no mapping, no work in flight)
#define NETUPNP_DISCOVER 1 // SSDP M-SEARCH sent, waiting for a gateway reply
#define NETUPNP_DESCRIBE 2 // fetching the gateway's description XML over HTTP
#define NETUPNP_MAPPING  3 // SOAP AddPortMapping in flight (also lease renewal)
#define NETUPNP_GETIP    4 // SOAP GetExternalIPAddress in flight (informational)
#define NETUPNP_MAPPED   5 // mapping active; renewing periodically if leased
#define NETUPNP_DELETING 6 // SOAP DeletePortMapping in flight (server stopped)
#define NETUPNP_FAILED   7 // gave up (no gateway / gateway refused); /upnp retry

// Runtime config (registered in netupnp.c): Net.UPnP.Enabled in pd.ini,
// default 1. The --no-upnp CLI flag clears it for the session (netInit).
extern s32 g_NetUpnpEnabled;

// Current phase (NETUPNP_*) — read-only outside netupnp.c.
extern s32 g_NetUpnpState;

// External (WAN) IP reported by the gateway via GetExternalIPAddress, "" until
// known. Purely informational — shown by /upnp status so the host can hand the
// address to friends joining directly.
extern char g_NetUpnpExternalIP[46];

// Begin the async discover -> describe -> map sequence for the given UDP game
// port (external port == internal port). Call after netStartServer succeeds.
// No-op when Net.UPnP.Enabled is 0. If the gateway control URL is still cached
// from an earlier mapping this session, discovery is skipped and the mapping
// is re-added directly.
void netUpnpStart(u16 port);

// Server is going away: abort any in-flight work and, if a mapping was (or may
// have been) added, start an async DeletePortMapping. Call from netDisconnect.
// The delete keeps pumping via netUpnpTick after the session ends.
void netUpnpStop(void);

// Per-frame pump. Cheap no-op when idle. Call once per frame unconditionally
// (NOT gated on g_NetMode — the delete must finish after disconnect).
void netUpnpTick(void);

// Process-exit path: if a mapping is still active (or a delete is mid-flight),
// finish the DeletePortMapping with a bounded blocking pump (~2s worst case)
// so a quit doesn't leave a stale permanent mapping on the router. Call from
// main.c's cleanup() after netDisconnect.
void netUpnpShutdown(void);

// "/upnp [status|on|off|retry]" console command body. Returns 1 (handled).
s32 netUpnpConsoleCommand(const char *arg);

#endif // _IN_NETUPNP_H
