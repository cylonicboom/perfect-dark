# Net Headers (`port/include/net/`)

> Design notes, protocol documentation, and rationale live in the implementation CLAUDE.md: [`../../src/net/CLAUDE.md`](../../src/net/CLAUDE.md).

Public headers for the netplay layer. Edit here when adding new message IDs, struct fields, or constants; update the implementation and its CLAUDE.md to match.

## Files

| File | Notes |
|---|---|
| `net.h` | `netclient`, `netplayermove`, `csp_snapshot`, `lagcomp_snapshot` structs; NETMODE/CLSTATE/UCMD/`DISCONNECT_PASSWORD` constants; `g_NetServerPassword`/`g_NetJoinPassword`/`g_NetServerActualPort` externs; runtime-tunable CSP/interp knobs (`g_NetCspCorrFramesMax`, etc.) |
| `netmsg.h` | SVC_*/CLC_* message ID constants; `NET_QF_*` query flags + `NET_QUERYTYPE_*`; `netmsgQuerySummaryWrite`/`netmsgQueryDetailsWrite`; all read/write declarations |
| `netmaster.h` | Master/browser constants (`NET_MASTER_*`, `NET_BROWSER_MAX`), `netserverentry`/`netserverdetails` structs, browser state externs, API. **Deliberately ENet-free** so `netmenu.c` can include it. |
| `netbuf.h` | `netbuf` struct; typed buffer read/write API (u8/u16/u32/f32/coord) |
| `netenet.h` | Thin ENet include wrapper — undefines `bool`, `near`, `far` after inclusion to avoid collisions |

## Gotchas

- **`NET_CSP_*` macros are aliases for the `g_Net*` extern globals.** Both names refer to the same variable; prefer `g_Net*` in new code.
- **`netplayermove.animnum`/`animframe` are excluded from `netClientNeedMove`'s memcmp.** Fields appended after them are also excluded. Insert change-detected fields before `animnum`, or explicitly extend the memcmp size.
- **Bump `NET_PROTOCOL_VER` when the wire format changes** — mismatched versions are rejected at auth time with `DISCONNECT_VERSION`. (Currently 32: server browser + master registration + join password.)
- **The query summary block (`netmsgQuerySummaryWrite`) is shared verbatim** by the direct PDQM query response and the master HEARTBEAT. Changing its field order changes both — and the VPS master must match (see `docs/PORT_MASTER_SERVER.md`).
- **`netmaster.h` must stay ENet-free.** Menu code (`netmenu.c`) includes it; ENet-typed glue (`netParseAddr`, `netSendConnectionless`) is `extern`-declared in `netmaster.c` instead.
