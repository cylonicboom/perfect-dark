# port-net-predict — Known Limitations

- Only Combat Sim (Combat scenario) works reliably; other scenarios broken.
- Cloaking device not synced.
- Slayer fly-by-wire and FarSight alt-fire don't work on clients.
- High bandwidth usage, especially with 8 players; recommend `Net.Server.UpdateFrames=2`.
- Sim bots are position-driven on clients — `SVC_CHR_FIRE` syncs shoot sound + muzzle-flash on/off, and the chr-state block in `SVC_PROP_MOVE` syncs body rotation and animation. `chr->actiontype` is **not** synced (would crash; see Sim Position Sync section); the client always dispatches sim chrTick as `ACT_STAND`, so visible animation comes only from the synced `animnum` and not from any per-action tick logic. Aim/look direction (head/torso) and partial-body animations (limb-specific layers) are still server-authoritative only.
- Punching and a few weapon-animation sounds still play first-person for every listener — the punch swing/hit goes through a code path outside the GUNCMD_PLAYSOUND hook we patched. Source TBD.
- Lag-comp is broad-phase (sphere) only. Narrow-phase bone matrix rewind was attempted and reverted after crashing the host — see Lag Compensation section. A safer narrow-phase pass needs to re-derive matrices on demand instead of writing into the per-frame `gfxAllocate` buffer.
- CSP is back to the simpler retarget-only behavior. At very high ping the local player may drift slightly behind the authoritative position when constantly diverging from the server, but it's stable — no exponential teleporting. Snap branch now uses `chrSetPos` (re-derives ground/rooms + resets `player->vv_manground`/`vv_ground`/`vv_theta`) so snaps actually stick — fixes the previous "client thinks it's standing, server says it's fallen, ack/snap loop forever" desync.
- Pitch-shift effects on remote players' weapon sounds (e.g., mauler charge) are skipped because `psCreate` returns a channel index, not a `struct sndstate *` handle. Local player still gets the effect via `sndStart`.
- Weapon equip / pickup sounds (the ~30 other `sndStart` sites in `bondgun.c`) are still non-positional for remote players. Less audible than fire sounds, so deferred.
- No build test performed — see "Environment Rules" at the top: this environment can't compile. The user builds externally from MSYS2 MinGW x64.
