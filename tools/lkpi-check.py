#!/usr/bin/env python3
"""Try an upstream Linux driver directory against the LinuxKPI headers.

    tools/lkpi-check.py drivers/net/ethernet/intel/igb
    tools/lkpi-check.py drivers/net/wireless/realtek/rtw89 --include net/lkpi_netcompat.h
    tools/lkpi-check.py --ours kernel/linuxkpi/drivers/net/ethernet/intel/igb   (an imported copy)

Every .c file the directory's Makefile names (or --files) is compiled with
the flags the kernel build uses for Linux code (make print-lkpi-cflags).
Nothing is written into the source tree: objects and the report go to
build/lkpi-check/<name>/. The report lists, most frequent first:

  missing headers         <linux/foo.h> we have no shim for
  undeclared functions    called but not declared by our headers
  undeclared identifiers  macros, constants, enum values
  missing struct members  fields our shim structs lack
  incomplete types        structs only forward-declared
  other errors            everything else
  unresolved symbols      what the objects that did compile still need at link
                          time and neither they nor build/syluxium.elf define

So "what does driver X still need?" is one command, and a shim is done when
the lists are empty. The upstream tree is read-only input; --upstream or
$LKPI_UPSTREAM picks it (default deps/linux-upstream, a git-ignored cache:
a symlink to an unpacked Linux tree, see docs/drivers.md).
"""
import argparse
import collections
import concurrent.futures
import os
import re
import shlex
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def lkpi_cflags():
    out = subprocess.run(["make", "-s", "--no-print-directory", "print-lkpi-cflags"], cwd=ROOT,
                         capture_output=True, text=True, check=True).stdout
    return shlex.split(out.strip().splitlines()[-1])


def makefile_sources(d):
    """The .c files a kbuild Makefile mentions (all branches of conditionals)."""
    mk = os.path.join(d, "Makefile")
    if not os.path.exists(mk):
        return sorted(f for f in os.listdir(d) if f.endswith(".c"))
    text = open(mk, errors="replace").read().replace("\\\n", " ")
    found = []
    for line in text.splitlines():
        line = line.split("#", 1)[0]
        if not re.search(r"[+:]?=", line):
            continue
        rhs = re.split(r"[+:]?=", line, maxsplit=1)[1]
        for tok in rhs.split():
            if tok.endswith(".o"):
                c = tok[:-2] + ".c"
                if os.path.exists(os.path.join(d, c)) and c not in found:
                    found.append(c)
    return found


PATTERNS = [
    ("missing headers", re.compile(r"^'([^']+)' file not found")),
    ("undeclared functions", re.compile(r"(?:call to undeclared (?:library )?function|implicit declaration of function) '([^']+)'")),
    ("undeclared identifiers", re.compile(r"use of undeclared identifier '([^']+)'")),
    ("missing struct members", re.compile(r"no member named '([^']+)' in '([^']+)'")),
    ("incomplete types", re.compile(r"(?:incomplete (?:definition of )?type|variable has incomplete type) '([^']+)'")),
    ("unknown types", re.compile(r"unknown type name '([^']+)'")),
]


def classify(stderr, counters, examples):
    for line in stderr.splitlines():
        if " error: " not in line:
            continue
        msg = line.split(" error: ", 1)[1].strip()
        for cat, rx in PATTERNS:
            m = rx.search(msg)
            if m:
                key = m.group(1) if cat != "missing struct members" else f"{m.group(2)}.{m.group(1)}"
                counters[cat][key] += 1
                break
        else:
            key = re.sub(r"'[^']*'", "'…'", msg)
            counters["other errors"][key] += 1
            examples.setdefault(key, os.path.basename(line.split(" error: ", 1)[0]) + " " + msg)


def defined_symbols(paths):
    syms = set()
    for p in paths:
        if not os.path.exists(p):
            continue
        out = subprocess.run(["nm", "--defined-only", p], capture_output=True, text=True).stdout
        for line in out.splitlines():
            parts = line.split()
            if len(parts) >= 3:
                syms.add(parts[2])
    return syms


def undefined_symbols(path):
    out = subprocess.run(["nm", "-u", path], capture_output=True, text=True).stdout
    return {l.split()[-1] for l in out.splitlines() if l.strip()}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dir", help="driver directory, relative to the upstream tree (or to ours with --ours)")
    ap.add_argument("--upstream", default=os.environ.get("LKPI_UPSTREAM", os.path.join(ROOT, "deps/linux-upstream")))
    ap.add_argument("--ours", action="store_true", help="DIR is a path in this repository (an imported driver)")
    ap.add_argument("--files", help="comma-separated .c files (default: what the Makefile names)")
    ap.add_argument("--exclude", default="", help="comma-separated .c files to skip")
    ap.add_argument("--include", action="append", default=[], help="force-included header (like the Makefile's per-driver -include)")
    ap.add_argument("-I", dest="incs", action="append", default=[], help="extra include directory")
    ap.add_argument("-D", dest="defs", action="append", default=[], help="extra define")
    ap.add_argument("--modname", help="KBUILD_MODNAME (default: the directory name)")
    ap.add_argument("--kernel", default=os.path.join(ROOT, "build/syluxium.elf"))
    ap.add_argument("-j", type=int, default=os.cpu_count() or 4)
    ap.add_argument("-v", "--verbose", action="store_true", help="also print every compiler error")
    ap.add_argument("--top", type=int, default=40, help="entries shown per list")
    ap.add_argument("--no-stubs", action="store_true", help="stop at missing headers (no empty stand-ins)")
    a = ap.parse_args()

    base = ROOT if a.ours else a.upstream
    src = os.path.join(base, a.dir)
    if not os.path.isdir(src):
        sys.exit(f"lkpi-check: no directory {src} (set --upstream or $LKPI_UPSTREAM)")
    name = a.dir.strip("/").replace("/", "_")
    out = os.path.join(ROOT, "build/lkpi-check", name)
    os.makedirs(os.path.join(out, "obj"), exist_ok=True)

    files = a.files.split(",") if a.files else makefile_sources(src)
    skip = set(filter(None, a.exclude.split(",")))
    files = [f for f in files if f not in skip]
    if not files:
        sys.exit("lkpi-check: no .c files found")
    modname = a.modname or os.path.basename(a.dir.rstrip("/"))
    flags = lkpi_cflags() + ["-ferror-limit=0", "-fno-color-diagnostics", "-I" + src]
    for h in a.include:
        flags += ["-include", h]
    flags += ["-I" + (i if os.path.isabs(i) else os.path.join(base, i)) for i in a.incs]
    flags += ["-D" + d for d in a.defs]
    flags += [f"-DKBUILD_MODNAME=\"{modname}\""]

    def compile_one(f):
        obj = os.path.join(out, "obj", f.replace("/", "_") + ".o")
        cmd = ["clang"] + flags + [f"-DKBUILD_BASENAME=\"{os.path.basename(f)[:-2]}\"", "-c", os.path.join(src, f), "-o", obj]
        r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
        return f, r.returncode, r.stderr, obj

    # A missing header stops the compiler at once and hides everything
    # behind it. Unless --no-stubs, each missing header gets an empty stand-in
    # (build/lkpi-check/<name>/stubs, searched after our headers) and the
    # files are compiled again, so one run shows the whole picture; the
    # stubbed headers are still reported as missing.
    stubdir = os.path.join(out, "stubs")
    if os.path.isdir(stubdir):
        subprocess.run(["rm", "-rf", stubdir])
    stubbed = collections.Counter()
    for _round in range(1 if a.no_stubs else 12):
        counters = collections.defaultdict(collections.Counter)
        examples = {}
        ok, failed = [], []
        with concurrent.futures.ThreadPoolExecutor(a.j) as ex:
            for f, rc, err, obj in ex.map(compile_one, files):
                classify(err, counters, examples)
                (ok if rc == 0 else failed).append((f, obj, err))
        new = [h for h in counters.get("missing headers", {}) if h not in stubbed]
        if a.no_stubs or not new:
            break
        for h in new:
            stubbed[h] += counters["missing headers"][h]
            p = os.path.join(stubdir, h)
            os.makedirs(os.path.dirname(p), exist_ok=True)
            open(p, "w").close()
        if "-idirafter" + stubdir not in flags:
            flags.append("-idirafter" + stubdir)
    if stubbed:
        counters["missing headers"] = stubbed + counters.get("missing headers", collections.Counter())
    if a.verbose:
        for _, _, err in failed:
            print(err)

    lines = [f"lkpi-check {a.dir} ({'ours' if a.ours else base})",
             f"compiled: {len(ok)}/{len(files)} files"]
    if failed:
        lines.append("failed:   " + " ".join(f for f, _, _ in failed))
    for cat in [c for c, _ in PATTERNS] + ["other errors"]:
        c = counters.get(cat)
        if not c:
            continue
        lines.append(f"\n== {cat} ({len(c)} distinct, {sum(c.values())} errors)")
        for key, n in c.most_common(a.top):
            lines.append(f"  {n:5}  {examples.get(key, key) if cat == 'other errors' else key}")
        if len(c) > a.top:
            lines.append(f"  ... {len(c) - a.top} more (see {out}/report.txt)")

    if ok:
        have = defined_symbols([o for _, o, _ in ok] + [a.kernel])
        need = collections.Counter()
        for f, obj, _ in ok:
            for s in undefined_symbols(obj) - have:
                need[s] += 1
        lines.append(f"\n== unresolved symbols of the compiled objects ({len(need)}; checked against "
                     f"{os.path.relpath(a.kernel, ROOT)})")
        for s, n in sorted(need.items(), key=lambda kv: (-kv[1], kv[0]))[: max(a.top, 200)]:
            lines.append(f"  {n:5}  {s}")
    report = "\n".join(lines) + "\n"
    with open(os.path.join(out, "report.txt"), "w") as fh:
        fh.write(report)
        for f, _, err in failed:
            fh.write(f"\n\n#### {f}\n{err}")
    print(report, end="")
    print(f"(full report with compiler output: {os.path.relpath(out, ROOT)}/report.txt)")
    return 0 if not failed else 1


if __name__ == "__main__":
    sys.exit(main())
