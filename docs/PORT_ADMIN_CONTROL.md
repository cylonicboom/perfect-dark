# Dedicated Server Admin Remote Control

Lets an authenticated player remotely administer a headless dedicated server —
which has no local console of its own. An admin can take exclusive control, end
the running match, reconfigure it like the Combat Simulator host, save weapon
presets, and add configured matches to the server's random rotation.

Protocol: `CLC_ADMIN` / `SVC_ADMIN` (added in `NET_PROTOCOL_VER 33`). All client
and server builds at v33 are required for these to interoperate.

> **Host Online Game** rides this machinery end-to-end: the master spawns an
> instance with a random `--admin-password` token and the requesting client
> auto-runs `login <token>` + `take`, then drives the *full* Combat Sim setup
> (pushes via the unchanged `CLC_ADMIN_SETUP`). See
> [`PORT_HOSTED_SERVER.md`](PORT_HOSTED_SERVER.md).

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

### Configure via the in-client menu (recommended)

Instead of the text `set` commands, an admin can configure the match in a small
**Admin: Match Setup** menu and push the result to the server:

```
/admin login <pw>            authenticate as admin
/admin take                  take control (server holds in the lobby; clients see "waiting")
/admin endmatch              if a match is running, end it so you're in the lobby
/admin configure             open the Admin: Match Setup menu  (press ~ to close
                             the console so the menu is visible)
   ... set Arena / Scenario / Simulants + difficulty / Weapons / Limits /
       Options ...
   Push & Start Match         (menu button) — or type /admin pushstart
```

The menu edits your already-synced `g_MpSetup` + bot configs **in place** (it does
*not* run the title-screen Combat Sim setup-load, so it's safe to open while
you're connected — that crash is gone). "Push & Start" / `pushstart` (alias `go`)
serializes `g_MpSetup` + bot configs to the server via `CLC_ADMIN_SETUP`; the
server validates you're the in-control admin, commits, and runs the same
`mpStartMatch` → `SVC_STAGE_START` path a normal host uses, so every client
transitions into the match as usual. When the menu opens it seeds the current
fields from the server's lobby state.

The menu is a compact list of openers (like the real Combat Sim "Game Setup"):
**Arena**, **Weapons**, and **Limits** open the *actual* Combat Sim sub-dialogs
(so Weapons is the full per-slot picker + weapon sets / custom presets, not a
single preset choice); **Scenario**, **Simulants** (0–8) and **Sim Difficulty**
are inline dropdowns; **Options** opens a sub-dialog of the common game toggles
(One Hit Kills, Slow Motion, Fast Movement, Teams, No Radar, No Auto-Aim,
Friendly Fire, Kills = Score). For the niche options, use the text `set` commands.

> The `set`/`apply`/`show` text commands remain available as a scriptable
> alternative (and for the Discord bot).
>
> Note: `/admin configure` pushes the menu onto the active lobby menu; in the
> normal connected-lobby state that's fine. If you invoked it from an in-world
> state and nothing appears, return to the lobby and try again.

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
- The in-client **Admin: Match Setup** menu (`/admin configure`, above) is the
  GUI flow; it edits `g_MpSetup` in place and reuses this same `CLC_ADMIN_SETUP`
  push. A *full* drop-into-the-real-Combat-Sim-menu flow was deliberately not
  built — it requires tearing the client world down (see
  `PORT_ADMIN_GUI_CONFIGURE.md`); the lightweight menu avoids that.
- `savepreset` captures the *currently active* match weapons, so it's most
  useful once a GUI configures them; from the text interface it snapshots
  whatever the running match uses.
- Full playability of a dedicated server still depends on the separate
  client/host sync work tracked in `PORT_DEDICATED_SERVER_TRIAGE.md`.
