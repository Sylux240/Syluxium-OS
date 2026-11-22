// syluxium-decor: Wayfire window frames drawn from an xfwm4 theme, so the
// Syluxium shell's windows look exactly like the Xfce desktop's (the Dusk
// theme in /usr/share/themes/Syluxium/xfwm4, made by tools/make-dusk-theme.py):
// the same corner, title, edge and button pictures, the same button layout
// ("O|HMC"), title font and alignment.
//
// Based on Wayfire 0.9's "decoration" plugin (MIT licence, Copyright (c)
// Wayfire contributors): the transaction handling that adds frames to
// windows and the frame's scene node are theirs; the drawing follows xfwm4's
// frame layout (frame.c) with the theme's pixmaps:
//   top-left | title-1 ... (tiled) ... | top-right      (title row, buttons on it)
//   left                               | right          (edges, tiled)
//   bottom-left | bottom (tiled)       | bottom-right
// Each frame part is drawn with cairo into a texture when the window's
// size, title, focus or a button state changes, and only copied otherwise.
#include <wayfire/core.hpp>
#include <wayfire/matcher.hpp>
#include <wayfire/output.hpp>
#include <wayfire/plugin.hpp>
#include <wayfire/scene-input.hpp>
#include <wayfire/scene-operations.hpp>
#include <wayfire/scene-render.hpp>
#include <wayfire/scene.hpp>
#include <wayfire/signal-definitions.hpp>
#include <wayfire/signal-provider.hpp>
#include <wayfire/toplevel-view.hpp>
#include <wayfire/toplevel.hpp>
#include <wayfire/txn/transaction-manager.hpp>
#include <wayfire/view.hpp>
#include <wayfire/window-manager.hpp>
#include <wayfire/workarea.hpp>
#include <wayfire/nonstd/wlroots.hpp>
#include <wayfire/opengl.hpp>
#include <wayfire/plugins/common/cairo-util.hpp>
#include <wayfire/util/log.hpp>

#include <linux/input-event-codes.h>
#include <cairo.h>
#include <pango/pangocairo.h>

#include <chrono>
#include <fstream>
#include <map>
#include <memory>
#include <string>

namespace
{
// ---- the xfwm4 theme: pixmaps and themerc ----
enum part_t
{
    TOP_LEFT, TOP_RIGHT, TITLE, LEFT, RIGHT, BOTTOM, BOTTOM_LEFT, BOTTOM_RIGHT, PART_COUNT
};
const char *const PART_NAMES[PART_COUNT] = {
    "top-left", "top-right", "title-1", "left", "right", "bottom", "bottom-left", "bottom-right"
};

enum button_t { BUTTON_MENU, BUTTON_HIDE, BUTTON_MAXIMIZE, BUTTON_CLOSE, BUTTON_SHADE, BUTTON_STICK, BUTTON_NONE };
enum button_state_t { STATE_NORMAL, STATE_PRELIGHT, STATE_PRESSED };

button_t button_from_letter(char c)
{
    switch (c)
    {
      case 'O': return BUTTON_MENU;
      case 'H': return BUTTON_HIDE;
      case 'M': return BUTTON_MAXIMIZE;
      case 'C': return BUTTON_CLOSE;
      case 'S': return BUTTON_SHADE;
      case 'T': return BUTTON_STICK;
      default: return BUTTON_NONE;
    }
}

const char *button_name(button_t b)
{
    switch (b)
    {
      case BUTTON_MENU: return "menu";
      case BUTTON_HIDE: return "hide";
      case BUTTON_MAXIMIZE: return "maximize";
      case BUTTON_CLOSE: return "close";
      case BUTTON_SHADE: return "shade";
      case BUTTON_STICK: return "stick";
      default: return "";
    }
}

struct xfwm_theme_t
{
    std::string dir;
    cairo_surface_t *parts[2][PART_COUNT] = {}; // [active][part]
    std::map<std::string, cairo_surface_t*> buttons; // "close-active", "maximize-toggled-prelight", ...
    std::map<std::string, std::string> rc;
    wf::color_t text_color[2] = {{0.52, 0.56, 0.72, 1}, {0.93, 0.95, 1, 1}};

    ~xfwm_theme_t()
    {
        for (auto& a : parts)
        {
            for (auto s : a)
            {
                if (s)
                {
                    cairo_surface_destroy(s);
                }
            }
        }

        for (auto& [_, s] : buttons)
        {
            if (s)
            {
                cairo_surface_destroy(s);
            }
        }
    }

    static cairo_surface_t *load(const std::string& path)
    {
        cairo_surface_t *s = cairo_image_surface_create_from_png(path.c_str());
        if (cairo_surface_status(s) != CAIRO_STATUS_SUCCESS)
        {
            cairo_surface_destroy(s);
            return nullptr;
        }

        return s;
    }

    static wf::color_t parse_color(const std::string& v, wf::color_t fallback)
    {
        if ((v.size() == 7) && (v[0] == '#'))
        {
            auto h = [&] (int i) { return std::stoi(v.substr(i, 2), nullptr, 16) / 255.0; };
            return {h(1), h(3), h(5), 1.0};
        }

        return fallback;
    }

    void load_dir(const std::string& d)
    {
        dir = d;
        for (int a = 0; a < 2; a++)
        {
            for (int p = 0; p < PART_COUNT; p++)
            {
                parts[a][p] = load(dir + "/" + PART_NAMES[p] + (a ? "-active.png" : "-inactive.png"));
            }
        }

        for (auto b : {BUTTON_MENU, BUTTON_HIDE, BUTTON_MAXIMIZE, BUTTON_CLOSE, BUTTON_SHADE, BUTTON_STICK})
        {
            for (std::string toggled : {"", "-toggled"})
            {
                for (std::string st : {"-active", "-inactive", "-prelight", "-pressed"})
                {
                    std::string key = std::string(button_name(b)) + toggled + st;
                    buttons[key] = load(dir + "/" + key + ".png");
                }
            }
        }

        std::ifstream f(dir + "/themerc");
        std::string line;
        while (std::getline(f, line))
        {
            auto eq = line.find('=');
            if ((eq == std::string::npos) || (line[0] == '#'))
            {
                continue;
            }

            rc[line.substr(0, eq)] = line.substr(eq + 1);
        }

        text_color[1] = parse_color(rc["active_text_color"], text_color[1]);
        text_color[0] = parse_color(rc["inactive_text_color"], text_color[0]);
    }

    int rc_int(const std::string& key, int fallback) const
    {
        auto it = rc.find(key);
        if ((it == rc.end()) || it->second.empty())
        {
            return fallback;
        }

        try {
            return std::stoi(it->second);
        } catch (...)
        {
            return fallback;
        }
    }

    int width(part_t p) const
    {
        return parts[1][p] ? cairo_image_surface_get_width(parts[1][p]) : 0;
    }

    int height(part_t p) const
    {
        return parts[1][p] ? cairo_image_surface_get_height(parts[1][p]) : 0;
    }

    int title_height() const { return std::max(height(TITLE), 1); }
    int left_width() const { return width(LEFT); }
    int right_width() const { return width(RIGHT); }
    int bottom_height() const { return height(BOTTOM); }

    cairo_surface_t *button_surface(button_t b, bool toggled, bool active, button_state_t st) const
    {
        const char *suffix = st == STATE_PRESSED ? "-pressed" : st == STATE_PRELIGHT ? "-prelight" :
            active ? "-active" : "-inactive";
        auto find = [&] (bool tog, const char *sfx) -> cairo_surface_t*
        {
            auto it = buttons.find(std::string(button_name(b)) + (tog ? "-toggled" : "") + sfx);
            return it == buttons.end() ? nullptr : it->second;
        };
        cairo_surface_t *s = find(toggled, suffix);
        if (!s && toggled)
        {
            s = find(false, suffix);
        }

        if (!s)
        {
            s = find(false, active ? "-active" : "-inactive");
        }

        return s;
    }
};

std::shared_ptr<xfwm_theme_t> g_theme;

// Fill a rectangle with a pixmap, repeated in both directions from its corner.
void tile(cairo_t *cr, cairo_surface_t *s, double x, double y, double w, double h)
{
    if (!s || (w <= 0) || (h <= 0))
    {
        return;
    }

    cairo_save(cr);
    cairo_rectangle(cr, x, y, w, h);
    cairo_clip(cr);
    cairo_pattern_t *p = cairo_pattern_create_for_surface(s);
    cairo_pattern_set_extend(p, CAIRO_EXTEND_REPEAT);
    cairo_matrix_t m;
    cairo_matrix_init_translate(&m, -x, -y);
    cairo_pattern_set_matrix(p, &m);
    cairo_set_source(cr, p);
    cairo_paint(cr);
    cairo_pattern_destroy(p);
    cairo_restore(cr);
}

void blit(cairo_t *cr, cairo_surface_t *s, double x, double y)
{
    if (s)
    {
        cairo_set_source_surface(cr, s, x, y);
        cairo_paint(cr);
    }
}

struct button_slot_t
{
    button_t type;
    wf::geometry_t box; // in frame coordinates (0,0 = frame's top-left)
};
} // namespace

class syluxium_frame_node_t : public wf::scene::node_t, public wf::pointer_interaction_t,
    public wf::touch_interaction_t
{
    std::weak_ptr<wf::toplevel_view_interface_t> _view;
    wf::option_wrapper_t<std::string> title_font{"syluxium-decor/title_font"};
    wf::option_wrapper_t<std::string> button_layout{"syluxium-decor/button_layout"};

    wf::signal::connection_t<wf::view_title_changed_signal> title_set = [=] (auto)
    {
        dirty = true;
        damage_all();
    };

    // The frame's pictures: title row, left and right edges, bottom row.
    wf::simple_texture_t tex_top, tex_left, tex_right, tex_bottom;
    bool dirty = true;
    struct
    {
        int width = -1, height = -1;
        bool active = false, maximized = false;
        std::string title;
        int hover = -1, pressed = -1;
    } drawn;

    std::vector<button_slot_t> slots;
    int hover_slot   = -1;
    int pressed_slot = -1;
    wf::pointf_t pointer{-1, -1};
    std::chrono::steady_clock::time_point last_title_click{};

  public:
    std::shared_ptr<xfwm_theme_t> theme = g_theme;
    wf::dimensions_t size{0, 0}; // whole frame (window + borders)
    wf::region_t cached_region;
    bool fullscreen = false;

    syluxium_frame_node_t(wayfire_toplevel_view view) : node_t(false)
    {
        _view = view->weak_from_this();
        view->connect(&title_set);
        update_fullscreen();
    }

    int top() const { return fullscreen ? 0 : theme->title_height(); }
    int left() const { return fullscreen ? 0 : theme->left_width(); }
    int right() const { return fullscreen ? 0 : theme->right_width(); }
    int bottom() const { return fullscreen ? 0 : theme->bottom_height(); }

    wf::point_t get_offset()
    {
        return {-left(), -top()};
    }

    void damage_all()
    {
        wf::scene::damage_node(shared_from_this(), get_bounding_box());
    }

    bool is_active()
    {
        auto view = _view.lock();
        return view && view->activated;
    }

    bool is_maximized()
    {
        auto view = _view.lock();
        return view && (view->pending_tiled_edges() == wf::TILED_EDGES_ALL);
    }

    // xfwm4's button placement: left-side letters from the left edge, the
    // ones after "|" from the right edge, button_offset in from the
    // borders, button_spacing apart, vertically centred on the title row.
    void layout_buttons()
    {
        slots.clear();
        std::string lay = button_layout;
        auto bar = lay.find('|');
        std::string left_side  = bar == std::string::npos ? lay : lay.substr(0, bar);
        std::string right_side = bar == std::string::npos ? "" : lay.substr(bar + 1);
        int offset  = theme->rc_int("button_offset", 0);
        int spacing = theme->rc_int("button_spacing", 0);
        int x = theme->left_width() + offset;
        for (char c : left_side)
        {
            button_t b = button_from_letter(c);
            auto *s    = b == BUTTON_NONE ? nullptr : theme->button_surface(b, false, true, STATE_NORMAL);
            if (!s)
            {
                continue;
            }

            int w = cairo_image_surface_get_width(s), h = cairo_image_surface_get_height(s);
            slots.push_back({b, {x, (top() - h + 1) / 2, w, h}});
            x += w + spacing;
        }

        int rx = size.width - theme->right_width() - offset;
        for (int i = (int)right_side.size() - 1; i >= 0; i--)
        {
            button_t b = button_from_letter(right_side[i]);
            auto *s    = b == BUTTON_NONE ? nullptr : theme->button_surface(b, false, true, STATE_NORMAL);
            if (!s)
            {
                continue;
            }

            int w = cairo_image_surface_get_width(s), h = cairo_image_surface_get_height(s);
            rx -= w;
            if (rx < x)
            {
                break;
            }

            slots.push_back({b, {rx, (top() - h + 1) / 2, w, h}});
            rx -= spacing;
        }
    }

    int title_left_end()
    {
        int end = theme->left_width() + theme->rc_int("button_offset", 0);
        for (auto& s : slots)
        {
            if (s.box.x < size.width / 2)
            {
                end = std::max(end, s.box.x + s.box.width);
            }
        }

        return end;
    }

    int title_right_start()
    {
        int start = size.width - theme->right_width() - theme->rc_int("button_offset", 0);
        for (auto& s : slots)
        {
            if (s.box.x >= size.width / 2)
            {
                start = std::min(start, s.box.x);
            }
        }

        return start;
    }

    void draw_top(bool active, const std::string& title, bool maximized)
    {
        int w = std::max(size.width, 1), h = top();
        cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
        cairo_t *cr = cairo_create(surf);
        auto& P     = theme->parts[active ? 1 : 0];
        int tlw     = theme->width(TOP_LEFT), trw = theme->width(TOP_RIGHT);
        tile(cr, P[TITLE], tlw, 0, w - tlw - trw, h);
        blit(cr, P[TOP_LEFT], 0, 0);
        blit(cr, P[TOP_RIGHT], w - trw, 0);

        // Title: after the left buttons, title-2's width and the theme's
        // horizontal offset in (xfwm4, full-width title, left aligned).
        int tx = title_left_end() + theme->width(TITLE) + theme->rc_int("title_horizontal_offset", 0);
        int tr = title_right_start();
        if (tr - tx > 4)
        {
            PangoLayout *pl = pango_cairo_create_layout(cr);
            PangoFontDescription *fd = pango_font_description_from_string(std::string(title_font).c_str());
            pango_layout_set_font_description(pl, fd);
            pango_layout_set_text(pl, title.c_str(), -1);
            pango_layout_set_width(pl, (tr - tx) * PANGO_SCALE);
            pango_layout_set_ellipsize(pl, PANGO_ELLIPSIZE_END);
            pango_layout_set_single_paragraph_mode(pl, TRUE);
            int lw, lh;
            pango_layout_get_pixel_size(pl, &lw, &lh);
            int voff = theme->rc_int(active ? "title_vertical_offset_active" : "title_vertical_offset_inactive", 0);
            auto c   = theme->text_color[active ? 1 : 0];
            cairo_set_source_rgba(cr, c.r, c.g, c.b, c.a);
            cairo_move_to(cr, tx, (h - lh) / 2 + voff);
            pango_cairo_show_layout(cr, pl);
            pango_font_description_free(fd);
            g_object_unref(pl);
        }

        for (int i = 0; i < (int)slots.size(); i++)
        {
            auto st = i == pressed_slot ? STATE_PRESSED : (i == hover_slot ? STATE_PRELIGHT : STATE_NORMAL);
            if ((pressed_slot >= 0) && (i != pressed_slot))
            {
                st = STATE_NORMAL;
            }

            bool toggled = slots[i].type == BUTTON_MAXIMIZE && maximized;
            blit(cr, theme->button_surface(slots[i].type, toggled, active, st), slots[i].box.x, slots[i].box.y);
        }

        cairo_destroy(cr);
        cairo_surface_flush(surf);
        cairo_surface_upload_to_texture(surf, tex_top);
        cairo_surface_destroy(surf);
    }

    void draw_edges(bool active)
    {
        auto& P = theme->parts[active ? 1 : 0];
        int ch  = std::max(size.height - top() - bottom(), 1);
        auto make = [&] (int w, int h, auto draw, wf::simple_texture_t& tex)
        {
            cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, std::max(w, 1), std::max(h, 1));
            cairo_t *cr = cairo_create(surf);
            draw(cr);
            cairo_destroy(cr);
            cairo_surface_flush(surf);
            cairo_surface_upload_to_texture(surf, tex);
            cairo_surface_destroy(surf);
        };
        make(left(), ch, [&] (cairo_t *cr) { tile(cr, P[LEFT], 0, 0, left(), ch); }, tex_left);
        make(right(), ch, [&] (cairo_t *cr) { tile(cr, P[RIGHT], 0, 0, right(), ch); }, tex_right);
        make(size.width, bottom(), [&] (cairo_t *cr)
        {
            int blw = theme->width(BOTTOM_LEFT), brw = theme->width(BOTTOM_RIGHT);
            tile(cr, P[BOTTOM], blw, 0, size.width - blw - brw, bottom());
            blit(cr, P[BOTTOM_LEFT], 0, 0);
            blit(cr, P[BOTTOM_RIGHT], size.width - brw, 0);
        }, tex_bottom);
    }

    // Called with the GL context bound (inside render).
    void update_textures()
    {
        auto view = _view.lock();
        if (!view || fullscreen)
        {
            return;
        }

        bool active = is_active(), maximized = is_maximized();
        std::string title = view->get_title();
        bool size_changed = drawn.width != size.width || drawn.height != size.height;
        if (dirty || size_changed || (drawn.active != active) || (drawn.title != title) ||
            (drawn.hover != hover_slot) || (drawn.pressed != pressed_slot) || (drawn.maximized != maximized))
        {
            if (size_changed || dirty)
            {
                layout_buttons();
            }

            draw_top(active, title, maximized);
            if (size_changed || dirty || (drawn.active != active))
            {
                draw_edges(active);
            }

            drawn.width  = size.width;
            drawn.height = size.height;
            drawn.active = active;
            drawn.title  = title;
            drawn.hover  = hover_slot;
            drawn.pressed   = pressed_slot;
            drawn.maximized = maximized;
            dirty = false;
        }
    }

    void render_box(const wf::render_target_t& fb, wf::point_t origin, const wlr_box& scissor)
    {
        OpenGL::render_begin(fb);
        update_textures();
        fb.logic_scissor(scissor);
        int ch = size.height - top() - bottom();
        auto draw = [&] (wf::simple_texture_t& tex, int x, int y, int w, int h)
        {
            if ((tex.tex != (GLuint)-1) && (w > 0) && (h > 0))
            {
                OpenGL::render_texture(wf::texture_t{tex.tex}, fb, {origin.x + x, origin.y + y, w, h},
                    glm::vec4(1.0f), OpenGL::TEXTURE_TRANSFORM_INVERT_Y);
            }
        };
        draw(tex_top, 0, 0, size.width, top());
        draw(tex_left, 0, top(), left(), ch);
        draw(tex_right, size.width - right(), top(), right(), ch);
        draw(tex_bottom, 0, size.height - bottom(), size.width, bottom());
        OpenGL::render_end();
    }

    // ---- input: buttons, moving by the title, resizing by the edges ----
    int slot_at(wf::pointf_t p)
    {
        for (int i = 0; i < (int)slots.size(); i++)
        {
            auto& b = slots[i].box;
            if ((p.x >= b.x) && (p.x < b.x + b.width) && (p.y >= b.y) && (p.y < b.y + b.height))
            {
                return i;
            }
        }

        return -1;
    }

    uint32_t edges_at(wf::pointf_t p)
    {
        const int corner = 16;
        uint32_t e = 0;
        bool in_left = p.x < std::max(left(), 1), in_right = p.x >= size.width - std::max(right(), 1);
        bool in_top  = p.y < 2, in_bottom = p.y >= size.height - std::max(bottom(), 1);
        if (in_left || ((in_top || in_bottom) && (p.x < corner)))
        {
            e |= WLR_EDGE_LEFT;
        }

        if (in_right || ((in_top || in_bottom) && (p.x >= size.width - corner)))
        {
            e |= WLR_EDGE_RIGHT;
        }

        if (in_top || ((in_left || in_right) && (p.y < corner)))
        {
            e |= WLR_EDGE_TOP;
        }

        if (in_bottom || ((in_left || in_right) && (p.y >= size.height - corner)))
        {
            e |= WLR_EDGE_BOTTOM;
        }

        if (!(in_left || in_right || in_top || in_bottom))
        {
            return 0;
        }

        return e;
    }

    void set_hover(int slot)
    {
        if (slot != hover_slot)
        {
            hover_slot = slot;
            damage_all();
        }
    }

    std::optional<wf::scene::input_node_t> find_node_at(const wf::pointf_t& at) override
    {
        if (auto view = _view.lock())
        {
            wf::pointf_t local = at - wf::pointf_t{get_offset()};
            if (cached_region.contains_pointf(local) && view->is_mapped())
            {
                return wf::scene::input_node_t{.node = this, .local_coords = local};
            }
        }

        return {};
    }

    pointer_interaction_t& pointer_interaction() override { return *this; }
    touch_interaction_t& touch_interaction() override { return *this; }

    void handle_pointer_enter(wf::pointf_t point) override
    {
        pointer = point - wf::pointf_t{get_offset()};
        set_hover(slot_at(pointer));
    }

    void handle_pointer_leave() override
    {
        pointer = {-1, -1};
        set_hover(-1);
    }

    void handle_pointer_motion(wf::pointf_t to, uint32_t) override
    {
        pointer = to - wf::pointf_t{get_offset()};
        set_hover(slot_at(pointer));
        if ((pressed_slot >= 0) && (slot_at(pointer) != pressed_slot))
        {
            pressed_slot = -1;
            damage_all();
        }
    }

    void press(bool down)
    {
        auto view = _view.lock();
        if (!view)
        {
            return;
        }

        int slot = slot_at(pointer);
        if (down)
        {
            if (slot >= 0)
            {
                pressed_slot = slot;
                damage_all();
                return;
            }

            if (uint32_t e = edges_at(pointer))
            {
                return wf::get_core().default_wm->resize_request(view, e);
            }

            auto now = std::chrono::steady_clock::now();
            if (now - last_title_click < std::chrono::milliseconds(400))
            {
                last_title_click = {};
                return toggle_maximize(view);
            }

            last_title_click = now;
            return wf::get_core().default_wm->move_request(view);
        }

        // Released: a button acts if the press started and ended on it.
        int pressed = pressed_slot;
        pressed_slot = -1;
        damage_all();
        if ((pressed < 0) || (pressed != slot))
        {
            return;
        }

        switch (slots[pressed].type)
        {
          case BUTTON_CLOSE:
            view->close();
            break;

          case BUTTON_MAXIMIZE:
            toggle_maximize(view);
            break;

          case BUTTON_HIDE:
            wf::get_core().default_wm->minimize_request(view, true);
            break;

          default:
            break;
        }
    }

    void toggle_maximize(wayfire_toplevel_view view)
    {
        wf::get_core().default_wm->tile_request(view,
            view->pending_tiled_edges() ? 0 : wf::TILED_EDGES_ALL);
    }

    void handle_pointer_button(const wlr_pointer_button_event& ev) override
    {
        if (ev.button == BTN_LEFT)
        {
            press(ev.state == WL_POINTER_BUTTON_STATE_PRESSED);
        }
    }

    void handle_touch_down(uint32_t, int, wf::pointf_t position) override
    {
        pointer = position - wf::pointf_t{get_offset()};
        press(true);
    }

    void handle_touch_up(uint32_t, int, wf::pointf_t) override
    {
        press(false);
        set_hover(-1);
    }

    void handle_touch_motion(uint32_t, int, wf::pointf_t position) override
    {
        pointer = position - wf::pointf_t{get_offset()};
    }

    // ---- rendering ----
    class frame_render_instance_t : public wf::scene::render_instance_t
    {
        std::shared_ptr<syluxium_frame_node_t> self;
        wf::scene::damage_callback push_damage;
        wf::signal::connection_t<wf::scene::node_damage_signal> on_damage = [=] (wf::scene::node_damage_signal *d)
        {
            push_damage(d->region);
        };

      public:
        frame_render_instance_t(syluxium_frame_node_t *self, wf::scene::damage_callback push_damage)
        {
            this->self = std::dynamic_pointer_cast<syluxium_frame_node_t>(self->shared_from_this());
            this->push_damage = push_damage;
            self->connect(&on_damage);
        }

        void schedule_instructions(std::vector<wf::scene::render_instruction_t>& instructions,
            const wf::render_target_t& target, wf::region_t& damage) override
        {
            wf::region_t ours = damage & (self->cached_region + self->get_offset());
            if (!ours.empty())
            {
                instructions.push_back(wf::scene::render_instruction_t{
                    .instance = this,
                    .target   = target,
                    .damage   = std::move(ours),
                });
            }
        }

        void render(const wf::render_target_t& target, const wf::region_t& region) override
        {
            for (const auto& box : region)
            {
                self->render_box(target, self->get_offset(), wlr_box_from_pixman_box(box));
            }
        }
    };

    void gen_render_instances(std::vector<wf::scene::render_instance_uptr>& instances,
        wf::scene::damage_callback push_damage, wf::output_t *output = nullptr) override
    {
        instances.push_back(std::make_unique<frame_render_instance_t>(this, push_damage));
    }

    wf::geometry_t get_bounding_box() override
    {
        return wf::construct_box(get_offset(), size);
    }

    // The frame's own area: the window's box minus the client area.
    void update_region()
    {
        cached_region.clear();
        if (fullscreen || (size.width <= left() + right()) || (size.height <= top() + bottom()))
        {
            return;
        }

        wf::region_t all{wf::geometry_t{0, 0, size.width, size.height}};
        all ^= wf::region_t{wf::geometry_t{left(), top(), size.width - left() - right(),
            size.height - top() - bottom()}};
        cached_region = all;
    }

    void resize(wf::dimensions_t dims)
    {
        if (auto view = _view.lock())
        {
            view->damage();
            size = dims;
            update_region();
            view->damage();
        }
    }

    void update_fullscreen()
    {
        auto view  = _view.lock();
        fullscreen = view && view->toplevel()->current().fullscreen;
        update_region();
    }
};

// The data attached to each decorated toplevel (as in Wayfire's plugin).
class syluxium_decorator_t : public wf::custom_data_t
{
    wayfire_toplevel_view view;

    wf::signal::connection_t<wf::view_activated_state_signal> on_activated = [=] (auto)
    {
        wf::scene::damage_node(deco, deco->get_bounding_box());
    };
    wf::signal::connection_t<wf::view_geometry_changed_signal> on_geometry = [=] (auto)
    {
        deco->resize(wf::dimensions(view->get_geometry()));
    };
    wf::signal::connection_t<wf::view_fullscreen_signal> on_fullscreen = [=] (auto)
    {
        deco->update_fullscreen();
        deco->resize(wf::dimensions(view->get_geometry()));
    };
    wf::signal::connection_t<wf::view_tiled_signal> on_tiled = [=] (auto)
    {
        wf::scene::damage_node(deco, deco->get_bounding_box());
    };

  public:
    std::shared_ptr<syluxium_frame_node_t> deco;

    syluxium_decorator_t(wayfire_toplevel_view v) : view(v)
    {
        deco = std::make_shared<syluxium_frame_node_t>(view);
        deco->resize(wf::dimensions(view->get_pending_geometry()));
        wf::scene::add_back(view->get_surface_root_node(), deco);
        view->connect(&on_activated);
        view->connect(&on_geometry);
        view->connect(&on_fullscreen);
        view->connect(&on_tiled);
    }

    ~syluxium_decorator_t()
    {
        wf::scene::remove_child(deco);
    }

    wf::decoration_margins_t get_margins(const wf::toplevel_state_t& state)
    {
        if (state.fullscreen)
        {
            return {0, 0, 0, 0};
        }

        auto& t = *g_theme;
        return wf::decoration_margins_t{
            .left   = t.left_width(),
            .right  = t.right_width(),
            .bottom = t.bottom_height(),
            .top    = t.title_height(),
        };
    }
};

class syluxium_decor_plugin : public wf::plugin_interface_t
{
    wf::view_matcher_t ignore_views{"syluxium-decor/ignore_views"};
    wf::option_wrapper_t<std::string> theme_dir{"syluxium-decor/theme"};

    wf::signal::connection_t<wf::txn::new_transaction_signal> on_new_tx = [=] (wf::txn::new_transaction_signal *ev)
    {
        for (const auto& obj : ev->tx->get_objects())
        {
            auto toplevel = std::dynamic_pointer_cast<wf::toplevel_t>(obj);
            if (!toplevel)
            {
                continue;
            }

            if (auto deco = toplevel->get_data<syluxium_decorator_t>())
            {
                toplevel->pending().margins = deco->get_margins(toplevel->pending());
                continue;
            }

            if (toplevel->current().mapped || !toplevel->pending().mapped)
            {
                continue;
            }

            auto view = wf::find_view_for_toplevel(toplevel);
            if (view && should_decorate(view))
            {
                add_decoration(view);
            }
        }
    };

    wf::signal::connection_t<wf::view_decoration_state_updated_signal> on_decoration_state =
        [=] (wf::view_decoration_state_updated_signal *ev) { update(ev->view); };

  public:
    void init() override
    {
        g_theme = std::make_shared<xfwm_theme_t>();
        g_theme->load_dir(theme_dir);
        if (!g_theme->parts[1][TITLE])
        {
            LOGE("syluxium-decor: no xfwm4 theme in ", std::string(theme_dir));
        }

        wf::get_core().connect(&on_decoration_state);
        wf::get_core().tx_manager->connect(&on_new_tx);
        for (auto& view : wf::get_core().get_all_views())
        {
            update(view);
        }
    }

    void fini() override
    {
        for (auto view : wf::get_core().get_all_views())
        {
            if (auto toplevel = wf::toplevel_cast(view))
            {
                remove_decoration(toplevel);
                wf::get_core().tx_manager->schedule_object(toplevel->toplevel());
            }
        }
    }

    bool should_decorate(wayfire_toplevel_view view)
    {
        return view->should_be_decorated() && !ignore_views.matches(view);
    }

    void add_decoration(wayfire_toplevel_view view)
    {
        auto toplevel = view->toplevel();
        toplevel->store_data(std::make_unique<syluxium_decorator_t>(view));
        auto deco     = toplevel->get_data<syluxium_decorator_t>();
        auto& pending = toplevel->pending();
        pending.margins = deco->get_margins(pending);
        if (!pending.fullscreen && !pending.tiled_edges)
        {
            pending.geometry = wf::expand_geometry_by_margins(pending.geometry, pending.margins);
            if (view->get_output())
            {
                pending.geometry = wf::clamp(pending.geometry, view->get_output()->workarea->get_workarea());
            }
        }
    }

    void remove_decoration(wayfire_toplevel_view view)
    {
        view->toplevel()->erase_data<syluxium_decorator_t>();
        auto& pending = view->toplevel()->pending();
        if (!pending.fullscreen && !pending.tiled_edges)
        {
            pending.geometry = wf::shrink_geometry_by_margins(pending.geometry, pending.margins);
        }

        pending.margins = {0, 0, 0, 0};
    }

    void update(wayfire_view view)
    {
        if (auto toplevel = wf::toplevel_cast(view))
        {
            if (should_decorate(toplevel))
            {
                if (!toplevel->toplevel()->get_data<syluxium_decorator_t>())
                {
                    add_decoration(toplevel);
                }
            } else
            {
                remove_decoration(toplevel);
            }

            wf::get_core().tx_manager->schedule_object(toplevel->toplevel());
        }
    }
};

DECLARE_WAYFIRE_PLUGIN(syluxium_decor_plugin);
