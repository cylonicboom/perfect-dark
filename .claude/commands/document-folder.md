Read the source files under $ARGUMENTS. Then create $ARGUMENTS/CLAUDE.md with exactly these four sections, in order:

1. **Responsibility** — 1–2 sentences on what this folder owns. State the invariant or ownership boundary, not just "this folder contains X files."

2. **Key abstractions** — a table of the most important symbols, structs, or files, with the file they live in and a one-line note. Omit anything self-evident from the filename or path.

3. **Conventions** — patterns specific to this folder: guard macros, naming schemes, include order, allocation rules, etc. Do not restate anything already in a parent CLAUDE.md.

4. **Gotchas** — non-obvious constraints, invariants that break silently, or things that have caused bugs before. One bullet per gotcha; lead with the broken-if-violated condition.

Rules:
- Cap at 80 lines total.
- Add a one-line header at the top noting this file auto-loads when working under the folder, with a relative cross-reference to the nearest parent CLAUDE.md that has relevant context.
- Do not rename symbols under src/ — they map to decompiled N64 binary addresses.
- Do not restate content from root CLAUDE.md or any parent CLAUDE.md — cross-reference instead.
- Do not create documentation files; only create the CLAUDE.md.
