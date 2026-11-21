"""Shared wording: how TDE's own names read on Syluxium (used by
make-wording.py and debrand-tde.py)."""
import re

# Specific names first; whole-phrase replacements.
PHRASES = [
    ("Trinity Desktop Environment", "Syluxium desktop"),
    ("Trinity Control Center", "Control Panel"),
    ("TDE Control Center", "Control Panel"),
    ("Trinity Info Center", "System Information"),
    ("TDE Info Center", "System Information"),
    ("TDE Help Center", "Help and Support"),
    ("Trinity Classic Menu Style", "Classic Start Menu"),
    ("Trinity Classic", "Classic"),
    ("TDE Menu", "Start Menu"),
    ("TDE menu", "Start menu"),
    ("The TDE panel", "The taskbar"),
    ("TDE panel", "taskbar"),
    ("Trinity Panel", "Taskbar"),
    ("TDE Desktop", "Desktop"),
    ("The TDE desktop", "The desktop"),
    ("TDE Rocks!", "Syluxium"),
    ("Non-TDE", "Other"),
    ("non-TDE", "other"),
    ("TDEPowersave", "Power Options"),
    ("TDERandRTray", "Display Settings"),
    ("TDEFontView", "Font Viewer"),
    ("Welcome to Trinity at %n", "Welcome to %n"),
    ("Login to TDE", "Log On"),
    ("Shutdown TDE", "Turn Off Computer"),
]

# Credits and copyright lines keep naming the people who wrote the software.
KEEP = re.compile(r"\(c\)|copyright|maintainer|developers|bugfix|project", re.I)


def rename(text):
    """Text with TDE's names replaced, or None when it should stay as is."""
    if KEEP.search(text):
        return None
    out = text
    for old, new in PHRASES:
        out = out.replace(old, new)
    out = re.sub(r"\bTrinity\b", "Syluxium", out)
    out = re.sub(r"(?<![\w-])TDE(?![\w-])", "Syluxium", out)
    return out if out != text else None
