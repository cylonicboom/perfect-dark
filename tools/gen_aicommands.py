#!/usr/bin/env python3
"""
Generate the AI-command scripting artifacts from the engine source so they can
never silently drift:

  - docs/aicommands.md   : human/agent reference. Every AI action-block command
                           with opcode, authoring macro + params, engine handler,
                           exact operand byte layout, length, and description.
  - scripts/ai.lua       : a Lua helper library. Every command becomes a named
                           function that packs its operands and calls ctx:run,
                           e.g. ai.try_attack_stand(ctx, flags, entity, label).
                           This turns raw ctx:run(0x15, 0x02,0x20, 0,0, 0) into
                           ai.try_attack_stand(ctx, 0x220, 0, 0).

Source of truth:
  - src/include/commands.h : one documented macro per command. The macro body
                             expands to the exact operand byte layout via the
                             mkshort()/mkword() helpers (util.h: mkshort = 2 bytes
                             big-endian, mkword = 4 bytes big-endian, a bare token
                             = 1 byte). The first mkshort(0xNNNN) is the opcode.
  - src/game/chrai.c       : g_CommandPointers[] maps opcode -> C handler name;
                             g_CommandLengths[] cross-checks the parsed length.

Usage:
  python tools/gen_aicommands.py            # write both artifacts
  python tools/gen_aicommands.py --check    # exit 1 if either is out of date
                                            # (CI / build verification; no write)

Run from the repo root. Stdlib only.
"""

import io
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
COMMANDS_H = os.path.join(ROOT, "src", "include", "commands.h")
CHRAI_C = os.path.join(ROOT, "src", "game", "chrai.c")
OUT_MD = os.path.join(ROOT, "docs", "aicommands.md")
OUT_LUA = os.path.join(ROOT, "scripts", "ai.lua")

# Baked C constants that appear as literal operand bytes inside a macro body
# (not as macro parameters). Resolved from src/include/constants.h.
BAKED_CONSTS = {
    "FUNC_PRIMARY": 0,
    "HITPART_HEAD": 8,
}


# --------------------------------------------------------------------------- #
# Parsing (shared by both emitters)
# --------------------------------------------------------------------------- #

def read(path):
    return open(path, encoding="utf-8", errors="replace").read()


def parse_command_pointers(text):
    out = {}
    m = re.search(r"g_CommandPointers\[\]\)\s*\(void\)\s*=\s*\{(.*?)\n\};", text, re.S)
    if m:
        for mm in re.finditer(r"/\*\s*(0x[0-9a-fA-F]+)\s*\*/\s*([A-Za-z_]\w*)", m.group(1)):
            out[int(mm.group(1), 16)] = mm.group(2)
    return out


def parse_command_lengths(text):
    out = {}
    m = re.search(r"g_CommandLengths\[\]\s*=\s*\{(.*?)\n\};", text, re.S)
    if m:
        for mm in re.finditer(r"/\*\s*(0x[0-9a-fA-F]+)\s*\*/\s*(\d+)", m.group(1)):
            out[int(mm.group(1), 16)] = int(mm.group(2))
    return out


def parse_macros(text):
    """List of {name, params, opcode, layout:[(token,width)], doc}."""
    lines = text.splitlines()
    cmds = []
    i = 0
    pending_doc = ""
    while i < len(lines):
        line = lines[i]
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
        m = re.match(r"#define\s+([A-Za-z_]\w*)\s*(\([^)]*\))?\s*\\?\s*$", line)
        if m:
            name = m.group(1)
            params = [p.strip() for p in m.group(2)[1:-1].split(",")] if m.group(2) else []
            params = [p for p in params if p]
            body = []
            if line.rstrip().endswith("\\"):
                i += 1
                while i < len(lines):
                    body.append(lines[i])
                    if not lines[i].rstrip().endswith("\\"):
                        break
                    i += 1
            opcode, layout = parse_body(body)
            cmds.append({"name": name, "params": params, "opcode": opcode,
                         "layout": layout, "doc": pending_doc.strip()})
            pending_doc = ""
            i += 1
            continue
        if line.strip() and not line.strip().startswith("#"):
            pending_doc = ""
        i += 1
    return [c for c in cmds if c["opcode"] is not None]


def clean_doc(doc_lines):
    out = []
    for l in doc_lines:
        l = l.strip()
        l = re.sub(r"^/\*\*?", "", l)
        l = re.sub(r"\*/$", "", l)
        l = re.sub(r"^\*\s?", "", l)
        out.append(l)
    return "\n".join(out).strip()


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


def parse_body(body):
    toks = []
    for l in body:
        l = l.rstrip().rstrip("\\").strip()
        if l:
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


def token_layout(t):
    m = re.match(r"mkshort\(\s*(.+?)\s*\)$", t)
    if m:
        return (m.group(1).strip(), 2)
    m = re.match(r"mkword\(\s*(.+?)\s*\)$", t)
    if m:
        return (m.group(1).strip(), 4)
    return (t.strip(), 1)


def total_len(layout):
    return 2 + sum(w for (_, w) in layout)


def has_variable(c):
    d = c["doc"].lower()
    return "string" in d or "text" in d or "variable" in d


# --------------------------------------------------------------------------- #
# Markdown emitter
# --------------------------------------------------------------------------- #

def fmt_layout_md(opcode, layout):
    parts = ["`%02X %02X`(opcode)" % (opcode >> 8, opcode & 0xff)]
    for (name, width) in layout:
        suffix = {1: "u8", 2: "u16 BE", 4: "u32 BE"}[width]
        parts.append("`%s`(%s)" % (name, suffix))
    return " + ".join(parts)


def first_sentence(doc):
    if not doc:
        return ""
    one = re.sub(r"\s+", " ", doc).strip().replace("|", "\\|")
    m = re.match(r"(.+?\.)(\s|$)", one)
    s = m.group(1) if m else one
    return (s[:157] + "...") if len(s) > 160 else s


def build_markdown(cmds, ptrs, lens):
    out = []
    w = out.append
    w("<!-- GENERATED by tools/gen_aicommands.py from src/include/commands.h +")
    w("     src/game/chrai.c. Do not edit by hand; re-run the generator. -->")
    w("")
    w("# AI Command Reference (action blocks / Lua)")
    w("")
    w("Every Perfect Dark AI action-block command, auto-generated from the engine")
    w("source so it cannot drift. Two ways to use a command from Lua:")
    w("")
    w("- **High-level (recommended):** the generated helper library")
    w("  [`scripts/ai.lua`](../scripts/ai.lua) wraps every command as a named")
    w("  function that packs the operands for you, e.g.")
    w("  `ai.try_attack_stand(ctx, flags, entity, label)`.")
    w("- **Low-level:** `ctx:run(opcode, b0, b1, ...)` with the raw operand bytes")
    w("  from the **Byte layout** column below.")
    w("")
    w("- **Opcode** - the 2-byte command id (big-endian).")
    w("- **Macro** - the C authoring macro in `src/include/commands.h`.")
    w("- **Handler** - the engine C function in `src/game/chraicommands.c`.")
    w("- **Byte layout** - operand encoding after the opcode. `u16 BE`/`u32 BE`")
    w("  are big-endian (the `mkshort`/`mkword` macros). **Len** includes the 2")
    w("  opcode bytes.")
    w("")
    w("See [`luascripting.md`](luascripting.md) for the `ctx`/`pd` API and")
    w("[`ailists.md`](ailists.md) for the action-block model (IDs, labels,")
    w("yielding). **%d commands** documented below." % len(cmds))
    w("")
    w("> **Control flow in Lua overrides.** Commands with a `label` operand")
    w("> (`goto_*`, `if_*`, `try_*`) jump *within the original bytecode*. In a")
    w("> hand-written Lua override there is no bytecode to jump into, so use Lua's")
    w("> own `if`/`while`/`goto` for control flow and use these commands for their")
    w("> *action* and *condition* effects. Condition/try commands still return")
    w("> their break flag; the trailing `label` byte is ignored in synthetic mode.")
    w("")
    w("| Opcode | Macro | Handler | Byte layout | Len | Description |")
    w("|---|---|---|---|---|---|")
    for c in cmds:
        op = c["opcode"]
        handler = ptrs.get(op, "")
        macro = "`%s%s`" % (c["name"], "(" + ", ".join(c["params"]) + ")" if c["params"] else "")
        tlen = total_len(c["layout"])
        eng = lens.get(op)
        lencell = str(tlen)
        if eng is not None and eng != tlen and not has_variable(c):
            lencell = "%d*" % tlen
        w("| `0x%04X` | %s | `%s` | %s | %s | %s |" % (
            op, macro, handler, fmt_layout_md(op, c["layout"]), lencell, first_sentence(c["doc"])))
    w("")
    w("\\* length differs from `g_CommandLengths[]` (usually a variable-length")
    w("command such as one with an embedded string).")
    return "\n".join(out) + "\n"


# --------------------------------------------------------------------------- #
# Lua helper-library emitter
# --------------------------------------------------------------------------- #

LUA_RESERVED = {
    "and", "break", "do", "else", "elseif", "end", "false", "for", "function",
    "goto", "if", "in", "local", "nil", "not", "or", "repeat", "return", "then",
    "true", "until", "while",
}


def lua_name(name):
    return name + "_" if name in LUA_RESERVED else name


def lua_value_expr(token, params):
    """Lua expression (a string) for an operand token's *value*, given the
    function params. Handles: bare param, param expression (e.g. distance / 10),
    baked C constant, and numeric literal."""
    t = token.strip()
    if re.fullmatch(r"0x[0-9a-fA-F]+|\d+", t):
        return str(int(t, 0))                     # numeric literal
    if t in BAKED_CONSTS:
        return str(BAKED_CONSTS[t])               # known C constant
    if t in params:
        return t                                  # bare parameter
    # expression referencing parameters, e.g. "distance / 10" or "1 - operator"
    expr = t
    # integer division for "/N" so byte math matches the C macro
    expr = re.sub(r"\s*/\s*(\d+)", r" // \1", expr)
    # if it references a known param, keep it; otherwise treat unknown bare ids
    # as a baked literal name we couldn't resolve -> emit 0 and flag in comment
    ids = set(re.findall(r"[A-Za-z_]\w*", expr))
    unknown = [i for i in ids if i not in params and i not in ("math",)]
    if unknown:
        return None  # caller emits a TODO-safe 0 with a comment
    return "(" + expr + ")"


def build_lua(cmds):
    out = []
    w = out.append
    w("-- GENERATED by tools/gen_aicommands.py from src/include/commands.h +")
    w("-- src/game/chrai.c. Do not edit by hand; re-run the generator.")
    w("--")
    w("-- AI command helper library for the Lua action-block scripting layer.")
    w("-- Every engine AI command is a function here that packs its operands and")
    w("-- calls ctx:run, so you can write readable AI instead of raw byte arrays:")
    w("--")
    w("--   local ai = dofile('scripts/ai.lua')")
    w("--   pd.register_ailist(0x0021, function(ctx)")
    w("--     ai.set_target_chr(ctx, 0xf6)        -- CHR_TARGET")
    w("--     ai.try_attack_stand(ctx, 0x220, 0, 0)")
    w("--     return 1                            -- yield this frame")
    w("--   end)")
    w("--")
    w("-- Each wrapper returns ctx:run's break flag. Multi-byte operands are")
    w("-- packed big-endian to match the engine. The trailing 'label' arg on")
    w("-- goto/if_*/try_* commands is accepted for signature parity but is not a")
    w("-- meaningful jump target in synthetic mode -- branch with Lua instead.")
    w("-- See docs/aicommands.md for the full per-command reference.")
    w("")
    w("local ai = {}")
    w("")
    w("-- big-endian byte splitters (match mkshort/mkword in src/include/util.h)")
    w("local function u16(v) return (v >> 8) & 0xff, v & 0xff end")
    w("local function u32(v)")
    w("  return (v >> 24) & 0xff, (v >> 16) & 0xff, (v >> 8) & 0xff, v & 0xff")
    w("end")
    w("")
    w("ai.OPCODE = {}  -- name -> opcode, for reference / ctx:run fallback")
    w("")

    flagged = 0
    for c in cmds:
        op = c["opcode"]
        fname = lua_name(c["name"])
        params = c["params"]
        arglist = ", ".join(["ctx"] + params)
        w("-- 0x%04X  %s" % (op, first_sentence(c["doc"])))
        w('ai.OPCODE["%s"] = 0x%04X' % (c["name"], op))
        w("function ai.%s(%s)" % (fname, arglist))
        # build the ctx:run argument list: opcode, then each operand expanded
        run_args = ["0x%04X" % op]
        note = None
        for (token, width) in c["layout"]:
            val = lua_value_expr(token, params)
            if val is None:
                # unresolved reference: emit 0 bytes of the right width, flag it
                note = note or []
                note.append(token)
                val = "0"
                flagged += 1
            if width == 1:
                run_args.append("(%s) & 0xff" % val)
            elif width == 2:
                run_args.append("u16(%s)" % val)
            elif width == 4:
                run_args.append("u32(%s)" % val)
        w("  return ctx:run(%s)" % ", ".join(run_args))
        if note:
            w("  -- NOTE: operand(s) %s could not be resolved to a parameter and"
              % ", ".join(note))
            w("  -- are packed as 0; pass via ctx:run directly if you need them.")
        w("end")
        w("")

    w("return ai")
    sys.stderr.write("ai.lua: %d commands, %d operand(s) packed as 0 (unresolved)\n"
                     % (len(cmds), flagged))
    return "\n".join(out) + "\n"


# --------------------------------------------------------------------------- #
# Driver
# --------------------------------------------------------------------------- #

def write_or_check(path, content, check):
    existing = read(path) if os.path.exists(path) else None
    if check:
        if existing != content:
            sys.stderr.write("OUT OF DATE: %s (re-run tools/gen_aicommands.py)\n" % path)
            return False
        return True
    io.open(path, "w", encoding="utf-8", newline="\n").write(content)
    sys.stderr.write("wrote %s\n" % path)
    return True


def main():
    check = "--check" in sys.argv[1:]
    chrai = read(CHRAI_C)
    ptrs = parse_command_pointers(chrai)
    lens = parse_command_lengths(chrai)
    cmds = sorted(parse_macros(read(COMMANDS_H)), key=lambda c: c["opcode"])

    md = build_markdown(cmds, ptrs, lens)
    lua = build_lua(cmds)

    ok = True
    ok &= write_or_check(OUT_MD, md, check)
    ok &= write_or_check(OUT_LUA, lua, check)
    if check and not ok:
        sys.exit(1)


if __name__ == "__main__":
    main()
