"""F-Droid's catalog of free Android apps, for the App Store.

The repository index (index-v2) is downloaded and checked before use:
entry.jar must carry a valid signature by F-Droid's repository key (its
certificate's SHA-256 fingerprint is pinned below), its manifest must match
entry.json, and the index file must match the SHA-256 that entry.json names.
APKs are checked against the SHA-256 in the index before they are installed.

    cat = Catalog(); cat.refresh(progress); apps = cat.load()
    path = cat.download(app, progress)    # a verified .apk
"""
import base64
import hashlib
import json
import os
import re
import subprocess
import tempfile
import time
import urllib.request
import zipfile

REPO = "https://f-droid.org/repo"
# F-Droid's repository signing certificate (SHA-256 of the DER certificate),
# as published on https://f-droid.org/docs/Release_Channels_and_Signing_Keys/
FINGERPRINT = "43238d512c1e5eb2d6569f4a3afbf5523418b82e0a3ed1552770abb9a9c9ccab"
CACHE = os.path.expanduser("~/.cache/syluxium-appstore/fdroid")
STATE = os.path.expanduser("~/.local/share/syluxium-android")
ANDROID_SDK = 33   # Android 13 (Waydroid's LineageOS 20)
ABIS = ("x86_64", "x86")
LANGS = ("en-US", "en", "en-GB")
USER_AGENT = "Syluxium-AppStore/1.0"

# F-Droid categories -> the store's groups (shown as filters).
CATEGORY_NAMES = {
    "Connectivity": "Internet", "Internet": "Internet", "Multimedia": "Music and Video",
    "Graphics": "Graphics", "Games": "Games", "Development": "Developer Tools",
    "Science & Education": "Science and Education", "Reading": "Office", "Writing": "Office",
    "Office": "Office", "Money": "Office", "Navigation": "Maps and Travel",
    "Phone & SMS": "Internet", "Security": "System Tools", "System": "System Tools",
    "Theming": "System Tools", "Time": "Office", "Sports & Health": "Health",
}


class FdroidError(Exception):
    pass


def _get(url, dest=None, progress=None, timeout=60):
    req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        total = int(r.headers.get("Content-Length") or 0)
        if dest is None:
            return r.read()
        done = 0
        tmp = dest + ".part"
        with open(tmp, "wb") as f:
            while True:
                chunk = r.read(65536)
                if not chunk:
                    break
                f.write(chunk)
                done += len(chunk)
                if progress:
                    progress(done, total)
        os.replace(tmp, dest)
        return None


def _localized(d, default=""):
    if not isinstance(d, dict):
        return d if isinstance(d, str) else default
    for lang in LANGS:
        if lang in d:
            return d[lang]
    return next(iter(d.values()), default) if d else default


def _b64sha256(data):
    return base64.b64encode(hashlib.sha256(data).digest()).decode()


def _manifest_sections(text):
    """JAR manifest/signature file -> {name or "": {key: value}} (continuation lines joined)."""
    text = text.replace("\r\n", "\n")
    lines = []
    for line in text.split("\n"):
        if line.startswith(" ") and lines:
            lines[-1] += line[1:]
        else:
            lines.append(line)
    sections, cur, name = {}, {}, ""
    for line in lines + [""]:
        if not line:
            if cur:
                sections[name] = cur
            cur, name = {}, ""
            continue
        k, _, v = line.partition(": ")
        if k == "Name":
            name = v
        cur[k] = v
    return sections


def verify_entry_jar(data):
    """entry.json's content, if entry.jar is signed by F-Droid's key; raises FdroidError otherwise."""
    with tempfile.TemporaryDirectory() as tmp:
        jar = os.path.join(tmp, "entry.jar")
        with open(jar, "wb") as f:
            f.write(data)
        try:
            z = zipfile.ZipFile(jar)
        except zipfile.BadZipFile:
            raise FdroidError("the catalog's signature file is damaged")
        names = z.namelist()
        sig = next((n for n in names if re.match(r"META-INF/[^/]+\.(RSA|EC|DSA)$", n)), None)
        sf = sig and sig.rsplit(".", 1)[0] + ".SF"
        if not sig or sf not in names or "META-INF/MANIFEST.MF" not in names or "entry.json" not in names:
            raise FdroidError("the catalog is not signed")
        sig_path, sf_path = os.path.join(tmp, "sig"), os.path.join(tmp, "sf")
        with open(sig_path, "wb") as f:
            f.write(z.read(sig))
        sf_data = z.read(sf)
        with open(sf_path, "wb") as f:
            f.write(sf_data)
        # 1. The signer's certificate is F-Droid's.
        p = subprocess.run(["openssl", "pkcs7", "-inform", "DER", "-in", sig_path, "-print_certs"],
                           capture_output=True, text=True)
        pems = re.findall(r"-----BEGIN CERTIFICATE-----(.*?)-----END CERTIFICATE-----", p.stdout, re.S)
        if p.returncode != 0 or not pems:
            raise FdroidError("the catalog's signature has no certificate")
        der = base64.b64decode("".join(pems[0].split()))
        if hashlib.sha256(der).hexdigest() != FINGERPRINT:
            raise FdroidError("the catalog is signed by an unknown key")
        # 2. The signature over the .SF file is valid (made with that certificate's key).
        p = subprocess.run(["openssl", "cms", "-verify", "-inform", "DER", "-in", sig_path, "-content", sf_path,
                            "-binary", "-noverify", "-out", os.devnull], capture_output=True, text=True)
        if p.returncode != 0:
            raise FdroidError("the catalog's signature is not valid")
        # 3. The .SF file covers this manifest, 4. the manifest covers entry.json.
        manifest = z.read("META-INF/MANIFEST.MF")
        entry = z.read("entry.json")
        sfs = _manifest_sections(sf_data.decode("utf-8", "replace"))
        whole = sfs.get("", {}).get("SHA-256-Digest-Manifest")
        if whole:
            if whole != _b64sha256(manifest):
                raise FdroidError("the catalog's manifest does not match its signature")
        else:  # per-entry digests of the manifest sections
            raise FdroidError("the catalog's signature format is not supported")
        mf = _manifest_sections(manifest.decode("utf-8", "replace"))
        if mf.get("entry.json", {}).get("SHA-256-Digest") != _b64sha256(entry):
            raise FdroidError("the catalog's contents do not match its signature")
        return json.loads(entry)


class App:
    __slots__ = ("package", "name", "summary", "description", "icon", "categories", "screenshots", "license",
                 "website", "author", "added", "updated", "version", "version_code", "apk", "apk_sha256",
                 "apk_size", "permissions", "min_sdk", "anti_features")

    def to_dict(self):
        return {k: getattr(self, k) for k in self.__slots__}

    @classmethod
    def from_dict(cls, d):
        a = cls()
        for k in cls.__slots__:
            setattr(a, k, d.get(k))
        return a

    def icon_url(self):
        return REPO + self.icon if self.icon else None

    def screenshot_urls(self):
        return [REPO + s for s in (self.screenshots or [])]


def _best_version(versions):
    """The newest version that runs here (x86 code or none, Android 13 or older)."""
    best = None
    for v in versions.values():
        m = v.get("manifest", {})
        native = m.get("nativecode") or []
        if native and not any(a in native for a in ABIS):
            continue
        if (m.get("usesSdk", {}).get("minSdkVersion") or 1) > ANDROID_SDK:
            continue
        if best is None or m.get("versionCode", 0) > best["manifest"].get("versionCode", 0):
            best = v
    return best


def reduce_index(index):
    """index-v2 -> the fields the store shows, English, one installable version per app."""
    out = {}
    for pkg, entry in index.get("packages", {}).items():
        meta = entry.get("metadata", {})
        v = _best_version(entry.get("versions", {}))
        if not v:
            continue
        m = v.get("manifest", {})
        a = App()
        a.package = pkg
        a.name = _localized(meta.get("name"), pkg)
        a.summary = _localized(meta.get("summary"))
        a.description = _localized(meta.get("description"))
        icon = _localized(meta.get("icon"), None)
        a.icon = icon.get("name") if isinstance(icon, dict) else None
        a.categories = meta.get("categories") or []
        shots = (meta.get("screenshots") or {})
        phone = _localized(shots.get("phone") or shots.get("sevenInch") or shots.get("tenInch") or {}, [])
        a.screenshots = [s.get("name") for s in phone if isinstance(s, dict) and s.get("name")][:6]
        a.license = meta.get("license", "")
        a.website = meta.get("webSite", "") or meta.get("sourceCode", "")
        a.author = meta.get("authorName", "")
        a.added, a.updated = meta.get("added", 0), meta.get("lastUpdated", 0)
        a.version, a.version_code = m.get("versionName", ""), m.get("versionCode", 0)
        f = v.get("file", {})
        a.apk, a.apk_sha256, a.apk_size = f.get("name"), f.get("sha256"), f.get("size", 0)
        a.permissions = [p.get("name") for p in (m.get("usesPermission") or []) if isinstance(p, dict)]
        a.min_sdk = m.get("usesSdk", {}).get("minSdkVersion", 0)
        a.anti_features = sorted((v.get("antiFeatures") or {}).keys() | (meta.get("antiFeatures") or {}).keys()) \
            if isinstance(meta.get("antiFeatures"), dict) else sorted((v.get("antiFeatures") or {}).keys())
        out[pkg] = a
    return out


class Catalog:
    def __init__(self, cache=CACHE):
        self.cache = cache
        os.makedirs(cache, exist_ok=True)
        self.reduced = os.path.join(cache, "apps.json")

    def age(self):
        """Seconds since the catalog was last checked, or None if it never was."""
        try:
            return time.time() - os.path.getmtime(self.reduced)
        except OSError:
            return None

    def refresh(self, progress=None):
        """Download (if changed) and verify the index; returns the number of apps."""
        entry = verify_entry_jar(_get(f"{REPO}/entry.jar"))
        idx = entry.get("index", {})
        name, sha = idx.get("name", "/index-v2.json"), idx.get("sha256")
        if not sha:
            raise FdroidError("the catalog does not name its index")
        meta_path = os.path.join(self.cache, "index.sha256")
        if os.path.exists(self.reduced) and open(meta_path).read().strip() == sha if os.path.exists(meta_path) \
                else False:
            os.utime(self.reduced)
            return len(json.load(open(self.reduced)))
        raw = os.path.join(self.cache, "index-v2.json")
        _get(REPO + name, raw, progress, timeout=120)
        h = hashlib.sha256()
        with open(raw, "rb") as f:
            for chunk in iter(lambda: f.read(1 << 20), b""):
                h.update(chunk)
        if h.hexdigest() != sha:
            os.unlink(raw)
            raise FdroidError("the downloaded catalog is damaged (checksum mismatch)")
        with open(raw, "rb") as f:
            apps = reduce_index(json.load(f))
        os.unlink(raw)  # large; the reduced copy is what the store reads
        tmp = self.reduced + ".tmp"
        with open(tmp, "w") as f:
            json.dump({k: a.to_dict() for k, a in apps.items()}, f)
        os.replace(tmp, self.reduced)
        with open(meta_path, "w") as f:
            f.write(sha + "\n")
        return len(apps)

    def load(self):
        try:
            with open(self.reduced) as f:
                return {k: App.from_dict(v) for k, v in json.load(f).items()}
        except (OSError, ValueError):
            return {}

    def cached_file(self, url, name=None):
        """A small file (icon, screenshot) from the repository, cached; path or None."""
        d = os.path.join(self.cache, "files")
        os.makedirs(d, exist_ok=True)
        path = os.path.join(d, name or hashlib.sha1(url.encode()).hexdigest())
        if not os.path.exists(path):
            try:
                _get(url, path, timeout=30)
            except OSError:
                return None
        return path

    def download(self, app, progress=None):
        """The app's .apk, downloaded and checked against the index's SHA-256."""
        d = os.path.join(self.cache, "apks")
        os.makedirs(d, exist_ok=True)
        path = os.path.join(d, os.path.basename(app.apk))
        _get(REPO + app.apk, path, progress, timeout=120)
        h = hashlib.sha256()
        with open(path, "rb") as f:
            for chunk in iter(lambda: f.read(1 << 20), b""):
                h.update(chunk)
        if h.hexdigest() != app.apk_sha256:
            os.unlink(path)
            raise FdroidError(f"the download of {app.name} is damaged (checksum mismatch)")
        return path


# ---- what is installed (without starting Android) ----
def installed():
    """{package: version code} of the Android apps the store installed, plus apps that
    have menu entries (installed some other way: version 0)."""
    out = {}
    apps = os.path.expanduser("~/.local/share/applications")
    try:
        for f in os.listdir(apps):
            if f.startswith("waydroid.") and f.endswith(".desktop"):
                out[f[len("waydroid."):-len(".desktop")]] = 0
    except OSError:
        pass
    try:
        with open(os.path.join(STATE, "installed.json")) as f:
            recorded = json.load(f)
        for pkg, code in recorded.items():
            if pkg in out or not os.path.isdir(apps):
                out[pkg] = code
    except (OSError, ValueError):
        pass
    return out


def record_installed(pkg, version_code):
    os.makedirs(STATE, exist_ok=True)
    path = os.path.join(STATE, "installed.json")
    try:
        with open(path) as f:
            data = json.load(f)
    except (OSError, ValueError):
        data = {}
    if version_code is None:
        data.pop(pkg, None)
    else:
        data[pkg] = version_code
    with open(path + ".tmp", "w") as f:
        json.dump(data, f)
    os.replace(path + ".tmp", path)


if __name__ == "__main__":
    import sys
    c = Catalog(sys.argv[1] if len(sys.argv) > 1 else CACHE)
    n = c.refresh(lambda d, t: print(f"\r{d >> 20}/{t >> 20} MiB", end="", flush=True))
    print(f"\n{n} apps that run here")
    apps = c.load()
    for k in ("org.fdroid.fdroid", "org.mozilla.fennec_fdroid", "org.videolan.vlc"):
        a = apps.get(k)
        if a:
            print(k, a.name, a.version, a.apk, a.apk_size, a.icon_url(), len(a.screenshots), a.categories)
