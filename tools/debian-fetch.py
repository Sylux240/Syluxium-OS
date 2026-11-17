#!/usr/bin/env python3
"""Download official Debian packages (with their dependencies) and unpack them.

    debian-fetch.py --out build/rootfs-debian --cache deps/debian bash busybox hello

Fetches the package index from deb.debian.org, resolves Depends/Pre-Depends
recursively, downloads each .deb (verifying its SHA256), and extracts the
data archive into --out using a merged-/usr layout (/bin -> usr/bin, ...).

This is what Syluxium's own package installer will do on the device later;
for now it runs at build time to put a Debian userland into the initramfs.
"""
import argparse
import gzip
import hashlib
import io
import lzma
import os
import re
import shutil
import stat
import subprocess
import sys
import tarfile
import urllib.request

MIRROR = "https://deb.debian.org/debian"
ARCH = "amd64"

# Paths not worth shipping in a RAM-backed root filesystem.
SKIP_PREFIXES = ("usr/share/doc/", "usr/share/man/", "usr/share/info/", "usr/share/locale/",
                 "usr/share/lintian/", "usr/share/bug/", "usr/share/doc-base/")


def fetch(url: str, dest: str) -> None:
    tmp = dest + ".part"
    # Some mirrors (Trinity's) reject Python's default user agent.
    req = urllib.request.Request(url, headers={"User-Agent": "Syluxium-debian-fetch/1.0"})
    with urllib.request.urlopen(req) as r, open(tmp, "wb") as f:
        shutil.copyfileobj(r, f)
    os.replace(tmp, dest)


def parse_index(text: str, base: str, packages: dict, provides: dict) -> None:
    """Add the stanzas of one Packages file; later repositories override earlier ones."""
    for stanza in text.split("\n\n"):
        fields, key = {}, None
        for line in stanza.splitlines():
            if line.startswith((" ", "\t")) and key:
                fields[key] += "\n" + line
            elif ":" in line:
                key, _, value = line.partition(":")
                fields[key] = value.strip()
        name = fields.get("Package")
        if not name:
            continue
        fields["_base"] = base
        packages[name] = fields
        for p in fields.get("Provides", "").split(","):
            p = p.strip().split(" ")[0]
            if p:
                provides[p] = name


def load_index(suite: str, cache: str, extra_repos: list) -> dict:
    """Package index for the global ARCH: Debian's main, then each extra
    repository ("BASEURL|SUITE|COMPONENT,COMPONENT", e.g. Trinity's)."""
    packages, provides = {}, {}
    path = os.path.join(cache, f"Packages-{suite}-{ARCH}.xz")
    if not os.path.exists(path):
        print(f"  fetching package index for {suite}/{ARCH}", file=sys.stderr)
        fetch(f"{MIRROR}/dists/{suite}/main/binary-{ARCH}/Packages.xz", path)
    parse_index(lzma.open(path, "rt", encoding="utf-8").read(), MIRROR, packages, provides)
    for spec in extra_repos:
        base, rsuite, comps = spec.split("|")
        tag = re.sub(r"[^A-Za-z0-9]+", "-", base.split("//")[-1]).strip("-")[-40:]
        for comp in comps.split(","):
            path = os.path.join(cache, f"Packages-{tag}-{rsuite}-{comp}-{ARCH}.gz")
            if not os.path.exists(path):
                print(f"  fetching package index {base} {rsuite}/{comp}/{ARCH}", file=sys.stderr)
                fetch(f"{base}/dists/{rsuite}/{comp}/binary-{ARCH}/Packages.gz", path)
            parse_index(gzip.open(path, "rt", encoding="utf-8").read(), base, packages, provides)
    # A real package always wins over a "Provides:" of another one.
    for name in packages:
        provides.pop(name, None)
    return {"packages": packages, "provides": provides}


def dependencies(fields: dict) -> list:
    deps = []
    for field in ("Pre-Depends", "Depends"):
        for group in fields.get(field, "").split(","):
            group = group.strip()
            if group:
                # Alternatives "a | b": take the first; drop version constraints and arch qualifiers.
                first = group.split("|")[0].strip()
                deps.append(re.split(r"[ (:]", first)[0])
    return deps


def resolve(index: dict, wanted: list, exclude: set) -> list:
    order, seen = [], set()

    def visit(name: str) -> None:
        real = name if name in index["packages"] else index["provides"].get(name)
        if not real:
            print(f"  warning: no package provides '{name}', skipping", file=sys.stderr)
            return
        if real in seen or real in exclude:
            return
        seen.add(real)
        for dep in dependencies(index["packages"][real]):
            visit(dep)
        order.append(real)

    for w in wanted:
        visit(w)
    return order


def download(index: dict, name: str, cache: str) -> str:
    fields = index["packages"][name]
    filename = fields["Filename"]
    dest = os.path.join(cache, os.path.basename(filename))
    if not os.path.exists(dest):
        print(f"  downloading {name} {fields['Version']}", file=sys.stderr)
        fetch(f"{fields['_base']}/{filename}", dest)
    digest = hashlib.sha256(open(dest, "rb").read()).hexdigest()
    if digest != fields["SHA256"]:
        os.remove(dest)
        sys.exit(f"SHA256 mismatch for {name}: refusing to use it")
    return dest


def ar_members(path: str) -> dict:
    data = open(path, "rb").read()
    if not data.startswith(b"!<arch>\n"):
        sys.exit(f"{path}: not an ar archive")
    pos, members = 8, {}
    while pos + 60 <= len(data):
        header = data[pos:pos + 60]
        name = header[0:16].decode().strip().rstrip("/")
        size = int(header[48:58].decode().strip())
        members[name] = data[pos + 60:pos + 60 + size]
        pos += 60 + size + (size & 1)
    return members


def open_data_tar(members: dict) -> tarfile.TarFile:
    for name, blob in members.items():
        if not name.startswith("data.tar"):
            continue
        if name.endswith(".zst"):
            blob = subprocess.run(["zstd", "-dc"], input=blob, capture_output=True, check=True).stdout
            return tarfile.open(fileobj=io.BytesIO(blob), mode="r:")
        return tarfile.open(fileobj=io.BytesIO(blob), mode="r:*")
    sys.exit("package has no data.tar member")


def extract(deb: str, out: str) -> int:
    count = 0
    with open_data_tar(ar_members(deb)) as tar:
        members = tar.getmembers()
        by_name = {m.name: m for m in members}
        for m in members:
            rel = m.name.lstrip("./")
            if not rel or rel.startswith(SKIP_PREFIXES) or ".." in rel.split("/"):
                continue
            dest = os.path.join(out, rel)
            if m.isdir():
                if not os.path.lexists(dest):
                    os.makedirs(dest, exist_ok=True)
                continue
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            if m.issym() and os.path.islink(dest) and os.readlink(dest) == m.linkname:
                continue  # e.g. base-files' /bin -> usr/bin, which we created already
            if os.path.islink(dest) or (os.path.lexists(dest) and not os.path.isdir(dest)):
                os.remove(dest)
            if m.issym():
                os.symlink(m.linkname, dest)
            elif m.islnk():
                # Hard links become copies: cpio newc stores hard-linked data only once.
                target = by_name.get(m.linkname) or by_name.get("./" + m.linkname.lstrip("./"))
                with tar.extractfile(target) as src, open(dest, "wb") as dst:
                    shutil.copyfileobj(src, dst)
                os.chmod(dest, target.mode & 0o7777)
            elif m.isfile():
                with tar.extractfile(m) as src, open(dest, "wb") as dst:
                    shutil.copyfileobj(src, dst)
                os.chmod(dest, m.mode & 0o7777)
            else:
                continue  # device nodes etc.: the kernel provides /dev itself
            count += 1
    return count


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--suite", default="trixie")
    ap.add_argument("--out", required=True)
    ap.add_argument("--cache", required=True)
    ap.add_argument("--exclude", default="", help="comma-separated packages to leave out")
    ap.add_argument("--arch", default="amd64", help="Debian architecture (amd64, i386, ...)")
    ap.add_argument("--repo", action="append", default=[],
                    help="extra repository BASEURL|SUITE|COMP1,COMP2 (packages there override Debian's)")
    ap.add_argument("packages", nargs="+")
    args = ap.parse_args()
    global ARCH
    ARCH = args.arch

    os.makedirs(args.cache, exist_ok=True)
    if os.path.exists(args.out):
        shutil.rmtree(args.out)
    os.makedirs(args.out)
    # Merged /usr, as on current Debian.
    for d in ("usr/bin", "usr/sbin", "usr/lib", "usr/lib64"):
        os.makedirs(os.path.join(args.out, d), exist_ok=True)
    for link, target in (("bin", "usr/bin"), ("sbin", "usr/sbin"), ("lib", "usr/lib"), ("lib64", "usr/lib64")):
        os.symlink(target, os.path.join(args.out, link))

    index = load_index(args.suite, args.cache, args.repo)
    exclude = {e for e in args.exclude.split(",") if e}
    order = resolve(index, args.packages, exclude)
    total = 0
    for name in order:
        total += extract(download(index, name, args.cache), args.out)
    print(f"  debian: {len(order)} packages, {total} files: {' '.join(order)}", file=sys.stderr)


if __name__ == "__main__":
    main()
