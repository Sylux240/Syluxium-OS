/* Syluxium boot picker: a graphical UEFI boot menu in the style of
 * OpenCore's OpenCanopy - large clickable icons on a blurred background,
 * mouse (relative and absolute pointers), keyboard and a countdown.
 *
 * It is \EFI\BOOT\BOOTX64.EFI; Limine is next to it as limine.efi.
 * Syluxium's entries come from Limine's own configuration
 * (\boot\limine\limine.conf); picking one sets Limine's one-shot entry
 * (Boot Loader Interface: LoaderEntryOneShot, LoaderConfigTimeoutOneShot)
 * and starts Limine, which boots it at once. Other systems found on any
 * EFI partition (Windows, macOS, Linux boot loaders) are started directly.
 * Esc opens Limine's own text menu; without graphics Limine starts as is. */
#include "efi.h"
#include "assets.h"

/* ---- freestanding basics ---- */
void *memset(void *d, int c, UINTN n) { u8 *p = d; while (n--) *p++ = (u8)c; return d; }
void *memcpy(void *d, const void *s, UINTN n) { u8 *p = d; const u8 *q = s; while (n--) *p++ = *q++; return d; }
int memcmp(const void *a, const void *b, UINTN n)
{
    const u8 *p = a, *q = b;
    for (; n--; p++, q++) if (*p != *q) return *p - *q;
    return 0;
}

static EFI_SYSTEM_TABLE *ST;
static EFI_BOOT_SERVICES *BS;
static EFI_RUNTIME_SERVICES *RT;
static EFI_HANDLE IMAGE;
static EFI_LOADED_IMAGE *SELF;

static const EFI_GUID GOP_GUID = {0x9042a9de, 0x23dc, 0x4a38, {0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a}};
static const EFI_GUID SPTR_GUID = {0x31878c87, 0x0b75, 0x11d5, {0x9a, 0x4f, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d}};
static const EFI_GUID APTR_GUID = {0x8d59d32b, 0xc655, 0x4ae9, {0x9b, 0x15, 0xf2, 0x59, 0x04, 0x99, 0x2a, 0x43}};
static const EFI_GUID LIMG_GUID = {0x5b1b31a1, 0x9562, 0x11d2, {0x8e, 0x3f, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}};
static const EFI_GUID SFS_GUID = {0x964e5b22, 0x6459, 0x11d2, {0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}};
static const EFI_GUID DP_GUID = {0x09576e91, 0x6d3f, 0x11d2, {0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}};
static const EFI_GUID STIN_GUID = {0x387477c1, 0x69c7, 0x11d2, {0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}};
static const EFI_GUID LOADER_GUID = {0x4a67b082, 0x0a4c, 0x41cf, {0xb6, 0xc7, 0x44, 0x0b, 0x29, 0xbb, 0x8c, 0x4f}};

static void *alloc(UINTN n)
{
    void *p = 0;
    return BS->AllocatePool(2 /* EfiLoaderData */, n, &p) == EFI_SUCCESS ? p : 0;
}

static UINTN wlen(const CHAR16 *s) { UINTN n = 0; while (s[n]) n++; return n; }
static void a2w(CHAR16 *d, const char *s, UINTN max) { UINTN i = 0; for (; s[i] && i + 1 < max; i++) d[i] = (u8)s[i]; d[i] = 0; }
static int has(const char *s, const char *w)
{
    for (; *s; s++) {
        const char *a = s, *b = w;
        while (*b && ((*a | 32) == (*b | 32))) a++, b++;
        if (!*b) return 1;
    }
    return 0;
}

/* ---- files ---- */
static EFI_FILE *volume_root(EFI_HANDLE dev)
{
    EFI_SIMPLE_FILE_SYSTEM *fs;
    EFI_FILE *root;
    if (BS->HandleProtocol(dev, &SFS_GUID, (void **)&fs) || fs->OpenVolume(fs, &root)) return 0;
    return root;
}
static int file_exists(EFI_HANDLE dev, const CHAR16 *path)
{
    EFI_FILE *root = volume_root(dev), *f;
    int ok = 0;
    if (!root) return 0;
    if (root->Open(root, &f, path, EFI_FILE_MODE_READ, 0) == EFI_SUCCESS) { ok = 1; f->Close(f); }
    root->Close(root);
    return ok;
}
static char *read_file(EFI_HANDLE dev, const CHAR16 *path, UINTN *len)
{
    EFI_FILE *root = volume_root(dev), *f;
    char *buf = 0;
    UINTN n = 64 * 1024;
    if (!root) return 0;
    if (root->Open(root, &f, path, EFI_FILE_MODE_READ, 0) == EFI_SUCCESS) {
        buf = alloc(n + 1);
        if (buf && f->Read(f, &n, buf) == EFI_SUCCESS) buf[n] = 0, *len = n;
        else buf = 0;
        f->Close(f);
    }
    root->Close(root);
    return buf;
}

/* A device path: the volume's own, then \path. */
static EFI_DEVICE_PATH *file_path(EFI_HANDLE dev, const CHAR16 *path)
{
    EFI_DEVICE_PATH *dp, *n;
    UINTN len = 0, plen = (wlen(path) + 1) * 2;
    u8 *out;
    if (BS->HandleProtocol(dev, &DP_GUID, (void **)&dp)) return 0;
    for (n = dp; !(n->Type == 0x7f && n->SubType == 0xff); n = (EFI_DEVICE_PATH *)((u8 *)n + (n->Length[0] | n->Length[1] << 8)))
        len += n->Length[0] | n->Length[1] << 8;
    out = alloc(len + 4 + plen + 4);
    if (!out) return 0;
    memcpy(out, dp, len);
    out[len] = 4, out[len + 1] = 4; /* media: file path */
    out[len + 2] = (u8)(4 + plen), out[len + 3] = (u8)((4 + plen) >> 8);
    memcpy(out + len + 4, path, plen);
    out[len + 4 + plen] = 0x7f, out[len + 5 + plen] = 0xff, out[len + 6 + plen] = 4, out[len + 7 + plen] = 0;
    return (EFI_DEVICE_PATH *)out;
}

static EFI_STATUS start(EFI_HANDLE dev, const CHAR16 *path)
{
    EFI_HANDLE img;
    EFI_DEVICE_PATH *dp = file_path(dev, path);
    EFI_STATUS s = dp ? BS->LoadImage(0, IMAGE, dp, 0, 0, &img) : EFI_ERR(14);
    return s ? s : BS->StartImage(img, 0, 0);
}

/* ---- boot entries ---- */
enum { K_SYLUXIUM, K_CONSOLE, K_SAFE, K_WINDOWS, K_LINUX, K_MACOS, K_EFI };
struct entry { char title[64]; char label[24]; int kind; EFI_HANDLE dev; CHAR16 path[96]; };
static struct entry ENTRIES[12];
static int NENTRIES, DEFAULT, TIMEOUT = 5;

static void limine_entries(void)
{
    UINTN len = 0;
    char *conf = read_file(SELF->DeviceHandle, L"\\boot\\limine\\limine.conf", &len), *p;
    if (!conf) conf = read_file(SELF->DeviceHandle, L"\\EFI\\BOOT\\limine.conf", &len);
    if (!conf) return;
    for (p = conf; *p;) {
        char *line = p;
        while (*p && *p != '\n') p++;
        if (*p) *p++ = 0;
        if (line[0] == '/' && line[1] != '/' && NENTRIES < 8) {
            struct entry *e = &ENTRIES[NENTRIES++];
            UINTN i = 0;
            for (line++; line[i] && line[i] != '\r' && i < sizeof(e->title) - 1; i++) e->title[i] = line[i];
            e->title[i] = 0;
            e->kind = has(e->title, "safe") || has(e->title, "recovery") ? K_SAFE : has(e->title, "text") || has(e->title, "console") ? K_CONSOLE : K_SYLUXIUM;
            /* Label under the icon: "Syluxium (text console)" -> "Text Console",
             * "... (safe mode: ...)" -> "Safe Mode". */
            char *lp = e->title, *q;
            UINTN n = 0;
            for (q = e->title; *q && *q != '('; q++) ;
            if (*q == '(') lp = q + 1;
            int up = 1;
            for (; *lp && *lp != ')' && *lp != ':' && n < sizeof(e->label) - 1; lp++) {
                char c = *lp;
                if (up && c >= 'a' && c <= 'z') c -= 32;
                up = c == ' ';
                e->label[n++] = c;
            }
            e->label[n] = 0;
        } else if (!__builtin_memcmp(line, "timeout:", 8)) {
            int v = 0;
            for (line += 8; *line == ' '; line++) ;
            while (*line >= '0' && *line <= '9') v = v * 10 + *line++ - '0';
            TIMEOUT = v;
        } else if (!__builtin_memcmp(line, "default_entry:", 14)) {
            int v = 0;
            for (line += 14; *line == ' '; line++) ;
            while (*line >= '0' && *line <= '9') v = v * 10 + *line++ - '0';
            DEFAULT = v > 0 ? v - 1 : 0;
        }
    }
}

/* Other systems' boot loaders on every EFI partition. */
static void other_systems(void)
{
    static const struct { const CHAR16 *path; const char *title; int kind; } KNOWN[] = {
        {L"\\EFI\\Microsoft\\Boot\\bootmgfw.efi", "Windows", K_WINDOWS},
        {L"\\System\\Library\\CoreServices\\boot.efi", "macOS", K_MACOS},
        {L"\\EFI\\ubuntu\\shimx64.efi", "Ubuntu", K_LINUX},
        {L"\\EFI\\debian\\shimx64.efi", "Debian", K_LINUX},
        {L"\\EFI\\fedora\\shimx64.efi", "Fedora", K_LINUX},
        {L"\\EFI\\opensuse\\shim.efi", "openSUSE", K_LINUX},
        {L"\\EFI\\arch\\grubx64.efi", "Arch Linux", K_LINUX},
        {L"\\EFI\\GRUB\\grubx64.efi", "Linux", K_LINUX},
        {L"\\EFI\\systemd\\systemd-bootx64.efi", "Linux", K_LINUX},
        {L"\\EFI\\refind\\refind_x64.efi", "rEFInd", K_EFI},
    };
    EFI_HANDLE *h;
    UINTN n, i, k;
    if (BS->LocateHandleBuffer(ByProtocol, &SFS_GUID, 0, &n, &h)) return;
    for (i = 0; i < n; i++) {
        if (h[i] == SELF->DeviceHandle) continue;
        for (k = 0; k < sizeof(KNOWN) / sizeof(KNOWN[0]) && NENTRIES < 12; k++) {
            if (!file_exists(h[i], KNOWN[k].path)) continue;
            struct entry *e = &ENTRIES[NENTRIES++];
            UINTN j = 0;
            for (; KNOWN[k].title[j]; j++) e->title[j] = e->label[j] = KNOWN[k].title[j];
            e->title[j] = e->label[j] = 0;
            e->kind = KNOWN[k].kind;
            e->dev = h[i];
            for (j = 0; KNOWN[k].path[j] && j < 95; j++) e->path[j] = KNOWN[k].path[j];
            e->path[j] = 0;
            break; /* one entry per partition */
        }
    }
    BS->FreePool(h);
}

/* The picker's own folder (\EFI\BOOT, or \EFI\Syluxium on an EFI partition
 * shared with another system): Limine is next to it. */
static CHAR16 SELF_PATH[128], SELF_DIR[128];
static void find_self(void)
{
    EFI_DEVICE_PATH *n = SELF->FilePath;
    UINTN len = 0, i, cut = 0;
    for (; n && !(n->Type == 0x7f && n->SubType == 0xff); n = (EFI_DEVICE_PATH *)((u8 *)n + (n->Length[0] | n->Length[1] << 8))) {
        if (n->Type != 4 || n->SubType != 4) continue;
        const CHAR16 *p = (const CHAR16 *)((u8 *)n + 4);
        UINTN plen = ((n->Length[0] | n->Length[1] << 8) - 4) / 2;
        if (len && SELF_PATH[len - 1] != '\\' && p[0] != '\\' && len < 127) SELF_PATH[len++] = '\\';
        for (i = 0; i < plen && p[i] && len < 127; i++) SELF_PATH[len++] = p[i] == '/' ? '\\' : p[i];
    }
    SELF_PATH[len] = 0;
    for (i = 0; i < len; i++) if (SELF_PATH[i] == '\\') cut = i;
    if (!len || SELF_PATH[0] != '\\') {
        static const CHAR16 def[] = {'\\', 'E', 'F', 'I', '\\', 'B', 'O', 'O', 'T', 0};
        memcpy(SELF_DIR, def, sizeof(def));
        return;
    }
    memcpy(SELF_DIR, SELF_PATH, cut * 2);
    SELF_DIR[cut] = 0;
}
static void self_file(CHAR16 *out, const char *name)
{
    UINTN n = wlen(SELF_DIR), i;
    memcpy(out, SELF_DIR, n * 2);
    out[n++] = '\\';
    for (i = 0; name[i] && n < 127; i++) out[n++] = (u8)name[i];
    out[n] = 0;
}

/* Installed next to another system (shared EFI partition), the installer
 * leaves "register-boot-entry" in our folder: add a "Syluxium" firmware boot
 * entry for this picker, first in the boot order, then remove the marker. */
static const EFI_GUID GLOBAL_GUID = {0x8be4df61, 0x93ca, 0x11d2, {0xaa, 0x0d, 0x00, 0xe0, 0x98, 0x03, 0x2b, 0x8c}};
static void register_boot_entry(void)
{
    /* Always \EFI\Syluxium (the firmware may have started the fallback copy
     * in \EFI\BOOT): the entry points at our own folder's picker. */
    static const CHAR16 marker[] = {'\\', 'E', 'F', 'I', '\\', 'S', 'y', 'l', 'u', 'x', 'i', 'u', 'm', '\\', 'r', 'e', 'g', 'i',
                                    's', 't', 'e', 'r', '-', 'b', 'o', 'o', 't', '-', 'e', 'n', 't', 'r', 'y', 0};
    static const CHAR16 target[] = {'\\', 'E', 'F', 'I', '\\', 'S', 'y', 'l', 'u', 'x', 'i', 'u', 'm', '\\', 'B', 'O', 'O', 'T',
                                    'X', '6', '4', '.', 'E', 'F', 'I', 0};
    CHAR16 name[9] = {'B', 'o', 'o', 't', 0, 0, 0, 0, 0};
    static const char hex[] = "0123456789ABCDEF";
    u16 order[128], num = 0xFFFF;
    UINTN osize = sizeof(order), i, k;
    if (!file_exists(SELF->DeviceHandle, marker) || !file_exists(SELF->DeviceHandle, target)) return;
    EFI_DEVICE_PATH *dp = file_path(SELF->DeviceHandle, target), *n;
    if (!dp) return;
    UINTN dplen = 0;
    for (n = dp;; n = (EFI_DEVICE_PATH *)((u8 *)n + (n->Length[0] | n->Length[1] << 8))) {
        dplen += n->Length[0] | n->Length[1] << 8;
        if (n->Type == 0x7f && n->SubType == 0xff) break;
    }
    static const CHAR16 desc[] = {'S', 'y', 'l', 'u', 'x', 'i', 'u', 'm', 0};
    UINTN size = 4 + 2 + sizeof(desc) + dplen;
    u8 *opt = alloc(size);
    if (!opt) return;
    *(u32 *)opt = 1; /* LOAD_OPTION_ACTIVE */
    *(u16 *)(opt + 4) = (u16)dplen;
    memcpy(opt + 6, desc, sizeof(desc));
    memcpy(opt + 6 + sizeof(desc), dp, dplen);
    if (RT->GetVariable(L"BootOrder", &GLOBAL_GUID, 0, &osize, order) != EFI_SUCCESS) osize = 0;
    /* Reuse our earlier entry, else the first free Boot#### number. */
    for (k = 0; k < 0x100 && num == 0xFFFF; k++) {
        u8 buf[512];
        UINTN bs = sizeof(buf);
        for (i = 0; i < 4; i++) name[4 + i] = hex[(k >> (12 - 4 * i)) & 15];
        EFI_STATUS st = RT->GetVariable(name, &GLOBAL_GUID, 0, &bs, buf);
        if (st == EFI_SUCCESS && bs > 6 + sizeof(desc) && !memcmp(buf + 6, desc, sizeof(desc))) num = (u16)k;
    }
    for (k = 0; k < 0x100 && num == 0xFFFF; k++) {
        u8 buf[8];
        UINTN bs = sizeof(buf);
        for (i = 0; i < 4; i++) name[4 + i] = hex[(k >> (12 - 4 * i)) & 15];
        if (RT->GetVariable(name, &GLOBAL_GUID, 0, &bs, buf) == EFI_ERR(14) /* not found */) num = (u16)k;
    }
    if (num == 0xFFFF) return;
    for (i = 0; i < 4; i++) name[4 + i] = hex[(num >> (12 - 4 * i)) & 15];
    u32 attrs = 7; /* non-volatile, boot services, runtime */
    if (RT->SetVariable(name, &GLOBAL_GUID, attrs, size, opt) != EFI_SUCCESS) return;
    u16 neworder[129];
    UINTN m = 0;
    neworder[m++] = num;
    for (i = 0; i < osize / 2 && m < 129; i++) if (order[i] != num) neworder[m++] = order[i];
    RT->SetVariable(L"BootOrder", &GLOBAL_GUID, attrs, m * 2, neworder);
    EFI_FILE *root = volume_root(SELF->DeviceHandle), *f;
    if (root && root->Open(root, &f, marker, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0) == EFI_SUCCESS) f->Delete(f);
    if (root) root->Close(root);
}

static EFI_STATUS limine(int entry)
{
    if (entry == -2) {
        /* Esc: Limine's own menu, waiting for a choice (no countdown that
         * would start the default entry while the menu is hard to see). */
        static const CHAR16 force[] = {'m', 'e', 'n', 'u', '-', 'f', 'o', 'r', 'c', 'e', 0};
        u32 attrs = EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS;
        RT->SetVariable(L"LoaderConfigTimeoutOneShot", &LOADER_GUID, attrs, sizeof(force), force);
        entry = -1;
    }
    if (entry >= 0) {
        /* "menu-hidden": boot the one-shot entry without showing the menu. */
        CHAR16 name[64], zero[] = {'m', 'e', 'n', 'u', '-', 'h', 'i', 'd', 'd', 'e', 'n', 0};
        a2w(name, ENTRIES[entry].title, 64);
        u32 attrs = EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS;
        RT->SetVariable(L"LoaderEntryOneShot", &LOADER_GUID, attrs, (wlen(name) + 1) * 2, name);
        RT->SetVariable(L"LoaderConfigTimeoutOneShot", &LOADER_GUID, attrs, sizeof(zero), zero);
    }
    CHAR16 path[128];
    self_file(path, "limine.efi");
    if (!file_exists(SELF->DeviceHandle, path)) return start(SELF->DeviceHandle, L"\\EFI\\BOOT\\limine.efi");
    return start(SELF->DeviceHandle, path);
}

/* ---- drawing ---- */
static EFI_GOP *GOP;
static u32 W, H, *BGBUF, *SCENE;

static inline u32 blend(u32 dst, u32 src, u32 a)
{
    u32 rb = ((src & 0xff00ff) * a + (dst & 0xff00ff) * (255 - a)) >> 8 & 0xff00ff;
    u32 g = ((src & 0xff00) * a + (dst & 0xff00) * (255 - a)) >> 8 & 0xff00;
    return rb | g;
}
static void draw_argb(u32 *buf, int x0, int y0, const u32 *img, int w, int h, u32 fade)
{
    for (int y = 0; y < h; y++) {
        int yy = y0 + y;
        if (yy < 0 || yy >= (int)H) continue;
        for (int x = 0; x < w; x++) {
            int xx = x0 + x;
            u32 p = img[y * w + x], a = (p >> 24) * fade / 255;
            if (xx < 0 || xx >= (int)W || !a) continue;
            buf[yy * W + xx] = blend(buf[yy * W + xx], p, a);
        }
    }
}
static void fill_round(u32 *buf, int x0, int y0, int w, int h, int r, u32 rgb, u32 a, u32 border_a)
{
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int dx = x < r ? r - x : x >= w - r ? x - (w - r - 1) : 0;
            int dy = y < r ? r - y : y >= h - r ? y - (h - r - 1) : 0;
            int d2 = dx * dx + dy * dy, xx = x0 + x, yy = y0 + y;
            if (d2 > r * r || xx < 0 || yy < 0 || xx >= (int)W || yy >= (int)H) continue;
            int edge = x == 0 || y == 0 || x == w - 1 || y == h - 1 || d2 > (r - 1) * (r - 1);
            buf[yy * W + xx] = blend(buf[yy * W + xx], rgb, edge ? border_a : a);
        }
}
static int text_width(const struct glyph *g, const char *s)
{
    int w = 0;
    for (; *s; s++) if (*s >= 32 && *s < 127) w += g[*s - 32].advance;
    return w;
}
static void draw_text(u32 *buf, int x, int y, const char *s, u32 rgb, int bold)
{
    const struct glyph *gs = bold ? FONTB_GLYPHS : FONT_GLYPHS;
    const unsigned char *alpha = bold ? FONTB_ALPHA : FONT_ALPHA;
    for (; *s; s++) {
        if (*s < 32 || *s >= 127) continue;
        const struct glyph *g = &gs[*s - 32];
        for (int r = 0; r < g->h; r++)
            for (int c = 0; c < g->w; c++) {
                int xx = x + g->x + c, yy = y + g->y + r;
                u32 a = alpha[g->offset + r * g->w + c];
                if (a && xx >= 0 && yy >= 0 && xx < (int)W && yy < (int)H) buf[yy * W + xx] = blend(buf[yy * W + xx], rgb, a);
            }
        x += g->advance;
    }
}
static void text_center(u32 *buf, int cx, int y, const char *s, u32 rgb, int bold)
{
    draw_text(buf, cx - text_width(bold ? FONTB_GLYPHS : FONT_GLYPHS, s) / 2, y, s, rgb, bold);
}

static void scale_background(void)
{
    /* Bilinear upscale of the small blurred wallpaper (cover the screen). */
    for (u32 y = 0; y < H; y++) {
        u32 fy = y * (BG_H - 1) * 256 / (H ? H : 1), y0 = fy >> 8, ty = fy & 255;
        for (u32 x = 0; x < W; x++) {
            u32 fx = x * (BG_W - 1) * 256 / (W ? W : 1), x0 = fx >> 8, tx = fx & 255;
            u32 a = BG[y0 * BG_W + x0], b = BG[y0 * BG_W + (x0 + 1 < BG_W ? x0 + 1 : x0)];
            u32 y1 = y0 + 1 < BG_H ? y0 + 1 : y0;
            u32 c = BG[y1 * BG_W + x0], d = BG[y1 * BG_W + (x0 + 1 < BG_W ? x0 + 1 : x0)];
            BGBUF[y * W + x] = blend(blend(a, b, tx), blend(c, d, tx), ty);
        }
    }
}

/* Layout */
#define ICON 128
#define SLOT 176
static int SEL, HOVER_BTN = -1, COUNTDOWN = -1;
static int icons_x0, icons_y;
static int btn_x[2], btn_y;

static const u32 *icon_of(int kind)
{
    switch (kind) {
    case K_CONSOLE: return ICON_CONSOLE;
    case K_SAFE: return ICON_SAFE;
    case K_WINDOWS: return ICON_WINDOWS;
    case K_LINUX: return ICON_LINUX;
    case K_MACOS: return ICON_MACOS;
    case K_EFI: return ICON_EFI;
    default: return ICON_SYLUXIUM;
    }
}

/* Entries in rows: as many as fit across the screen (at a low resolution
 * eight entries don't fit in one row), the rows centred as a block. */
#define ROW_H (ICON + 70)
static int PER_ROW = 1, ROWS = 1;
static void slot_pos(int i, int *x, int *y)
{
    int row = i / PER_ROW, col = i % PER_ROW;
    int in_row = row == ROWS - 1 ? NENTRIES - row * PER_ROW : PER_ROW;
    *x = (int)W / 2 - in_row * SLOT / 2 + col * SLOT;
    *y = icons_y + row * ROW_H;
}
static void layout(void)
{
    PER_ROW = ((int)W - 32) / SLOT;
    if (PER_ROW < 1) PER_ROW = 1;
    if (PER_ROW > NENTRIES) PER_ROW = NENTRIES ? NENTRIES : 1;
    ROWS = (NENTRIES + PER_ROW - 1) / PER_ROW;
    icons_x0 = (int)W / 2 - PER_ROW * SLOT / 2;
    icons_y = (int)H / 2 - ROWS * ROW_H / 2 - 10;
    if (icons_y < 130) icons_y = 130; /* below the logo and the key hint */
    btn_y = (int)H - 120;
    btn_x[0] = (int)W / 2 - 70 - BTN_RESTART_W / 2;
    btn_x[1] = (int)W / 2 + 70 - BTN_SHUTDOWN_W / 2;
}

static void compose(void)
{
    memcpy(SCENE, BGBUF, (UINTN)W * H * 4);
    draw_argb(SCENE, (int)W / 2 - 32, 40, ICON_SYLUXIUM, ICON_SYLUXIUM_W, ICON_SYLUXIUM_H, 255);
    for (int i = 0; i < NENTRIES; i++) {
        int sx, sy;
        slot_pos(i, &sx, &sy);
        int x = sx + (SLOT - ICON) / 2;
        if (i == SEL) fill_round(SCENE, x - 14, sy - 14, ICON + 28, ICON + 28, 22, 0xffffff, 40, 150);
        draw_argb(SCENE, x, sy, icon_of(ENTRIES[i].kind), ICON, ICON, i == SEL ? 255 : 190);
        text_center(SCENE, sx + SLOT / 2, sy + ICON + 34, ENTRIES[i].label,
                    i == SEL ? 0xffffff : 0xb8c0cc, 0);
    }
    if (COUNTDOWN > 0) {
        char msg[96] = "Starting ";
        UINTN n = 9, i;
        for (i = 0; ENTRIES[SEL].label[i] && n < 80; i++) msg[n++] = ENTRIES[SEL].label[i];
        const char *in = " in ";
        for (i = 0; in[i]; i++) msg[n++] = in[i];
        msg[n++] = (char)('0' + (COUNTDOWN > 9 ? 9 : COUNTDOWN));
        msg[n] = 0;
        text_center(SCENE, (int)W / 2, icons_y + (ROWS - 1) * ROW_H + ICON + 66, msg, 0xdfe6f0, 0);
    }
    for (int b = 0; b < 2; b++) {
        draw_argb(SCENE, btn_x[b], btn_y, b ? BTN_SHUTDOWN : BTN_RESTART, BTN_RESTART_W, BTN_RESTART_H, HOVER_BTN == b ? 255 : 170);
        text_center(SCENE, btn_x[b] + BTN_RESTART_W / 2, btn_y + BTN_RESTART_H + 22, b ? "Shut Down" : "Restart",
                    HOVER_BTN == b ? 0xffffff : 0xaab2be, 0);
    }
    draw_text(SCENE, 16, 26, "Esc: text menu   Arrows/Enter or click", 0x8a93a0, 0);
}

/* Cursor over the scene: only its rectangle is redrawn when it moves. */
static int CX, CY, CURSOR_ON;
static u32 CURBUF[CURSOR_W * CURSOR_H];
static void blit_scene(int x, int y, int w, int h)
{
    if (x < 0) w += x, x = 0;
    if (y < 0) h += y, y = 0;
    if (x + w > (int)W) w = (int)W - x;
    if (y + h > (int)H) h = (int)H - y;
    if (w > 0 && h > 0) GOP->Blt(GOP, SCENE, EfiBltBufferToVideo, x, y, x, y, w, h, W * 4);
}
static void draw_cursor(void)
{
    if (!CURSOR_ON) return;
    int w = CURSOR_W, h = CURSOR_H;
    if (CX + w > (int)W) w = (int)W - CX;
    if (CY + h > (int)H) h = (int)H - CY;
    if (w <= 0 || h <= 0) return;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            u32 p = CURSOR[y * CURSOR_W + x];
            CURBUF[y * w + x] = blend(SCENE[(CY + y) * W + CX + x], p, p >> 24);
        }
    GOP->Blt(GOP, CURBUF, EfiBltBufferToVideo, 0, 0, CX, CY, w, h, w * 4);
}
static void present(void)
{
    blit_scene(0, 0, W, H);
    draw_cursor();
}
static void move_cursor(int x, int y)
{
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= (int)W) x = W - 1;
    if (y >= (int)H) y = H - 1;
    if (x == CX && y == CY && CURSOR_ON) return;
    blit_scene(CX, CY, CURSOR_W, CURSOR_H);
    CX = x, CY = y, CURSOR_ON = 1;
    draw_cursor();
}

static int hit_entry(int x, int y)
{
    for (int i = 0; i < NENTRIES; i++) {
        int ix, iy;
        slot_pos(i, &ix, &iy);
        if (x >= ix && x < ix + SLOT && y >= iy - 14 && y < iy + ICON + 50) return i;
    }
    return -1;
}
static int hit_button(int x, int y)
{
    for (int b = 0; b < 2; b++)
        if (x >= btn_x[b] && x < btn_x[b] + BTN_RESTART_W && y >= btn_y && y < btn_y + BTN_RESTART_H + 30) return b;
    return -1;
}

static void boot(int i)
{
    if (ENTRIES[i].dev) start(ENTRIES[i].dev, ENTRIES[i].path);
    else limine(i);
    /* Only reached if it failed: back to the picker. */
}

/* ---- input ---- */
static EFI_SIMPLE_POINTER *SPTR[8];
static EFI_ABSOLUTE_POINTER *APTR[8];
static int NSPTR, NAPTR;
static u64 ALAST[8][2]; /* last absolute reading: only changes move the pointer */
/* Firmware often connects only the console's devices: connect every driver
 * so USB mice, touchpads and other disks' file systems show up. */
static void connect_all(void)
{
    EFI_HANDLE *h;
    UINTN n, i;
    if (BS->LocateHandleBuffer(AllHandles, 0, 0, &n, &h)) return;
    for (i = 0; i < n; i++) BS->ConnectController(h[i], 0, 0, 1);
    BS->FreePool(h);
}

/* Every keyboard the firmware drives, not only the console's: on some
 * boards (a Gigabyte H97) USB keyboards never reach ConIn once all drivers
 * are connected. */
static EFI_SIMPLE_TEXT_INPUT *KBD[8];
static int NKBD;
static void find_keyboards(void)
{
    EFI_HANDLE *h;
    UINTN n, i;
    if (BS->LocateHandleBuffer(ByProtocol, &STIN_GUID, 0, &n, &h)) return;
    for (i = 0; i < n && NKBD < 8; i++) {
        EFI_SIMPLE_TEXT_INPUT *in;
        if (BS->HandleProtocol(h[i], &STIN_GUID, (void **)&in) || in == ST->ConIn) continue;
        KBD[NKBD++] = in;
    }
    BS->FreePool(h);
}

static void find_pointers(void)
{
    EFI_HANDLE *h;
    UINTN n, i;
    if (!BS->LocateHandleBuffer(ByProtocol, &SPTR_GUID, 0, &n, &h)) {
        for (i = 0; i < n && NSPTR < 8; i++)
            if (!BS->HandleProtocol(h[i], &SPTR_GUID, (void **)&SPTR[NSPTR])) SPTR[NSPTR]->Reset(SPTR[NSPTR], 0), NSPTR++;
        BS->FreePool(h);
    }
    if (!BS->LocateHandleBuffer(ByProtocol, &APTR_GUID, 0, &n, &h)) {
        for (i = 0; i < n && NAPTR < 8; i++)
            if (!BS->HandleProtocol(h[i], &APTR_GUID, (void **)&APTR[NAPTR])) APTR[NAPTR]->Reset(APTR[NAPTR], 0), NAPTR++;
        BS->FreePool(h);
    }
}

/* Esc: the entries as a plain text list on the firmware's console, with no
 * countdown - for when the picture is wrong (a screen that cuts it off) or
 * to read the entries' full names. Arrows + Enter or a number; Esc goes
 * back to the picture. */
static void text_out(const char *a)
{
    CHAR16 w[128];
    UINTN n = 0;
    for (; *a && n < 127; a++) w[n++] = (CHAR16)(unsigned char)*a;
    w[n] = 0;
    ST->ConOut->OutputString(ST->ConOut, w);
}
static void text_menu(void)
{
    int sel = SEL;
    /* The console may cover only part of the screen (its mode is the
     * firmware's, not the picture's): black out the rest first. */
    for (UINTN p = 0; p < (UINTN)W * H; p++) SCENE[p] = 0;
    present();
    for (;;) {
        ST->ConOut->ClearScreen(ST->ConOut);
        text_out("\r\n  Syluxium boot menu\r\n\r\n");
        for (int i = 0; i < NENTRIES; i++) {
            char line[96] = "    1. ";
            line[2] = i == sel ? '>' : ' ';
            line[4] = (char)('1' + i);
            UINTN n = 7;
            for (const char *t = ENTRIES[i].title; *t && n < 92; t++) line[n++] = *t;
            line[n++] = '\r', line[n++] = '\n', line[n] = 0;
            text_out(line);
        }
        text_out("\r\n  Up/Down and Enter, or a number. Esc: back to the picture.\r\n");
        for (;;) {
            EFI_INPUT_KEY key;
            int got = 0;
            for (int k = -1; k < NKBD && !got; k++)
                got = (k < 0 ? ST->ConIn : KBD[k])->ReadKeyStroke(k < 0 ? ST->ConIn : KBD[k], &key) == EFI_SUCCESS;
            if (!got) { BS->Stall(10000); continue; }
            if (key.ScanCode == 1) { sel = (sel + NENTRIES - 1) % NENTRIES; break; }
            if (key.ScanCode == 2) { sel = (sel + 1) % NENTRIES; break; }
            if (key.ScanCode == 0x17) { SEL = sel; return; }
            if (key.UnicodeChar == '\r') { SEL = sel; boot(sel); break; }
            if (key.UnicodeChar >= '1' && key.UnicodeChar < '1' + NENTRIES) { SEL = key.UnicodeChar - '1'; boot(SEL); break; }
        }
    }
}

/* Some firmware (a Gigabyte H97 with a GeForce) leaves the screen in a
 * small mode such as 1024x768, where everything is huge. Below 1280x720,
 * switch to the largest mode that fits the monitor (its EDID; 1920x1080
 * without one), up to 2560x1600 so that drawing stays quick; the system
 * then starts in that mode too. */
static const EFI_GUID EDID_ACTIVE_GUID = {0xbd8c1056, 0x9f36, 0x44ec, {0x92, 0xa8, 0xa6, 0x33, 0x7f, 0x81, 0x79, 0x86}};
static const EFI_GUID EDID_FOUND_GUID = {0x1c0c34f6, 0xd380, 0x41fa, {0xa0, 0x49, 0x8a, 0xd0, 0x6c, 0x1a, 0x66, 0xaa}};
struct edid_proto { u32 size; u8 *edid; };

/* The monitor's own resolution: the first detailed timing of its EDID. */
static int monitor_size(u32 *w, u32 *h)
{
    struct edid_proto *e;
    if (BS->LocateProtocol(&EDID_ACTIVE_GUID, 0, (void **)&e) && BS->LocateProtocol(&EDID_FOUND_GUID, 0, (void **)&e))
        return 0;
    if (!e || e->size < 128 || !e->edid) return 0;
    const u8 *d = e->edid + 54;
    if (d[0] == 0 && d[1] == 0) return 0; /* not a timing descriptor */
    *w = d[2] | (u32)(d[4] & 0xf0) << 4;
    *h = d[5] | (u32)(d[7] & 0xf0) << 4;
    return *w >= 640 && *h >= 480;
}

static void best_mode(void)
{
    EFI_GOP_MODE_INFO *cur = GOP->Mode->Info;
    if (cur->HorizontalResolution >= 1280 && cur->VerticalResolution >= 720) return;
    /* Never above the monitor's resolution (it would show nothing); with no
     * EDID, at most 1920x1080, which nearly every monitor takes. */
    u32 maxw = 1920, maxh = 1080;
    monitor_size(&maxw, &maxh);
    if (maxw > 2560) maxw = 2560;
    if (maxh > 1600) maxh = 1600;
    u32 best = GOP->Mode->Mode, bw = cur->HorizontalResolution, bh = cur->VerticalResolution;
    for (u32 m = 0; m < GOP->Mode->MaxMode; m++) {
        EFI_GOP_MODE_INFO *info;
        UINTN size;
        if (GOP->QueryMode(GOP, m, &size, &info) != EFI_SUCCESS) continue;
        u32 w = info->HorizontalResolution, h = info->VerticalResolution;
        int ok = info->PixelFormat <= 1 && w <= maxw && h <= maxh && (u64)w * h > (u64)bw * bh;
        if (ok) best = m, bw = w, bh = h;
        BS->FreePool(info);
    }
    if (best != GOP->Mode->Mode) GOP->SetMode(GOP, best);
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st)
{
    ST = st, BS = st->BootServices, RT = st->RuntimeServices, IMAGE = image;
    BS->SetWatchdogTimer(0, 0, 0, 0);
    if (BS->HandleProtocol(image, &LIMG_GUID, (void **)&SELF)) return EFI_ERR(1);
    find_self();
    register_boot_entry();
    limine_entries();
    if (!NENTRIES) return limine(-1); /* no configuration: Limine knows what to do */
    connect_all();
    other_systems();
    if (DEFAULT >= NENTRIES) DEFAULT = 0;
    SEL = DEFAULT;

    if (BS->LocateProtocol(&GOP_GUID, 0, (void **)&GOP) || !GOP->Mode || !GOP->Mode->Info) return limine(-1);
    best_mode();
    W = GOP->Mode->Info->HorizontalResolution, H = GOP->Mode->Info->VerticalResolution;
    BGBUF = alloc((UINTN)W * H * 4), SCENE = alloc((UINTN)W * H * 4);
    if (!BGBUF || !SCENE) return limine(-1);
    st->ConOut->EnableCursor(st->ConOut, 0);
    scale_background();
    find_pointers();
    find_keyboards();
    layout();
    COUNTDOWN = TIMEOUT > 0 ? TIMEOUT : -1;
    CX = W / 2, CY = H / 2 + 160;
    CURSOR_ON = NSPTR || NAPTR; /* a pointer only if there is something to move it */
    compose();
    present();
    if (TIMEOUT == 0) boot(SEL);

    EFI_EVENT tick;
    BS->CreateEvent(EVT_TIMER, 0, 0, 0, &tick);
    BS->SetTimer(tick, TimerPeriodic, 100000); /* 10 ms */
    int ticks = 0, buttons_down = 0, primed = 0;
    for (;;) {
        UINTN idx;
        EFI_EVENT ev[1] = {tick};
        BS->WaitForEvent(1, ev, &idx);
        int dirty = 0, activity = 0, click = 0;

        EFI_INPUT_KEY key;
        for (int k = -1; k < NKBD; k++)
        while ((k < 0 ? st->ConIn : KBD[k])->ReadKeyStroke(k < 0 ? st->ConIn : KBD[k], &key) == EFI_SUCCESS) {
            activity = 1;
            if (key.ScanCode == 4 || key.ScanCode == 1) SEL = (SEL + NENTRIES - 1) % NENTRIES, dirty = 1; /* left/up */
            else if (key.ScanCode == 3 || key.ScanCode == 2) SEL = (SEL + 1) % NENTRIES, dirty = 1;     /* right/down */
            else if (key.ScanCode == 0x17) { COUNTDOWN = -1; text_menu(); compose(), present(); }  /* Esc */
            else if (key.UnicodeChar == '\r' || key.UnicodeChar == ' ') boot(SEL), dirty = 1;
            else if (key.UnicodeChar >= '1' && key.UnicodeChar < '1' + NENTRIES) SEL = key.UnicodeChar - '1', boot(SEL), dirty = 1;
        }

        int nx = CX, ny = CY, down = 0;
        for (int i = 0; i < NSPTR; i++) {
            EFI_SIMPLE_POINTER_STATE s;
            if (SPTR[i]->GetState(SPTR[i], &s) != EFI_SUCCESS) continue;
            i64 rx = SPTR[i]->Mode->ResolutionX ? (i64)SPTR[i]->Mode->ResolutionX : 1;
            i64 ry = SPTR[i]->Mode->ResolutionY ? (i64)SPTR[i]->Mode->ResolutionY : 1;
            /* Counts per millimetre -> pixels: about 4 px per mm, at least 1 per count. */
            int dx = (int)(s.RelativeMovementX * 4 / rx), dy = (int)(s.RelativeMovementY * 4 / ry);
            if (!dx && s.RelativeMovementX) dx = s.RelativeMovementX > 0 ? 1 : -1;
            if (!dy && s.RelativeMovementY) dy = s.RelativeMovementY > 0 ? 1 : -1;
            nx += dx, ny += dy;
            down |= s.LeftButton;
            if (dx || dy || s.LeftButton) activity = 1;
        }
        for (int i = 0; i < NAPTR; i++) {
            EFI_ABSOLUTE_POINTER_STATE s;
            EFI_ABSOLUTE_POINTER_MODE *m = APTR[i]->Mode;
            if (APTR[i]->GetState(APTR[i], &s) != EFI_SUCCESS || m->AbsoluteMaxX <= m->AbsoluteMinX || m->AbsoluteMaxY <= m->AbsoluteMinY) continue;
            if (s.CurrentX != ALAST[i][0] || s.CurrentY != ALAST[i][1] || !primed) {
                ALAST[i][0] = s.CurrentX, ALAST[i][1] = s.CurrentY;
                nx = (int)((s.CurrentX - m->AbsoluteMinX) * (W - 1) / (m->AbsoluteMaxX - m->AbsoluteMinX));
                ny = (int)((s.CurrentY - m->AbsoluteMinY) * (H - 1) / (m->AbsoluteMaxY - m->AbsoluteMinY));
            }
            down |= s.ActiveButtons & 1;
        }
        if (!primed) { /* where the pointers rest is not someone moving them */
            primed = 1;
            if (NAPTR) CX = nx, CY = ny;
            continue;
        }
        if (nx != CX || ny != CY || down) activity = 1;
        if (nx != CX || ny != CY) {
            move_cursor(nx, ny);
            int e = hit_entry(CX, CY), b = hit_button(CX, CY);
            if (e >= 0 && e != SEL) SEL = e, dirty = 1;
            if (b != HOVER_BTN) HOVER_BTN = b, dirty = 1;
        }
        if (down && !buttons_down) click = 1;
        buttons_down = down;
        if (click) {
            int e = hit_entry(CX, CY), b = hit_button(CX, CY);
            if (e >= 0) SEL = e, boot(e), dirty = 1;
            else if (b >= 0) RT->ResetSystem(b ? EfiResetShutdown : EfiResetCold, 0, 0, 0);
        }

        if (activity && COUNTDOWN > 0) COUNTDOWN = -1, dirty = 1; /* someone is here: wait */
        if (COUNTDOWN > 0 && ++ticks % 100 == 0) {
            if (--COUNTDOWN == 0) boot(SEL);
            dirty = 1;
        }
        if (dirty) compose(), present();
    }
}
