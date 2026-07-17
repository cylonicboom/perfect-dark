#!/usr/bin/env python3
"""Symbolicate a pd crash log against an archived symbol file.

Usage:
  python tools/symbolicate.py <crashlog|->  [symbolfile]
      Parse a pd.crash.log (or paste it on stdin with '-') and print the
      backtrace with function names + file:line resolved via addr2line.
      If no symbolfile is given, the log's "BUILD: <branch> <hash> (...)"
      line is matched against symbols/*<hash>* in the repo; failing that,
      the newest file in symbols/, failing that build_debug_sdl3/pd.x86_64.exe
      (with a staleness warning).

  python tools/symbolicate.py --archive <exe>
      Extract the exe's embedded "version: <branch> <hash> (<target>)"
      string and objcopy --only-keep-debug it into
      symbols/pd-<branch>-<hash>.debug so future crash logs from that build
      stay symbolicatable after the exe is rebuilt. Run this every time an
      exe is shipped to users (see docs/PORT_CRASH_SYMBOLS.md).

Notes:
  - Works on the Windows crash-log format: frames like
        #00: 00007ff70310c20a: [00007ff703060000]+00000000000ac20a
    Only frames inside MAIN MODULE are resolvable.
  - addr2line wants the PE preferred image base + offset (ASLR makes the
    runtime base in the log meaningless). The base is read from the symbol
    file's PE header; falls back to 0x140000000 (MinGW x64 default).
"""

import os
import re
import struct
import subprocess
import sys
import glob

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SYMBOLS_DIR = os.path.join(REPO_ROOT, "symbols")

TOOL_CANDIDATE_DIRS = [
    r"C:\msys64\mingw64\bin",
    r"C:\msys2\mingw64\bin",
    r"C:\tools\msys64\mingw64\bin",
]


def find_tool(name):
    import shutil
    p = shutil.which(name)
    if p:
        return p
    exe = name + (".exe" if os.name == "nt" else "")
    for d in TOOL_CANDIDATE_DIRS:
        cand = os.path.join(d, exe)
        if os.path.isfile(cand):
            return cand
    return None


def pe_image_base(path):
    """Read the preferred ImageBase from a PE (or objcopy'd .debug) file."""
    try:
        with open(path, "rb") as f:
            hdr = f.read(0x40)
            if len(hdr) < 0x40 or hdr[:2] != b"MZ":
                return None
            (e_lfanew,) = struct.unpack_from("<I", hdr, 0x3C)
            f.seek(e_lfanew)
            nt = f.read(0x120)
            if nt[:4] != b"PE\0\0":
                return None
            (magic,) = struct.unpack_from("<H", nt, 0x18)
            if magic == 0x20B:  # PE32+
                (base,) = struct.unpack_from("<Q", nt, 0x18 + 0x18)
            elif magic == 0x10B:  # PE32
                (base,) = struct.unpack_from("<I", nt, 0x18 + 0x1C)
            else:
                return None
            return base
    except OSError:
        return None


def pe_link_timestamp(path):
    """Read the COFF FileHeader TimeDateStamp — unique per link, unlike the
    configure-time git hash. Matches the 'link=XXXXXXXX' in crash logs."""
    try:
        with open(path, "rb") as f:
            hdr = f.read(0x40)
            if len(hdr) < 0x40 or hdr[:2] != b"MZ":
                return None
            (e_lfanew,) = struct.unpack_from("<I", hdr, 0x3C)
            f.seek(e_lfanew)
            nt = f.read(12)
            if nt[:4] != b"PE\0\0":
                return None
            (stamp,) = struct.unpack_from("<I", nt, 8)
            return stamp
    except OSError:
        return None


def embedded_version(exe_path):
    """Find the 'version: <branch> <hash> (<target>)' literal baked into the exe."""
    with open(exe_path, "rb") as f:
        blob = f.read()
    idx = blob.find(b"version: ")
    while idx >= 0:
        end = blob.find(b"\0", idx)
        s = blob[idx:end].decode("ascii", "replace")
        m = re.match(r"version: (\S+) ([0-9a-f]{6,40}) \((\S+)\)", s)
        if m:
            return m.group(1), m.group(2), m.group(3)
        idx = blob.find(b"version: ", idx + 1)
    return None


def archive(exe_path):
    ver = embedded_version(exe_path)
    if not ver:
        sys.exit("error: no embedded 'version: <branch> <hash> (<target>)' string found in %s" % exe_path)
    branch, ghash, target = ver
    objcopy = find_tool("objcopy")
    if not objcopy:
        sys.exit("error: objcopy not found (install MSYS2 mingw64 binutils)")
    os.makedirs(SYMBOLS_DIR, exist_ok=True)
    stamp = pe_link_timestamp(exe_path)
    name = "pd-%s-%s%s.debug" % (branch, ghash,
                                 ("-%08x" % stamp) if stamp else "")
    out = os.path.join(SYMBOLS_DIR, name)
    if os.path.exists(out):
        print("already archived: %s" % out)
        return
    subprocess.check_call([objcopy, "--only-keep-debug", exe_path, out])
    print("archived %s %s (%s) -> %s (%.1f MB)"
          % (branch, ghash, target, out, os.path.getsize(out) / 1e6))


def pick_symbol_file(build_hash, link_stamp):
    if link_stamp:
        hits = sorted(glob.glob(os.path.join(SYMBOLS_DIR, "*%08x*" % link_stamp)))
        if hits:
            return hits[0], "matched link stamp %08x" % link_stamp
    if build_hash:
        hits = sorted(glob.glob(os.path.join(SYMBOLS_DIR, "*%s*" % build_hash)))
        if hits:
            if len(hits) > 1:
                print("warning: %d archives share hash %s - using %s"
                      % (len(hits), build_hash, os.path.basename(hits[-1])))
            return hits[-1], "matched BUILD hash %s" % build_hash
        print("warning: no symbols/*%s* archive; falling back" % build_hash)
    cands = sorted(glob.glob(os.path.join(SYMBOLS_DIR, "*.debug")),
                   key=os.path.getmtime, reverse=True)
    if cands:
        return cands[0], "newest archive (hash unmatched - lines may be wrong!)"
    fallback = os.path.join(REPO_ROOT, "build_debug_sdl3", "pd.x86_64.exe")
    if os.path.isfile(fallback):
        return fallback, "current build dir exe (may not match the crashing build!)"
    return None, None


def symbolicate(log_text, symbol_file):
    m = re.search(r"BUILD: (\S+) ([0-9a-f]{6,40})", log_text)
    build_hash = m.group(2) if m else None
    if build_hash:
        print("crash build: %s %s" % (m.group(1), build_hash))
    m = re.search(r"\blink=([0-9a-fA-F]{1,8})", log_text)
    link_stamp = int(m.group(1), 16) if m else None

    if not symbol_file:
        symbol_file, why = pick_symbol_file(build_hash, link_stamp)
        if not symbol_file:
            sys.exit("error: no symbol file given, none in symbols/, no build dir exe")
        print("symbols: %s (%s)" % (symbol_file, why))

    a2l = find_tool("addr2line")
    if not a2l:
        sys.exit("error: addr2line not found (install MSYS2 mingw64 binutils)")

    base = pe_image_base(symbol_file) or 0x140000000

    m = re.search(r"MAIN MODULE: \[?([0-9a-fA-F]+)\]?", log_text)
    main_base = int(m.group(1), 16) if m else None

    # Collect (label, module_offset) pairs: the PC line + each backtrace frame.
    frames = []
    m = re.search(r"^PC: ([0-9a-fA-F]{8,16})\b", log_text, re.M)
    if m and main_base is not None:
        pc = int(m.group(1), 16)
        if 0 <= pc - main_base < 0x40000000:
            frames.append(("PC ", pc - main_base))
    for m in re.finditer(r"^(#\d+): [0-9a-fA-F]+: \[([0-9a-fA-F]+)\]\+([0-9a-fA-F]+)", log_text, re.M):
        label, modbase, ofs = m.group(1), int(m.group(2), 16), int(m.group(3), 16)
        if main_base is None or modbase == main_base:
            frames.append((label, ofs))
        else:
            print("%s: (other module +0x%x - skipped)" % (label, ofs))

    if not frames:
        sys.exit("error: no main-module frames found in the log (wrong format?)")

    addrs = ["0x%x" % (base + ofs) for _, ofs in frames]
    out = subprocess.check_output([a2l, "-e", symbol_file, "-f", "-C", "-p", "-i"] + addrs,
                                  stderr=subprocess.STDOUT).decode("utf-8", "replace")
    # -p -i prints one line per frame plus " (inlined by) ..." continuation lines.
    lines = out.splitlines()
    i = 0
    for label, ofs in frames:
        first = True
        while i < len(lines):
            ln = lines[i]
            if not first and not ln.lstrip().startswith("(inlined by)"):
                break
            print("%s +0x%-8x %s" % (label if first else " " * len(label), ofs, ln.strip()))
            i += 1
            first = False


def main():
    args = sys.argv[1:]
    if not args:
        sys.exit(__doc__)
    if args[0] == "--archive":
        if len(args) != 2:
            sys.exit("usage: symbolicate.py --archive <exe>")
        archive(args[1])
        return
    log_path = args[0]
    symbol_file = args[1] if len(args) > 1 else None
    text = sys.stdin.read() if log_path == "-" else open(log_path, "r", errors="replace").read()
    symbolicate(text, symbol_file)


if __name__ == "__main__":
    main()
