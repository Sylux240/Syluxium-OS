#include <lib/string.hpp>
#include <video/console.hpp>
#include <video/font.hpp>
#include <video/framebuffer.hpp>

// Framebuffer text console with a VT100 / Linux-console compatible escape
// code interpreter: enough for shells (readline), pagers and editors with
// TERM=linux. Text lives in a cell grid in RAM; the framebuffer is only
// written, never read back (slow on real hardware).

namespace console {

namespace {

constexpr u32 MAX_COLS = 480; // 4K at 8x16; larger screens use the top-left area
constexpr u32 MAX_ROWS = 135;
constexpr u32 TAB_WIDTH = 8;

struct Cell {
    u8 ch;
    u8 fg; // palette index
    u8 bg;
};

// 16-color ANSI palette (0-7 normal, 8-15 bright).
constexpr u32 palette_rgb[16] = {
    0x101418, 0xe05561, 0x8cc265, 0xd18f52, 0x4aa5f0, 0xc162de, 0x42b3c2, 0xc8ccd4,
    0x4f5666, 0xff616e, 0xa5e075, 0xf0a45d, 0x4dc4ff, 0xde73ff, 0x4cd1e0, 0xe6e6e6,
};

constexpr u8 DEFAULT_FG = 7;
constexpr u8 DEFAULT_BG = 0;

Cell g_cells[MAX_ROWS][MAX_COLS];
u32 g_palette[16];

video::Framebuffer* g_fb = nullptr;
const video::Font* g_font = &video::default_font;
u32 g_cols = 0, g_rows = 0;

// Cursor and attributes.
u32 g_cx = 0, g_cy = 0;
bool g_wrap_pending = false; // VT100 "last column" behaviour: wrap on the next printable char
u8 g_fg = DEFAULT_FG, g_bg = DEFAULT_BG;
bool g_bold = false, g_reverse = false;
u32 g_top = 0, g_bottom = 0;  // scroll region (inclusive)
u32 g_saved_x = 0, g_saved_y = 0;
bool g_cursor_visible = true;
bool g_cursor_drawn = false;

// Escape sequence parser.
enum class State { Normal, Escape, Csi, Osc, Charset };
State g_state = State::Normal;
constexpr int MAX_PARAMS = 16;
int g_params[MAX_PARAMS];
int g_param_count = 0;
bool g_private = false; // CSI ? ...

// ---- drawing ----

// KD_GRAPHICS (an X server owns the screen): keep the text in memory, draw nothing.
bool g_graphics = false;

void draw_cell_colors(u32 col, u32 row, u32 fg, u32 bg, bool underline_cursor) {
    if (g_graphics)
        return;
    const Cell& cell = g_cells[row][col];
    const u8* glyph = g_font->glyph(cell.ch);
    u32 px = col * g_font->width;
    u32 py = row * g_font->height;
    for (u32 y = 0; y < g_font->height; y++) {
        u32* line = g_fb->row(py + y) + px;
        u8 bits = glyph[y];
        bool bar = underline_cursor && y >= g_font->height - 2;
        for (u32 x = 0; x < 8; x++)
            line[x] = (bar || (bits & (0x80 >> x))) ? fg : bg;
    }
}

void draw_cell(u32 col, u32 row) {
    const Cell& c = g_cells[row][col];
    draw_cell_colors(col, row, g_palette[c.fg], g_palette[c.bg], false);
}

void hide_cursor() {
    if (g_cursor_drawn) {
        draw_cell(min(g_cx, g_cols - 1), g_cy);
        g_cursor_drawn = false;
    }
}

void show_cursor() {
    if (g_cursor_visible && !g_cursor_drawn && g_fb) {
        u32 x = min(g_cx, g_cols - 1);
        const Cell& c = g_cells[g_cy][x];
        draw_cell_colors(x, g_cy, g_palette[15], g_palette[c.bg], true);
        g_cursor_drawn = true;
    }
}

Cell blank() { return Cell{' ', g_fg, g_bg}; }

void redraw_rows(u32 first, u32 last) {
    for (u32 r = first; r <= last && r < g_rows; r++)
        for (u32 c = 0; c < g_cols; c++)
            draw_cell(c, r);
}

void clear_cells(u32 row, u32 col_from, u32 col_to) { // [from, to)
    for (u32 c = col_from; c < col_to && c < g_cols; c++) {
        g_cells[row][c] = blank();
        draw_cell(c, row);
    }
}

// Scroll rows [top, bottom] up (n > 0) or down (n < 0) by |n| lines.
void scroll_region(u32 top, u32 bottom, int n) {
    if (top > bottom || bottom >= g_rows)
        return;
    u32 height = bottom - top + 1;
    u32 count = static_cast<u32>(n > 0 ? n : -n);
    if (count > height)
        count = height;
    if (n > 0) {
        memmove(&g_cells[top][0], &g_cells[top + count][0], sizeof(g_cells[0]) * (height - count));
        for (u32 r = bottom + 1 - count; r <= bottom; r++)
            for (u32 c = 0; c < g_cols; c++)
                g_cells[r][c] = blank();
    } else {
        memmove(&g_cells[top + count][0], &g_cells[top][0], sizeof(g_cells[0]) * (height - count));
        for (u32 r = top; r < top + count; r++)
            for (u32 c = 0; c < g_cols; c++)
                g_cells[r][c] = blank();
    }
    redraw_rows(top, bottom);
}

void line_feed() {
    if (g_cy == g_bottom)
        scroll_region(g_top, g_bottom, 1);
    else if (g_cy + 1 < g_rows)
        g_cy++;
}

void reverse_index() {
    if (g_cy == g_top)
        scroll_region(g_top, g_bottom, -1);
    else if (g_cy > 0)
        g_cy--;
}

void put_printable(u8 c) {
    if (g_wrap_pending) {
        g_wrap_pending = false;
        g_cx = 0;
        line_feed();
    }
    u8 fg = g_fg, bg = g_bg;
    if (g_reverse) {
        u8 t = fg;
        fg = bg;
        bg = t;
    }
    g_cells[g_cy][g_cx] = Cell{c, fg, bg};
    draw_cell(g_cx, g_cy);
    if (g_cx + 1 < g_cols)
        g_cx++;
    else
        g_wrap_pending = true;
}

// ---- escape sequences ----

int param(int i, int def) { return (i < g_param_count && g_params[i] > 0) ? g_params[i] : def; }

void move_to(int row, int col) { // 0-based, clamped
    g_cy = static_cast<u32>(row < 0 ? 0 : (row >= static_cast<int>(g_rows) ? g_rows - 1 : row));
    g_cx = static_cast<u32>(col < 0 ? 0 : (col >= static_cast<int>(g_cols) ? g_cols - 1 : col));
    g_wrap_pending = false;
}

void apply_sgr() {
    if (g_param_count == 0)
        g_params[g_param_count++] = 0;
    for (int i = 0; i < g_param_count; i++) {
        int p = g_params[i];
        if (p == 0) {
            g_fg = DEFAULT_FG;
            g_bg = DEFAULT_BG;
            g_bold = g_reverse = false;
        } else if (p == 1) {
            g_bold = true;
            if (g_fg < 8) g_fg += 8;
        } else if (p == 22) {
            g_bold = false;
            if (g_fg >= 8) g_fg -= 8;
        } else if (p == 7) {
            g_reverse = true;
        } else if (p == 27) {
            g_reverse = false;
        } else if (p >= 30 && p <= 37) {
            g_fg = static_cast<u8>(p - 30 + (g_bold ? 8 : 0));
        } else if (p == 39) {
            g_fg = DEFAULT_FG;
        } else if (p >= 40 && p <= 47) {
            g_bg = static_cast<u8>(p - 40);
        } else if (p == 49) {
            g_bg = DEFAULT_BG;
        } else if (p >= 90 && p <= 97) {
            g_fg = static_cast<u8>(p - 90 + 8);
        } else if (p >= 100 && p <= 107) {
            g_bg = static_cast<u8>(p - 100 + 8);
        } else if ((p == 38 || p == 48) && i + 1 < g_param_count) {
            // 256-color / true-color: map 256-color indexes 0-15 directly, skip the rest.
            if (g_params[i + 1] == 5 && i + 2 < g_param_count) {
                int idx = g_params[i + 2];
                if (idx < 16) (p == 38 ? g_fg : g_bg) = static_cast<u8>(idx);
                i += 2;
            } else if (g_params[i + 1] == 2) {
                i += 4;
            }
        }
    }
}

void csi_dispatch(u8 final) {
    int rows = static_cast<int>(g_rows), cols = static_cast<int>(g_cols);
    int cy = static_cast<int>(g_cy), cx = static_cast<int>(g_cx);
    switch (final) {
    case 'A': move_to(cy - param(0, 1), cx); break;
    case 'B': case 'e': move_to(cy + param(0, 1), cx); break;
    case 'C': case 'a': move_to(cy, cx + param(0, 1)); break;
    case 'D': move_to(cy, cx - param(0, 1)); break;
    case 'E': move_to(cy + param(0, 1), 0); break;
    case 'F': move_to(cy - param(0, 1), 0); break;
    case 'G': case '`': move_to(cy, param(0, 1) - 1); break;
    case 'd': move_to(param(0, 1) - 1, cx); break;
    case 'H': case 'f': move_to(param(0, 1) - 1, param(1, 1) - 1); break;
    case 'J': {
        int mode = g_param_count ? g_params[0] : 0;
        if (mode == 0) {
            clear_cells(g_cy, g_cx, g_cols);
            for (u32 r = g_cy + 1; r < g_rows; r++) clear_cells(r, 0, g_cols);
        } else if (mode == 1) {
            for (u32 r = 0; r < g_cy; r++) clear_cells(r, 0, g_cols);
            clear_cells(g_cy, 0, g_cx + 1);
        } else {
            for (u32 r = 0; r < g_rows; r++) clear_cells(r, 0, g_cols);
        }
        break;
    }
    case 'K': {
        int mode = g_param_count ? g_params[0] : 0;
        if (mode == 0) clear_cells(g_cy, g_cx, g_cols);
        else if (mode == 1) clear_cells(g_cy, 0, g_cx + 1);
        else clear_cells(g_cy, 0, g_cols);
        break;
    }
    case 'L': if (g_cy >= g_top && g_cy <= g_bottom) scroll_region(g_cy, g_bottom, -param(0, 1)); break;
    case 'M': if (g_cy >= g_top && g_cy <= g_bottom) scroll_region(g_cy, g_bottom, param(0, 1)); break;
    case 'S': scroll_region(g_top, g_bottom, param(0, 1)); break;
    case 'T': scroll_region(g_top, g_bottom, -param(0, 1)); break;
    case 'P': { // delete characters
        u32 n = min<u32>(static_cast<u32>(param(0, 1)), g_cols - g_cx);
        for (u32 c = g_cx; c + n < g_cols; c++) g_cells[g_cy][c] = g_cells[g_cy][c + n];
        for (u32 c = g_cols - n; c < g_cols; c++) g_cells[g_cy][c] = blank();
        for (u32 c = g_cx; c < g_cols; c++) draw_cell(c, g_cy);
        break;
    }
    case '@': { // insert blank characters
        u32 n = min<u32>(static_cast<u32>(param(0, 1)), g_cols - g_cx);
        for (u32 c = g_cols - 1; c >= g_cx + n; c--) g_cells[g_cy][c] = g_cells[g_cy][c - n];
        for (u32 c = g_cx; c < g_cx + n; c++) g_cells[g_cy][c] = blank();
        for (u32 c = g_cx; c < g_cols; c++) draw_cell(c, g_cy);
        break;
    }
    case 'X': clear_cells(g_cy, g_cx, g_cx + static_cast<u32>(param(0, 1))); break;
    case 'r': { // scroll region
        int top = param(0, 1) - 1, bottom = param(1, rows) - 1;
        if (top < bottom && bottom < rows) {
            g_top = static_cast<u32>(top);
            g_bottom = static_cast<u32>(bottom);
        }
        move_to(0, 0);
        break;
    }
    case 's': g_saved_x = g_cx; g_saved_y = g_cy; break;
    case 'u': move_to(static_cast<int>(g_saved_y), static_cast<int>(g_saved_x)); break;
    case 'm': apply_sgr(); break;
    case 'h':
    case 'l':
        if (g_private && param(0, 0) == 25)
            g_cursor_visible = final == 'h';
        break;
    default: break; // unsupported: ignore
    }
    (void)cols;
}

void process(u8 c) {
    switch (g_state) {
    case State::Escape:
        g_state = State::Normal;
        switch (c) {
        case '[':
            g_state = State::Csi;
            g_param_count = 0;
            g_params[0] = 0;
            g_private = false;
            break;
        case ']': g_state = State::Osc; break;
        case '(': case ')': g_state = State::Charset; break;
        case '7': g_saved_x = g_cx; g_saved_y = g_cy; break;
        case '8': move_to(static_cast<int>(g_saved_y), static_cast<int>(g_saved_x)); break;
        case 'D': line_feed(); break;
        case 'E': g_cx = 0; line_feed(); break;
        case 'M': reverse_index(); break;
        case 'c': clear(); break;
        default: break;
        }
        return;

    case State::Charset: // ESC ( B etc.: only one charset here
        g_state = State::Normal;
        return;

    case State::Osc: // ESC ] ... BEL / ESC \ : window titles, ignored
        if (c == 7)
            g_state = State::Normal;
        else if (c == 0x1B)
            g_state = State::Escape;
        return;

    case State::Csi:
        if (c >= '0' && c <= '9') {
            if (g_param_count == 0)
                g_param_count = 1;
            int& p = g_params[g_param_count - 1];
            p = p * 10 + (c - '0');
        } else if (c == ';') {
            if (g_param_count == 0)
                g_param_count = 1;
            if (g_param_count < MAX_PARAMS)
                g_params[g_param_count++] = 0;
        } else if (c == '?' || c == '>' || c == '=') {
            g_private = true;
        } else if (c >= 0x40 && c <= 0x7E) {
            g_state = State::Normal;
            csi_dispatch(c);
        }
        return;

    case State::Normal:
        break;
    }

    switch (c) {
    case 0x1B: g_state = State::Escape; break;
    case '\n': case 0x0B: case 0x0C: g_wrap_pending = false; g_cx = 0; line_feed(); break; // ONLCR: newline returns too
    case '\r': g_cx = 0; g_wrap_pending = false; break;
    case '\t': {
        u32 next = (g_cx / TAB_WIDTH + 1) * TAB_WIDTH;
        g_cx = min(next, g_cols - 1);
        break;
    }
    case '\b': if (g_cx > 0) g_cx--; g_wrap_pending = false; break;
    case 7: break; // bell
    default:
        if (c >= 0x20)
            put_printable(c);
        break;
    }
}

} // namespace

void init(video::Framebuffer& fb) {
    g_fb = &fb;
    g_cols = min(fb.width() / g_font->width, MAX_COLS);
    g_rows = min(fb.height() / g_font->height, MAX_ROWS);
    g_top = 0;
    g_bottom = g_rows - 1;
    for (int i = 0; i < 16; i++)
        g_palette[i] = fb.color(palette_rgb[i]);
    clear();
}

// Another framebuffer takes over (a GPU driver's console framebuffer):
// keep the text, redraw it there. Rows that no longer fit scroll away.
void rebind(video::Framebuffer& fb) {
    u32 cols = min(fb.width() / g_font->width, MAX_COLS);
    u32 rows = min(fb.height() / g_font->height, MAX_ROWS);
    if (!g_fb || !cols || !rows) {
        init(fb);
        return;
    }
    if (g_cy >= rows) { // keep the bottom (cursor) part on screen
        u32 shift = g_cy - rows + 1;
        for (u32 r = 0; r + shift < g_rows && r < MAX_ROWS; r++)
            for (u32 c = 0; c < MAX_COLS; c++)
                g_cells[r][c] = g_cells[r + shift][c];
        g_cy -= shift;
    }
    g_fb = &fb;
    g_cols = cols;
    g_rows = rows;
    g_top = 0;
    g_bottom = g_rows - 1;
    if (g_cx >= g_cols)
        g_cx = g_cols - 1;
    for (int i = 0; i < 16; i++)
        g_palette[i] = fb.color(palette_rgb[i]);
    if (!g_graphics) {
        g_fb->fill_rect(0, 0, g_fb->width(), g_fb->height(), g_palette[DEFAULT_BG]);
        g_cursor_drawn = false;
        redraw_rows(0, g_rows - 1);
        show_cursor();
    }
}

bool ready() { return g_fb != nullptr; }

void set_graphics(bool graphics) {
    if (!g_fb || graphics == g_graphics)
        return;
    g_graphics = graphics;
    if (!graphics) { // back to text: repaint everything the screen owner drew over
        g_fb->fill_rect(0, 0, g_fb->width(), g_fb->height(), g_palette[DEFAULT_BG]);
        g_cursor_drawn = false;
        redraw_rows(0, g_rows - 1);
        show_cursor();
    }
}

bool graphics() { return g_graphics; }
u32 rows() { return g_rows; }
u32 cols() { return g_cols; }

void clear() {
    if (!g_fb)
        return;
    g_fg = DEFAULT_FG;
    g_bg = DEFAULT_BG;
    g_bold = g_reverse = false;
    for (u32 r = 0; r < g_rows; r++)
        for (u32 c = 0; c < g_cols; c++)
            g_cells[r][c] = blank();
    // Also clear the margin that doesn't fit a whole character cell.
    if (!g_graphics)
        g_fb->fill_rect(0, 0, g_fb->width(), g_fb->height(), g_palette[DEFAULT_BG]);
    g_cx = g_cy = 0;
    g_top = 0;
    g_bottom = g_rows - 1;
    g_wrap_pending = false;
    g_cursor_drawn = false;
}

void putc(char ch) {
    if (!g_fb)
        return;
    hide_cursor();
    process(static_cast<u8>(ch));
    show_cursor();
}

void write(const char* s, usize len) {
    if (!g_fb)
        return;
    hide_cursor();
    for (usize i = 0; i < len; i++)
        process(static_cast<u8>(s[i]));
    show_cursor();
}

} // namespace console
