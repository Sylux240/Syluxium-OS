"""Shared code for Syluxium's system tools (System, Device Manager, Computer
Management, Image Writer): reading the machine's state and common GTK bits."""
import os
import shlex
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
    """Every device: one dict each (id, bus, location, category, ids, status,
    driver, name, status_text)."""
    if os.path.exists(DEVICE_LIST):  # Syluxium's own kernel's device registry
        out = []
        for line in read(DEVICE_LIST).splitlines():
            f = line.split("\t")
            if len(f) < 9:
                continue
            out.append({"id": f[0], "bus": f[1], "location": f[2], "category": f[3], "ids": f[4],
                        "status": f[5], "driver": f[6], "name": f[7].strip(),
                        "status_text": f[8] if len(f) > 8 else ""})
        return out
    return linux_devices()


# PCI classes (class, subclass) -> Device Manager category.
PCI_CATEGORIES = {
    0x01: "Storage controllers", 0x02: "Network adapters", 0x03: "Display adapters",
    0x04: "Sound, video and game controllers", 0x06: "Bridges", 0x07: "Ports (COM & LPT)",
    0x08: "System devices", 0x09: "Keyboards and mice", 0x0b: "Processors", 0x0d: "Network adapters",
    0x0c: "System devices", (0x0c, 0x03): "Universal Serial Bus controllers", (0x04, 0x03): "Sound, video and game controllers",
    0x05: "System devices", 0x10: "System devices", 0x11: "System devices", 0x12: "System devices",
}
# USB interface classes -> category.
USB_CATEGORIES = {
    0x01: "Sound, video and game controllers", 0x02: "Network adapters", 0x03: "Human Interface Devices",
    0x06: "Imaging devices", 0x07: "Printers", 0x08: "Disk drives", 0x09: "Universal Serial Bus controllers",
    0x0a: "Network adapters", 0x0b: "Smart card readers", 0x0e: "Cameras", 0xe0: "Bluetooth",
}


def _udev_names():
    """{sysfs path: (vendor, model)} from udev's hardware database (one call)."""
    names, path, props = {}, None, {}
    try:
        out = subprocess.run(["udevadm", "info", "--export-db"], capture_output=True, text=True,
                             timeout=15).stdout
    except (OSError, subprocess.SubprocessError):
        return names
    for line in out.splitlines() + [""]:
        if line.startswith("P: "):
            path, props = "/sys" + line[3:], {}
        elif line.startswith("E: ") and "=" in line:
            k, v = line[3:].split("=", 1)
            props[k] = v
        elif not line.strip() and path:
            v = props.get("ID_VENDOR_FROM_DATABASE", "")
            m = props.get("ID_MODEL_FROM_DATABASE", "")
            if v or m:
                names[os.path.realpath(path)] = (v, m)
            path = None
    return names


def _driver(path):
    link = os.path.join(path, "driver")
    return os.path.basename(os.readlink(link)) if os.path.islink(link) else ""


def _hex(path, default=0):
    try:
        return int(read(path, "0") or "0", 16)
    except ValueError:
        return default


def linux_devices():
    """The Linux kernel's devices from sysfs: PCI functions, USB devices (by
    their interfaces' classes and drivers) and the processors."""
    names = _udev_names()
    ok_text, none_text = "This device is working properly.", "No driver is installed for this device."
    out, lspci = [], None
    base = "/sys/bus/pci/devices"
    for slot in sorted(os.listdir(base)) if os.path.isdir(base) else []:
        p = os.path.realpath(os.path.join(base, slot))
        cls = _hex(f"{p}/class")
        cat = PCI_CATEGORIES.get((cls >> 16, (cls >> 8) & 0xff)) or PCI_CATEGORIES.get(cls >> 16, "Other devices")
        ven, dev = _hex(f"{p}/vendor"), _hex(f"{p}/device")
        v, m = names.get(p, ("", ""))
        if not m:  # not in udev's database: lspci's (pci.ids)
            if lspci is None:
                lspci = {}
                try:
                    for line in subprocess.run(["lspci", "-mm", "-D"], capture_output=True, text=True,
                                               timeout=10).stdout.splitlines():
                        f = shlex.split(line)
                        if len(f) >= 4:
                            lspci[f[0]] = (f[2], f[3])
                except (OSError, ValueError, subprocess.SubprocessError):
                    pass
            v, m = lspci.get(slot, ("", ""))
        name = (f"{v} {m}".strip() if m else "") or f"PCI device {ven:04x}:{dev:04x}"
        drv = _driver(p)
        # Bridges, the chipset's internal functions and the like work without one.
        needs = (cls >> 16) in (0x01, 0x02, 0x03, 0x04, 0x07, 0x09, 0x0c, 0x0d)
        out.append({"id": slot, "bus": "pci", "location": slot, "category": cat, "ids": f"{ven:04x}:{dev:04x}",
                    "status": "ok" if drv or not needs else "nodriver", "driver": drv or "-", "name": name,
                    "status_text": ok_text if drv or not needs else none_text})
    base = "/sys/bus/usb/devices"
    for d in sorted(os.listdir(base)) if os.path.isdir(base) else []:
        p = os.path.realpath(os.path.join(base, d))
        if ":" in d or not os.path.exists(f"{p}/idVendor") or d.startswith("usb"):
            continue  # interfaces, and the controllers' root hubs (the controller is listed)
        ven, prod = _hex(f"{p}/idVendor"), _hex(f"{p}/idProduct")
        cat, drivers = "", []
        for i in sorted(os.listdir(p)):
            ip = os.path.join(p, i)
            if not i.startswith(d + ":") or not os.path.isdir(ip):
                continue
            ic, proto = _hex(f"{ip}/bInterfaceClass"), _hex(f"{ip}/bInterfaceProtocol")
            if ic == 0x03 and proto == 1:
                c = "Keyboards"
            elif ic == 0x03 and proto == 2:
                c = "Mice and other pointing devices"
            elif ic == 0xff and os.path.isdir(f"{ip}/net"):
                c = "Network adapters"
            else:
                c = USB_CATEGORIES.get(ic, "")
            if c and (not cat or cat == "Human Interface Devices"):
                cat = c
            dr = _driver(ip)
            if dr and dr not in drivers:
                drivers.append(dr)
        cat = cat or USB_CATEGORIES.get(_hex(f"{p}/bDeviceClass"), "Other devices")
        v, m = names.get(p, ("", ""))
        name = " ".join(x for x in (read(f"{p}/manufacturer"), read(f"{p}/product")) if x) \
            or f"{v} {m}".strip() or f"USB device {ven:04x}:{prod:04x}"
        out.append({"id": d, "bus": "usb", "location": f"port {d}", "category": cat, "ids": f"{ven:04x}:{prod:04x}",
                    "status": "ok" if drivers else "nodriver", "driver": ", ".join(drivers) or "-", "name": name,
                    "status_text": ok_text if drivers else none_text})
    model, cores = cpu_info()
    for n in range(cores):
        out.append({"id": f"cpu{n}", "bus": "cpu", "location": f"CPU {n}", "category": "Processors", "ids": "",
                    "status": "ok", "driver": "-", "name": model, "status_text": ok_text})
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
        # (Linux: also CD/DVD drives, compressed swap, device-mapper and network block devices)
        if name.startswith(("loop", "ram", "sr", "zram", "dm-", "nbd", "fd", "mtdblock")):
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
                       # (Linux calls USB hard disks and SSDs fixed: anything on USB counts as removable)
                       "removable": read(f"{base}/removable") == "1" or "/usb" in os.path.realpath(base),
                       "parts": parts,
                       "system": any(m in ("/", "/boot", "/boot/efi") for _, _, _, _, m in parts)})
    return result


def is_admin():
    return os.geteuid() == 0


def desktop_session():
    """"xfce", "tde", "wayland" (the Syluxium shell) or "" (unknown)."""
    if os.environ.get("XDG_SESSION_TYPE") == "wayland" or os.environ.get("WAYLAND_DISPLAY"):
        return "wayland"
    current = os.environ.get("XDG_CURRENT_DESKTOP", "").upper()
    if "XFCE" in current:
        return "xfce"
    if "TDE" in current or "TRINITY" in current or os.environ.get("TDE_FULL_SESSION"):
        return "tde"
    return ""


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


class AdminRunner:
    """Runs commands as root through sudo with the user's own password (asked
    once per window), like the App Store's administrator prompt."""

    def __init__(self, parent, why="This changes the computer for everyone."):
        self.parent, self.why, self.password = parent, why, None

    def ready(self):
        if os.geteuid() == 0 or self.password is not None:
            return True
        if subprocess.run(["sudo", "-n", "true"], capture_output=True).returncode == 0:
            self.password = ""
            return True
        user = os.environ.get("USER") or os.environ.get("LOGNAME") or "you"
        for attempt in range(3):
            d = Gtk.Dialog(title="Administrator Permission", transient_for=self.parent, modal=True)
            d.add_buttons("Cancel", Gtk.ResponseType.CANCEL, "OK", Gtk.ResponseType.OK)
            d.set_default_response(Gtk.ResponseType.OK)
            box = d.get_content_area()
            box.set_spacing(8)
            box.set_margin_start(12)
            box.set_margin_end(12)
            box.set_margin_top(12)
            row = Gtk.Box(spacing=12)
            row.pack_start(Gtk.Image.new_from_icon_name("dialog-password", Gtk.IconSize.DIALOG), False, False, 0)
            row.pack_start(label(f"{self.why}\nType the password of {user} (an administrator) to continue."
                                 + ("\n\nThat password was not right. Try again." if attempt else ""),
                                 wrap=True), True, True, 0)
            box.add(row)
            entry = Gtk.Entry(visibility=False, activates_default=True)
            box.add(entry)
            d.show_all()
            r = d.run()
            pw = entry.get_text()
            d.destroy()
            if r != Gtk.ResponseType.OK:
                return False
            p = subprocess.run(["sudo", "-S", "-k", "-p", "", "true"], input=pw + "\n", capture_output=True,
                               text=True)
            if p.returncode == 0:
                self.password = pw
                return True
            if "not in the sudoers" in p.stderr or "may not run sudo" in p.stderr:
                message(self.parent, "Only administrators can do this.",
                        f"{user} is not an administrator.", Gtk.MessageType.WARNING)
                return False
        return False

    def run(self, cmd, timeout=60):
        """(exit code, output) of cmd run as root; (-1, "") when not allowed."""
        if not self.ready():
            return -1, ""
        full = list(cmd) if os.geteuid() == 0 else ["sudo", "-S", "-p", ""] + list(cmd)
        try:
            p = subprocess.run(full, input=(self.password or "") + "\n", capture_output=True, text=True,
                               timeout=timeout)
        except subprocess.TimeoutExpired:
            return 124, "It took too long."
        return p.returncode, (p.stdout + p.stderr).strip()


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
    "Keyboards and mice": "input-keyboard", "Sound": "audio-card", "USB controllers": "drive-removable-media",
    "Disk drives": "drive-harddisk", "Cameras": "camera-web", "Bluetooth": "bluetooth",
    "Human Interface Devices": "input-gaming", "Printers": "printer", "Imaging devices": "scanner",
    "Smart card readers": "media-flash", "Ports (COM & LPT)": "network-wired",
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
        edit.connect("clicked", lambda *_: launch("syluxium-sudo", "gparted") if not is_admin() else launch("gparted"))
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
