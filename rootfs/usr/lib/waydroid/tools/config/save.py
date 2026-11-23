# Copyright 2021 Oliver Smith
# SPDX-License-Identifier: GPL-3.0-or-later
# Syluxium: written to a temporary file and renamed over the old one, so a
# reader never sees a half-written config (the session's services read it
# while the container manager saves it; a truncated file gave KeyError
# 'binder' and stopped the session).
import os
import logging


def save(args, cfg):
    logging.debug("Save config: " + args.config)
    os.makedirs(os.path.dirname(args.config), 0o700, True)
    tmp = args.config + ".tmp"
    with open(tmp, "w") as handle:
        cfg.write(handle)
        handle.flush()
        os.fsync(handle.fileno())
    try:
        mode = os.stat(args.config).st_mode & 0o7777
    except OSError:
        mode = 0o644
    os.chmod(tmp, mode)
    os.replace(tmp, args.config)
