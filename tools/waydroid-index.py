#!/usr/bin/env python3
"""Package index for Waydroid's repository (repo.waydro.id), as
tools/debian-fetch.py expects to find it in its cache.

The repository publishes a single binary-all index that also lists its amd64
and arm64 packages (libgbinder, python3-gbinder, ...); debian-fetch.py asks
for binary-amd64, which isn't there. This writes the amd64 view (Architecture
all or amd64) to the cache file debian-fetch.py would have downloaded:
    <cache>/Packages-repo-waydro-id-<suite>-main-amd64.gz
Usage: waydroid-index.py <cache dir> [suite]
"""
import gzip
import os
import sys
import time
import urllib.request

BASE = "https://repo.waydro.id"


def main() -> int:
    cache = sys.argv[1]
    suite = sys.argv[2] if len(sys.argv) > 2 else "trixie"
    out = os.path.join(cache, f"Packages-repo-waydro-id-{suite}-main-amd64.gz")
    if os.path.exists(out) and time.time() - os.path.getmtime(out) < 7 * 86400:
        return 0
    os.makedirs(cache, exist_ok=True)
    url = f"{BASE}/dists/{suite}/main/binary-all/Packages.gz"
    print(f"  fetching package index {url}", file=sys.stderr)
    with urllib.request.urlopen(url, timeout=60) as r:
        text = gzip.decompress(r.read()).decode("utf-8")
    keep = []
    for stanza in text.split("\n\n"):
        arch = ""
        for line in stanza.splitlines():
            if line.startswith("Architecture:"):
                arch = line.split(":", 1)[1].strip()
        if stanza.strip() and arch in ("all", "amd64"):
            keep.append(stanza.strip("\n"))
    tmp = out + ".tmp"
    with gzip.open(tmp, "wt", encoding="utf-8") as f:
        f.write("\n\n".join(keep) + "\n")
    os.replace(tmp, out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
