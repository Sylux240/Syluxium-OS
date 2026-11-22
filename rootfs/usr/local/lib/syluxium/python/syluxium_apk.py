"""Reading Android packages (.apk) without Android's tools: the package name,
version, label, icon and permissions, from the binary AndroidManifest.xml and
the resource table (resources.arsc).

    info = read_apk("app.apk")
    info.package, info.label, info.version, info.permissions, info.icon_png (bytes or None)
"""
import struct
import zipfile

# Android framework attribute ids (android:label, ...): obfuscated packages
# may strip the attribute names, so the resource map is checked first.
ATTR_IDS = {0x01010001: "label", 0x01010002: "icon", 0x01010003: "name", 0x0101021b: "versionCode",
            0x0101021c: "versionName", 0x0101020c: "minSdkVersion", 0x01010270: "targetSdkVersion",
            0x01010588: "roundIcon"}

T_REFERENCE, T_STRING, T_INT_DEC, T_INT_HEX, T_BOOLEAN = 0x01, 0x03, 0x10, 0x11, 0x12

# What the common permissions mean, in words (the dangerous ones and a few
# that people ask about). Others are listed by name.
PERMISSION_TEXT = {
    "INTERNET": "Full network access",
    "ACCESS_NETWORK_STATE": "See network connections",
    "ACCESS_WIFI_STATE": "See Wi-Fi connections",
    "CAMERA": "Take pictures and videos",
    "RECORD_AUDIO": "Record audio",
    "ACCESS_FINE_LOCATION": "Precise location",
    "ACCESS_COARSE_LOCATION": "Approximate location",
    "ACCESS_BACKGROUND_LOCATION": "Location in the background",
    "READ_CONTACTS": "Read your contacts",
    "WRITE_CONTACTS": "Change your contacts",
    "GET_ACCOUNTS": "Find accounts on the device",
    "READ_CALENDAR": "Read calendar events",
    "WRITE_CALENDAR": "Add or change calendar events",
    "READ_EXTERNAL_STORAGE": "Read your shared storage",
    "WRITE_EXTERNAL_STORAGE": "Change or delete your shared storage",
    "MANAGE_EXTERNAL_STORAGE": "Access all files",
    "READ_MEDIA_IMAGES": "Read your pictures",
    "READ_MEDIA_VIDEO": "Read your videos",
    "READ_MEDIA_AUDIO": "Read your music and audio",
    "POST_NOTIFICATIONS": "Show notifications",
    "READ_PHONE_STATE": "Read phone status and identity",
    "CALL_PHONE": "Call phone numbers",
    "SEND_SMS": "Send text messages",
    "READ_SMS": "Read your text messages",
    "RECEIVE_SMS": "Receive text messages",
    "BLUETOOTH": "Pair with Bluetooth devices",
    "BLUETOOTH_CONNECT": "Connect to paired Bluetooth devices",
    "BLUETOOTH_SCAN": "Find nearby Bluetooth devices",
    "NEARBY_WIFI_DEVICES": "Find nearby Wi-Fi devices",
    "BODY_SENSORS": "Body sensors (heart rate)",
    "ACTIVITY_RECOGNITION": "Physical activity",
    "VIBRATE": "Control vibration",
    "WAKE_LOCK": "Keep the device awake",
    "FOREGROUND_SERVICE": "Run in the foreground",
    "RECEIVE_BOOT_COMPLETED": "Start when Android starts",
    "REQUEST_INSTALL_PACKAGES": "Install other apps",
    "SYSTEM_ALERT_WINDOW": "Draw over other apps",
    "USE_BIOMETRIC": "Use fingerprint or face unlock",
    "USE_FINGERPRINT": "Use fingerprint",
    "QUERY_ALL_PACKAGES": "See all installed apps",
}
# Shown first, marked: these reach private data or hardware.
SENSITIVE = {"CAMERA", "RECORD_AUDIO", "ACCESS_FINE_LOCATION", "ACCESS_COARSE_LOCATION",
             "ACCESS_BACKGROUND_LOCATION", "READ_CONTACTS", "WRITE_CONTACTS", "READ_CALENDAR", "WRITE_CALENDAR",
             "READ_EXTERNAL_STORAGE", "WRITE_EXTERNAL_STORAGE", "MANAGE_EXTERNAL_STORAGE", "READ_MEDIA_IMAGES",
             "READ_MEDIA_VIDEO", "READ_MEDIA_AUDIO", "READ_PHONE_STATE", "CALL_PHONE", "SEND_SMS", "READ_SMS",
             "RECEIVE_SMS", "BODY_SENSORS", "ACTIVITY_RECOGNITION", "GET_ACCOUNTS", "REQUEST_INSTALL_PACKAGES",
             "SYSTEM_ALERT_WINDOW", "QUERY_ALL_PACKAGES"}

DENSITY_ORDER = {"xxxhdpi": 640, "xxhdpi": 480, "xhdpi": 320, "hdpi": 240, "mdpi": 160, "ldpi": 120}


class ApkError(Exception):
    pass


def _string_pool(data, off):
    """The strings of a ResStringPool chunk at `off`."""
    _, header_size, _size = struct.unpack_from("<HHI", data, off)
    count, _styles, flags, strings_start, _ = struct.unpack_from("<IIIII", data, off + 8)
    utf8 = flags & 0x100
    offsets = struct.unpack_from(f"<{count}I", data, off + header_size)
    base = off + strings_start
    out = []
    for o in offsets:
        p = base + o
        try:
            if utf8:
                n = data[p]  # length in characters (skipped)
                p += 2 if n & 0x80 else 1
                n = data[p]
                if n & 0x80:
                    n = ((n & 0x7F) << 8) | data[p + 1]
                    p += 2
                else:
                    p += 1
                out.append(data[p:p + n].decode("utf-8", "replace"))
            else:
                n = struct.unpack_from("<H", data, p)[0]
                p += 2
                if n & 0x8000:
                    n = ((n & 0x7FFF) << 16) | struct.unpack_from("<H", data, p)[0]
                    p += 2
                out.append(data[p:p + 2 * n].decode("utf-16-le", "replace"))
        except (IndexError, struct.error):
            out.append("")
    return out


def parse_axml(data):
    """Android binary XML -> list of (depth, tag, {attr: (type, value)}) for each start tag.
    A string value is the text; a reference is the resource id."""
    if len(data) < 8 or struct.unpack_from("<H", data, 0)[0] != 0x0003:
        raise ApkError("not an Android binary XML file")
    strings, resmap, elements = [], [], []
    depth = 0
    off = struct.unpack_from("<H", data, 2)[0]
    while off + 8 <= len(data):
        ctype, header_size, size = struct.unpack_from("<HHI", data, off)
        if size < 8:
            break
        if ctype == 0x0001:
            strings = _string_pool(data, off)
        elif ctype == 0x0180:
            resmap = list(struct.unpack_from(f"<{(size - header_size) // 4}I", data, off + header_size))
        elif ctype == 0x0102:
            _ns, name, attr_start, attr_size, attr_count = struct.unpack_from("<IIHHH", data, off + 16)
            tag = strings[name] if name < len(strings) else ""
            attrs = {}
            p = off + 16 + attr_start
            for _ in range(attr_count):
                _ans, aname, raw, _vsize, _r, vtype, vdata = struct.unpack_from("<IIIHBBI", data, p)
                key = ATTR_IDS.get(resmap[aname]) if aname < len(resmap) else None
                if not key:
                    key = strings[aname] if aname < len(strings) else ""
                if vtype == T_STRING:
                    val = strings[vdata] if vdata < len(strings) else ""
                elif raw != 0xFFFFFFFF and raw < len(strings):
                    val, vtype = strings[raw], T_STRING
                else:
                    val = vdata
                attrs[key] = (vtype, val)
                p += max(attr_size, 20)
            elements.append((depth, tag, attrs))
            depth += 1
        elif ctype == 0x0103:
            depth -= 1
        off += size
    return elements


class ResourceTable:
    """resources.arsc: values of resource ids, per configuration."""

    def __init__(self, data):
        self.data = data
        self.values = {}  # resource id -> [(config dict, type, value)]
        if len(data) < 12 or struct.unpack_from("<H", data, 0)[0] != 0x0002:
            raise ApkError("not a resource table")
        header_size = struct.unpack_from("<H", data, 2)[0]
        off = header_size
        self.strings = []
        while off + 8 <= len(data):
            ctype, hsize, size = struct.unpack_from("<HHI", data, off)
            if size < 8:
                break
            if ctype == 0x0001:
                self.strings = _string_pool(data, off)
            elif ctype == 0x0200:
                self._package(off, hsize, size)
            off += size

    def _package(self, off, hsize, size):
        d = self.data
        pkg_id = struct.unpack_from("<I", d, off + 8)[0]
        p = off + hsize
        end = off + size
        while p + 8 <= end:
            ctype, chs, csize = struct.unpack_from("<HHI", d, p)
            if csize < 8:
                break
            if ctype == 0x0201:
                self._type(p, chs, pkg_id)
            p += csize

    def _type(self, off, hsize, pkg_id):
        d = self.data
        type_id, flags = d[off + 8], d[off + 9]
        count, entries_start = struct.unpack_from("<II", d, off + 12)
        cfg_off = off + 20
        cfg_size = struct.unpack_from("<I", d, cfg_off)[0]
        lang = d[cfg_off + 8:cfg_off + 10].rstrip(b"\0").decode("ascii", "replace") if cfg_size >= 12 else ""
        density = struct.unpack_from("<H", d, cfg_off + 14)[0] if cfg_size >= 16 else 0
        sdk = struct.unpack_from("<H", d, cfg_off + 24)[0] if cfg_size >= 28 else 0
        config = {"lang": lang, "density": density, "sdk": sdk}
        idx_off = off + hsize
        if flags & 0x01:  # sparse: (index, offset/4) pairs
            pairs = [struct.unpack_from("<HH", d, idx_off + 4 * i) for i in range(count)]
            entries = [(i, o * 4) for i, o in pairs]
        elif flags & 0x02:  # 16-bit offsets (in units of 4)
            entries = [(i, o * 4) for i, o in enumerate(struct.unpack_from(f"<{count}H", d, idx_off))
                       if o != 0xFFFF]
        else:
            entries = [(i, o) for i, o in enumerate(struct.unpack_from(f"<{count}I", d, idx_off))
                       if o != 0xFFFFFFFF]
        base = off + entries_start
        for index, eoff in entries:
            p = base + eoff
            try:
                esize, eflags = struct.unpack_from("<HH", d, p)
                if eflags & 0x0001:  # a map (style, array): not needed here
                    continue
                if eflags & 0x0008:  # compact entry (Android 14+): data inline
                    vtype, vdata = esize & 0xFF, struct.unpack_from("<I", d, p + 4)[0]
                else:
                    _vs, _r, vtype, vdata = struct.unpack_from("<HBBI", d, p + esize)
            except struct.error:
                continue
            rid = (pkg_id << 24) | (type_id << 16) | index
            val = self.strings[vdata] if vtype == T_STRING and vdata < len(self.strings) else vdata
            self.values.setdefault(rid, []).append((config, vtype, val))

    def resolve(self, rid, depth=0):
        """All (config, type, value) a resource id ends at (references followed)."""
        out = []
        for config, vtype, val in self.values.get(rid, []):
            if vtype == T_REFERENCE and depth < 5 and val:
                out.extend(self.resolve(val, depth + 1))
            else:
                out.append((config, vtype, val))
        return out

    def string(self, rid, lang="en"):
        cands = [(c, v) for c, t, v in self.resolve(rid) if t == T_STRING]
        for want in (lang, ""):
            for c, v in cands:
                if c["lang"] == want:
                    return v
        return cands[0][1] if cands else None


class ApkInfo:
    def __init__(self):
        self.package = self.label = self.version = ""
        self.version_code = 0
        self.min_sdk = self.target_sdk = 0
        self.permissions = []
        self.icon_png = None
        self.abis = []
        self.launchable = False


def _icon_candidates(res, rid):
    """Icon files for a resource id, best first: PNG/WebP by density, adaptive icons' foreground."""
    files = []
    for config, vtype, val in res.resolve(rid):
        if vtype == T_STRING and isinstance(val, str):
            files.append((config.get("density", 0) or 0, val))
    files.sort(key=lambda f: (not f[1].endswith((".png", ".webp")), -(f[0] if f[0] < 0xFFFE else 0)))
    return [f for _, f in files]


def _load_icon(z, res, rid):
    names = set(z.namelist())
    for path in _icon_candidates(res, rid) if res else []:
        if path not in names:
            continue
        if path.endswith((".png", ".webp")):
            return z.read(path)
        if path.endswith(".xml"):  # adaptive icon: its foreground (or background) drawable
            try:
                for _d, tag, attrs in parse_axml(z.read(path)):
                    if tag in ("foreground", "background") and "drawable" in attrs:
                        t, v = attrs["drawable"]
                        if t == T_REFERENCE:
                            for sub in _icon_candidates(res, v):
                                if sub in names and sub.endswith((".png", ".webp")):
                                    return z.read(sub)
            except (ApkError, struct.error, KeyError):
                pass
    # Fallback: the largest launcher-looking bitmap.
    best = None
    for n in names:
        base = n.rsplit("/", 1)[-1]
        if base.startswith("ic_launcher") and base.endswith((".png", ".webp")) and "foreground" not in base:
            dens = next((v for k, v in DENSITY_ORDER.items() if f"-{k}" in n), 0)
            if best is None or dens > best[0]:
                best = (dens, n)
    return z.read(best[1]) if best else None


def read_apk(path):
    try:
        z = zipfile.ZipFile(path)
    except (zipfile.BadZipFile, OSError) as e:
        raise ApkError(f"not an Android package: {e}")
    with z:
        try:
            manifest = parse_axml(z.read("AndroidManifest.xml"))
        except KeyError:
            raise ApkError("this file has no Android manifest")
        res = None
        if "resources.arsc" in z.namelist():
            try:
                res = ResourceTable(z.read("resources.arsc"))
            except (ApkError, struct.error):
                res = None
        info = ApkInfo()
        icon_ref = None
        for depth, tag, attrs in manifest:
            if tag == "manifest" and depth == 0:
                info.package = str(attrs.get("package", (0, ""))[1])
                vn = attrs.get("versionName")
                if vn:
                    info.version = vn[1] if vn[0] == T_STRING else (res.string(vn[1]) if res else "") or ""
                vc = attrs.get("versionCode")
                if vc and isinstance(vc[1], int):
                    info.version_code = vc[1]
            elif tag == "uses-sdk":
                for key, field in (("minSdkVersion", "min_sdk"), ("targetSdkVersion", "target_sdk")):
                    if key in attrs and isinstance(attrs[key][1], int):
                        setattr(info, field, attrs[key][1])
            elif tag in ("uses-permission", "uses-permission-sdk-23") and "name" in attrs:
                name = str(attrs["name"][1])
                if name and name not in info.permissions:
                    info.permissions.append(name)
            elif tag == "application":
                lab = attrs.get("label")
                if lab:
                    info.label = lab[1] if lab[0] == T_STRING else ((res.string(lab[1]) if res else "") or "")
                ic = attrs.get("icon") or attrs.get("roundIcon")
                if ic and ic[0] == T_REFERENCE:
                    icon_ref = ic[1]
            elif tag == "category" and attrs.get("name", (0, ""))[1] == "android.intent.category.LAUNCHER":
                info.launchable = True
        info.label = info.label or info.package
        if icon_ref:
            try:
                info.icon_png = _load_icon(z, res, icon_ref)
            except (KeyError, struct.error, zipfile.BadZipFile):
                info.icon_png = None
        info.abis = sorted({n.split("/")[1] for n in z.namelist() if n.startswith("lib/") and n.count("/") >= 2})
    return info


def describe_permissions(perms):
    """[(text, sensitive)] in words, sensitive ones first."""
    out = []
    for p in perms:
        short = p.rsplit(".", 1)[-1]
        if p.startswith("android.permission."):
            out.append((PERMISSION_TEXT.get(short, short.replace("_", " ").capitalize()), short in SENSITIVE))
        else:
            out.append((p, False))
    seen, uniq = set(), []
    for text, sens in sorted(out, key=lambda x: (not x[1], x[0])):
        if text not in seen:
            seen.add(text)
            uniq.append((text, sens))
    return uniq


if __name__ == "__main__":
    import sys
    for f in sys.argv[1:]:
        i = read_apk(f)
        print(f"{f}: {i.package} {i.version} ({i.version_code}) label={i.label!r} sdk={i.min_sdk}/{i.target_sdk} "
              f"icon={len(i.icon_png) if i.icon_png else 0} bytes abis={i.abis} launchable={i.launchable}")
        for text, sens in describe_permissions(i.permissions):
            print(("  ! " if sens else "    ") + text)
