Read the source files under $ARGUMENTS. Then create $ARGUMENTS/CLAUDE.md covering the netplay layer in that folder. Use these sections, omitting any that genuinely have nothing to say:

1. **Responsibility** — 1–2 sentences on what this folder owns in the netplay stack and where it sits in the send/receive pipeline.

2. **Key abstractions** — table of the most important structs, message types, and functions, with file locations and one-line notes.

3. **Wire format** — message layout for each SVC_*/CLC_* type owned here: field order, widths, bit flags, variable-length or optional blocks. Note which fields are new additions vs original. Flag any field whose absence at old protocol versions would silently corrupt reads.

4. **Send / receive loop** — where messages are enqueued, which ENet channel (reliable vs unreliable), when they are flushed, and where the read dispatch lives. Include tick timing if it matters (e.g., "sent every N ticks in netEndFrame").

5. **Authority model** — what the server owns authoritatively, what the client predicts locally, what the server echoes back unchanged, and what triggers a force-correction (`UCMD_FL_FORCEMASK`). Be explicit about which fields are server-authoritative even when echoed.

6. **Prediction-relevant gaps** — fields or state intentionally not synced, why, and what breaks if you try to sync them. Reference reverted attempts by name if documented in PORT_NET_REVERTED_EXPERIMENTS.md.

7. **Byteswap / alignment gotchas** — struct padding traps, endianness assumptions baked into netbuf reads/writes, fields that must be written in a specific order to match the reader.

8. **Conventions** — guard macros, channel selection rules (`NETCHAN_DEFAULT` vs `NETCHAN_CONTROL`), naming patterns for read/write pairs, any invariants on message ordering.

9. **Gotchas** — non-obvious constraints not covered above. One bullet per gotcha; lead with the broken-if-violated condition.

Rules:
- Cap at 120 lines total.
- Add a one-line header noting this auto-loads when working under the folder, with cross-references to port/src/net/CLAUDE.md and any relevant docs/ files.
- Do not restate content from port/src/net/CLAUDE.md verbatim — cross-reference it instead.
- Do not rename symbols under src/ — they map to decompiled N64 binary addresses.
- Bump NET_PROTOCOL_VER in port/include/net/net.h whenever the wire format changes.
- Do not create documentation files; only create the CLAUDE.md.
