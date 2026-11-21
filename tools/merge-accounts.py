#!/usr/bin/env python3
"""Syluxium's own /etc/passwd, group, shadow and gshadow (rootfs/etc) plus the
system accounts Debian's install scripts created (adduser, base-passwd):
entries missing from ours are added; group members are combined.
Usage: merge-accounts.py <assembled rootfs> <configured Debian root>"""
import os
import sys

root, deb = sys.argv[1], sys.argv[2]
added = 0
for name, members_field in (("passwd", None), ("group", 3), ("shadow", None), ("gshadow", 3)):
    ours_p, theirs_p = os.path.join(root, "etc", name), os.path.join(deb, "etc", name)
    if not os.path.exists(theirs_p):
        continue
    ours = [l.rstrip("\n") for l in open(ours_p)] if os.path.exists(ours_p) else []
    index = {l.split(":")[0]: i for i, l in enumerate(ours) if l}
    for line in open(theirs_p):
        line = line.rstrip("\n")
        if not line:
            continue
        f = line.split(":")
        if f[0] not in index:
            ours.append(line)
            index[f[0]] = len(ours) - 1
            added += 1
        elif members_field is not None:
            mine = ours[index[f[0]]].split(":")
            if len(mine) > members_field and len(f) > members_field:
                both = [m for m in mine[members_field].split(",") if m]
                both += [m for m in f[members_field].split(",") if m and m not in both]
                mine[members_field] = ",".join(both)
                ours[index[f[0]]] = ":".join(mine)
    with open(ours_p, "w") as out:
        out.write("\n".join(ours) + "\n")
print(f"  ACCT  {added} system accounts/groups added from the install scripts")
