# Port-only "Tonal Inversion" Cheat

Cheats → Gameplay checkbox (and `/tonal [on|off]` live console toggle) that
applies **melodic inversion** to the soundtrack: each music channel's notes
are mirrored about that channel's **first note** (classic "inversion about
the first note"), so ascending lines descend and descending lines ascend —
the mirrored musical contour of the original score, anchored in each
instrument's original register. **Percussion is excluded** (drums would
otherwise scramble into different kit pieces). Sound effects are untouched
(they don't go through the sequence player).

Strictly speaking the mirror is **strict/real inversion** (exact semitone
reflection, `key' = 2*axis - key`, clamped 0..127), not *tonal* inversion in
the music-theory sense — true tonal inversion bends interval qualities to
stay in the original key, and the N64 sequence data carries no key signature
to bend against. The name keeps the friendlier term.

## Where it hooks (`src/lib/naudio/n_csplayer.c`)

`__n_CSPHandleMIDIMsg` is the single decode point for every music MIDI event
(PD's music all flows through the compressed sequence player — there is no
second player; `__n_lookupSoundQuick` is a pure binary search, safe to call
an extra time). The hook lives right after `key` is read from the event and
only touches the three statuses where byte1 is a note number — for other
statuses byte1 is a controller/program number.

Three cooperating pieces:

- **Per-channel axis** (`sndTonalAxis[16]`, stored key+1, 0 = unlatched):
  latched from the first *melodic* note-on per channel, so the opening note
  maps to itself and the line mirrors within its own register. A fixed
  global axis (the v1 design, middle C) shoved low-register tracks up an
  octave-plus — "everything sounds too sharp". Axes reset on
  `AL_SEQP_PLAY_EVT` (each song re-latches from its own first notes) and
  when the cheat is switched on.
- **Percussion exclusion**: at note-on the ORIGINAL key's sound is looked up
  (`__n_lookupSoundQuick`, guarded by the same state/chanMask/instrument
  checks as the player's own note-on path — the lookup derefs the channel
  instrument unguarded); a **single-key keymap (`keyMin == keyMax`) means a
  drum-kit hit** (one sample rooted per key) and is left alone. Melodic
  instruments span key ranges. Unknown/unmatched sounds are conservatively
  left alone too.
- **Active-note map** (`sndTonalMap[16][128]`, stored key+1, 0 = identity):
  records what each sounding (channel, original key) was remapped to at
  note-on. Note-off and per-key aftertouch resolve through the map
  **regardless of the cheat's current state**, then clear the entry — so a
  release always lands on the voice that was actually allocated, across
  axis changes, song changes and mid-note toggles alike.

**The double-inversion trap (read before changing anything here):** N64
sequences encode note lengths as durations, and the player *self-posts* the
matching `AL_CSP_NOTEOFF_EVT`, which **re-enters `__n_CSPHandleMIDIMsg`**
(dispatch at the `AL_CSP_NOTEOFF_EVT` case). The posted events therefore
carry the **original** key (`byte1`, two port-guarded post sites) and
resolve through the active-note map on re-entry. Posting the remapped key
instead would miss the map (or, in a mapless design, invert twice) →
`__n_lookupVoice` miss → voice never released → hung note.

State global: `u8 g_SndTonalInversion` (defined in n_csplayer.c), synced once
per frame from the cheat bank in `bgTickPortals` (`bg.c`) — the
`gfx_wireframe_mode` pattern, including the **1-byte vs game-side
`bool`==s32 gotcha** (extern it as `unsigned char`, never `bool`).

## Cheat plumbing (the Mirror pattern)

`CHEAT_TONALINVERSION 48` (append-only). **Cosmetic-only** like Mirror: lives
in the ENABLED bank only, never copied to the active banks
(`cheatIsActive` / `cheatActivate` / `cheatsReset` special-cases, plus the
"Cheat Solo Missions" relabel mask in `mainmenu.c`) — so missions still
save and challenges still count with it on. `CHEATFLAG_ALWAYSUNLOCKED`, name
via `s_cheat_literal_names`.

## Quirks / known limits

- **Percussion detection is a heuristic**: a melodic instrument sampled with
  single-key keymaps would be (harmlessly) skipped; a tonally-pitched
  percussion patch spanning key ranges would still invert.
- The axis latches from the first melodic note per channel *after the cheat
  comes on*, so enabling mid-song anchors to wherever the music happens to
  be — re-anchoring properly at the next track change.
- Inverted notes clamp to MIDI 0..127 at extremes (rare with per-channel
  axes).
- Pitch bends, instrument programs, volume/pan are untouched — only note
  pitch mirrors, "retaining the original character".
- Mid-note toggles cannot hang voices (the active-note map resolves releases
  under either state).

N64 build is byte-identical (all changes `#ifndef PLATFORM_N64`-guarded; the
posted-note-off sites keep the original `key` expression in the N64 branch).
