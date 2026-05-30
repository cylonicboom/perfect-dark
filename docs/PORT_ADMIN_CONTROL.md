# Dedicated Server Admin Remote Control

Lets an authenticated player remotely administer a headless dedicated server —
which has no local console of its own. An admin can take exclusive control, end
the running match, reconfigure it like the Combat Simulator host, save weapon
presets, and add configured matches to the server's random rotation.

Protocol: `CLC_ADMIN` / `SVC_ADMIN` (added in `NET_PROTOCOL_VER 33`). All client
and server builds at v33 are required for these to interoperate.

## Setup

Set an admin password on the server (separate from the join password; empty
disables admin entirely):

- CLI: `--admin-password <pw>`
- or `pd.ini`: `Server.AdminPassword = <pw>`

For a systemd instance, add it to the instance's `EXTRA=` (see
`dist/linux/server/pd-server.conf.example`), e.g. `EXTRA=--admin-password hunter2`.

> Security: ENet traffic is unencrypted, so the password is access-gating, not
> strong security. Don't reuse a sensitive password, and prefer running the
> server behind a trusted network for admin use.

## Using it

From a connected client's console (`~`), commands are typed as `/admin <cmd>`.
The line is sent to the server, executed there, and the response prints back in
your console. The local host (listen server) can run them directly.

```
/admin login <password>      authenticate as admin (only command allowed first)
/admin take                  take exclusive control (suspends auto-rotation + voting)
/admin release               release control (auto-rotation resumes)
/admin status                show who holds control + match state
/admin help                  list commands
```

While you hold control the dedicated playlist auto-start and the end-of-round
vote are suspended, so the server won't advance the match under you. Control is
released automatically if you disconnect.

### Match control

```
/admin endmatch              end the current match, return to the lobby
/admin start [index]         start a playlist entry (random pick if index omitted)
```

### Configure a custom match (the "host it yourself" flow)

`take` seeds a **scratch config** from the running match. Edit it, then `apply`
to start it, or `saverotation` to add it to the rotation.

```
/admin set stage <name>          e.g. SKEDAR, TEMPLE, COMPLEX, RAVINE ...
/admin set scenario <name>       COMBAT, HTB, HTM, POPACAP, KOH, CTC
/admin set timelimit <minutes>   0..255 (0 = none)
/admin set scorelimit <n>        0..255
/admin set teamscorelimit <n>    0..65535
/admin set bots <n> [difficulty] 0..8, difficulty MEAT|EASY|NORMAL|HARD|PERFECT|DARK
/admin set option <name> [on|off]  e.g. TEAMS, FASTMOVEMENT, ONEHITKILLS, NORADAR
/admin set preset <name>         use a saved weapon preset by name
/admin show                      print the scratch config
/admin apply                     start a match from the scratch config
```

Valid `option` names match the playlist `options=` vocabulary (see
`docs/netplay.md` / `dist/linux/server/server_playlist.example.ini`).

### Configure via the built-in Combat Simulator menu (recommended)

Instead of the text `set` commands, an admin can configure the match with the
**normal built-in Combat Simulator menu** and push the result to the server —
exactly like hosting locally, but the dedicated server is the authoritative host:

```
/admin take                  take control (server holds in the lobby; clients see "waiting")
/admin endmatch              if a match is running, end it so you're in the lobby
/admin configure             load the Combat Sim setup so you can edit it
   ... open the Combat Simulator menu and set stage / scenario / weapons /
       options / bots exactly as you would when hosting a local game ...
/admin pushstart             send the configured match to the server; it adopts the
                             setup, starts the match, and broadcasts it to all clients
```

`pushstart` (alias `go`) serializes your locally-configured `g_MpSetup` + bot
configs and sends them to the server via `CLC_ADMIN_SETUP`. The server validates
that you're the in-control admin, commits the setup, and runs the same
`mpStartMatch` → `SVC_STAGE_START` path a normal host uses — so clients
transition into the match exactly as usual. While you're in the lobby
configuring, the server only broadcasts lobby state, so your local edits aren't
overwritten.

> The `set`/`apply`/`show` text commands remain available as a scriptable
> alternative (and for the Discord bot); the menu flow above is the "as if
> hosting locally" experience.

### Presets & rotation

```
/admin savepreset <name>     save the active match's weapons as a named preset,
                             persisted to mpsetups.bin; usable by playlist
                             `preset=` and by `/admin set preset <name>`
/admin saverotation <name>   append the scratch config to the live rotation AND
                             to the playlist file on disk, so it survives a
                             restart (an [entry.admin_*] block is appended)
```

`saverotation` writes back to the same playlist file the server loaded. If that
file isn't writable the entry still applies live for the session, and the
command says so.

### Moderation

```
/admin players               list connected clients (id, name, team, admin/spec)
/admin kick <name|id>        disconnect a client
/admin say <message>         broadcast a server message
```

## Notes & limitations

- The text/`/admin` interface is also the surface a Discord bot (or any tooling)
  can drive via `CLC_ADMIN`.
- A full in-client GUI flow ("take control → drop into the real Combat Sim menu →
  push") is a planned follow-up; it builds on this same server-side command set.
- `savepreset` captures the *currently active* match weapons, so it's most
  useful once a GUI configures them; from the text interface it snapshots
  whatever the running match uses.
- Full playability of a dedicated server still depends on the separate
  client/host sync work tracked in `PORT_DEDICATED_SERVER_TRIAGE.md`.
