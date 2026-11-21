"""Shared code for Syluxium's system tools (System, Device Manager, Computer
Management, Image Writer): reading the machine's state and common GTK bits."""
import os
import subprocess

import gi

gi.require_version("Gtk", "3.0")
from gi.repository import Gtk, GLib, Pango  # noqa: E402

DEVICE_LIST = "/sys/kernel/syluxium/devices"


def read(path, default=""):
    try:
        with open(path) as f:
            return f.read().strip()
    except OSError:
        return default


def human(n):
    for unit in ("bytes", "KB", "MB", "GB", "TB"):
        if n < 1024 or unit == "TB":
            return f"{n:.0f} {unit}" if unit == "bytes" else f"{n:.1f} {unit}"
        n /= 1024


# ---- the machine ----
def os_version():
    v = read("/etc/syluxium-release") or "Syluxium"
    return v


def cpu_info():
    model, cores = "Unknown processor", 0
    for line in read("/proc/cpuinfo").splitlines():
        if line.startswith("model name") and model == "Unknown processor":
            model = line.split(":", 1)[1].strip()
        if line.startswith("processor"):
            cores += 1
    return " ".join(model.split()), cores


def memory_total():
    for line in read("/proc/meminfo").splitlines():
        if line.startswith("MemTotal:"):
            return int(line.split()[1]) * 1024
    return 0


def uptime():
    s = int(float(read("/proc/uptime", "0").split()[0]))
    d, s = divmod(s, 86400)
    h, s = divmod(s, 3600)
    m = s // 60
    return (f"{d} days, " if d else "") + f"{h}:{m:02d}"


def devices():
    """The kernel's device registry: one dict per device."""
    out = []
    for line in read(DEVICE_LIST).splitlines():
        f = line.split("\t")
        if len(f) < 9:
            continue
        out.append({"id": f[0], "bus": f[1], "location": f[2], "category": f[3], "ids": f[4],
                    "status": f[5], "driver": f[6], "name": f[7].strip(),
                    "status_text": f[8] if len(f) > 8 else ""})
    return out


def graphics():
    names = [d["name"] for d in devices() if d["category"] == "Display adapters"]
    return names or ["Firmware framebuffer"]


def disks():
    """Whole disks: name, size, model, removable, partitions [(name, size, fstype, label, mount)]."""
    mounts = {}
    for line in read("/proc/mounts").splitlines():
        p = line.split()
        if len(p) > 2 and p[0].startswith("/dev/"):
            mounts[p[0][5:]] = p[1]
    blk = {}
    try:
        out = subprocess.run(["blkid", "-o", "export"], capture_output=True, text=True, timeout=10).stdout
        cur = {}
        for line in out.splitlines() + [""]:
            if not line.strip():
                if "DEVNAME" in cur:
                    blk[cur["DEVNAME"][5:]] = cur
                cur = {}
            elif "=" in line:
                k, v = line.split("=", 1)
                cur[k] = v
    except (OSError, subprocess.SubprocessError):
        pass
    result = []
    for name in sorted(os.listdir("/sys/block") if os.path.isdir("/sys/block") else []):
        if name.startswith(("loop", "ram")):
            continue
        base = f"/sys/block/{name}"
        size = int(read(f"{base}/size", "0") or 0) * 512
        model = read(f"{base}/device/model") or read(f"{base}/device/name")
        parts = []
        for p in sorted(os.listdir(base)):
            if p.startswith(name) and os.path.isdir(f"{base}/{p}"):
                b = blk.get(p, {})
                parts.append((p, int(read(f"{base}/{p}/size", "0") or 0) * 512, b.get("TYPE", ""),
                              b.get("LABEL", ""), mounts.get(p, "")))
        if not parts and name in blk:  # a stick formatted without partitions
            b = blk[name]
            parts.append((name, size, b.get("TYPE", ""), b.get("LABEL", ""), mounts.get(name, "")))
        result.append({"name": name, "size": size, "model": model or "Disk",
                       "removable": read(f"{base}/removable") == "1", "parts": parts,
                       "system": any(m in ("/", "/boot", "/boot/efi") for _, _, _, _, m in parts)})
    return result


def is_admin():
    return os.geteuid() == 0


# ---- GTK helpers ----
def window(title, w=640, h=480, icon="computer"):
    win = Gtk.Window(title=title)
    win.set_default_size(w, h)
    win.set_icon_name(icon)
    win.set_position(Gtk.WindowPosition.CENTER)
    win.connect("destroy", Gtk.main_quit)
    return win


def label(text, bold=False, size=None, wrap=False, xalign=0.0):
    lab = Gtk.Label(xalign=xalign)
    markup = GLib.markup_escape_text(text)
    if bold:
        markup = f"<b>{markup}</b>"
    if size:
        markup = f"<span size='{size}'>{markup}</span>"
    lab.set_markup(markup)
    lab.set_line_wrap(wrap)
    lab.set_selectable(False)
    return lab


def message(parent, text, secondary="", kind=Gtk.MessageType.INFO, buttons=Gtk.ButtonsType.OK):
    d = Gtk.MessageDialog(transient_for=parent, modal=True, message_type=kind, buttons=buttons, text=text)
    if secondary:
        d.format_secondary_text(secondary)
    r = d.run()
    d.destroy()
    return r


def launch(*cmd):
    try:
        subprocess.Popen(list(cmd), start_new_session=True)
    except OSError as e:
        print("cannot start", cmd, e)


# ---- shared views (used standalone and inside Computer Management) ----
CATEGORY_ICONS = {
    "Processors": "cpu", "Display adapters": "video-display", "Network adapters": "network-wired",
    "Storage controllers": "drive-harddisk", "Sound, video and game controllers": "audio-card",
    "Universal Serial Bus controllers": "drive-removable-media", "Keyboards": "input-keyboard",
    "Mice and other pointing devices": "input-mouse", "Bridges": "computer", "System devices": "computer",
}


class DeviceTree(Gtk.Box):
    """Device Manager: devices grouped by category; problem devices flagged."""

    def __init__(self, parent):
        super().__init__(orientation=Gtk.Orientation.VERTICAL, spacing=4)
        self.parent_win = parent
        bar = Gtk.Box(spacing=4)
        for text, cb in (("Properties", self.on_properties), ("Refresh", lambda *_: self.refresh())):
            b = Gtk.Button(label=text)
            b.connect("clicked", cb)
            bar.pack_start(b, False, False, 0)
        self.pack_start(bar, False, False, 0)
        self.store = Gtk.TreeStore(str, str, object)  # icon, text, device
        self.view = Gtk.TreeView(model=self.store, headers_visible=False)
        col = Gtk.TreeViewColumn()
        icon = Gtk.CellRendererPixbuf()
        text = Gtk.CellRendererText()
        col.pack_start(icon, False)
        col.pack_start(text, True)
        col.add_attribute(icon, "icon-name", 0)
        col.add_attribute(text, "text", 1)
        self.view.append_column(col)
        self.view.connect("row-activated", lambda *_: self.on_properties())
        sw = Gtk.ScrolledWindow()
        sw.add(self.view)
        self.pack_start(sw, True, True, 0)
        self.refresh()

    def refresh(self):
        self.store.clear()
        host = os.uname().nodename or "This computer"
        root = self.store.append(None, ["computer", host, None])
        cats = {}
        for d in sorted(devices(), key=lambda d: (d["category"], d["name"])):
            if d["category"] not in cats:
                cats[d["category"]] = self.store.append(
                    root, [CATEGORY_ICONS.get(d["category"], "computer"), d["category"], None])
            problem = d["status"] not in ("ok", "running", "bound", "-")
            name = d["name"] or d["location"]
            self.store.append(cats[d["category"]], ["dialog-warning" if problem else "", name, d])
        self.view.expand_row(self.store.get_path(root), False)

    def on_properties(self, *_):
        model, it = self.view.get_selection().get_selected()
        d = model[it][2] if it else None
        if not d:
            return
        dlg = Gtk.Dialog(title=f"{d['name']} Properties", transient_for=self.parent_win, modal=True)
        dlg.add_button("OK", Gtk.ResponseType.OK)
        nb = Gtk.Notebook()
        for tab, rows in (("General", [("Device type", d["category"]), ("Location", f"{d['bus']} {d['location']}"),
                                        ("Device status", d["status_text"] or d["status"])]),
                          ("Driver", [("Driver", d["driver"] if d["driver"] != "-" else "No driver loaded"),
                                      ("Hardware ID", d["ids"]), ("Bus", d["bus"])])):
            grid = Gtk.Grid(column_spacing=16, row_spacing=8, margin=14)
            grid.attach(label(d["name"], bold=True), 0, 0, 2, 1)
            for i, (k, v) in enumerate(rows, 1):
                grid.attach(label(k + ":"), 0, i, 1, 1)
                grid.attach(label(v, wrap=True), 1, i, 1, 1)
            nb.append_page(grid, Gtk.Label(label=tab))
        dlg.get_content_area().add(nb)
        dlg.set_default_size(420, 300)
        dlg.show_all()
        dlg.run()
        dlg.destroy()


class DiskView(Gtk.Box):
    """Disk Management overview: disks, partitions, file systems, usage."""

    def __init__(self, parent):
        super().__init__(orientation=Gtk.Orientation.VERTICAL, spacing=4)
        bar = Gtk.Box(spacing=4)
        edit = Gtk.Button(label="Partition and Format...")
        edit.connect("clicked", lambda *_: launch("tdesudo", "gparted") if not is_admin() else launch("gparted"))
        refresh = Gtk.Button(label="Refresh")
        refresh.connect("clicked", lambda *_: self.refresh())
        bar.pack_start(edit, False, False, 0)
        bar.pack_start(refresh, False, False, 0)
        self.pack_start(bar, False, False, 0)
        self.store = Gtk.TreeStore(str, str, str, str, str, str)
        view = Gtk.TreeView(model=self.store)
        for i, title in enumerate(("Volume", "Size", "File System", "Label", "Mounted at")):
            if i == 0:
                c = Gtk.TreeViewColumn(title)
                ic, tx = Gtk.CellRendererPixbuf(), Gtk.CellRendererText()
                c.pack_start(ic, False)
                c.pack_start(tx, True)
                c.add_attribute(ic, "icon-name", 0)
                c.add_attribute(tx, "text", 1)
            else:
                c = Gtk.TreeViewColumn(title, Gtk.CellRendererText(), text=i + 1)
            c.set_resizable(True)
            view.append_column(c)
        self.view = view
        sw = Gtk.ScrolledWindow()
        sw.add(view)
        self.pack_start(sw, True, True, 0)
        self.refresh()

    def refresh(self):
        self.store.clear()
        for d in disks():
            kind = "Removable disk" if d["removable"] else "Disk"
            icon = "drive-removable-media" if d["removable"] else "drive-harddisk"
            top = self.store.append(None, [icon, f"{kind} {d['name']} ({d['model']})", human(d["size"]), "", "",
                                           "System disk" if d["system"] else ""])
            for name, size, fs, lab, mnt in d["parts"]:
                self.store.append(top, ["drive-harddisk", name, human(size), fs or "Unknown", lab, mnt])
        self.view.expand_all()
