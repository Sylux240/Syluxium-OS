"""The App Store's "Android Apps" page: F-Droid's catalog of free and
open-source Android apps (browse, search, screenshots), installed, updated
and removed through Android on this computer (Waydroid, syluxium-android).
Also: installing the F-Droid app itself, and the optional Google Play setup.
"""
import os
import subprocess
import threading
import time

import gi

gi.require_version("Gtk", "3.0")
gi.require_version("GdkPixbuf", "2.0")
from gi.repository import GdkPixbuf, GLib, Gtk, Pango  # noqa: E402

from syluxium_apk import describe_permissions  # noqa: E402
from syluxium_fdroid import Catalog, FdroidError, installed, record_installed  # noqa: E402
from syluxium_sys import label, message  # noqa: E402

CATALOG_MAX_AGE = 24 * 3600
HELPER = ["sudo", "-n", "/usr/local/sbin/syluxium-android-container"]
FEATURED_ANDROID = ["org.fdroid.fdroid", "org.mozilla.fennec_fdroid", "org.videolan.vlc", "net.osmand.plus",
                    "org.telegram.messenger", "com.termux", "de.danoeh.antennapod", "org.wikipedia",
                    "com.simplemobiletools.gallery.pro", "org.schabi.newpipe", "com.github.libretube",
                    "org.thoughtcrime.securesms"]


def human(n):
    for unit in ("bytes", "KB", "MB", "GB"):
        if n < 1000 or unit == "GB":
            return f"{n:.0f} {unit}" if unit == "bytes" else f"{n:.1f} {unit}"
        n /= 1000


def android_status():
    try:
        return subprocess.run(["syluxium-android", "status"], capture_output=True, text=True,
                              timeout=20).stdout.strip()
    except (OSError, subprocess.TimeoutExpired):
        return "unknown"  # (a busy Android can be slow to answer)


class AndroidPage:
    def __init__(self, store):
        self.store, self.win = store, store.win
        self.catalog = Catalog()
        self.apps = {}
        self.installed = {}
        self.current = None
        self.busy = False
        self.icon_cache = {}
        self.loading = False
        # Test aid (android-ui-test store): SYLUXIUM_STORE_TEST=PACKAGE[,PACKAGE...] installs it,
        # opens it, updates it (an older version recorded first) and removes it,
        # through the same code as the buttons; "STORETEST" lines on stdout.
        self.test_pkg = os.environ.get("SYLUXIUM_STORE_TEST") or None
        self.test_step = None
        self.widget = self.build()

    # ---- layout ----
    def build(self):
        outer = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=6)
        outer.set_margin_top(8), outer.set_margin_end(8)
        bar = Gtk.Box(spacing=8)
        self.title = label("Android Apps", bold=True, size="large")
        bar.pack_start(self.title, False, False, 0)
        self.filter = Gtk.ComboBoxText()
        self.filter.append("featured", "Recommended")
        self.filter.append("all", "All apps")
        self.filter.append("installed", "Installed")
        self.filter.append("updates", "Updates")
        self.filter.set_active_id("featured")
        self.filter.connect("changed", lambda *_: self.fill())
        bar.pack_start(self.filter, False, False, 0)
        self.category = Gtk.ComboBoxText()
        self.category.append("", "Any category")
        self.category.set_active_id("")
        self.category.connect("changed", lambda *_: self.fill())
        bar.pack_start(self.category, False, False, 0)
        self.search = Gtk.SearchEntry(placeholder_text="Search Android apps")
        self.search.connect("search-changed", lambda *_: self.fill())
        bar.pack_start(self.search, True, True, 0)
        gplay = Gtk.Button(label="Google Play...")
        gplay.set_tooltip_text("Optional: Google's apps and services (you choose; not installed by default)")
        gplay.connect("clicked", lambda *_: GooglePlayDialog(self))
        bar.pack_end(gplay, False, False, 0)
        refresh = Gtk.Button.new_from_icon_name("view-refresh", Gtk.IconSize.BUTTON)
        refresh.set_tooltip_text("Download the newest F-Droid catalog")
        refresh.connect("clicked", lambda *_: self.refresh(force=True))
        bar.pack_end(refresh, False, False, 0)
        outer.pack_start(bar, False, False, 0)
        self.note = label("Free and open-source Android apps from F-Droid. They run in Android on this computer, "
                          "each in its own window, and appear in the Start menu under Android Apps.", wrap=True)
        outer.pack_start(self.note, False, False, 0)

        paned = Gtk.Paned(orientation=Gtk.Orientation.VERTICAL)
        # icon, markup, size, status, package, sort key
        self.rows = Gtk.ListStore(GdkPixbuf.Pixbuf, str, str, str, str)
        self.view = Gtk.TreeView(model=self.rows)
        self.view.set_enable_search(False)
        col = Gtk.TreeViewColumn("App")
        ic, tx = Gtk.CellRendererPixbuf(xpad=4, ypad=2), Gtk.CellRendererText(ellipsize=Pango.EllipsizeMode.END)
        col.pack_start(ic, False), col.pack_start(tx, True)
        col.add_attribute(ic, "pixbuf", 0), col.add_attribute(tx, "markup", 1)
        col.set_expand(True)
        self.view.append_column(col)
        self.view.append_column(Gtk.TreeViewColumn("Download", Gtk.CellRendererText(xalign=1.0), text=2))
        self.view.append_column(Gtk.TreeViewColumn("Status", Gtk.CellRendererText(), text=3))
        self.view.get_selection().connect("changed", self.on_row)
        sw = Gtk.ScrolledWindow(shadow_type=Gtk.ShadowType.IN)
        sw.add(self.view)
        self.list_stack = Gtk.Stack()
        self.list_stack.add_named(sw, "list")
        msg = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=10, valign=Gtk.Align.CENTER)
        self.msg_spinner = Gtk.Spinner()
        msg.pack_start(self.msg_spinner, False, False, 0)
        self.msg_label = label("", wrap=True, xalign=0.5)
        msg.pack_start(self.msg_label, False, False, 0)
        self.msg_button = Gtk.Button(label="Download catalog")
        self.msg_button.set_halign(Gtk.Align.CENTER)
        self.msg_button.connect("clicked", lambda *_: self.refresh(force=True))
        msg.pack_start(self.msg_button, False, False, 0)
        self.list_stack.add_named(msg, "message")
        paned.pack1(self.list_stack, True, False)
        paned.pack2(self.details(), False, False)
        paned.set_position(230)
        self.paned = paned
        outer.pack_start(paned, True, True, 0)
        return outer

    def details(self):
        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=4)
        box.set_margin_top(6)
        head = Gtk.Box(spacing=10)
        self.d_icon = Gtk.Image()
        head.pack_start(self.d_icon, False, False, 0)
        names = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
        self.d_name = label("", bold=True, size="large")
        self.d_info = label("", xalign=0, wrap=True)
        names.pack_start(self.d_name, False, False, 0)
        names.pack_start(self.d_info, False, False, 0)
        head.pack_start(names, True, True, 0)
        self.d_open = Gtk.Button.new_with_mnemonic("_Open")
        self.d_remove = Gtk.Button.new_with_mnemonic("_Remove")
        self.d_install = Gtk.Button.new_with_mnemonic("_Install")
        self.d_install.get_style_context().add_class("suggested-action")
        self.d_open.connect("clicked", lambda *_: self.open_app(self.current))
        self.d_remove.connect("clicked", lambda *_: self.remove(self.current))
        self.d_install.connect("clicked", lambda *_: self.install(self.current))
        for b in (self.d_install, self.d_remove, self.d_open):
            b.set_valign(Gtk.Align.CENTER)
            head.pack_end(b, False, False, 0)
        box.pack_start(head, False, False, 0)
        self.shots = Gtk.Box(spacing=6)
        ssw = Gtk.ScrolledWindow(vscrollbar_policy=Gtk.PolicyType.NEVER)
        ssw.set_size_request(-1, 112)
        ssw.add(self.shots)
        box.pack_start(ssw, False, False, 0)
        self.d_text = Gtk.TextView(editable=False, cursor_visible=False, wrap_mode=Gtk.WrapMode.WORD)
        self.d_text.set_left_margin(6), self.d_text.set_right_margin(6)
        sw = Gtk.ScrolledWindow(shadow_type=Gtk.ShadowType.IN)
        sw.set_size_request(-1, 70)
        sw.add(self.d_text)
        box.pack_start(sw, True, True, 0)
        self.show_app(None)
        return box

    # ---- catalog ----
    def activate(self):
        """The page was opened: load the catalog (download it if missing or old)."""
        self.installed = installed()
        self.show_app(self.current)  # (the window's show_all() showed every button)
        if not self.apps:
            self.apps = self.catalog.load()
            self.fill_categories()
        age = self.catalog.age()
        if not self.apps:
            # First visit: fetch the catalog right away (the page is useless without it).
            self.refresh(force=True)
            return
        self.fill()
        if age is None or age > CATALOG_MAX_AGE:
            self.refresh()

    def show_message(self, text, button=False, spinning=False):
        self.msg_label.set_text(text)
        self.msg_button.set_visible(button)
        self.msg_spinner.set_visible(spinning)
        (self.msg_spinner.start if spinning else self.msg_spinner.stop)()
        self.list_stack.set_visible_child_name("message")

    def refresh(self, force=False):
        if self.loading:
            return
        self.loading = True
        if not self.apps:
            self.show_message("Downloading F-Droid's catalog of Android apps (about 70 MB, checked against "
                              "F-Droid's signature)...", spinning=True)
        self.store.status.set_text("Checking F-Droid's catalog...")
        progress = self.store.progress

        def step(done, total):
            GLib.idle_add(lambda: (progress.set_fraction(done / total) if total else progress.pulse(),
                                   progress.set_text(f"{done >> 20} of {total >> 20} MB" if total else ""))
                          and False)

        def work():
            try:
                n = self.catalog.refresh(step)
                err = None
            except (FdroidError, OSError, ValueError) as e:
                n, err = 0, str(e)
            GLib.idle_add(lambda: self.refreshed(n, err) and False)
        threading.Thread(target=work, daemon=True).start()

    def refreshed(self, n, err):
        self.loading = False
        self.store.progress.set_fraction(0)
        self.store.progress.set_text("")
        if err:
            self.store.status.set_text(f"F-Droid's catalog could not be updated: {err}")
            if not self.apps:
                self.show_message(f"F-Droid's catalog could not be downloaded: {err}\n\n"
                                  "Check the Internet connection and try again.", button=True)
            return
        self.apps = self.catalog.load()
        self.fill_categories()
        self.store.status.set_text(f"{n:,} Android apps from F-Droid.")
        self.fill()

    def fill_categories(self):
        cats = sorted({c for a in self.apps.values() for c in (a.categories or [])})
        active = self.category.get_active_id() or ""
        self.category.remove_all()
        self.category.append("", "Any category")
        for c in cats:
            self.category.append(c, c)
        self.category.set_active_id(active)

    def updates(self):
        return [p for p, code in self.installed.items() if p in self.apps and code and
                self.apps[p].version_code > code]

    def fill(self):
        if not self.apps:
            return
        mode = self.filter.get_active_id()
        cat = self.category.get_active_id() or ""
        q = self.search.get_text().strip().lower()
        if q:
            words = q.split()
            hits = []
            for a in self.apps.values():
                hay = f"{a.package} {a.name} {a.summary}".lower()
                if all(w in hay for w in words):
                    hits.append(((0 if q == a.name.lower() else 1 if a.name.lower().startswith(words[0]) else 2),
                                 a.name.lower(), a.package))
            names = [p for *_, p in sorted(hits)][:1500]
            title = f"Search results for “{q}” ({len(hits)})"
        elif mode == "installed":
            names = sorted((p for p in self.installed if p in self.apps), key=lambda p: self.apps[p].name.lower())
            title = "Installed Android apps"
        elif mode == "updates":
            names = self.updates()
            title = "Updates" if names else "All Android apps are up to date"
        elif mode == "featured" and not cat:
            names = [p for p in FEATURED_ANDROID if p in self.apps]
            # then the most recently updated apps
            recent = sorted(self.apps.values(), key=lambda a: -(a.updated or 0))
            names += [a.package for a in recent if a.package not in names][:60]
            title = "Recommended and recently updated"
        else:
            names = sorted((p for p, a in self.apps.items() if not cat or cat in (a.categories or [])),
                           key=lambda p: self.apps[p].name.lower())
            title = cat or "All Android apps"
        if cat and (q or mode in ("installed", "updates", "featured")):
            names = [p for p in names if cat in (self.apps[p].categories or [])]
        self.title.set_text(title)
        self.view.set_model(None)
        self.rows.clear()
        ups = set(self.updates())
        for p in names:
            a = self.apps[p]
            status = "Update available" if p in ups else "Installed" if p in self.installed else ""
            self.rows.append([self.icon_cache.get(p), f"<b>{GLib.markup_escape_text(a.name)}</b>\n"
                              f"<small>{GLib.markup_escape_text(a.summary or '')}</small>",
                              human(a.apk_size or 0), status, p])
        self.view.set_model(self.rows)
        self.list_stack.set_visible_child_name("list")
        if len(self.rows):
            self.view.get_selection().select_path(Gtk.TreePath(0))
        else:
            self.show_app(None)
        self.load_icons([p for p in names[:200] if p not in self.icon_cache])
        if self.test_pkg and self.test_step is None:
            GLib.idle_add(self.test_start)

    # ---- test aid ----
    def test_say(self, text):
        print("STORETEST", text, flush=True)

    def test_start(self):
        if self.test_step is not None:
            return False
        # a comma-separated list: the first one in the catalog
        pkg = next((p for p in self.test_pkg.split(",") if p in self.apps), self.test_pkg)
        self.test_pkg = pkg
        a = self.apps.get(pkg)
        self.test_say(f"catalog: {len(self.apps)} apps; {pkg}: {a.name + ' ' + a.version if a else 'NOT IN THE CATALOG'}")
        if not a:
            self.test_step = "failed"
            return False
        self.test_step = "install"
        self.search.set_text(a.name)
        self.fill_keep(pkg)
        self.show_app(pkg)
        self.test_say(f"button: {self.d_install.get_label()} (visible {self.d_install.get_visible()})")
        self.install(pkg)
        return False

    def test_after_install(self, ok):
        pkg = self.test_pkg
        if self.test_step == "install":
            self.test_say(f"installed: ok={ok}; status: {self.store.status.get_text()}")
            if not ok:
                return
            self.test_step = "open"
            GLib.timeout_add_seconds(3, lambda: self.open_app(pkg) and False)
            GLib.timeout_add_seconds(110, lambda: self.test_say("open ready for screenshot") and False)
            GLib.timeout_add_seconds(140, self.test_update)
        elif self.test_step == "update":
            self.test_say(f"updated: ok={ok}; recorded {installed().get(pkg)}; status: {self.store.status.get_text()}")
            self.test_step = "remove"
            GLib.timeout_add_seconds(5, lambda: self.test_say("update ready for screenshot") and False)
            GLib.timeout_add_seconds(25, lambda: self.remove(pkg) and False)

    def test_update(self):
        pkg = self.test_pkg
        a = self.apps[pkg]
        record_installed(pkg, a.version_code - 1)
        self.installed = installed()
        self.show_app(pkg)
        self.test_say(f"after recording an older version: button {self.d_install.get_label()} "
                      f"(visible {self.d_install.get_visible()}), updates {self.updates()}")
        self.test_step = "update"
        self.install(pkg)
        return False

    def load_icons(self, names):
        if not names:
            return

        def work():
            for p in names:
                a = self.apps.get(p)
                url = a and a.icon_url()
                path = url and self.catalog.cached_file(url)
                if path:
                    GLib.idle_add(self.set_icon, p, path)
        threading.Thread(target=work, daemon=True).start()

    def set_icon(self, pkg, path, size=32):
        try:
            pix = GdkPixbuf.Pixbuf.new_from_file_at_size(path, size, size)
        except GLib.Error:
            return False
        self.icon_cache[pkg] = pix
        for r in self.rows:
            if r[4] == pkg:
                r[0] = pix
        return False

    # ---- details ----
    def on_row(self, sel):
        model, it = sel.get_selected()
        self.show_app(model[it][4] if it else None)

    def show_app(self, pkg):
        self.current = pkg
        a = self.apps.get(pkg) if pkg else None
        for c in self.shots.get_children():
            self.shots.remove(c)
        if not a:
            self.d_name.set_text("")
            self.d_info.set_text("")
            self.d_icon.clear()
            self.d_text.get_buffer().set_text("")
            for b in (self.d_install, self.d_remove, self.d_open):
                b.hide()
            return
        self.d_name.set_text(a.name)
        info = [f"Version {a.version}", human(a.apk_size or 0)]
        if a.license:
            info.append(a.license)
        if a.author:
            info.append(f"by {a.author}")
        self.d_info.set_text("   •   ".join(info))
        inst = pkg in self.installed
        newer = pkg in self.updates()
        self.d_install.set_label("_Update" if inst and newer else "_Install")
        self.d_install.set_visible(not inst or newer)
        self.d_remove.set_visible(inst)
        self.d_open.set_visible(inst)
        for b in (self.d_install, self.d_remove, self.d_open):
            b.set_sensitive(not self.busy)
        self.d_icon.set_from_icon_name("android", Gtk.IconSize.DIALOG)
        text = (a.summary or "") + "\n\n" + strip_html(a.description or "")
        if a.anti_features:
            text += "\n\nNote (F-Droid): " + ", ".join(ANTI_FEATURES.get(x, x) for x in a.anti_features) + "."
        perms = describe_permissions(p for p in (a.permissions or []) if not p.startswith(pkg + "."))
        if perms:
            text += "\n\nIt may: " + "; ".join(t for t, _ in perms) + "."
        if a.website:
            text += f"\n\nWebsite: {a.website}"
        self.d_text.get_buffer().set_text(text)

        def work():
            url = a.icon_url()
            path = url and self.catalog.cached_file(url)
            if path:
                GLib.idle_add(lambda: self.current == pkg and self.d_icon.set_from_pixbuf(
                    GdkPixbuf.Pixbuf.new_from_file_at_size(path, 64, 64)) and False)
            for shot in a.screenshot_urls()[:5]:
                spath = self.catalog.cached_file(shot)
                if spath and self.current == pkg:
                    GLib.idle_add(self.add_shot, pkg, spath)
        threading.Thread(target=work, daemon=True).start()

    def add_shot(self, pkg, path):
        if self.current != pkg:
            return False
        try:
            pix = GdkPixbuf.Pixbuf.new_from_file_at_size(path, -1, 100)
        except GLib.Error:
            return False
        img = Gtk.Image.new_from_pixbuf(pix)
        self.shots.pack_start(img, False, False, 0)
        img.show()
        return False

    # ---- actions ----
    def set_busy(self, on, text=""):
        self.busy = on
        for b in (self.d_install, self.d_remove, self.d_open):
            b.set_sensitive(not on)
        if text:
            self.store.status.set_text(text)
        if on:
            self._pulse = GLib.timeout_add(150, lambda: self.store.progress.pulse() or True)
        elif getattr(self, "_pulse", None):
            GLib.source_remove(self._pulse)
            self._pulse = None
            self.store.progress.set_fraction(0)
            self.store.progress.set_text("")

    def first_start_note(self):
        status = android_status()
        if status == "not-set-up":
            return " Android is set up first (a one-time download of about 1 GB)."
        if status in ("stopped", "starting"):
            return " Android starts first (this takes a minute)."
        return ""

    def install(self, pkg):
        a = self.apps.get(pkg)
        if not a or self.busy:
            return
        updating = pkg in self.installed
        self.set_busy(True, f"Downloading {a.name}...")
        progress = self.store.progress

        def step(done, total):
            GLib.idle_add(lambda: (progress.set_fraction(done / total) if total else None,
                                   progress.set_text(f"{done >> 20} of {total >> 20} MB" if total else ""))
                          and False)

        def work():
            try:
                path = self.catalog.download(a, step)
            except (FdroidError, OSError) as e:
                GLib.idle_add(self.done, pkg, False, f"The download did not work: {e}", updating)
                return
            GLib.idle_add(lambda: self.store.status.set_text(
                f"Installing {a.name}...{self.first_start_note()}") and False)
            if self.test_pkg == pkg:
                self.test_say(f"downloaded: {path} ({os.path.getsize(path)} bytes); android {android_status()}")
            p = subprocess.run(["syluxium-android", "install", path], capture_output=True, text=True,
                               env=dict(os.environ, SYLUXIUM_ANDROID_QUIET="1"))
            try:
                os.unlink(path)
            except OSError:
                pass
            if p.returncode == 0:
                record_installed(pkg, a.version_code)
            GLib.idle_add(self.done, pkg, p.returncode == 0, (p.stdout + p.stderr).strip(), updating)
        threading.Thread(target=work, daemon=True).start()

    def remove(self, pkg):
        a = self.apps.get(pkg)
        name = a.name if a else pkg
        if self.busy or not self.test_pkg and message(self.win, f"Remove {name}?", "The app and its data are removed from Android.",
                                Gtk.MessageType.QUESTION, Gtk.ButtonsType.YES_NO) != Gtk.ResponseType.YES:
            return
        self.set_busy(True, f"Removing {name}...")

        def work():
            p = subprocess.run(["syluxium-android", "remove", pkg], capture_output=True, text=True,
                               env=dict(os.environ, SYLUXIUM_ANDROID_QUIET="1"))
            if p.returncode == 0:
                record_installed(pkg, None)
            GLib.idle_add(self.removed, pkg, p.returncode == 0, (p.stdout + p.stderr).strip())
        threading.Thread(target=work, daemon=True).start()

    def open_app(self, pkg):
        subprocess.Popen(["syluxium-android", "launch", pkg], start_new_session=True)
        self.store.status.set_text("Opening the app" + (" (Android is starting)..." if android_status() in ("stopped", "starting")
                                                        else "..."))

    def done(self, pkg, ok, out, updating):
        self.set_busy(False)
        a = self.apps.get(pkg)
        name = a.name if a else pkg
        if ok:
            self.installed = installed()
            self.installed.setdefault(pkg, a.version_code if a else 0)
            self.store.status.set_text(f"{name} was {'updated' if updating else 'installed'}. "
                                       "It is in the Start menu under Android Apps.")
        else:
            self.store.status.set_text(f"{name} was not installed.")
            if self.test_pkg == pkg:
                self.test_say(f"install failed: {out[-400:]!r}")
            message(self.win, f"{name} could not be installed.",
                    out[-600:] or "See ~/.cache/syluxium-android.log.", Gtk.MessageType.ERROR)
        self.fill_keep(pkg)
        if self.test_pkg == pkg:
            self.test_after_install(ok)
        return False

    def removed(self, pkg, ok, out):
        self.set_busy(False)
        if ok:
            self.installed = installed()
            self.installed.pop(pkg, None)
            self.store.status.set_text("The app was removed.")
        else:
            message(self.win, "The app could not be removed.", out[-600:], Gtk.MessageType.ERROR)
        self.fill_keep(pkg)
        if self.test_pkg == pkg:
            menu = os.path.expanduser(f"~/.local/share/applications/waydroid.{pkg}.desktop")
            self.test_say(f"removed: ok={ok}; menu entry {'STILL THERE' if os.path.exists(menu) else 'gone'}; "
                          f"recorded {installed().get(pkg)}; status: {self.store.status.get_text()}")
        return False

    def fill_keep(self, pkg):
        self.fill()
        for r in self.rows:
            if r[4] == pkg:
                self.view.get_selection().select_iter(r.iter)
                break


ANTI_FEATURES = {
    "Ads": "shows advertising", "Tracking": "tracks you", "NonFreeNet": "uses a non-free network service",
    "NonFreeAdd": "promotes non-free add-ons", "NonFreeDep": "needs non-free software",
    "NSFW": "contains content not suitable for work", "UpstreamNonFree": "its upstream source is not fully free",
    "NonFreeAssets": "contains non-free media", "KnownVuln": "has a known security problem",
    "ApplicationDebuggable": "is a debug build", "NoSourceSince": "its source code is no longer published",
    "TetheredNet": "relies on its makers' servers",
}


def strip_html(text):
    import html
    import re
    text = re.sub(r"<\s*(br|/p|/li)\s*/?>", "\n", text, flags=re.I)
    text = re.sub(r"<\s*li\s*>", "• ", text, flags=re.I)
    text = re.sub(r"<[^>]+>", "", text)
    text = html.unescape(text)
    return re.sub(r"\n{3,}", "\n\n", text).strip()


class GooglePlayDialog:
    """Optional, the user's choice: switch Android to Waydroid's official image
    with Google's apps and services (GAPPS), then register this device with
    Google (an uncertified device must be registered before Play works)."""

    def __init__(self, page):
        self.page, self.win = page, page.win
        status = subprocess.run(HELPER + ["variant"], capture_output=True, text=True).stdout.strip()
        d = Gtk.Dialog(title="Google Play", transient_for=self.win, modal=True)
        d.set_default_size(540, -1)
        box = d.get_content_area()
        box.set_spacing(10), box.set_margin_start(16), box.set_margin_end(16), box.set_margin_top(12)
        if status == "GAPPS":
            box.add(label("Google Play services are installed.", bold=True, size="large"))
            box.add(label("If the Play Store says the device is not certified, register it with Google: "
                          "Syluxium shows this device's Android ID, which you enter on Google's page.", wrap=True))
            d.add_buttons("Close", Gtk.ResponseType.CLOSE, "Register this device...", 1,
                          "Remove Google Play (back to Android without Google)", 2)
        else:
            box.add(label("Google Play is optional", bold=True, size="large"))
            box.add(label(
                "Android on Syluxium comes without Google's apps and services. If you want the Play Store "
                "and apps that need Google (some banking, games or messengers), Syluxium can switch Android "
                "to Waydroid's official system image that includes them.\n\n"
                "• It downloads that system (about 1 GB) and restarts Android. Your apps and their data stay.\n"
                "• You then sign in with a Google account inside Android; Google's terms and privacy policy apply.\n"
                "• Google only lets registered devices use the Play Store. After the switch, Syluxium shows this "
                "device's Android ID and opens Google's registration page, where you enter it. It can take a "
                "while (up to a few hours) before Google accepts the device.\n"
                "• You can switch back to Android without Google at any time.", wrap=True))
            d.add_buttons("Cancel", Gtk.ResponseType.CANCEL, "Install Google Play services", 3)
        d.show_all()
        r = d.run()
        d.destroy()
        if r == 3:
            self.switch("GAPPS")
        elif r == 2:
            self.switch("VANILLA")
        elif r == 1:
            self.register()

    def switch(self, variant):
        page = self.page
        page.set_busy(True, "Switching Android to the system " +
                      ("with Google Play services..." if variant == "GAPPS" else "without Google..."))

        def work():
            subprocess.run(["syluxium-android", "stop"], capture_output=True)
            p = subprocess.run(HELPER + ["switch", variant], capture_output=True, text=True)
            GLib.idle_add(self.switched, variant, p.returncode == 0, (p.stdout + p.stderr).strip())
        threading.Thread(target=work, daemon=True).start()

    def switched(self, variant, ok, out):
        self.page.set_busy(False)
        if not ok:
            message(self.win, "Android's system could not be switched.", out[-600:], Gtk.MessageType.ERROR)
            return False
        if variant == "GAPPS":
            self.page.store.status.set_text("Google Play services are installed. Register the device next.")
            self.register()
        else:
            self.page.store.status.set_text("Android runs without Google again.")
        return False

    def register(self):
        page = self.page
        page.set_busy(True, "Starting Android to read this device's Android ID...")

        def work():
            subprocess.run(["syluxium-android", "ensure"], capture_output=True)
            aid = ""
            for _ in range(30):  # Google services write the ID shortly after the first start
                p = subprocess.run(HELPER + ["android-id"], capture_output=True, text=True)
                aid = p.stdout.strip()
                if aid.isdigit():
                    break
                time.sleep(5)
            GLib.idle_add(self.show_id, aid)
        threading.Thread(target=work, daemon=True).start()

    def show_id(self, aid):
        self.page.set_busy(False)
        url = "https://www.google.com/android/uncertified"
        d = Gtk.Dialog(title="Register this device with Google", transient_for=self.win, modal=True)
        box = d.get_content_area()
        box.set_spacing(10), box.set_margin_start(16), box.set_margin_end(16), box.set_margin_top(12)
        if aid.isdigit():
            box.add(label("Step 1: copy this device's Android ID", bold=True))
            e = Gtk.Entry(text=aid, editable=False)
            box.add(e)
            box.add(label("Step 2: on Google's page, sign in with your Google account, paste the ID and "
                          "click Register.", bold=True, wrap=True))
            box.add(label("Step 3: wait until Google accepts the device (often a few minutes, sometimes a few "
                          "hours), then open the Play Store from the Start menu (Android Apps) and sign in. If it "
                          "still says the device is not certified, stop Android (Android Apps overview) and "
                          "start it again.", wrap=True))
            d.add_buttons("Close", Gtk.ResponseType.CLOSE, "Copy the ID and open Google's page", 1)
        else:
            box.add(label("The Android ID is not there yet.", bold=True))
            box.add(label("Google's services create it in the first minutes after Android starts with Google Play "
                          "services. Try again in a moment (Google Play... > Register this device).", wrap=True))
            d.add_buttons("Close", Gtk.ResponseType.CLOSE)
        d.show_all()
        r = d.run()
        d.destroy()
        if r == 1:
            Gtk.Clipboard.get_default(self.win.get_display()).set_text(aid, -1)
            subprocess.Popen(["xdg-open", url], start_new_session=True)
        return False
