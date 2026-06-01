#!/usr/bin/env python3
"""
Generate docs/aicommands.md: the comprehensive AI-command reference for the Lua
action-block scripting layer.

Source of truth (so the doc can never silently drift from the engine):
  - src/include/commands.h   - one documented macro per command; the macro body
                               expands to the exact operand byte layout via the
                               mkshort()/mkword() helpers (util.h).
  - src/game/chrai.c         - g_CommandPointers[] maps opcode -> C handler name
                               and g_CommandLengths[] gives each command's byte
                               length (a cross-check on the parsed layout).

For every command we emit: opcode, the authoring macro name + its parameters,
the engine handler name, the byte layout (so a Lua author knows exactly what to
pass to ctx:run), the total length, and the human description from the macro's
doc comment.

Usage:  python tools/gen_aicommands.py > docs/aicommands.md
Run from the repo root. No third-party deps (stdlib only).
"""

import re
import sys
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
COMMANDS_H = os.path.join(ROOT, "src", "include", "commands.h")
CHRAI_C = os.path.join(ROOT, "src", "game", "chrai.c")


def parse_command_pointers(path):
    """opcode(int) -> engine handler name, from g_CommandPointers[]."""
    out = {}
    text = open(path, encoding="utf-8", errors="replace").read()
    m = re.search(r"g_CommandPointers\[\]\)\s*\(void\)\s*=\s*\{(.*?)\n\};", text, re.S)
    if not m:
        return out
    for line in m.group(1).splitlines():
        mm = re.search(r"/\*\s*(0x[0-9a-fA-F]+)\s*\*/\s*([A-Za-z_][A-Za-z0-9_]*)", line)
        if mm:
            out[int(mm.group(1), 16)] = mm.group(2)
    return out


def parse_command_lengths(path):
    """opcode(int) -> declared byte length, from g_CommandLengths[]."""
    out = {}
    text = open(path, encoding="utf-8", errors="replace").read()
    m = re.search(r"g_CommandLengths\[\]\s*=\s*\{(.*?)\n\};", text, re.S)
    if not m:
        return out
    # entries look like:  /*0x0004*/ 2,  /*0x0005*/ 5, ...
    for mm in re.finditer(r"/\*\s*(0x[0-9a-fA-F]+)\s*\*/\s*(\d+)", m.group(1)):
        out[int(mm.group(1), 16)] = int(mm.group(2))
    return out


def parse_macros(path):
    """
    Parse commands.h into a list of dicts:
      { 'name', 'params':[...], 'opcode':int|None, 'layout':[...], 'doc':str }
    Each macro's first mkshort(0xNNNN) is the opcode; remaining tokens are the
    operand layout.
    """
    text = open(path, encoding="utf-8", errors="replace").read()
    lines = text.splitlines()
    cmds = []
    i = 0
    pending_doc = ""
    while i < len(lines):
        line = lines[i]
        # collect a /** ... */ doc block
        if line.strip().startswith("/**"):
            doc_lines = []
            while i < len(lines):
                doc_lines.append(lines[i])
                if "*/" in lines[i]:
                    break
                i += 1
            pending_doc = clean_doc(doc_lines)
            i += 1
            continue
        m = re.match(r"#define\s+([A-Za-z_][A-Za-z0-9_]*)\s*(\([^)]*\))?\s*\\?\s*$", line)
        if m:
            name = m.group(1)
            params = []
            if m.group(2):
                params = [p.strip() for p in m.group(2)[1:-1].split(",") if p.strip()]
            # gather continuation lines (macro body) until a line without trailing backslash
            body = []
            # If the #define line itself ended without a backslash, body is empty.
            if line.rstrip().endswith("\\"):
                i += 1
                while i < len(lines):
                    body.append(lines[i])
                    if not lines[i].rstrip().endswith("\\"):
                        break
                    i += 1
            opcode, layout = parse_body(body)
            cmds.append({
                "name": name,
                "params": params,
                "opcode": opcode,
                "layout": layout,
                "doc": pending_doc.strip(),
            })
            pending_doc = ""
            i += 1
            continue
        # any non-doc, non-define line clears a dangling doc only if blank-ish
        if line.strip() and not line.strip().startswith("#"):
            pending_doc = ""
        i += 1
    return cmds


def clean_doc(doc_lines):
    out = []
    for l in doc_lines:
        l = l.strip()
        l = re.sub(r"^/\*\*?", "", l)
        l = re.sub(r"\*/$", "", l)
        l = re.sub(r"^\*\s?", "", l)
        out.append(l)
    return "\n".join(out).strip()


def parse_body(body):
    """Return (opcode:int|None, layout:[str]) from macro continuation lines."""
    # flatten, strip backslashes and trailing commas
    toks = []
    for l in body:
        l = l.rstrip().rstrip("\\").strip()
        if not l:
            continue
        # split on commas at top level (mkshort(...) has its own parens)
        toks.extend(split_top_commas(l))
    opcode = None
    layout = []
    for t in toks:
        t = t.strip().rstrip(",").strip()
        if not t:
            continue
        ms = re.match(r"mkshort\(\s*(0x[0-9a-fA-F]+)\s*\)$", t)
        if ms and opcode is None and not layout:
            opcode = int(ms.group(1), 16)
            continue
        layout.append(token_layout(t))
    return opcode, layout


def split_top_commas(s):
    out, depth, cur = [], 0, ""
    for ch in s:
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
        if ch == "," and depth == 0:
            out.append(cur)
            cur = ""
        else:
            cur += ch
    if cur.strip():
        out.append(cur)
    return out


def token_layout(t):
    """Describe one operand token as 'name:width'."""
    m = re.match(r"mkshort\(\s*(.+?)\s*\)$", t)
    if m:
        return (m.group(1), 2)
    m = re.match(r"mkword\(\s*(.+?)\s*\)$", t)
    if m:
        return (m.group(1), 4)
    return (t, 1)


def fmt_layout(opcode, layout):
    parts = ["`%02X %02X`(opcode)" % (opcode >> 8, opcode & 0xff)]
    for (name, width) in layout:
        if width == 1:
            parts.append("`%s`(u8)" % name)
        elif width == 2:
            parts.append("`%s`(u16 BE)" % name)
        elif width == 4:
            parts.append("`%s`(u32 BE)" % name)
    return " + ".join(parts)


def total_len(layout):
    return 2 + sum(w for (_, w) in layout)


def main():
    ptrs = parse_command_pointers(CHRAI_C)
    lens = parse_command_lengths(CHRAI_C)
    cmds = [c for c in parse_macros(COMMANDS_H) if c["opcode"] is not None]
    cmds.sort(key=lambda c: c["opcode"])

    out = []
    w = out.append
    w("<!-- GENERATED by tools/gen_aicommands.py from src/include/commands.h +")
    w("     src/game/chrai.c. Do not edit by hand; re-run the generator. -->")
    w("")
    w("# AI Command Reference (action blocks / Lua `ctx:run`)")
    w("")
    w("Every Perfect Dark AI action-block command, auto-generated from the engine")
    w("source so it cannot drift. Use this to author behaviour in Lua via")
    w("`ctx:run(opcode, b0, b1, ...)` - the **Byte layout** column tells you")
    w("exactly which operand bytes to pass after the opcode.")
    w("")
    w("- **Opcode** - the 2-byte command id (big-endian on the wire).")
    w("- **Macro** - the C authoring macro in `src/include/commands.h` (the")
    w("  human-readable name + its parameters).")
    w("- **Handler** - the engine C function in `src/game/chraicommands.c`.")
    w("- **Byte layout** - operand encoding. `u16 BE` / `u32 BE` are big-endian;")
    w("  `mkshort`/`mkword` in the macros expand to these. Total length includes")
    w("  the 2 opcode bytes.")
    w("")
    w("See [`luascripting.md`](luascripting.md) for the `ctx`/`pd` API and")
    w("[`ailists.md`](ailists.md) for the action-block model (IDs, labels,")
    w("yielding). Counts: %d commands documented below." % len(cmds))
    w("")
    w("> **`ctx:run` and control flow.** Commands that take a `label` operand")
    w("> (gotos, `if_*`, `try_*`) perform jumps *within the original bytecode*.")
    w("> In a hand-written Lua override there is no bytecode to jump into, so use")
    w("> Lua's own `if`/`while`/`goto` for control flow and reserve `ctx:run` for")
    w("> the *action/effect* and *condition* commands. Condition commands still")
    w("> return their break flag, but the `label` byte is ignored in synthetic")
    w("> mode.")
    w("")
    w("| Opcode | Macro | Handler | Byte layout | Len | Description |")
    w("|---|---|---|---|---|---|")

    for c in cmds:
        op = c["opcode"]
        handler = ptrs.get(op, "")
        macro = "`%s%s`" % (c["name"], "(" + ", ".join(c["params"]) + ")" if c["params"] else "")
        layout = fmt_layout(op, c["layout"])
        tlen = total_len(c["layout"])
        # cross-check against the engine length table where present
        eng = lens.get(op)
        lencell = str(tlen)
        if eng is not None and eng != tlen and not has_variable(c):
            lencell = "%d*" % tlen  # * = differs from g_CommandLengths (e.g. variable)
        desc = first_sentence(c["doc"])
        w("| `0x%04X` | %s | `%s` | %s | %s | %s |" % (op, macro, handler, layout, lencell, desc))

    w("")
    w("\\* length differs from `g_CommandLengths[]` (usually a variable-length")
    w("command such as one with an embedded string).")
    out_text = "\n".join(out) + "\n"
    # Write the doc directly as UTF-8 (LF) so output is independent of the shell
    # codepage; also echo to stdout for piping if desired.
    out_path = os.path.join(ROOT, "docs", "aicommands.md")
    import io
    io.open(out_path, "w", encoding="utf-8", newline="\n").write(out_text)
    sys.stderr.write("wrote %s (%d commands)\n" % (out_path, sum(1 for _ in out_text.splitlines() if _.startswith("| `0x"))))


def has_variable(c):
    # commands whose doc mentions a string / variable length
    d = c["doc"].lower()
    return "string" in d or "text" in d or "variable" in d


def first_sentence(doc):
    if not doc:
        return ""
    # collapse whitespace, take up to first period for the table cell
    one = re.sub(r"\s+", " ", doc).strip()
    one = one.replace("|", "\\|")
    m = re.match(r"(.+?\.)(\s|$)", one)
    s = m.group(1) if m else one
    if len(s) > 160:
        s = s[:157] + "..."
    return s


if __name__ == "__main__":
    main()
