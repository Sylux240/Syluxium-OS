/*
 * syluxium-wayland-host: Wayland programs as ordinary windows of the Xfce
 * (X11) desktop.
 *
 * A small wlroots compositor on its X11 backend, one per X session. Every
 * Wayland window (xdg_toplevel) gets its own X11 window: xfwm4 frames it in
 * the Dusk theme, the taskbar and Task Manager list it with its title, class
 * (the Wayland app id), process and icon; resizing the X window resizes the
 * Wayland window, closing it asks the program to close. Menus and tooltips
 * (xdg_popup) become override-redirect X windows at the right place on the
 * screen. The clipboard and the primary selection are bridged both ways.
 *
 * How the windows are built: the X11 backend makes one X window per wlroots
 * output and draws into it with the Present extension (MIT-SHM buffers in
 * software rendering, DRI3/dma-buf on a GPU); its keyboard and pointer
 * events come through XInput 2. That window is put inside a window of our
 * own (the "frame", what xfwm4 manages): close requests, focus and the
 * window properties then belong to us rather than to the backend, which
 * would destroy its output on WM_DELETE_WINDOW.
 *
 *   syluxium-wayland-host [socket-name]     (default syluxium-x11-<display>)
 *
 * Run by /usr/local/bin/syluxium-wayland-run, which starts it on demand.
 * Diagnostics: /run/syluxium-diag/wayland-host.txt (copied to the stick's
 * log folder).
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <math.h>

#include <xcb/xcb.h>
#include <xcb/render.h>
#include <xcb/xfixes.h>
#include <xcb/xinput.h>
#include <xcb/randr.h>
#include <xkbcommon/xkbcommon.h>
#include <drm_fourcc.h>
#include <xf86drm.h>
#include <linux/input-event-codes.h>

#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/backend/headless.h>
#include <wlr/backend/x11.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/render/allocator.h>
#include <wlr/render/gles2.h>
#include <wlr/render/pixman.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor_shape_v1.h>
#include <wlr/types/wlr_data_control_v1.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_fractional_scale_v1.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_presentation_time.h>
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_server_decoration.h>
#include <wlr/types/wlr_single_pixel_buffer_v1.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_activation_v1.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#include <wlr/xcursor.h>

/* ------------------------------------------------------------------------
 * The X11 backend's window: wlroots keeps it in a private structure whose
 * first members (wlroots 0.18, backend/x11.h) are these. Checked at run time
 * (the window must exist with the backend's initial size) before use.
 */
struct x11_output_prefix {
	struct wlr_output wlr_output;
	void *x11;
	struct wl_list link;
	xcb_window_t win;
};
struct x11_backend_prefix {
	struct wlr_backend backend;
	struct wl_event_loop *event_loop;
	bool started;
	xcb_connection_t *xcb;
};

enum view_kind { VIEW_TOPLEVEL, VIEW_POPUP };

struct view {
	struct wl_list link;            /* server.views */
	enum view_kind kind;
	struct wlr_xdg_toplevel *toplevel;
	struct wlr_xdg_popup *popup;
	struct wlr_xdg_surface *xdg;
	struct wlr_scene *scene;
	struct wlr_scene_rect *bg;
	struct wlr_scene_tree *surface_tree;

	/* the X side (while mapped) */
	struct wlr_output *output;
	struct wlr_scene_output *sout;
	xcb_window_t frame;             /* ours: what the window manager sees */
	xcb_window_t child;             /* the backend's, inside the frame */
	int w, h;                       /* frame/output size (pixels) */
	double scale;                   /* pixels per Wayland unit (HiDPI) */
	bool x_mapped;
	uint32_t size_serial;           /* configure that carries our size */
	int min_w, min_h, max_w, max_h;
	bool ssd_decided, csd;
	char *icon_for;                 /* app id the icon was looked up for */
	pid_t icon_pid; int icon_fd; struct wl_event_source *icon_src;
	char *icon_buf; size_t icon_len, icon_cap;

	struct wlr_xdg_toplevel_decoration_v1 *xdg_deco;
	struct wl_listener deco_request_mode, deco_destroy;

	struct wl_listener map, unmap, commit, destroy;
	struct wl_listener set_title, set_app_id, set_parent;
	struct wl_listener req_move, req_resize, req_max, req_full, req_min, req_menu;
	struct wl_listener reposition;
	struct wl_listener out_frame, out_request_state, out_destroy, out_present, out_commit;
	struct wl_event_source *pace;               /* frames at the monitor's rate */
	uint64_t last_frame_ns, next_due_ns;
	uint32_t n_frames, n_commits, n_presents;   /* SYLUXIUM_WAYLAND_HOST_DEBUG statistics */
};

struct kde_deco {
	struct wl_list link;
	struct wlr_server_decoration *deco;
	struct wl_listener destroy, mode;
};

struct pointer_dev {
	struct wl_list link;
	struct wlr_pointer *pointer;
	struct wl_listener motion_abs, button, axis, frame, destroy;
};

/* Clipboard bridge */
enum { SEL_CLIPBOARD, SEL_PRIMARY, SEL_N, SEL_XDND = SEL_N /* drag and drop */ };
struct x_proxy_source {        /* an X selection offered to Wayland programs */
	struct wlr_data_source base;
	int sel;
};
struct x_proxy_primary {
	struct wlr_primary_selection_source base;
	int sel;
};
struct incoming {               /* X -> Wayland transfer (one at a time) */
	struct wl_list link;
	int sel;
	xcb_atom_t target;
	int fd;                     /* -1: the TARGETS query for a new owner */
	bool incr, started;
	uint32_t owner_gen;
};
struct outgoing {               /* Wayland -> X transfer */
	struct wl_list link;
	xcb_window_t requestor;
	xcb_atom_t property, target, selection;
	xcb_timestamp_t time;
	int fd;
	struct wl_event_source *src, *timer;
	char *data; size_t len, cap, sent;
	bool incr_wait;
};
struct writer {
	int fd; char *data; size_t len, off;
	struct wl_event_source *src;
};

static struct {
	struct wl_display *display;
	struct wl_event_loop *loop;
	struct wlr_backend *backend, *headless;
	struct x11_backend_prefix *bx;
	struct wlr_renderer *renderer;
	struct wlr_allocator *allocator;
	struct wlr_output *screen_output;
	struct wlr_xdg_shell *xdg_shell;
	struct wlr_seat *seat;
	struct wlr_keyboard *keyboard;
	struct wlr_xcursor_manager *xcursor;
	struct wlr_server_decoration_manager *kde_deco_mgr;
	struct wl_list kde_decos;
	struct wl_list views;
	struct wl_list pointers;
	struct view *pending_view;      /* the view whose output is being created */
	bool test_output;               /* the start-up check's output is being created */
	struct wlr_output *test_out;
	bool test_presented;
	char gpu_desc[160];             /* the rendering path, for the diagnostics */
	double scale;                   /* the X session's scale (XSETTINGS), pixels per Wayland unit */
	char scale_desc[96];
	xcb_window_t xsettings_owner;
	int cursor_size;
	struct view *pointer_view;      /* under the pointer */
	struct view *focused;           /* has the X keyboard focus */
	const char *socket;
	char hostname[256];

	/* our own X connection */
	xcb_connection_t *x;
	xcb_screen_t *screen;
	xcb_window_t root, selwin;
	struct wl_event_source *x_src, *x_idle;
	xcb_render_pictformat_t argb32;
	uint8_t xfixes_event;
	uint8_t xinput_opcode;          /* our connection's XInput 2 (raw button presses) */
	xcb_cursor_t cursor_default, cursor_hidden, cursor_current;
	struct wlr_surface *cursor_surface;
	int cursor_hx, cursor_hy;
	struct wl_listener cursor_commit, cursor_destroy;
	struct wl_event_source *focus_check;

	/* clipboard */
	xcb_atom_t sel_atom[SEL_N + 1];
	bool sel_we_own[SEL_N + 1];
	uint32_t owner_gen[SEL_N + 1];
	struct wl_list incoming, outgoing;
	struct wl_event_source *incoming_timer;
	xcb_timestamp_t last_time;

	FILE *diag;
	int hosted;
	uint64_t frame_ns;              /* one refresh of the monitor */
} S;

static struct {
	xcb_atom_t WM_PROTOCOLS, WM_DELETE_WINDOW, WM_STATE, WM_CHANGE_STATE, WM_CLIENT_MACHINE,
		WM_TRANSIENT_FOR_, _NET_WM_NAME, _NET_WM_PID, _NET_WM_ICON, _NET_WM_STATE,
		_NET_WM_STATE_MAXIMIZED_VERT, _NET_WM_STATE_MAXIMIZED_HORZ, _NET_WM_STATE_FULLSCREEN,
		_NET_WM_STATE_HIDDEN, _NET_WM_MOVERESIZE, _NET_WM_WINDOW_TYPE, _NET_WM_WINDOW_TYPE_NORMAL,
		_NET_WM_WINDOW_TYPE_DIALOG, _NET_WM_WINDOW_TYPE_POPUP_MENU, _NET_WM_WINDOW_TYPE_TOOLTIP,
		_NET_ACTIVE_WINDOW, _NET_WORKAREA, _MOTIF_WM_HINTS, _GTK_SHOW_WINDOW_MENU, UTF8_STRING,
		CLIPBOARD, TARGETS, TIMESTAMP, MULTIPLE, TEXT, INCR, MIME_UTF8, MIME_TEXT,
		SYLUXIUM_SELECTION, _SYLUXIUM_WAYLAND, RESOURCE_MANAGER;
} A;

/* ------------------------------------------------------------------------ */

static void diag(const char *fmt, ...) {
	va_list ap;
	char line[1024];
	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	struct timespec ts;
	clock_gettime(CLOCK_BOOTTIME, &ts);
	fprintf(stderr, "[%ld.%02ld] %s\n", (long)ts.tv_sec, ts.tv_nsec / 10000000, line);
	if (S.diag) {
		fprintf(S.diag, "%ld.%02ld %s\n", (long)ts.tv_sec, ts.tv_nsec / 10000000, line);
		fflush(S.diag);
	}
}

static uint32_t now_msec(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/* (drag and drop, further down) */
static xcb_atom_t XdndAware;
static struct wlr_data_source *dnd_wayland_source(void);
static bool w_drag_motion(uint32_t time);
static bool w_drag_button_release(uint32_t time);
static void scale_changed(void);
static void read_scale(void);

static xcb_atom_t intern(const char *name) {
	xcb_intern_atom_reply_t *r = xcb_intern_atom_reply(S.x, xcb_intern_atom(S.x, 0, strlen(name), name), NULL);
	xcb_atom_t a = r ? r->atom : XCB_ATOM_NONE;
	free(r);
	return a;
}

static char *atom_name(xcb_atom_t a) {
	xcb_get_atom_name_reply_t *r = xcb_get_atom_name_reply(S.x, xcb_get_atom_name(S.x, a), NULL);
	if (!r)
		return NULL;
	char *s = strndup(xcb_get_atom_name_name(r), xcb_get_atom_name_name_length(r));
	free(r);
	return s;
}

static void x_after_blocking(void);

static void set_prop(xcb_window_t w, xcb_atom_t prop, xcb_atom_t type, int format, uint32_t n, const void *data) {
	xcb_change_property(S.x, XCB_PROP_MODE_REPLACE, w, prop, type, format, n, data);
}
static void set_text(xcb_window_t w, xcb_atom_t prop, xcb_atom_t type, const char *s) {
	set_prop(w, prop, type, 8, strlen(s), s);
}

static void send_root_message(xcb_connection_t *c, xcb_window_t w, xcb_atom_t type, uint32_t d0, uint32_t d1,
		uint32_t d2, uint32_t d3, uint32_t d4) {
	xcb_client_message_event_t ev = {0};
	ev.response_type = XCB_CLIENT_MESSAGE;
	ev.format = 32;
	ev.window = w;
	ev.type = type;
	ev.data.data32[0] = d0; ev.data.data32[1] = d1; ev.data.data32[2] = d2;
	ev.data.data32[3] = d3; ev.data.data32[4] = d4;
	xcb_send_event(c, 0, S.root, XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY,
		(const char *)&ev);
}

/* ------------------------------------------------------------------------
 * Cursors: X cursors made from ARGB images (the client's cursor surface or
 * the cursor theme), set on the backend's window under the pointer.
 */
static xcb_cursor_t make_cursor(const uint32_t *argb, int w, int h, int stride_px, int hx, int hy) {
	if (!S.argb32 || w <= 0 || h <= 0 || w > 256 || h > 256)
		return XCB_NONE;
	xcb_pixmap_t pix = xcb_generate_id(S.x);
	xcb_create_pixmap(S.x, 32, pix, S.root, w, h);
	xcb_gcontext_t gc = xcb_generate_id(S.x);
	xcb_create_gc(S.x, gc, pix, 0, NULL);
	uint32_t *tight = malloc((size_t)w * h * 4);
	for (int y = 0; y < h; y++)
		memcpy(tight + y * w, argb + y * stride_px, w * 4);
	xcb_put_image(S.x, XCB_IMAGE_FORMAT_Z_PIXMAP, pix, gc, w, h, 0, 0, 0, 32, w * h * 4, (uint8_t *)tight);
	free(tight);
	xcb_render_picture_t pic = xcb_generate_id(S.x);
	xcb_render_create_picture(S.x, pic, pix, S.argb32, 0, NULL);
	xcb_cursor_t cur = xcb_generate_id(S.x);
	if (hx < 0) hx = 0;
	if (hy < 0) hy = 0;
	if (hx >= w) hx = w - 1;
	if (hy >= h) hy = h - 1;
	xcb_render_create_cursor(S.x, cur, pic, hx, hy);
	xcb_render_free_picture(S.x, pic);
	xcb_free_gc(S.x, gc);
	xcb_free_pixmap(S.x, pix);
	return cur;
}

static xcb_cursor_t theme_cursor(const char *name) {
	float sc = S.scale > 0 ? S.scale : 1;
	struct wlr_xcursor *xc = S.xcursor ? wlr_xcursor_manager_get_xcursor(S.xcursor, name, sc) : NULL;
	if (!xc && strcmp(name, "default"))
		xc = S.xcursor ? wlr_xcursor_manager_get_xcursor(S.xcursor, "default", sc) : NULL;
	if (!xc || xc->image_count == 0)
		return XCB_NONE;
	struct wlr_xcursor_image *im = xc->images[0];
	return make_cursor((const uint32_t *)im->buffer, im->width, im->height, im->width, im->hotspot_x, im->hotspot_y);
}

static void apply_cursor(struct view *v) {
	if (!v || !v->child)
		return;
	uint32_t c = S.cursor_current;
	xcb_change_window_attributes(S.x, v->child, XCB_CW_CURSOR, &c);
	xcb_flush(S.x);
}

static void set_current_cursor(xcb_cursor_t c, bool owned) {
	xcb_cursor_t old = S.cursor_current;
	S.cursor_current = c;
	apply_cursor(S.pointer_view);
	if (old && old != S.cursor_default && old != S.cursor_hidden)
		xcb_free_cursor(S.x, old);
	(void)owned;
}

/* An X cursor from a surface's picture (a cursor or a drag icon), read
 * back from its texture: wlroots hands shared-memory buffers back to the
 * program as soon as they are copied, so the buffer itself is often gone.
 * XCB_NONE when it can't be read; the hidden cursor when there is none. */
static xcb_cursor_t surface_cursor(struct wlr_surface *s, int hx, int hy) {
	if (!s || !wlr_surface_has_buffer(s) || !s->buffer || !s->buffer->texture)
		return S.cursor_hidden;
	struct wlr_texture *t = s->buffer->texture;
	if (t->width == 0 || t->height == 0 || t->width > 256 || t->height > 256)
		return XCB_NONE;
	uint32_t *px = calloc((size_t)t->width * t->height, 4);
	if (!px)
		return XCB_NONE;
	xcb_cursor_t c = XCB_NONE;
	if (wlr_texture_read_pixels(t, &(struct wlr_texture_read_pixels_options){
			.data = px, .format = DRM_FORMAT_ARGB8888, .stride = t->width * 4 }))
		c = make_cursor(px, t->width, t->height, t->width, hx, hy);
	free(px);
	return c;
}

static void cursor_surface_update(void) {
	struct wlr_surface *s = S.cursor_surface;
	if (!s)
		return;
	xcb_cursor_t c = surface_cursor(s, S.cursor_hx * s->current.scale, S.cursor_hy * s->current.scale);
	if (c == XCB_NONE)
		c = S.cursor_default;
	set_current_cursor(c, true);
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	wlr_surface_send_frame_done(s, &now);
}

static void cursor_surface_commit(struct wl_listener *l, void *data) {
	cursor_surface_update();
}

static void cursor_surface_forget(void) {
	if (S.cursor_surface) {
		wl_list_remove(&S.cursor_commit.link);
		wl_list_remove(&S.cursor_destroy.link);
		S.cursor_surface = NULL;
	}
}

static void cursor_surface_destroy(struct wl_listener *l, void *data) {
	cursor_surface_forget();
}

static void seat_request_cursor(struct wl_listener *l, void *data) {
	struct wlr_seat_pointer_request_set_cursor_event *ev = data;
	if (ev->seat_client != S.seat->pointer_state.focused_client)
		return;
	cursor_surface_forget();
	if (!ev->surface) {
		set_current_cursor(S.cursor_hidden, false);
		return;
	}
	S.cursor_surface = ev->surface;
	S.cursor_hx = ev->hotspot_x;
	S.cursor_hy = ev->hotspot_y;
	S.cursor_commit.notify = cursor_surface_commit;
	wl_signal_add(&ev->surface->events.commit, &S.cursor_commit);
	S.cursor_destroy.notify = cursor_surface_destroy;
	wl_signal_add(&ev->surface->events.destroy, &S.cursor_destroy);
	cursor_surface_update();
}

static void cursor_shape_request(struct wl_listener *l, void *data) {
	struct wlr_cursor_shape_manager_v1_request_set_shape_event *ev = data;
	if (ev->seat_client != S.seat->pointer_state.focused_client)
		return;
	cursor_surface_forget();
	const char *name = wlr_cursor_shape_v1_name(ev->shape);
	xcb_cursor_t c = strcmp(name, "default") ? theme_cursor(name) : XCB_NONE;
	set_current_cursor(c ? c : S.cursor_default, false);
}

/* ------------------------------------------------------------------------
 * Views
 */
static struct view *view_from_surface(struct wlr_surface *s) {
	struct wlr_xdg_surface *x = s ? wlr_xdg_surface_try_from_wlr_surface(s) : NULL;
	return x ? x->data : NULL;
}

static struct view *root_view(struct view *v) {
	while (v && v->kind == VIEW_POPUP) {
		struct view *p = view_from_surface(v->popup->parent);
		if (!p)
			return NULL;
		v = p;
	}
	return v;
}

static struct view *view_by_window(xcb_window_t w) {
	struct view *v;
	wl_list_for_each(v, &S.views, link)
		if (w && (v->frame == w || v->child == w))
			return v;
	return NULL;
}

static struct view *view_by_output_name(const char *name) {
	struct view *v;
	wl_list_for_each(v, &S.views, link)
		if (v->output && name && strcmp(v->output->name, name) == 0)
			return v;
	return NULL;
}

/* Pixels (X) from Wayland units of a window, and back (HiDPI). */
static int px(struct view *v, double units) {
	return (int)lround(units * v->scale);
}
static int units(struct view *v, double pixels) {
	return (int)lround(pixels / v->scale);
}

static void view_render_resize(struct view *v, int w, int h) {
	if (w < 1) w = 1;
	if (h < 1) h = 1;
	bool rescale = v->output && v->output->scale != (float)v->scale;
	if (!v->output || (w == v->w && h == v->h && !rescale))
		return;
	v->w = w; v->h = h;
	wlr_scene_rect_set_size(v->bg, (int)ceil(w / v->scale), (int)ceil(h / v->scale));
	struct wlr_output_state st;
	wlr_output_state_init(&st);
	wlr_output_state_set_custom_mode(&st, w, h, 0);
	wlr_output_state_set_scale(&st, v->scale);
	if (!wlr_scene_output_build_state(v->sout, &st, NULL) || !wlr_output_commit_state(v->output, &st))
		diag("resizing the window of %s to %dx%d failed", v->toplevel && v->toplevel->app_id ?
			v->toplevel->app_id : "a popup", w, h);
	wlr_output_state_finish(&st);
}

/* Window manager hints from the Wayland window's state. */
static void view_update_size_hints(struct view *v) {
	struct wlr_xdg_toplevel_state *c = &v->toplevel->current;
	if (c->min_width == v->min_w && c->min_height == v->min_h && c->max_width == v->max_w &&
	    c->max_height == v->max_h && v->x_mapped)
		return;
	v->min_w = c->min_width; v->min_h = c->min_height;
	v->max_w = c->max_width; v->max_h = c->max_height;
	/* WM_SIZE_HINTS: flags, pad x4, min w/h, max w/h, inc w/h, aspect x4, base w/h, gravity */
	uint32_t hints[18] = {0};
	if (v->min_w > 0 || v->min_h > 0) {
		hints[0] |= 16;  /* PMinSize */
		hints[5] = v->min_w > 0 ? px(v, v->min_w) : 1;
		hints[6] = v->min_h > 0 ? px(v, v->min_h) : 1;
	}
	if (v->max_w > 0 || v->max_h > 0) {
		hints[0] |= 32;  /* PMaxSize */
		hints[7] = v->max_w > 0 ? px(v, v->max_w) : 32767;
		hints[8] = v->max_h > 0 ? px(v, v->max_h) : 32767;
	}
	set_prop(v->frame, XCB_ATOM_WM_NORMAL_HINTS, XCB_ATOM_WM_SIZE_HINTS, 32, 18, hints);
}

static void view_set_title(struct view *v) {
	if (!v->frame || !v->toplevel)
		return;
	const char *t = v->toplevel->title ? v->toplevel->title : (v->toplevel->app_id ? v->toplevel->app_id : "");
	set_text(v->frame, A._NET_WM_NAME, A.UTF8_STRING, t);
	set_text(v->frame, XCB_ATOM_WM_NAME, A.UTF8_STRING, t);
	set_text(v->frame, XCB_ATOM_WM_ICON_NAME, A.UTF8_STRING, t);
	xcb_flush(S.x);
}

static pid_t view_pid(struct view *v) {
	pid_t pid = 0; uid_t uid; gid_t gid;
	wl_client_get_credentials(wl_resource_get_client(v->xdg->resource), &pid, &uid, &gid);
	return pid;
}

static char *view_app_id(struct view *v) {
	if (v->toplevel->app_id && *v->toplevel->app_id)
		return strdup(v->toplevel->app_id);
	char path[64], comm[64] = "wayland-app";
	snprintf(path, sizeof(path), "/proc/%d/comm", (int)view_pid(v));
	FILE *f = fopen(path, "r");
	if (f) {
		if (fgets(comm, sizeof(comm), f))
			comm[strcspn(comm, "\n")] = 0;
		fclose(f);
	}
	return strdup(comm);
}

/* The icon: /usr/lib/syluxium/wayland-host-icon finds the program's icon in
 * the icon theme (via its .desktop file) and prints _NET_WM_ICON data. */
static void icon_done(struct view *v) {
	if (v->icon_src) wl_event_source_remove(v->icon_src);
	if (v->icon_fd >= 0) close(v->icon_fd);
	v->icon_src = NULL; v->icon_fd = -1;
	if (v->icon_pid > 0) waitpid(v->icon_pid, NULL, WNOHANG);
	v->icon_pid = 0;
}

static int icon_readable(int fd, uint32_t mask, void *data) {
	struct view *v = data;
	if (v->icon_cap - v->icon_len < 65536) {
		v->icon_cap = v->icon_cap * 2 + 65536;
		v->icon_buf = realloc(v->icon_buf, v->icon_cap);
	}
	ssize_t n = read(fd, v->icon_buf + v->icon_len, v->icon_cap - v->icon_len);
	if (n > 0) {
		v->icon_len += n;
		return 0;
	}
	if (n < 0 && (errno == EAGAIN || errno == EINTR))
		return 0;
	if (v->icon_len >= 12 && v->frame) {
		set_prop(v->frame, A._NET_WM_ICON, XCB_ATOM_CARDINAL, 32, v->icon_len / 4, v->icon_buf);
		xcb_flush(S.x);
	}
	free(v->icon_buf); v->icon_buf = NULL; v->icon_len = v->icon_cap = 0;
	icon_done(v);
	return 0;
}

static void view_request_icon(struct view *v, const char *app_id) {
	if (v->icon_for && strcmp(v->icon_for, app_id) == 0)
		return;
	free(v->icon_for);
	v->icon_for = strdup(app_id);
	icon_done(v);
	int p[2];
	if (pipe2(p, O_CLOEXEC) < 0)
		return;
	pid_t pid = fork();
	if (pid == 0) {
		dup2(p[1], 1);
		char spid[16];
		snprintf(spid, sizeof(spid), "%d", (int)view_pid(v));
		setenv("GDK_BACKEND", "x11", 1);
		execl("/usr/lib/syluxium/wayland-host-icon", "wayland-host-icon", app_id, spid, (char *)NULL);
		_exit(127);
	}
	close(p[1]);
	if (pid < 0) {
		close(p[0]);
		return;
	}
	fcntl(p[0], F_SETFL, O_NONBLOCK);
	v->icon_pid = pid;
	v->icon_fd = p[0];
	v->icon_src = wl_event_loop_add_fd(S.loop, p[0], WL_EVENT_READABLE, icon_readable, v);
}

static void view_set_class(struct view *v) {
	if (!v->frame)
		return;
	char *id = view_app_id(v);
	size_t n = strlen(id);
	char *cls = malloc(2 * n + 2);
	memcpy(cls, id, n + 1);
	memcpy(cls + n + 1, id, n + 1);
	set_prop(v->frame, XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, 8, 2 * n + 2, cls);
	free(cls);
	xcb_flush(S.x);
	view_request_icon(v, id);
	free(id);
}

static void view_set_transient(struct view *v) {
	if (!v->frame)
		return;
	struct view *p = v->toplevel->parent ? v->toplevel->parent->base->data : NULL;
	uint32_t type = p ? A._NET_WM_WINDOW_TYPE_DIALOG : A._NET_WM_WINDOW_TYPE_NORMAL;
	set_prop(v->frame, A._NET_WM_WINDOW_TYPE, XCB_ATOM_ATOM, 32, 1, &type);
	if (p && p->frame)
		set_prop(v->frame, XCB_ATOM_WM_TRANSIENT_FOR, XCB_ATOM_WINDOW, 32, 1, &p->frame);
	else
		xcb_delete_property(S.x, v->frame, XCB_ATOM_WM_TRANSIENT_FOR);
	xcb_flush(S.x);
}

/* Where the root toplevel's window (its geometry origin) is on the screen. */
static bool view_root_origin(struct view *v, int *x, int *y) {
	if (!v || !v->child)
		return false;
	xcb_translate_coordinates_reply_t *r = xcb_translate_coordinates_reply(S.x,
		xcb_translate_coordinates(S.x, v->child, S.root, 0, 0), NULL);
	x_after_blocking();
	if (!r)
		return false;
	*x = r->dst_x; *y = r->dst_y;
	free(r);
	return true;
}

static void work_area(struct wlr_box *box) {
	box->x = 0; box->y = 0;
	box->width = S.screen->width_in_pixels; box->height = S.screen->height_in_pixels;
	xcb_get_property_reply_t *r = xcb_get_property_reply(S.x,
		xcb_get_property(S.x, 0, S.root, A._NET_WORKAREA, XCB_ATOM_CARDINAL, 0, 4), NULL);
	x_after_blocking();
	if (r && xcb_get_property_value_length(r) >= 16) {
		uint32_t *wa = xcb_get_property_value(r);
		if (wa[2] > 100 && wa[3] > 100) {
			box->x = wa[0]; box->y = wa[1]; box->width = wa[2]; box->height = wa[3];
		}
	}
	free(r);
}

/* Popups: kept on the screen (the work area), in the root toplevel's
 * surface coordinates. */
static void popup_unconstrain(struct view *v) {
	struct view *top = root_view(v);
	struct wlr_box wa, box;
	work_area(&wa);
	int ox, oy;
	if (!top || !view_root_origin(top, &ox, &oy)) {
		ox = 0; oy = 0;
	}
	struct wlr_box geo;
	wlr_xdg_surface_get_geometry(top ? top->xdg : v->xdg, &geo);
	struct view *u = top ? top : v;
	box.x = units(u, wa.x - ox) + geo.x;
	box.y = units(u, wa.y - oy) + geo.y;
	box.width = units(u, wa.width); box.height = units(u, wa.height);
	wlr_xdg_popup_unconstrain_from_box(v->popup, &box);
}

static bool popup_screen_pos(struct view *v, int *x, int *y) {
	struct view *top = root_view(v);
	int ox, oy, tx, ty;
	if (!top || !view_root_origin(top, &ox, &oy))
		return false;
	struct wlr_box geo;
	wlr_xdg_surface_get_geometry(top->xdg, &geo);
	wlr_xdg_popup_get_toplevel_coords(v->popup, 0, 0, &tx, &ty);
	*x = ox + px(top, tx - geo.x);
	*y = oy + px(top, ty - geo.y);
	return true;
}

static uint64_t mono_ns(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

static void view_frame(struct view *v) {
	v->n_frames++;
	uint64_t now_ns = mono_ns();
	v->last_frame_ns = now_ns;
	/* the next frame one refresh after this one's due time (no drift) */
	v->next_due_ns = (v->next_due_ns && now_ns - v->next_due_ns < S.frame_ns) ? v->next_due_ns + S.frame_ns
		: now_ns + S.frame_ns;
	v->n_commits += wlr_scene_output_commit(v->sout, NULL) ? 1 : 0;
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	wlr_scene_output_send_frame_done(v->sout, &now);
}

static int pace_timer(void *data) {
	struct view *v = data;
	if (v->sout)
		view_frame(v);
	return 0;
}

/* The X server says when a frame was shown (Present). Without vertical
 * blank events from the display driver (simpledrm, the firmware
 * framebuffer) it says so at once, and programs would draw hundreds of
 * frames a second for nothing: frames are paced to the monitor's rate. */
static void output_frame(struct wl_listener *l, void *data) {
	struct view *v = wl_container_of(l, v, out_frame);
	uint64_t now = mono_ns();
	if (v->next_due_ns == 0 || now + 1500000 >= v->next_due_ns) {
		view_frame(v);
		return;
	}
	if (!v->pace)
		v->pace = wl_event_loop_add_timer(S.loop, pace_timer, v);
	uint64_t wait_ms = (v->next_due_ns - now) / 1000000;
	wl_event_source_timer_update(v->pace, wait_ms > 0 ? wait_ms : 1);
}

/* The monitor's refresh rate (the fastest one with several), from RandR. */
static void read_refresh_rate(void) {
	double best = 0;
	const xcb_query_extension_reply_t *ext = xcb_get_extension_data(S.x, &xcb_randr_id);
	if (ext && ext->present) {
		xcb_randr_get_screen_resources_current_reply_t *res = xcb_randr_get_screen_resources_current_reply(S.x,
			xcb_randr_get_screen_resources_current(S.x, S.root), NULL);
		if (res) {
			xcb_randr_crtc_t *crtcs = xcb_randr_get_screen_resources_current_crtcs(res);
			xcb_randr_mode_info_t *modes = xcb_randr_get_screen_resources_current_modes(res);
			int nmodes = xcb_randr_get_screen_resources_current_modes_length(res);
			for (int i = 0; i < xcb_randr_get_screen_resources_current_crtcs_length(res); i++) {
				xcb_randr_get_crtc_info_reply_t *ci = xcb_randr_get_crtc_info_reply(S.x,
					xcb_randr_get_crtc_info(S.x, crtcs[i], res->config_timestamp), NULL);
				if (ci && ci->mode)
					for (int m = 0; m < nmodes; m++)
						if (modes[m].id == ci->mode && modes[m].htotal && modes[m].vtotal) {
							double hz = (double)modes[m].dot_clock / ((double)modes[m].htotal * modes[m].vtotal);
							if (hz > best && hz < 1000)
								best = hz;
						}
				free(ci);
			}
			free(res);
		}
	}
	const char *e = getenv("SYLUXIUM_WAYLAND_HOST_HZ");
	if (e && atof(e) > 1)
		best = atof(e);
	if (best < 20)
		best = 60;
	S.frame_ns = (uint64_t)(1e9 / best);
}

/* The backend asks for a size (its window was resized; with our frame
 * around it, only ever the size we gave it). */
static void output_request_state(struct wl_listener *l, void *data) {
	struct view *v = wl_container_of(l, v, out_request_state);
	const struct wlr_output_event_request_state *ev = data;
	if (ev->state->committed & WLR_OUTPUT_STATE_MODE) {
		int w = ev->state->custom_mode.width, h = ev->state->custom_mode.height;
		if (w != v->w || h != v->h) {
			uint32_t vals[2] = { v->w, v->h };
			xcb_configure_window(S.bx->xcb, v->child, XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, vals);
			xcb_flush(S.bx->xcb);
		}
	}
}

static void output_present(struct wl_listener *l, void *data) {
	struct view *v = wl_container_of(l, v, out_present);
	v->n_presents++;
}

static void output_commit_dbg(struct wl_listener *l, void *data) {
	struct view *v = wl_container_of(l, v, out_commit);
	struct wlr_output_event_commit *ev = data;
	pixman_box32_t *e = pixman_region32_extents(&ev->state->damage);
	diag("debug: commit %s output %s win 0x%x buffer %p damage %d,%d-%d,%d committed 0x%x",
		v->toplevel && v->toplevel->app_id ? v->toplevel->app_id : "popup", v->output->name, v->child,
		(void *)ev->state->buffer, e->x1, e->y1, e->x2, e->y2, ev->state->committed);
}

static int debug_stats(void *data) {
	struct view *v;
	wl_list_for_each(v, &S.views, link)
		if (v->output)
			diag("debug: %s %dx%d frames %u commits %u presents %u",
				v->toplevel && v->toplevel->app_id ? v->toplevel->app_id : "popup", v->w, v->h,
				v->n_frames, v->n_commits, v->n_presents);
	wl_event_source_timer_update(data ? *(struct wl_event_source **)data : NULL, 3000);
	return 0;
}

static void view_unmap_x(struct view *v);

static void output_destroy(struct wl_listener *l, void *data) {
	struct view *v = wl_container_of(l, v, out_destroy);
	wl_list_remove(&v->out_frame.link);
	wl_list_remove(&v->out_request_state.link);
	wl_list_remove(&v->out_destroy.link);
	wl_list_remove(&v->out_present.link);
	if (v->pace)
		wl_event_source_remove(v->pace);
	v->pace = NULL;
	v->last_frame_ns = v->next_due_ns = 0;
	wl_list_remove(&v->out_commit.link);
	if (v->sout)
		wlr_scene_output_destroy(v->sout);
	v->sout = NULL;
	v->output = NULL;
	v->child = 0;
	if (v->frame) {
		/* not by us (the X server went away?) */
		xcb_destroy_window(S.x, v->frame);
		xcb_flush(S.x);
		v->frame = 0;
		v->x_mapped = false;
	}
}

static void test_present(struct wl_listener *l, void *data) {
	S.test_presented = true;
}
static struct wl_listener l_test_present = { .notify = test_present };

static void server_new_output(struct wl_listener *l, void *data) {
	struct wlr_output *out = data;
	struct view *v = S.pending_view;
	if (S.test_output && wlr_output_is_x11(out)) {
		S.test_output = false;
		wlr_output_init_render(out, S.allocator, S.renderer);
		S.test_out = out;
		return;
	}
	if (!v || !wlr_output_is_x11(out))
		return;
	S.pending_view = NULL;
	wlr_output_init_render(out, S.allocator, S.renderer);
	v->output = out;
	v->sout = wlr_scene_output_create(v->scene, out);
	v->out_frame.notify = output_frame;
	wl_signal_add(&out->events.frame, &v->out_frame);
	v->out_request_state.notify = output_request_state;
	wl_signal_add(&out->events.request_state, &v->out_request_state);
	v->out_destroy.notify = output_destroy;
	wl_signal_add(&out->events.destroy, &v->out_destroy);
	v->out_present.notify = output_present;
	wl_signal_add(&out->events.present, &v->out_present);
	v->out_commit.notify = output_commit_dbg;
	wl_list_init(&v->out_commit.link);
	if (getenv("SYLUXIUM_WAYLAND_HOST_DEBUG_COMMITS"))
		wl_signal_add(&out->events.commit, &v->out_commit);
}

/* Is the backend's window really where we expect it (wlroots' private
 * layout)? It exists, has the backend's initial size and is ours to move. */
static bool child_window_ok(xcb_window_t w) {
	for (int i = 0; i < 200; i++) {
		xcb_get_geometry_reply_t *g = xcb_get_geometry_reply(S.x, xcb_get_geometry(S.x, w), NULL);
		if (g) {
			bool ok = g->root == S.root;
			free(g);
			x_after_blocking();
			return ok;
		}
		usleep(1000);
	}
	x_after_blocking();
	return false;
}

static bool view_map_x(struct view *v) {
	struct wlr_box geo;
	wlr_xdg_surface_get_geometry(v->xdg, &geo);
	v->scale = S.scale;
	int w = geo.width > 0 ? px(v, geo.width) : 1, h = geo.height > 0 ? px(v, geo.height) : 1;
	int x = 0, y = 0;
	bool popup = v->kind == VIEW_POPUP;
	if (popup && !popup_screen_pos(v, &x, &y))
		return false;

	v->frame = xcb_generate_id(S.x);
	uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_BIT_GRAVITY | XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK;
	uint32_t vals[] = { 0x1d2552, XCB_GRAVITY_NORTH_WEST, popup,
		XCB_EVENT_MASK_STRUCTURE_NOTIFY | XCB_EVENT_MASK_FOCUS_CHANGE | XCB_EVENT_MASK_PROPERTY_CHANGE };
	xcb_create_window(S.x, XCB_COPY_FROM_PARENT, v->frame, S.root, x, y, w, h, 0,
		XCB_WINDOW_CLASS_INPUT_OUTPUT, S.screen->root_visual, mask, vals);
	pid_t pid = view_pid(v);
	set_prop(v->frame, A._NET_WM_PID, XCB_ATOM_CARDINAL, 32, 1, &pid);
	set_text(v->frame, A.WM_CLIENT_MACHINE, XCB_ATOM_STRING, S.hostname);
	uint32_t one = 1;
	set_prop(v->frame, A._SYLUXIUM_WAYLAND, XCB_ATOM_CARDINAL, 32, 1, &one);
	if (popup) {
		uint32_t type = v->popup->seat ? A._NET_WM_WINDOW_TYPE_POPUP_MENU : A._NET_WM_WINDOW_TYPE_TOOLTIP;
		set_prop(v->frame, A._NET_WM_WINDOW_TYPE, XCB_ATOM_ATOM, 32, 1, &type);
		struct view *top = root_view(v);
		if (top) {
			char *id = view_app_id(top);
			size_t n = strlen(id);
			char *cls = malloc(2 * n + 2);
			memcpy(cls, id, n + 1);
			memcpy(cls + n + 1, id, n + 1);
			set_prop(v->frame, XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, 8, 2 * n + 2, cls);
			free(cls);
			free(id);
		}
	} else {
		xcb_atom_t protos[] = { A.WM_DELETE_WINDOW };
		set_prop(v->frame, A.WM_PROTOCOLS, XCB_ATOM_ATOM, 32, 1, protos);
		uint32_t xdnd_version = 5;  /* X11 programs can drop on it */
		set_prop(v->frame, XdndAware, XCB_ATOM_ATOM, 32, 1, &xdnd_version);
		uint32_t wmhints[9] = { 1 | 2, 1, 1 };  /* InputHint | StateHint, input, NormalState */
		set_prop(v->frame, XCB_ATOM_WM_HINTS, XCB_ATOM_WM_HINTS, 32, 9, wmhints);
		v->min_w = -1;
		view_update_size_hints(v);
		view_set_title(v);
		view_set_class(v);
		view_set_transient(v);
		if (v->csd) {
			uint32_t motif[5] = { 2, 0, 0, 0, 0 };  /* MWM_HINTS_DECORATIONS: none */
			set_prop(v->frame, A._MOTIF_WM_HINTS, A._MOTIF_WM_HINTS, 32, 5, motif);
		}
		if (v->toplevel->requested.maximized || v->toplevel->requested.fullscreen) {
			uint32_t st[2]; int n = 0;
			if (v->toplevel->requested.fullscreen) st[n++] = A._NET_WM_STATE_FULLSCREEN;
			else { st[n++] = A._NET_WM_STATE_MAXIMIZED_VERT; st[n++] = A._NET_WM_STATE_MAXIMIZED_HORZ; }
			set_prop(v->frame, A._NET_WM_STATE, XCB_ATOM_ATOM, 32, n, st);
		}
	}
	xcb_flush(S.x);

	/* The backend's window, inside ours. */
	S.pending_view = v;
	struct wlr_output *out = wlr_x11_output_create(S.backend);
	S.pending_view = NULL;
	if (!out || !v->output) {
		diag("ERROR: the X11 backend made no window");
		xcb_destroy_window(S.x, v->frame);
		v->frame = 0;
		return false;
	}
	v->child = ((struct x11_output_prefix *)out)->win;
	if (!child_window_ok(v->child)) {
		diag("ERROR: the X11 backend's window 0x%x was not found (wlroots version?)", v->child);
		v->child = 0;
		wlr_output_destroy(out);
		xcb_destroy_window(S.x, v->frame);
		v->frame = 0;
		return false;
	}
	xcb_reparent_window(S.x, v->child, v->frame, 0, 0);
	/* The keyboard: the window manager gives the focus to our frame; the
	 * backend reads keys (XInput 2) from its own window only, so it also
	 * listens on the frame (through its connection, so the events reach it). */
	struct {
		xcb_input_event_mask_t head;
		uint32_t mask;
	} keys = { { XCB_INPUT_DEVICE_ALL_MASTER, 1 },
		XCB_INPUT_XI_EVENT_MASK_KEY_PRESS | XCB_INPUT_XI_EVENT_MASK_KEY_RELEASE };
	if (!popup) {
		xcb_input_xi_select_events(S.bx->xcb, v->frame, 1, &keys.head);
		xcb_flush(S.bx->xcb);
	}
	uint32_t ev = XCB_EVENT_MASK_FOCUS_CHANGE;
	xcb_change_window_attributes(S.x, v->child, XCB_CW_EVENT_MASK, &ev);
	uint32_t c = S.cursor_default;
	xcb_change_window_attributes(S.x, v->child, XCB_CW_CURSOR, &c);
	/* make sure the X server has done this before the backend maps its window */
	free(xcb_get_input_focus_reply(S.x, xcb_get_input_focus(S.x), NULL));
	x_after_blocking();

	v->w = w; v->h = h;
	wlr_scene_rect_set_size(v->bg, (int)ceil(w / v->scale), (int)ceil(h / v->scale));
	struct wlr_output_state st;
	wlr_output_state_init(&st);
	wlr_output_state_set_enabled(&st, true);
	wlr_output_state_set_custom_mode(&st, w, h, 0);
	wlr_output_state_set_scale(&st, v->scale);
	bool ok = wlr_scene_output_build_state(v->sout, &st, NULL) && wlr_output_commit_state(out, &st);
	wlr_output_state_finish(&st);
	if (!ok)
		diag("ERROR: first frame of a window failed (%dx%d)", w, h);
	xcb_map_window(S.x, v->frame);
	xcb_flush(S.x);
	v->x_mapped = true;
	if (S.screen_output)
		wlr_surface_send_enter(v->xdg->surface, S.screen_output);
	return true;
}

static void view_unmap_x(struct view *v) {
	if (S.pointer_view == v)
		S.pointer_view = NULL;
	if (S.focused == v) {
		S.focused = NULL;
		if (S.seat->keyboard_state.focused_surface == v->xdg->surface)
			wlr_seat_keyboard_notify_clear_focus(S.seat);
	}
	if (S.seat->pointer_state.focused_surface &&
	    view_from_surface(wlr_surface_get_root_surface(S.seat->pointer_state.focused_surface)) == v)
		wlr_seat_pointer_notify_clear_focus(S.seat);
	xcb_window_t frame = v->frame;
	v->frame = 0;
	if (v->output) {
		/* the backend's window leaves our frame before both go */
		if (v->child)
			xcb_reparent_window(S.x, v->child, S.root, -10000, -10000);
		xcb_flush(S.x);
		wlr_output_destroy(v->output);
	}
	if (frame) {
		xcb_destroy_window(S.x, frame);
		xcb_flush(S.x);
	}
	v->x_mapped = false;
}

static void view_decide_decorations(struct view *v) {
	if (v->kind != VIEW_TOPLEVEL)
		return;
	/* Client-side decorations only when the program asked for them (KDE's
	 * protocol, which GTK uses); the frame is xfwm4's otherwise, as for
	 * programs that use no decoration protocol at all. */
	v->csd = false;
	struct kde_deco *k;
	wl_list_for_each(k, &S.kde_decos, link)
		if (k->deco->surface == v->xdg->surface && k->deco->mode == WLR_SERVER_DECORATION_MANAGER_MODE_CLIENT)
			v->csd = true;
	if (v->xdg_deco)
		v->csd = false;
}

static void handle_map(struct wl_listener *l, void *data) {
	struct view *v = wl_container_of(l, v, map);
	view_decide_decorations(v);
	if (!view_map_x(v))
		return;
	if (v->kind == VIEW_TOPLEVEL) {
		S.hosted++;
		char *id = view_app_id(v);
		struct wlr_dmabuf_attributes dmabuf;
		bool gpu = v->xdg->surface->buffer && wlr_buffer_get_dmabuf(&v->xdg->surface->buffer->base, &dmabuf);
		diag("hosted: %s (pid %d) \"%s\" %dx%d, %s frame, draws into %s", id, (int)view_pid(v),
			v->toplevel->title ? v->toplevel->title : "", v->w, v->h, v->csd ? "its own" : "xfwm4",
			gpu ? "dma-bufs (GPU)" : "shared memory");
		free(id);
	}
}

static void handle_unmap(struct wl_listener *l, void *data) {
	struct view *v = wl_container_of(l, v, unmap);
	view_unmap_x(v);
}

static void handle_commit(struct wl_listener *l, void *data) {
	struct view *v = wl_container_of(l, v, commit);
	struct wlr_xdg_surface *xdg = v->xdg;
	if (xdg->initial_commit) {
		if (v->kind == VIEW_TOPLEVEL) {
			struct wlr_xdg_toplevel *t = v->toplevel;
			if (xdg->client->shell->version >= 2)
				wlr_xdg_toplevel_set_tiled(t, WLR_EDGE_TOP | WLR_EDGE_BOTTOM | WLR_EDGE_LEFT | WLR_EDGE_RIGHT);
			if (xdg->client->shell->version >= 4) {
				struct wlr_box wa;
				work_area(&wa);
				wlr_xdg_toplevel_set_bounds(t, (int)(wa.width / S.scale), (int)(wa.height / S.scale));
			}
			if (xdg->client->shell->version >= 5)
				wlr_xdg_toplevel_set_wm_capabilities(t, WLR_XDG_TOPLEVEL_WM_CAPABILITIES_WINDOW_MENU |
					WLR_XDG_TOPLEVEL_WM_CAPABILITIES_MAXIMIZE | WLR_XDG_TOPLEVEL_WM_CAPABILITIES_FULLSCREEN |
					WLR_XDG_TOPLEVEL_WM_CAPABILITIES_MINIMIZE);
			if (v->xdg_deco)
				wlr_xdg_toplevel_decoration_v1_set_mode(v->xdg_deco, WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
			wlr_xdg_toplevel_set_size(t, 0, 0);
		} else {
			popup_unconstrain(v);
		}
		return;
	}
	struct wlr_box geo;
	wlr_xdg_surface_get_geometry(xdg, &geo);
	wlr_scene_node_set_position(&v->surface_tree->node, -geo.x, -geo.y);
	if (!v->x_mapped)
		return;
	if (v->kind == VIEW_TOPLEVEL) {
		view_update_size_hints(v);
		/* The program's own size, once it has seen our latest one. */
		int32_t d = (int32_t)(xdg->current.configure_serial - v->size_serial);
		int pw = px(v, geo.width), ph = px(v, geo.height);
		if ((v->size_serial == 0 || d >= 0) && (pw != v->w || ph != v->h) && geo.width > 0 && geo.height > 0 &&
		    (units(v, v->w) != geo.width || units(v, v->h) != geo.height)) {
			uint32_t vals[2] = { pw, ph };
			xcb_configure_window(S.x, v->frame, XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, vals);
			xcb_flush(S.x);
			view_render_resize(v, pw, ph);
		}
	} else {
		int x, y;
		if (popup_screen_pos(v, &x, &y)) {
			v->scale = root_view(v) ? root_view(v)->scale : S.scale;
			uint32_t vals[4] = { x, y, geo.width > 0 ? px(v, geo.width) : 1, geo.height > 0 ? px(v, geo.height) : 1 };
			xcb_configure_window(S.x, v->frame, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
				XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, vals);
			xcb_flush(S.x);
			view_render_resize(v, vals[2], vals[3]);
		}
	}
}

static void handle_destroy(struct wl_listener *l, void *data) {
	struct view *v = wl_container_of(l, v, destroy);
	view_unmap_x(v);
	icon_done(v);
	free(v->icon_buf);
	free(v->icon_for);
	if (v->xdg_deco) {
		wl_list_remove(&v->deco_request_mode.link);
		wl_list_remove(&v->deco_destroy.link);
	}
	wl_list_remove(&v->map.link);
	wl_list_remove(&v->unmap.link);
	wl_list_remove(&v->commit.link);
	wl_list_remove(&v->destroy.link);
	if (v->kind == VIEW_TOPLEVEL) {
		wl_list_remove(&v->set_title.link);
		wl_list_remove(&v->set_app_id.link);
		wl_list_remove(&v->set_parent.link);
		wl_list_remove(&v->req_move.link);
		wl_list_remove(&v->req_resize.link);
		wl_list_remove(&v->req_max.link);
		wl_list_remove(&v->req_full.link);
		wl_list_remove(&v->req_min.link);
		wl_list_remove(&v->req_menu.link);
	} else {
		wl_list_remove(&v->reposition.link);
	}
	wl_list_remove(&v->link);
	v->xdg->data = NULL;
	wlr_scene_node_destroy(&v->scene->tree.node);
	free(v);
}

static void handle_set_title(struct wl_listener *l, void *data) {
	struct view *v = wl_container_of(l, v, set_title);
	view_set_title(v);
}
static void handle_set_app_id(struct wl_listener *l, void *data) {
	struct view *v = wl_container_of(l, v, set_app_id);
	view_set_class(v);
}
static void handle_set_parent(struct wl_listener *l, void *data) {
	struct view *v = wl_container_of(l, v, set_parent);
	view_set_transient(v);
}

/* Moving and resizing by the program (client-side title bars): the window
 * manager does it, as for GTK on X11. The pointer is still grabbed by the
 * backend's connection (the button press), so the request goes through
 * that connection after releasing the grab. */
static void moveresize(struct view *v, uint32_t direction) {
	if (!v->frame || !S.bx)
		return;
	xcb_query_pointer_reply_t *p = xcb_query_pointer_reply(S.x, xcb_query_pointer(S.x, S.root), NULL);
	x_after_blocking();
	if (!p)
		return;
	xcb_ungrab_pointer(S.bx->xcb, XCB_CURRENT_TIME);
	send_root_message(S.bx->xcb, v->frame, A._NET_WM_MOVERESIZE, p->root_x, p->root_y, direction, 1, 1);
	xcb_flush(S.bx->xcb);
	free(p);
}

static void handle_req_move(struct wl_listener *l, void *data) {
	struct view *v = wl_container_of(l, v, req_move);
	moveresize(v, 8);
}

static void handle_req_resize(struct wl_listener *l, void *data) {
	struct view *v = wl_container_of(l, v, req_resize);
	struct wlr_xdg_toplevel_resize_event *ev = data;
	static const struct { uint32_t edges, dir; } map[] = {
		{ WLR_EDGE_TOP | WLR_EDGE_LEFT, 0 }, { WLR_EDGE_TOP, 1 }, { WLR_EDGE_TOP | WLR_EDGE_RIGHT, 2 },
		{ WLR_EDGE_RIGHT, 3 }, { WLR_EDGE_BOTTOM | WLR_EDGE_RIGHT, 4 }, { WLR_EDGE_BOTTOM, 5 },
		{ WLR_EDGE_BOTTOM | WLR_EDGE_LEFT, 6 }, { WLR_EDGE_LEFT, 7 },
	};
	for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++)
		if (map[i].edges == ev->edges)
			moveresize(v, map[i].dir);
}

static void net_wm_state(struct view *v, bool add, xcb_atom_t a, xcb_atom_t b) {
	if (v->frame)
		send_root_message(S.x, v->frame, A._NET_WM_STATE, add ? 1 : 0, a, b, 1, 0);
	xcb_flush(S.x);
}

static void handle_req_max(struct wl_listener *l, void *data) {
	struct view *v = wl_container_of(l, v, req_max);
	if (v->x_mapped)
		net_wm_state(v, v->toplevel->requested.maximized, A._NET_WM_STATE_MAXIMIZED_VERT, A._NET_WM_STATE_MAXIMIZED_HORZ);
	if (v->xdg->initialized)
		wlr_xdg_surface_schedule_configure(v->xdg);
}

static void handle_req_full(struct wl_listener *l, void *data) {
	struct view *v = wl_container_of(l, v, req_full);
	if (v->x_mapped)
		net_wm_state(v, v->toplevel->requested.fullscreen, A._NET_WM_STATE_FULLSCREEN, 0);
	if (v->xdg->initialized)
		wlr_xdg_surface_schedule_configure(v->xdg);
}

static void handle_req_min(struct wl_listener *l, void *data) {
	struct view *v = wl_container_of(l, v, req_min);
	if (v->frame) {
		send_root_message(S.x, v->frame, A.WM_CHANGE_STATE, 3 /* IconicState */, 0, 0, 0, 0);
		xcb_flush(S.x);
	}
}

static void handle_req_menu(struct wl_listener *l, void *data) {
	struct view *v = wl_container_of(l, v, req_menu);
	if (!v->frame || !S.bx)
		return;
	xcb_query_pointer_reply_t *p = xcb_query_pointer_reply(S.x, xcb_query_pointer(S.x, S.root), NULL);
	x_after_blocking();
	if (!p)
		return;
	xcb_ungrab_pointer(S.bx->xcb, XCB_CURRENT_TIME);
	xcb_client_message_event_t ev = {0};
	ev.response_type = XCB_CLIENT_MESSAGE;
	ev.format = 32;
	ev.window = v->frame;
	ev.type = A._GTK_SHOW_WINDOW_MENU;
	ev.data.data32[0] = 0; ev.data.data32[1] = p->root_x; ev.data.data32[2] = p->root_y;
	xcb_send_event(S.bx->xcb, 0, S.root, XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY,
		(const char *)&ev);
	xcb_flush(S.bx->xcb);
	free(p);
}

static void handle_reposition(struct wl_listener *l, void *data) {
	struct view *v = wl_container_of(l, v, reposition);
	popup_unconstrain(v);
}

static struct view *view_create(struct wlr_xdg_surface *xdg) {
	struct view *v = calloc(1, sizeof(*v));
	v->xdg = xdg;
	v->icon_fd = -1;
	v->scale = 1;
	xdg->data = v;
	v->scene = wlr_scene_create();
	const float bg[4] = { 0.114f, 0.145f, 0.322f, 1.0f };
	v->bg = wlr_scene_rect_create(&v->scene->tree, 1, 1, bg);
	v->surface_tree = wlr_scene_subsurface_tree_create(&v->scene->tree, xdg->surface);
	v->map.notify = handle_map;
	wl_signal_add(&xdg->surface->events.map, &v->map);
	v->unmap.notify = handle_unmap;
	wl_signal_add(&xdg->surface->events.unmap, &v->unmap);
	v->commit.notify = handle_commit;
	wl_signal_add(&xdg->surface->events.commit, &v->commit);
	wl_list_insert(&S.views, &v->link);
	return v;
}

static void new_toplevel(struct wl_listener *l, void *data) {
	struct wlr_xdg_toplevel *t = data;
	struct view *v = view_create(t->base);
	v->kind = VIEW_TOPLEVEL;
	v->toplevel = t;
	v->destroy.notify = handle_destroy;
	wl_signal_add(&t->events.destroy, &v->destroy);
	v->set_title.notify = handle_set_title;
	wl_signal_add(&t->events.set_title, &v->set_title);
	v->set_app_id.notify = handle_set_app_id;
	wl_signal_add(&t->events.set_app_id, &v->set_app_id);
	v->set_parent.notify = handle_set_parent;
	wl_signal_add(&t->events.set_parent, &v->set_parent);
	v->req_move.notify = handle_req_move;
	wl_signal_add(&t->events.request_move, &v->req_move);
	v->req_resize.notify = handle_req_resize;
	wl_signal_add(&t->events.request_resize, &v->req_resize);
	v->req_max.notify = handle_req_max;
	wl_signal_add(&t->events.request_maximize, &v->req_max);
	v->req_full.notify = handle_req_full;
	wl_signal_add(&t->events.request_fullscreen, &v->req_full);
	v->req_min.notify = handle_req_min;
	wl_signal_add(&t->events.request_minimize, &v->req_min);
	v->req_menu.notify = handle_req_menu;
	wl_signal_add(&t->events.request_show_window_menu, &v->req_menu);
}

static void new_popup(struct wl_listener *l, void *data) {
	struct wlr_xdg_popup *p = data;
	struct view *v = view_create(p->base);
	v->kind = VIEW_POPUP;
	v->popup = p;
	v->destroy.notify = handle_destroy;
	wl_signal_add(&p->events.destroy, &v->destroy);
	v->reposition.notify = handle_reposition;
	wl_signal_add(&p->events.reposition, &v->reposition);
}

/* Decorations: server-side (xfwm4's frame) whenever the program can do without its own. */
static void deco_request_mode(struct wl_listener *l, void *data) {
	struct view *v = wl_container_of(l, v, deco_request_mode);
	if (v->xdg->initialized)
		wlr_xdg_toplevel_decoration_v1_set_mode(v->xdg_deco, WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
}
static void deco_destroy(struct wl_listener *l, void *data) {
	struct view *v = wl_container_of(l, v, deco_destroy);
	wl_list_remove(&v->deco_request_mode.link);
	wl_list_remove(&v->deco_destroy.link);
	v->xdg_deco = NULL;
}
static void new_xdg_decoration(struct wl_listener *l, void *data) {
	struct wlr_xdg_toplevel_decoration_v1 *d = data;
	struct view *v = d->toplevel->base->data;
	if (!v)
		return;
	v->xdg_deco = d;
	v->deco_request_mode.notify = deco_request_mode;
	wl_signal_add(&d->events.request_mode, &v->deco_request_mode);
	v->deco_destroy.notify = deco_destroy;
	wl_signal_add(&d->events.destroy, &v->deco_destroy);
	if (v->xdg->initialized)
		wlr_xdg_toplevel_decoration_v1_set_mode(d, WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
}

static void kde_deco_destroy(struct wl_listener *l, void *data) {
	struct kde_deco *k = wl_container_of(l, k, destroy);
	wl_list_remove(&k->destroy.link);
	wl_list_remove(&k->mode.link);
	wl_list_remove(&k->link);
	free(k);
}
static void kde_deco_mode(struct wl_listener *l, void *data) {
	struct kde_deco *k = wl_container_of(l, k, mode);
	struct view *v = view_from_surface(k->deco->surface);
	if (!v || !v->frame || v->kind != VIEW_TOPLEVEL)
		return;
	bool csd = k->deco->mode == WLR_SERVER_DECORATION_MANAGER_MODE_CLIENT;
	if (csd == v->csd)
		return;
	v->csd = csd;
	uint32_t motif[5] = { 2, 0, csd ? 0 : 1, 0, 0 };
	set_prop(v->frame, A._MOTIF_WM_HINTS, A._MOTIF_WM_HINTS, 32, 5, motif);
	xcb_flush(S.x);
}
static void new_kde_decoration(struct wl_listener *l, void *data) {
	struct kde_deco *k = calloc(1, sizeof(*k));
	k->deco = data;
	k->destroy.notify = kde_deco_destroy;
	wl_signal_add(&k->deco->events.destroy, &k->destroy);
	k->mode.notify = kde_deco_mode;
	wl_signal_add(&k->deco->events.mode, &k->mode);
	wl_list_insert(&S.kde_decos, &k->link);
}

/* A program asks to be brought to the front (xdg-activation). */
static void request_activate(struct wl_listener *l, void *data) {
	struct wlr_xdg_activation_v1_request_activate_event *ev = data;
	struct view *v = root_view(view_from_surface(ev->surface));
	if (v && v->frame && v->kind == VIEW_TOPLEVEL) {
		send_root_message(S.x, v->frame, A._NET_ACTIVE_WINDOW, 2, XCB_CURRENT_TIME, 0, 0, 0);
		xcb_flush(S.x);
	}
}

/* ------------------------------------------------------------------------
 * Keyboard focus follows the X focus (xfwm4 decides).
 */
/* Close open menus: each menu whose parent is a window (its submenus go
 * with it). The list changes with every one closed, so start over. */
static void dismiss_popups(void) {
	for (bool again = true; again;) {
		again = false;
		struct view *v;
		wl_list_for_each(v, &S.views, link)
			if (v->kind == VIEW_POPUP && v->popup->seat && !wlr_xdg_popup_try_from_wlr_surface(v->popup->parent)) {
				wlr_xdg_popup_destroy(v->popup);
				again = true;
				break;
			}
	}
}

static void focus_check(void *data) {
	S.focus_check = NULL;
	xcb_get_input_focus_reply_t *r = xcb_get_input_focus_reply(S.x, xcb_get_input_focus(S.x), NULL);
	x_after_blocking();
	if (!r)
		return;
	struct view *v = view_by_window(r->focus);
	free(r);
	if (v && v->kind != VIEW_TOPLEVEL)
		v = root_view(v);
	if (v == S.focused)
		return;
	if (S.focused && S.focused->toplevel) {
		wlr_xdg_toplevel_set_activated(S.focused->toplevel, false);
		dismiss_popups();
	}
	S.focused = v;
	if (v) {
		wlr_xdg_toplevel_set_activated(v->toplevel, true);
		struct wlr_keyboard *kb = S.keyboard;
		if (kb)
			wlr_seat_keyboard_notify_enter(S.seat, v->xdg->surface, kb->keycodes, kb->num_keycodes, &kb->modifiers);
		else
			wlr_seat_keyboard_notify_enter(S.seat, v->xdg->surface, NULL, 0, NULL);
	} else {
		wlr_seat_keyboard_notify_clear_focus(S.seat);
	}
}

/* The topmost of our windows under the pointer, if any. */
static struct view *view_under_pointer(int16_t *root_x, int16_t *root_y) {
	struct view *found = NULL;
	xcb_window_t w = S.root;
	for (int depth = 0; depth < 8 && w; depth++) {
		xcb_query_pointer_reply_t *p = xcb_query_pointer_reply(S.x, xcb_query_pointer(S.x, w), NULL);
		if (!p)
			break;
		if (depth == 0 && root_x) {
			*root_x = p->root_x;
			*root_y = p->root_y;
		}
		w = p->child;
		free(p);
		struct view *v = view_by_window(w);
		if (v) {
			found = v;
			break;
		}
	}
	x_after_blocking();
	return found;
}

static void raw_button_press(void) {
	bool open = false;
	struct view *v;
	wl_list_for_each(v, &S.views, link)
		open |= v->kind == VIEW_POPUP && v->popup->seat != NULL;
	if (open && !view_under_pointer(NULL, NULL))
		dismiss_popups();
}

static void schedule_focus_check(void) {
	if (!S.focus_check)
		S.focus_check = wl_event_loop_add_idle(S.loop, focus_check, NULL);
}

/* ------------------------------------------------------------------------
 * Input from the backend
 */
static void keyboard_key(struct wl_listener *l, void *data) {
	struct wlr_keyboard_key_event *ev = data;
	if (S.focus_check) {
		/* a focus change not looked at yet: first, so the key goes to the right window */
		wl_event_source_remove(S.focus_check);
		focus_check(NULL);
	}
	wlr_seat_set_keyboard(S.seat, S.keyboard);
	wlr_seat_keyboard_notify_key(S.seat, ev->time_msec, ev->keycode, ev->state);
}
static void keyboard_modifiers(struct wl_listener *l, void *data) {
	wlr_seat_set_keyboard(S.seat, S.keyboard);
	wlr_seat_keyboard_notify_modifiers(S.seat, &S.keyboard->modifiers);
}
static struct wl_listener kb_key = { .notify = keyboard_key }, kb_mods = { .notify = keyboard_modifiers };

/* The keyboard layout of the X server (libxkbcommon-x11, loaded when there),
 * so keys mean the same as in X programs. */
static struct xkb_keymap *x_keymap(struct xkb_context *ctx) {
	void *lib = dlopen("libxkbcommon-x11.so.0", RTLD_NOW | RTLD_LOCAL);
	if (!lib)
		return NULL;
	int (*setup)(xcb_connection_t *, uint16_t, uint16_t, int, uint16_t *, uint16_t *, uint8_t *, uint8_t *) =
		dlsym(lib, "xkb_x11_setup_xkb_extension");
	int32_t (*core_id)(xcb_connection_t *) = dlsym(lib, "xkb_x11_get_core_keyboard_device_id");
	struct xkb_keymap *(*from_dev)(struct xkb_context *, xcb_connection_t *, int32_t, int) =
		dlsym(lib, "xkb_x11_keymap_new_from_device");
	if (!setup || !core_id || !from_dev || !setup(S.x, 1, 0, 0, NULL, NULL, NULL, NULL))
		return NULL;
	int32_t id = core_id(S.x);
	struct xkb_keymap *km = id >= 0 ? from_dev(ctx, S.x, id, 0) : NULL;
	x_after_blocking();
	return km;
}

static void setup_keyboard(struct wlr_keyboard *kb) {
	S.keyboard = kb;
	struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	struct xkb_keymap *km = x_keymap(ctx);
	if (!km) {
		diag("keyboard layout: the X server's could not be read, using the default");
		km = xkb_keymap_new_from_names(ctx, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS);
	}
	wlr_keyboard_set_keymap(kb, km);
	xkb_keymap_unref(km);
	xkb_context_unref(ctx);
	int rate = 20, delay = 500;
	const char *e = getenv("SYLUXIUM_KEY_REPEAT");  /* "rate delay" (from the session's settings) */
	if (e)
		sscanf(e, "%d %d", &rate, &delay);
	wlr_keyboard_set_repeat_info(kb, rate, delay);
	wl_signal_add(&kb->events.key, &kb_key);
	wl_signal_add(&kb->events.modifiers, &kb_mods);
	wlr_seat_set_keyboard(S.seat, kb);
}

static void pointer_motion(struct pointer_dev *p, uint32_t time, double nx, double ny) {
	if (w_drag_motion(time))
		return;  /* a Wayland program's drag: maybe over X11 windows */
	struct view *v = view_by_output_name(p->pointer->output_name);
	if (!v || !v->output)
		return;
	if (v != S.pointer_view) {
		S.pointer_view = v;
		apply_cursor(v);
	}
	double lx = nx * v->output->width / v->scale, ly = ny * v->output->height / v->scale, sx, sy;
	struct wlr_scene_node *n = wlr_scene_node_at(&v->scene->tree.node, lx, ly, &sx, &sy);
	struct wlr_surface *surface = NULL;
	if (n && n->type == WLR_SCENE_NODE_BUFFER) {
		struct wlr_scene_surface *ss = wlr_scene_surface_try_from_buffer(wlr_scene_buffer_from_node(n));
		if (ss)
			surface = ss->surface;
	}
	if (surface) {
		wlr_seat_pointer_notify_enter(S.seat, surface, sx, sy);
		wlr_seat_pointer_notify_motion(S.seat, time, sx, sy);
	} else {
		wlr_seat_pointer_notify_clear_focus(S.seat);
	}
}

static void ptr_motion_abs(struct wl_listener *l, void *data) {
	struct pointer_dev *p = wl_container_of(l, p, motion_abs);
	struct wlr_pointer_motion_absolute_event *ev = data;
	pointer_motion(p, ev->time_msec, ev->x, ev->y);
}
static void ptr_button(struct wl_listener *l, void *data) {
	struct wlr_pointer_button_event *ev = data;
	if (ev->state == WL_POINTER_BUTTON_STATE_RELEASED && w_drag_button_release(ev->time_msec))
		return;  /* dropped on an X11 window: finished when it has the data */
	wlr_seat_pointer_notify_button(S.seat, ev->time_msec, ev->button, ev->state);
}
static void ptr_axis(struct wl_listener *l, void *data) {
	struct wlr_pointer_axis_event *ev = data;
	wlr_seat_pointer_notify_axis(S.seat, ev->time_msec, ev->orientation, ev->delta, ev->delta_discrete,
		ev->source, ev->relative_direction);
}
static void ptr_frame(struct wl_listener *l, void *data) {
	wlr_seat_pointer_notify_frame(S.seat);
}
static void ptr_destroy(struct wl_listener *l, void *data) {
	struct pointer_dev *p = wl_container_of(l, p, destroy);
	wl_list_remove(&p->motion_abs.link);
	wl_list_remove(&p->button.link);
	wl_list_remove(&p->axis.link);
	wl_list_remove(&p->frame.link);
	wl_list_remove(&p->destroy.link);
	wl_list_remove(&p->link);
	free(p);
}

static void server_new_input(struct wl_listener *l, void *data) {
	struct wlr_input_device *dev = data;
	if (dev->type == WLR_INPUT_DEVICE_KEYBOARD) {
		setup_keyboard(wlr_keyboard_from_input_device(dev));
	} else if (dev->type == WLR_INPUT_DEVICE_POINTER) {
		struct pointer_dev *p = calloc(1, sizeof(*p));
		p->pointer = wlr_pointer_from_input_device(dev);
		p->motion_abs.notify = ptr_motion_abs;
		wl_signal_add(&p->pointer->events.motion_absolute, &p->motion_abs);
		p->button.notify = ptr_button;
		wl_signal_add(&p->pointer->events.button, &p->button);
		p->axis.notify = ptr_axis;
		wl_signal_add(&p->pointer->events.axis, &p->axis);
		p->frame.notify = ptr_frame;
		wl_signal_add(&p->pointer->events.frame, &p->frame);
		p->destroy.notify = ptr_destroy;
		wl_signal_add(&dev->events.destroy, &p->destroy);
		wl_list_insert(&S.pointers, &p->link);
	}
	wlr_seat_set_capabilities(S.seat, WL_SEAT_CAPABILITY_POINTER | (S.keyboard ? WL_SEAT_CAPABILITY_KEYBOARD : 0));
}

/* ------------------------------------------------------------------------
 * Clipboard and primary selection, X <-> Wayland.
 */
static int sel_index(xcb_atom_t a) {
	return a == A.CLIPBOARD ? SEL_CLIPBOARD : a == XCB_ATOM_PRIMARY ? SEL_PRIMARY :
		a && a == S.sel_atom[SEL_XDND] ? SEL_XDND : -1;
}

static int writer_ready(int fd, uint32_t mask, void *data) {
	struct writer *w = data;
	while (w->off < w->len) {
		ssize_t n = write(w->fd, w->data + w->off, w->len - w->off);
		if (n < 0 && errno == EAGAIN)
			return 0;
		if (n <= 0)
			break;
		w->off += n;
	}
	wl_event_source_remove(w->src);
	close(w->fd);
	free(w->data);
	free(w);
	return 0;
}

/* Hand data to a Wayland program's pipe without blocking. */
static void write_async(int fd, const void *data, size_t len, bool more) {
	(void)more;
	struct writer *w = calloc(1, sizeof(*w));
	w->fd = fd;
	w->data = malloc(len ? len : 1);
	memcpy(w->data, data, len);
	w->len = len;
	fcntl(fd, F_SETFL, O_NONBLOCK);
	w->src = wl_event_loop_add_fd(S.loop, fd, WL_EVENT_WRITABLE, writer_ready, w);
}

/* INCR transfers keep appending to one writer */
static struct writer *incr_writer;

static void incoming_next(void);

static void incoming_finish(struct incoming *in) {
	wl_list_remove(&in->link);
	if (in->fd >= 0 && !(incr_writer && incr_writer->fd == in->fd))
		close(in->fd);
	free(in);
	incoming_next();
}

static int incoming_timeout(void *data) {
	if (!wl_list_empty(&S.incoming)) {
		struct incoming *in = wl_container_of(S.incoming.next, in, link);
		diag("clipboard: no answer from the X program owning the selection");
		if (incr_writer && incr_writer->fd == in->fd) {
			struct writer *w = incr_writer;
			incr_writer = NULL;
			w->src = wl_event_loop_add_fd(S.loop, w->fd, WL_EVENT_WRITABLE, writer_ready, w);
		}
		incoming_finish(in);
	}
	return 0;
}

static void incoming_next(void) {
	if (wl_list_empty(&S.incoming))
		return;
	struct incoming *in = wl_container_of(S.incoming.next, in, link);
	if (in->started)
		return;
	in->started = true;
	xcb_delete_property(S.x, S.selwin, A.SYLUXIUM_SELECTION);
	xcb_convert_selection(S.x, S.selwin, S.sel_atom[in->sel], in->target, A.SYLUXIUM_SELECTION, XCB_CURRENT_TIME);
	xcb_flush(S.x);
	wl_event_source_timer_update(S.incoming_timer, 3000);
}

static void incoming_add(int sel, xcb_atom_t target, int fd) {
	struct incoming *in = calloc(1, sizeof(*in));
	in->sel = sel; in->target = target; in->fd = fd;
	in->owner_gen = S.owner_gen[sel];
	wl_list_insert(S.incoming.prev, &in->link);
	incoming_next();
}

static xcb_atom_t mime_to_target(const char *mime) {
	if (!strcmp(mime, "text/plain;charset=utf-8") || !strcmp(mime, "UTF8_STRING"))
		return A.UTF8_STRING;
	if (!strcmp(mime, "text/plain") || !strcmp(mime, "STRING"))
		return XCB_ATOM_STRING;
	if (!strcmp(mime, "TEXT"))
		return A.TEXT;
	return intern(mime);
}

static void proxy_send(int sel, const char *mime, int fd) {
	if (sel < 0 || S.sel_we_own[sel]) {
		close(fd);
		return;
	}
	incoming_add(sel, mime_to_target(mime), fd);
}

static void proxy_data_send(struct wlr_data_source *src, const char *mime, int fd) {
	proxy_send(((struct x_proxy_source *)src)->sel, mime, fd);
}
static void proxy_data_destroy(struct wlr_data_source *src) {
	free(src);
}
static const struct wlr_data_source_impl proxy_data_impl = { .send = proxy_data_send, .destroy = proxy_data_destroy };

static void proxy_primary_send(struct wlr_primary_selection_source *src, const char *mime, int fd) {
	proxy_send(((struct x_proxy_primary *)src)->sel, mime, fd);
}
static void proxy_primary_destroy(struct wlr_primary_selection_source *src) {
	free(src);
}
static const struct wlr_primary_selection_source_impl proxy_primary_impl = {
	.send = proxy_primary_send, .destroy = proxy_primary_destroy };

static void add_mime(struct wl_array *arr, const char *m) {
	char **p;
	wl_array_for_each(p, arr)
		if (!strcmp(*p, m))
			return;
	p = wl_array_add(arr, sizeof(*p));
	if (p)
		*p = strdup(m);
}

static void fill_mimes(struct wl_array *mimes, const xcb_atom_t *targets, int n);

/* A new X owner: offer what it has (its TARGETS) to Wayland programs. */
static void offer_x_selection(int sel, xcb_atom_t *targets, int n) {
	struct wl_array *mimes;
	struct x_proxy_source *ds = NULL;
	struct x_proxy_primary *ps = NULL;
	if (sel == SEL_CLIPBOARD) {
		ds = calloc(1, sizeof(*ds));
		ds->sel = sel;
		wlr_data_source_init(&ds->base, &proxy_data_impl);
		mimes = &ds->base.mime_types;
	} else {
		ps = calloc(1, sizeof(*ps));
		ps->sel = sel;
		wlr_primary_selection_source_init(&ps->base, &proxy_primary_impl);
		mimes = &ps->base.mime_types;
	}
	fill_mimes(mimes, targets, n);
	if (ds)
		wlr_seat_set_selection(S.seat, &ds->base, wl_display_next_serial(S.display));
	else
		wlr_seat_set_primary_selection(S.seat, &ps->base, wl_display_next_serial(S.display));
}

/* Wayland MIME types for X selection targets (TARGETS, XdndTypeList). */
static void fill_mimes(struct wl_array *mimes, const xcb_atom_t *targets, int n) {
	bool text = n == 0;
	for (int i = 0; i < n; i++) {
		xcb_atom_t t = targets[i];
		if (t == A.UTF8_STRING || t == XCB_ATOM_STRING || t == A.TEXT || t == A.MIME_UTF8 || t == A.MIME_TEXT) {
			text = true;
			continue;
		}
		if (t == A.TARGETS || t == A.TIMESTAMP || t == A.MULTIPLE)
			continue;
		char *name = atom_name(t);
		if (name && strchr(name, '/'))
			add_mime(mimes, name);
		free(name);
	}
	if (text) {
		add_mime(mimes, "text/plain;charset=utf-8");
		add_mime(mimes, "text/plain");
		add_mime(mimes, "UTF8_STRING");
		add_mime(mimes, "STRING");
		add_mime(mimes, "TEXT");
	}
}

static void handle_selection_notify(xcb_selection_notify_event_t *ev) {
	if (wl_list_empty(&S.incoming))
		return;
	struct incoming *in = wl_container_of(S.incoming.next, in, link);
	if (ev->property == XCB_ATOM_NONE) {
		if (in->fd < 0)
			offer_x_selection(in->sel, NULL, 0);
		incoming_finish(in);
		return;
	}
	xcb_get_property_reply_t *r = xcb_get_property_reply(S.x,
		xcb_get_property(S.x, 1, S.selwin, A.SYLUXIUM_SELECTION, XCB_GET_PROPERTY_TYPE_ANY, 0, 0x1fffffff), NULL);
	x_after_blocking();
	if (!r) {
		incoming_finish(in);
		return;
	}
	if (r->type == A.INCR) {
		/* large data comes in pieces: each new value of the property, until an empty one */
		in->incr = true;
		if (in->fd >= 0) {
			incr_writer = calloc(1, sizeof(*incr_writer));
			incr_writer->fd = in->fd;
		}
		free(r);
		wl_event_source_timer_update(S.incoming_timer, 5000);
		return;
	}
	int len = xcb_get_property_value_length(r);
	if (in->fd < 0) {
		if (in->owner_gen == S.owner_gen[in->sel])
			offer_x_selection(in->sel, xcb_get_property_value(r), r->format == 32 ? len / 4 : 0);
	} else if (in->owner_gen == S.owner_gen[in->sel]) {
		write_async(in->fd, xcb_get_property_value(r), len, false);
		in->fd = -1;  /* the writer closes it */
	}
	free(r);
	wl_event_source_timer_update(S.incoming_timer, 0);
	incoming_finish(in);
}

static void incoming_incr_piece(void) {
	struct incoming *in = wl_container_of(S.incoming.next, in, link);
	xcb_get_property_reply_t *r = xcb_get_property_reply(S.x,
		xcb_get_property(S.x, 1, S.selwin, A.SYLUXIUM_SELECTION, XCB_GET_PROPERTY_TYPE_ANY, 0, 0x1fffffff), NULL);
	x_after_blocking();
	if (!r)
		return;
	int len = xcb_get_property_value_length(r);
	if (len > 0 && incr_writer) {
		incr_writer->data = realloc(incr_writer->data, incr_writer->len + len);
		memcpy(incr_writer->data + incr_writer->len, xcb_get_property_value(r), len);
		incr_writer->len += len;
	}
	free(r);
	wl_event_source_timer_update(S.incoming_timer, 5000);
	if (len == 0) {
		if (incr_writer) {
			struct writer *w = incr_writer;
			incr_writer = NULL;
			fcntl(w->fd, F_SETFL, O_NONBLOCK);
			w->src = wl_event_loop_add_fd(S.loop, w->fd, WL_EVENT_WRITABLE, writer_ready, w);
			in->fd = -1;
		}
		wl_event_source_timer_update(S.incoming_timer, 0);
		incoming_finish(in);
	}
}

/* The X selection changed hands. */
static void x_owner_changed(int sel, xcb_window_t owner) {
	if (owner == S.selwin)
		return;
	S.owner_gen[sel]++;
	S.sel_we_own[sel] = false;
	if (owner == XCB_NONE) {
		/* the X owner went away: drop our copy of its offer, keep Wayland programs' own */
		if (sel == SEL_CLIPBOARD && S.seat->selection_source &&
		    S.seat->selection_source->impl == &proxy_data_impl)
			wlr_seat_set_selection(S.seat, NULL, wl_display_next_serial(S.display));
		if (sel == SEL_PRIMARY && S.seat->primary_selection_source &&
		    S.seat->primary_selection_source->impl == &proxy_primary_impl)
			wlr_seat_set_primary_selection(S.seat, NULL, wl_display_next_serial(S.display));
		return;
	}
	incoming_add(sel, A.TARGETS, -1);
}

/* A Wayland program set the selection: X programs get it from us. */
static void wayland_selection_changed(int sel) {
	bool ours, empty;
	if (sel == SEL_CLIPBOARD) {
		empty = !S.seat->selection_source;
		ours = !empty && S.seat->selection_source->impl == &proxy_data_impl;
	} else {
		empty = !S.seat->primary_selection_source;
		ours = !empty && S.seat->primary_selection_source->impl == &proxy_primary_impl;
	}
	if (ours)
		return;
	if (empty) {
		if (S.sel_we_own[sel]) {
			xcb_set_selection_owner(S.x, XCB_NONE, S.sel_atom[sel], XCB_CURRENT_TIME);
			S.sel_we_own[sel] = false;
		}
	} else {
		xcb_set_selection_owner(S.x, S.selwin, S.sel_atom[sel], XCB_CURRENT_TIME);
		S.sel_we_own[sel] = true;
	}
	xcb_flush(S.x);
}

static void seat_set_selection(struct wl_listener *l, void *data) {
	wayland_selection_changed(SEL_CLIPBOARD);
}
static void seat_set_primary(struct wl_listener *l, void *data) {
	wayland_selection_changed(SEL_PRIMARY);
}
static void seat_request_set_selection(struct wl_listener *l, void *data) {
	struct wlr_seat_request_set_selection_event *ev = data;
	wlr_seat_set_selection(S.seat, ev->source, ev->serial);
}
static void seat_request_set_primary(struct wl_listener *l, void *data) {
	struct wlr_seat_request_set_primary_selection_event *ev = data;
	wlr_seat_set_primary_selection(S.seat, ev->source, ev->serial);
}

static struct wl_array *wayland_mimes(int sel) {
	if (sel == SEL_XDND)
		return dnd_wayland_source() ? &dnd_wayland_source()->mime_types : NULL;
	if (sel == SEL_CLIPBOARD)
		return S.seat->selection_source ? &S.seat->selection_source->mime_types : NULL;
	return S.seat->primary_selection_source ? &S.seat->primary_selection_source->mime_types : NULL;
}

static void selection_reply(xcb_selection_request_event_t *req, xcb_atom_t property) {
	xcb_selection_notify_event_t ev = {0};
	ev.response_type = XCB_SELECTION_NOTIFY;
	ev.time = req->time;
	ev.requestor = req->requestor;
	ev.selection = req->selection;
	ev.target = req->target;
	ev.property = property;
	xcb_send_event(S.x, 0, req->requestor, XCB_EVENT_MASK_NO_EVENT, (const char *)&ev);
	xcb_flush(S.x);
}

static void outgoing_free(struct outgoing *o) {
	if (o->src) wl_event_source_remove(o->src);
	if (o->timer) wl_event_source_remove(o->timer);
	if (o->fd >= 0) close(o->fd);
	wl_list_remove(&o->link);
	free(o->data);
	free(o);
}

static size_t max_chunk(void) {
	size_t m = xcb_get_maximum_request_length(S.x) * 4;
	m = m > 65536 ? m - 4096 : 60000;
	return m > (1 << 20) ? (1 << 20) : m;
}

static void outgoing_deliver(struct outgoing *o) {
	xcb_selection_request_event_t req = { .time = o->time, .requestor = o->requestor,
		.selection = o->selection, .target = o->target, .property = o->property };
	if (o->len <= max_chunk()) {
		xcb_change_property(S.x, XCB_PROP_MODE_REPLACE, o->requestor, o->property, o->target, 8, o->len, o->data);
		selection_reply(&req, o->property);
		outgoing_free(o);
		return;
	}
	/* INCR: the requestor deletes the property for each next piece */
	uint32_t mask = XCB_EVENT_MASK_PROPERTY_CHANGE;
	xcb_change_window_attributes(S.x, o->requestor, XCB_CW_EVENT_MASK, &mask);
	uint32_t size = o->len;
	xcb_change_property(S.x, XCB_PROP_MODE_REPLACE, o->requestor, o->property, A.INCR, 32, 1, &size);
	o->incr_wait = true;
	selection_reply(&req, o->property);
}

static void outgoing_incr_next(struct outgoing *o) {
	size_t n = o->len - o->sent;
	if (n > max_chunk())
		n = max_chunk();
	xcb_change_property(S.x, XCB_PROP_MODE_REPLACE, o->requestor, o->property, o->target, 8, n, o->data + o->sent);
	xcb_flush(S.x);
	if (n == 0) {
		uint32_t mask = 0;
		xcb_change_window_attributes(S.x, o->requestor, XCB_CW_EVENT_MASK, &mask);
		outgoing_free(o);
		return;
	}
	o->sent += n;
}

static int outgoing_readable(int fd, uint32_t mask, void *data) {
	struct outgoing *o = data;
	if (o->cap - o->len < 65536) {
		o->cap = o->cap * 2 + 65536;
		o->data = realloc(o->data, o->cap);
	}
	ssize_t n = read(fd, o->data + o->len, o->cap - o->len);
	if (n > 0) {
		o->len += n;
		return 0;
	}
	if (n < 0 && (errno == EAGAIN || errno == EINTR))
		return 0;
	wl_event_source_remove(o->src);
	o->src = NULL;
	close(o->fd);
	o->fd = -1;
	outgoing_deliver(o);
	return 0;
}

static int outgoing_timeout(void *data) {
	struct outgoing *o = data;
	o->timer = NULL;
	diag("clipboard: a Wayland program did not hand over its selection in time");
	if (!o->incr_wait) {
		xcb_selection_request_event_t req = { .time = o->time, .requestor = o->requestor,
			.selection = o->selection, .target = o->target, .property = o->property };
		selection_reply(&req, XCB_ATOM_NONE);
	}
	outgoing_free(o);
	return 0;
}

static const char *pick_mime(struct wl_array *mimes, xcb_atom_t target, char **name_out) {
	char **p;
	if (target == A.UTF8_STRING || target == XCB_ATOM_STRING || target == A.TEXT) {
		static const char *pref[] = { "text/plain;charset=utf-8", "UTF8_STRING", "text/plain", "TEXT", "STRING" };
		for (size_t i = 0; i < 5; i++)
			wl_array_for_each(p, mimes)
				if (!strcmp(*p, pref[i]))
					return *p;
		return NULL;
	}
	char *name = atom_name(target);
	*name_out = name;
	if (!name)
		return NULL;
	wl_array_for_each(p, mimes)
		if (!strcmp(*p, name))
			return *p;
	return NULL;
}

static void handle_selection_request(xcb_selection_request_event_t *req) {
	int sel = sel_index(req->selection);
	struct wl_array *mimes = sel >= 0 ? wayland_mimes(sel) : NULL;
	xcb_atom_t property = req->property ? req->property : req->target;
	if (sel < 0 || (!S.sel_we_own[sel] && sel != SEL_XDND) || !mimes) {
		selection_reply(req, XCB_ATOM_NONE);
		return;
	}
	if (req->target == A.TARGETS) {
		xcb_atom_t list[64];
		int n = 0;
		list[n++] = A.TARGETS;
		list[n++] = A.TIMESTAMP;
		char **p;
		wl_array_for_each(p, mimes) {
			if (n > 58)
				break;
			if (!strcmp(*p, "text/plain;charset=utf-8") || !strcmp(*p, "text/plain") ||
			    !strcmp(*p, "UTF8_STRING") || !strcmp(*p, "STRING") || !strcmp(*p, "TEXT")) {
				bool have = false;
				for (int i = 0; i < n; i++)
					have |= list[i] == A.UTF8_STRING;
				if (!have) {
					list[n++] = A.UTF8_STRING;
					list[n++] = XCB_ATOM_STRING;
					list[n++] = A.TEXT;
				}
				if (!strchr(*p, '/'))
					continue;
			}
			xcb_atom_t a = intern(*p);
			if (a)
				list[n++] = a;
		}
		x_after_blocking();
		xcb_change_property(S.x, XCB_PROP_MODE_REPLACE, req->requestor, property, XCB_ATOM_ATOM, 32, n, list);
		selection_reply(req, property);
		return;
	}
	if (req->target == A.TIMESTAMP) {
		uint32_t t = S.last_time;
		xcb_change_property(S.x, XCB_PROP_MODE_REPLACE, req->requestor, property, XCB_ATOM_INTEGER, 32, 1, &t);
		selection_reply(req, property);
		return;
	}
	char *name = NULL;
	const char *mime = pick_mime(mimes, req->target, &name);
	x_after_blocking();
	free(name);
	int p[2];
	if (!mime || pipe2(p, O_CLOEXEC) < 0) {
		selection_reply(req, XCB_ATOM_NONE);
		return;
	}
	struct outgoing *o = calloc(1, sizeof(*o));
	o->requestor = req->requestor;
	o->property = property;
	o->target = req->target;
	o->selection = req->selection;
	o->time = req->time;
	o->fd = p[0];
	fcntl(p[0], F_SETFL, O_NONBLOCK);
	wl_list_insert(&S.outgoing, &o->link);
	o->src = wl_event_loop_add_fd(S.loop, p[0], WL_EVENT_READABLE, outgoing_readable, o);
	o->timer = wl_event_loop_add_timer(S.loop, outgoing_timeout, o);
	wl_event_source_timer_update(o->timer, 5000);
	if (sel == SEL_XDND)
		wlr_data_source_send(dnd_wayland_source(), mime, p[1]);
	else if (sel == SEL_CLIPBOARD)
		wlr_data_source_send(S.seat->selection_source, mime, p[1]);
	else
		wlr_primary_selection_source_send(S.seat->primary_selection_source, mime, p[1]);
}

/* ------------------------------------------------------------------------
 * Drag and drop between Wayland programs and X11 programs (XDND, version 5).
 *
 * X11 -> Wayland: our frame windows are XDND targets. The X program's drag
 * becomes a Wayland drag (wlr_drag) of an X-backed data source, started on
 * the Wayland program under the pointer and moved by XdndPosition; the drop
 * fetches the data from XdndSelection.
 *
 * Wayland -> X11: while a Wayland program drags and the pointer is outside
 * our windows, we are the XDND source: the window under the pointer gets
 * XdndEnter/Position, the drop is held back from wlroots until the X
 * program has fetched the data (XdndFinished), and the drag icon is the
 * pointer's picture over the whole screen.
 */
struct x_dnd_source {
	struct wlr_data_source base;
};

static struct {
	/* X11 -> Wayland */
	xcb_window_t in_source;          /* the X program's XDND source window */
	struct x_dnd_source *in_src;
	struct wlr_drag *in_drag;
	struct wl_listener in_drag_destroy;
	bool in_dropped, in_finished;
	xcb_timestamp_t in_time;
	struct wl_event_source *in_timer;

	/* Wayland -> X11 */
	struct wlr_drag *w_drag;
	struct wl_listener w_drag_destroy, w_icon_commit;
	struct view *w_origin;           /* the window the drag started in (its grab) */
	xcb_window_t w_target;           /* the X window the pointer is over, if any */
	bool w_target_accepts, w_dropping, w_grabbed;
	xcb_atom_t w_target_action;
	int w_icon_x, w_icon_y;          /* the icon's offset from the pointer */
	xcb_cursor_t w_icon_cursor;
	struct wl_event_source *w_timer;
	uint32_t w_serial;
	int16_t w_root_x, w_root_y;
	xcb_input_device_id_t pointer_id; /* the master pointer (XInput 2) */
} D;

static xcb_atom_t XdndEnter, XdndPosition, XdndStatus, XdndLeave, XdndDrop, XdndFinished,
	XdndSelection, XdndTypeList, XdndActionCopy, XdndActionMove, XdndActionLink, XdndActionAsk;

static void dnd_atoms(void) {
#define DATOM(n) n = intern(#n)
	DATOM(XdndAware); DATOM(XdndEnter); DATOM(XdndPosition); DATOM(XdndStatus); DATOM(XdndLeave);
	DATOM(XdndDrop); DATOM(XdndFinished); DATOM(XdndSelection); DATOM(XdndTypeList);
	DATOM(XdndActionCopy); DATOM(XdndActionMove); DATOM(XdndActionLink); DATOM(XdndActionAsk);
#undef DATOM
	S.sel_atom[SEL_XDND] = XdndSelection;
	/* the master pointer's XInput 2 id (for the grab with the drag icon) */
	D.pointer_id = 2;
	xcb_input_xi_query_device_reply_t *r = xcb_input_xi_query_device_reply(S.x,
		xcb_input_xi_query_device(S.x, XCB_INPUT_DEVICE_ALL_MASTER), NULL);
	if (r) {
		xcb_input_xi_device_info_iterator_t it = xcb_input_xi_query_device_infos_iterator(r);
		for (; it.rem; xcb_input_xi_device_info_next(&it))
			if (it.data->type == XCB_INPUT_DEVICE_TYPE_MASTER_POINTER) {
				D.pointer_id = it.data->deviceid;
				break;
			}
		free(r);
	}
}

static uint32_t action_from_atom(xcb_atom_t a) {
	if (a == XdndActionMove)
		return WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE;
	if (a == XdndActionAsk)
		return WL_DATA_DEVICE_MANAGER_DND_ACTION_ASK;
	if (a == XdndActionLink)
		return WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY;  /* no links on Wayland */
	return WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY;  /* copy, private */
}

static xcb_atom_t atom_from_action(uint32_t a) {
	if (a & WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE)
		return XdndActionMove;
	if (a & WL_DATA_DEVICE_MANAGER_DND_ACTION_ASK)
		return XdndActionAsk;
	if (a & WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY)
		return XdndActionCopy;
	return XCB_ATOM_NONE;
}

static void xdnd_send(xcb_window_t to, xcb_atom_t type, uint32_t d0, uint32_t d1, uint32_t d2, uint32_t d3,
		uint32_t d4) {
	xcb_client_message_event_t ev = {0};
	ev.response_type = XCB_CLIENT_MESSAGE;
	ev.format = 32;
	ev.window = to;
	ev.type = type;
	ev.data.data32[0] = d0; ev.data.data32[1] = d1; ev.data.data32[2] = d2;
	ev.data.data32[3] = d3; ev.data.data32[4] = d4;
	xcb_send_event(S.x, 0, to, XCB_EVENT_MASK_NO_EVENT, (const char *)&ev);
	xcb_flush(S.x);
}

/* -- X11 -> Wayland -- */

static void in_status(void) {
	if (!D.in_source || !D.in_src)
		return;
	struct view *v = D.in_drag && D.in_drag->focus ? root_view(view_from_surface(D.in_drag->focus)) : NULL;
	bool ok = D.in_src->base.accepted && D.in_src->base.current_dnd_action;
	xdnd_send(D.in_source, XdndStatus, v && v->frame ? v->frame : S.selwin, (ok ? 1 : 0) | 2, 0, 0,
		ok ? atom_from_action(D.in_src->base.current_dnd_action) : XCB_ATOM_NONE);
}

static void in_reset(void) {
	if (D.in_timer)
		wl_event_source_remove(D.in_timer);
	D.in_timer = NULL;
	D.in_source = 0;
	D.in_dropped = D.in_finished = false;
}

static void in_finish(bool accepted) {
	if (D.in_source && !D.in_finished) {
		D.in_finished = true;
		xdnd_send(D.in_source, XdndFinished, S.selwin, accepted ? 1 : 0,
			accepted && D.in_src ? atom_from_action(D.in_src->base.current_dnd_action) : XCB_ATOM_NONE, 0, 0);
	}
}

static void xsrc_send(struct wlr_data_source *src, const char *mime, int fd) {
	incoming_add(SEL_XDND, mime_to_target(mime), fd);
}
static void xsrc_accept(struct wlr_data_source *src, uint32_t serial, const char *mime) {
	in_status();  /* the X program learns at once whether a drop would be taken */
}
static void xsrc_action(struct wlr_data_source *src, enum wl_data_device_manager_dnd_action a) {
	in_status();
}
static void xsrc_drop(struct wlr_data_source *src) {
}
static void xsrc_finish(struct wlr_data_source *src) {
	in_finish(true);
	diag("drag and drop: an X11 program's drop taken by a Wayland program");
}
static void xsrc_destroy(struct wlr_data_source *src) {
	struct x_dnd_source *x = wl_container_of(src, x, base);
	if (D.in_src == x) {
		if (D.in_dropped)
			in_finish(false);
		D.in_src = NULL;
		in_reset();
	}
	free(x);
}
static const struct wlr_data_source_impl x_dnd_source_impl = {
	.send = xsrc_send, .accept = xsrc_accept, .destroy = xsrc_destroy,
	.dnd_drop = xsrc_drop, .dnd_finish = xsrc_finish, .dnd_action = xsrc_action,
};

static void in_drag_destroyed(struct wl_listener *l, void *data) {
	wl_list_remove(&D.in_drag_destroy.link);
	D.in_drag = NULL;
}

static int in_timeout(void *data) {
	D.in_timer = NULL;
	if (D.in_src) {
		diag("drag and drop: the Wayland program did not finish the drop in time");
		in_finish(false);
		wlr_data_source_destroy(&D.in_src->base);
	}
	return 0;
}

static void in_cancel(void) {
	if (D.in_src)
		wlr_data_source_destroy(&D.in_src->base);  /* ends the wlroots drag too */
	if (S.seat->pointer_state.button_count)
		wlr_seat_pointer_notify_button(S.seat, now_msec(), BTN_LEFT, WL_POINTER_BUTTON_STATE_RELEASED);
	in_reset();
}

static void in_enter(xcb_client_message_event_t *ev) {
	in_cancel();
	D.in_source = ev->data.data32[0];
	xcb_atom_t types[64];
	int n = 0;
	if (ev->data.data32[1] & 1) {
		xcb_get_property_reply_t *r = xcb_get_property_reply(S.x,
			xcb_get_property(S.x, 0, D.in_source, XdndTypeList, XCB_ATOM_ATOM, 0, 64), NULL);
		x_after_blocking();
		if (r) {
			n = xcb_get_property_value_length(r) / 4;
			if (n > 64)
				n = 64;
			memcpy(types, xcb_get_property_value(r), n * 4);
			free(r);
		}
	} else {
		for (int i = 2; i < 5; i++)
			if (ev->data.data32[i])
				types[n++] = ev->data.data32[i];
	}
	struct x_dnd_source *x = calloc(1, sizeof(*x));
	wlr_data_source_init(&x->base, &x_dnd_source_impl);
	fill_mimes(&x->base.mime_types, types, n);
	x->base.actions = WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY | WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE |
		WL_DATA_DEVICE_MANAGER_DND_ACTION_ASK;
	D.in_src = x;
}

static void in_position(struct view *v, xcb_client_message_event_t *ev) {
	if (!D.in_src || ev->data.data32[0] != D.in_source)
		return;
	int rx = ev->data.data32[2] >> 16, ry = ev->data.data32[2] & 0xffff;
	D.in_time = ev->data.data32[3];
	D.in_src->base.compositor_action = action_from_atom(ev->data.data32[4]);
	int ox, oy;
	struct wlr_surface *surface = NULL;
	double sx = 0, sy = 0;
	if (v && view_root_origin(v, &ox, &oy)) {
		struct wlr_scene_node *n = wlr_scene_node_at(&v->scene->tree.node, (rx - ox) / v->scale,
			(ry - oy) / v->scale, &sx, &sy);
		struct wlr_scene_surface *ss = n && n->type == WLR_SCENE_NODE_BUFFER ?
			wlr_scene_surface_try_from_buffer(wlr_scene_buffer_from_node(n)) : NULL;
		surface = ss ? ss->surface : NULL;
	}
	if (surface && !D.in_drag && !S.seat->drag) {
		struct wlr_seat_client *sc = wlr_seat_client_for_wl_client(S.seat, wl_resource_get_client(surface->resource));
		if (sc) {
			D.in_drag = wlr_drag_create(sc, &D.in_src->base, NULL);
			if (D.in_drag) {
				D.in_drag_destroy.notify = in_drag_destroyed;
				wl_signal_add(&D.in_drag->events.destroy, &D.in_drag_destroy);
				wlr_seat_start_pointer_drag(S.seat, D.in_drag, wl_display_next_serial(S.display));
				/* the drop is the release of the button the drag "started" with */
				wlr_seat_pointer_notify_button(S.seat, now_msec(), BTN_LEFT, WL_POINTER_BUTTON_STATE_PRESSED);
			}
		}
	}
	if (D.in_drag) {
		if (surface) {
			wlr_seat_pointer_notify_enter(S.seat, surface, sx, sy);
			wlr_seat_pointer_notify_motion(S.seat, now_msec(), sx, sy);
		} else {
			wlr_seat_pointer_notify_clear_focus(S.seat);
		}
	}
	in_status();
}

static void in_drop(xcb_client_message_event_t *ev) {
	if (!D.in_src || ev->data.data32[0] != D.in_source)
		return;
	D.in_time = ev->data.data32[2];
	bool ok = D.in_drag && D.in_drag->focus && D.in_src->base.accepted && D.in_src->base.current_dnd_action;
	if (!ok) {
		in_finish(false);
		in_cancel();
		return;
	}
	D.in_dropped = true;
	wlr_seat_pointer_notify_button(S.seat, now_msec(), BTN_LEFT, WL_POINTER_BUTTON_STATE_RELEASED);
	if (!D.in_timer)
		D.in_timer = wl_event_loop_add_timer(S.loop, in_timeout, NULL);
	wl_event_source_timer_update(D.in_timer, 10000);
}

/* -- Wayland -> X11 -- */

static void w_leave_target(void) {
	if (D.w_target)
		xdnd_send(D.w_target, XdndLeave, S.selwin, 0, 0, 0, 0);
	D.w_target = 0;
	D.w_target_accepts = false;
}

static bool xdnd_aware(xcb_window_t w) {
	xcb_get_property_reply_t *r = xcb_get_property_reply(S.x,
		xcb_get_property(S.x, 0, w, XdndAware, XCB_ATOM_ATOM, 0, 1), NULL);
	bool aware = r && xcb_get_property_value_length(r) >= 4 && *(uint32_t *)xcb_get_property_value(r) >= 3;
	free(r);
	return aware;
}

/* What is under the pointer: one of our windows, an XDND-aware X window, or nothing. */
static void drop_target_at(struct view **view, xcb_window_t *xwin, int16_t *rx, int16_t *ry) {
	*view = NULL;
	*xwin = 0;
	xcb_window_t w = S.root;
	for (int depth = 0; depth < 10 && w; depth++) {
		xcb_query_pointer_reply_t *p = xcb_query_pointer_reply(S.x, xcb_query_pointer(S.x, w), NULL);
		if (!p)
			break;
		if (depth == 0) {
			*rx = p->root_x;
			*ry = p->root_y;
		}
		w = p->child;
		free(p);
		if (!w)
			break;
		if ((*view = view_by_window(w)))
			break;
		if (xdnd_aware(w)) {
			*xwin = w;
			break;
		}
	}
	x_after_blocking();
}

static xcb_atom_t w_action(void) {
	struct wlr_data_source *src = D.w_drag ? D.w_drag->source : NULL;
	uint32_t acts = src ? src->actions : WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY;
	uint32_t mods = S.keyboard ? wlr_keyboard_get_modifiers(S.keyboard) : 0;
	if ((mods & WLR_MODIFIER_SHIFT) && (acts & WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE))
		return XdndActionMove;
	if (acts & WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY)
		return XdndActionCopy;
	if (acts & WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE)
		return XdndActionMove;
	return XdndActionAsk;
}

static void w_enter_target(xcb_window_t w) {
	struct wlr_data_source *src = D.w_drag->source;
	xcb_atom_t types[64];
	int n = 0;
	char **m;
	wl_array_for_each(m, &src->mime_types) {
		if (n >= 60)
			break;
		if (!strcmp(*m, "text/plain;charset=utf-8") || !strcmp(*m, "text/plain")) {
			types[n++] = A.UTF8_STRING;
			types[n++] = XCB_ATOM_STRING;
		}
		xcb_atom_t a = intern(*m);
		if (a)
			types[n++] = a;
	}
	x_after_blocking();
	set_prop(S.selwin, XdndTypeList, XCB_ATOM_ATOM, 32, n, types);
	xdnd_send(w, XdndEnter, S.selwin, (5u << 24) | (n > 3 ? 1 : 0), n > 0 ? types[0] : 0, n > 1 ? types[1] : 0,
		n > 2 ? types[2] : 0);
	D.w_target = w;
	D.w_target_accepts = false;
}

/* The pointer moved during a Wayland program's drag (lx, ly: in the window
 * the events come from, the one that has the grab). */
static bool w_drag_motion(uint32_t time) {
	if (!D.w_drag || D.w_dropping)
		return false;
	struct view *tv;
	xcb_window_t xw;
	drop_target_at(&tv, &xw, &D.w_root_x, &D.w_root_y);
	if (tv) {
		w_leave_target();
		int ox, oy;
		if (!view_root_origin(tv, &ox, &oy))
			return true;
		double sx, sy;
		struct wlr_scene_node *n = wlr_scene_node_at(&tv->scene->tree.node, (D.w_root_x - ox) / tv->scale,
			(D.w_root_y - oy) / tv->scale, &sx, &sy);
		struct wlr_scene_surface *ss = n && n->type == WLR_SCENE_NODE_BUFFER ?
			wlr_scene_surface_try_from_buffer(wlr_scene_buffer_from_node(n)) : NULL;
		if (ss) {
			wlr_seat_pointer_notify_enter(S.seat, ss->surface, sx, sy);
			wlr_seat_pointer_notify_motion(S.seat, time, sx, sy);
		} else {
			wlr_seat_pointer_notify_clear_focus(S.seat);
		}
		return true;
	}
	wlr_seat_pointer_notify_clear_focus(S.seat);
	if (xw != D.w_target) {
		w_leave_target();
		if (xw)
			w_enter_target(xw);
	}
	if (D.w_target)
		xdnd_send(D.w_target, XdndPosition, S.selwin, 0, ((uint32_t)D.w_root_x << 16) | (uint16_t)D.w_root_y,
			XCB_CURRENT_TIME, w_action());
	return true;
}

static void w_end(bool finished_ok, xcb_atom_t action) {
	struct wlr_drag *drag = D.w_drag;
	struct wlr_data_source *src = drag ? drag->source : NULL;
	if (D.w_timer)
		wl_event_source_remove(D.w_timer);
	D.w_timer = NULL;
	if (src && finished_ok) {
		wlr_data_source_dnd_action(src, action_from_atom(action));
		wlr_data_source_dnd_drop(src);
		wlr_data_source_dnd_finish(src);
		diag("drag and drop: a Wayland program's drop taken by an X11 program (%s)",
			action == XdndActionMove ? "move" : "copy");
	}
	D.w_dropping = false;
	D.w_target = 0;
	if (drag) {
		if (finished_ok) {
			/* ends the drag without cancelling the (finished) source */
			wlr_seat_pointer_end_grab(S.seat);
			if (S.seat->pointer_state.button_count)
				wlr_seat_pointer_notify_button(S.seat, now_msec(), BTN_LEFT, WL_POINTER_BUTTON_STATE_RELEASED);
		} else if (src) {
			wlr_data_source_destroy(src);  /* cancelled: the program gets it back */
		}
	}
}

static int w_timeout(void *data) {
	D.w_timer = NULL;
	diag("drag and drop: the X11 program did not finish the drop in time");
	w_end(false, 0);
	return 0;
}

/* The button was released during a Wayland program's drag. True if we
 * took it over (dropping on an X11 window). */
static bool w_drag_button_release(uint32_t time) {
	if (!D.w_drag || !D.w_target || D.w_dropping)
		return false;
	if (!D.w_target_accepts) {
		w_leave_target();
		return false;  /* wlroots cancels the drag (no Wayland target either) */
	}
	D.w_dropping = true;
	xcb_set_selection_owner(S.x, S.selwin, XdndSelection, XCB_CURRENT_TIME);
	xdnd_send(D.w_target, XdndDrop, S.selwin, 0, XCB_CURRENT_TIME, 0, 0);
	if (!D.w_timer)
		D.w_timer = wl_event_loop_add_timer(S.loop, w_timeout, NULL);
	wl_event_source_timer_update(D.w_timer, 5000);
	return true;
}

static void w_status(xcb_client_message_event_t *ev) {
	if (!D.w_drag || ev->data.data32[0] != D.w_target)
		return;
	D.w_target_accepts = ev->data.data32[1] & 1;
	D.w_target_action = ev->data.data32[4];
	struct wlr_data_source *src = D.w_drag->source;
	if (src && !D.w_dropping) {
		const char *mime = NULL;
		if (D.w_target_accepts && src->mime_types.size)
			mime = *(char **)src->mime_types.data;
		wlr_data_source_accept(src, D.w_serial, mime);
		wlr_data_source_dnd_action(src, D.w_target_accepts ? action_from_atom(D.w_target_action) : 0);
	}
}

static void w_finished(xcb_client_message_event_t *ev) {
	if (!D.w_drag || !D.w_dropping)
		return;
	bool ok = ev->data.data32[1] & 1;
	w_end(ok, ev->data.data32[2] ? ev->data.data32[2] : D.w_target_action);
}

/* The drag icon as the pointer's picture, over every window. */
static void w_update_icon(void) {
	struct wlr_drag_icon *icon = D.w_drag ? D.w_drag->icon : NULL;
	xcb_cursor_t c = icon ? surface_cursor(icon->surface, -D.w_icon_x, -D.w_icon_y) : XCB_NONE;
	if (c == S.cursor_hidden || c == XCB_NONE)
		c = S.cursor_current ? S.cursor_current : S.cursor_default;
	if (D.w_origin && D.w_origin->child && S.bx) {
		struct {
			xcb_input_event_mask_t head;
			uint32_t mask;
		} m = { { D.pointer_id, 1 }, XCB_INPUT_XI_EVENT_MASK_MOTION | XCB_INPUT_XI_EVENT_MASK_BUTTON_PRESS |
			XCB_INPUT_XI_EVENT_MASK_BUTTON_RELEASE };
		/* (the backend's own connection holds the button grab: it changes it) */
		xcb_input_xi_grab_device(S.bx->xcb, D.w_origin->child, XCB_CURRENT_TIME, c, D.pointer_id,
			XCB_INPUT_GRAB_MODE_22_ASYNC, XCB_INPUT_GRAB_MODE_22_ASYNC, 1, 1, &m.mask);
		xcb_flush(S.bx->xcb);
		D.w_grabbed = true;
	}
	if (D.w_icon_cursor && D.w_icon_cursor != c && D.w_icon_cursor != S.cursor_default &&
	    D.w_icon_cursor != S.cursor_hidden && D.w_icon_cursor != S.cursor_current)
		xcb_free_cursor(S.x, D.w_icon_cursor);
	D.w_icon_cursor = c;
}

static void w_icon_commit(struct wl_listener *l, void *data) {
	struct wlr_surface *s = data;
	D.w_icon_x += s->current.dx;
	D.w_icon_y += s->current.dy;
	w_update_icon();
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	wlr_surface_send_frame_done(s, &now);
}

static void w_drag_destroyed(struct wl_listener *l, void *data) {
	wl_list_remove(&D.w_drag_destroy.link);
	wl_list_remove(&D.w_icon_commit.link);
	wl_list_init(&D.w_icon_commit.link);
	if (D.w_target && !D.w_dropping)
		w_leave_target();
	if (D.w_grabbed && S.bx) {
		xcb_input_xi_ungrab_device(S.bx->xcb, XCB_CURRENT_TIME, D.pointer_id);
		xcb_flush(S.bx->xcb);
	}
	D.w_grabbed = false;
	D.w_drag = NULL;
	D.w_origin = NULL;
	D.w_dropping = false;
	apply_cursor(S.pointer_view);
}

static void seat_request_start_drag(struct wl_listener *l, void *data) {
	struct wlr_seat_request_start_drag_event *ev = data;
	if (!wlr_seat_validate_pointer_grab_serial(S.seat, ev->origin, ev->serial)) {
		if (ev->drag->source)
			wlr_data_source_destroy(ev->drag->source);
		return;
	}
	wlr_seat_start_pointer_drag(S.seat, ev->drag, ev->serial);
}

static void seat_start_drag(struct wl_listener *l, void *data) {
	struct wlr_drag *drag = data;
	if (drag == D.in_drag)
		return;  /* ours, from an X11 program */
	D.w_drag = drag;
	D.w_serial = S.seat->pointer_state.grab_serial;
	D.w_origin = S.pointer_view;
	D.w_target = 0;
	D.w_icon_x = D.w_icon_y = 0;
	D.w_drag_destroy.notify = w_drag_destroyed;
	wl_signal_add(&drag->events.destroy, &D.w_drag_destroy);
	wl_list_init(&D.w_icon_commit.link);
	if (drag->icon) {
		D.w_icon_commit.notify = w_icon_commit;
		wl_signal_add(&drag->icon->surface->events.commit, &D.w_icon_commit);
		D.w_icon_x = drag->icon->surface->current.dx;
		D.w_icon_y = drag->icon->surface->current.dy;
	}
	w_update_icon();
}

static struct wl_listener l_req_drag = { .notify = seat_request_start_drag },
	l_start_drag = { .notify = seat_start_drag };

/* XDND messages to our windows (frames) and to the selection window. */
static bool dnd_client_message(xcb_client_message_event_t *ev) {
	struct view *v = view_by_window(ev->window);
	if (ev->type == XdndEnter)
		in_enter(ev);
	else if (ev->type == XdndPosition)
		in_position(v && v->kind == VIEW_TOPLEVEL ? v : NULL, ev);
	else if (ev->type == XdndLeave) {
		if (ev->data.data32[0] == D.in_source && !D.in_dropped)
			in_cancel();
	} else if (ev->type == XdndDrop)
		in_drop(ev);
	else if (ev->type == XdndStatus)
		w_status(ev);
	else if (ev->type == XdndFinished)
		w_finished(ev);
	else
		return false;
	return true;
}

static struct wlr_data_source *dnd_wayland_source(void) {
	return D.w_drag ? D.w_drag->source : NULL;
}

/* ------------------------------------------------------------------------
 * Our X connection's events
 */
static void x_event(xcb_generic_event_t *e) {
	uint8_t type = e->response_type & 0x7f;
	switch (type) {
	case XCB_CONFIGURE_NOTIFY: {
		xcb_configure_notify_event_t *ev = (void *)e;
		struct view *v = view_by_window(ev->window);
		if (!v || v->frame != ev->window || v->kind != VIEW_TOPLEVEL || !v->x_mapped)
			break;
		if (ev->width != v->w || ev->height != v->h) {
			/* the user (or xfwm4) resized the window */
			v->size_serial = wlr_xdg_toplevel_set_size(v->toplevel, units(v, ev->width), units(v, ev->height));
			view_render_resize(v, ev->width, ev->height);
		}
		break;
	}
	case XCB_FOCUS_IN:
	case XCB_FOCUS_OUT: {
		xcb_focus_in_event_t *ev = (void *)e;
		struct view *v = view_by_window(ev->event);
		(void)v;
		schedule_focus_check();
		break;
	}
	case XCB_PROPERTY_NOTIFY: {
		xcb_property_notify_event_t *ev = (void *)e;
		S.last_time = ev->time;
		if (ev->window == S.selwin && ev->atom == A.SYLUXIUM_SELECTION && ev->state == XCB_PROPERTY_NEW_VALUE &&
		    !wl_list_empty(&S.incoming)) {
			struct incoming *in = wl_container_of(S.incoming.next, in, link);
			if (in->incr)
				incoming_incr_piece();
			break;
		}
		if (ev->state == XCB_PROPERTY_DELETE) {
			struct outgoing *o, *tmp;
			wl_list_for_each_safe(o, tmp, &S.outgoing, link)
				if (o->incr_wait && o->requestor == ev->window && o->property == ev->atom) {
					outgoing_incr_next(o);
					break;
				}
		}
		if (ev->window && ev->window == S.xsettings_owner) {
			scale_changed();
			break;
		}
		struct view *v = view_by_window(ev->window);
		if (v && v->frame == ev->window && ev->atom == A._NET_WM_STATE && v->kind == VIEW_TOPLEVEL) {
			xcb_get_property_reply_t *r = xcb_get_property_reply(S.x,
				xcb_get_property(S.x, 0, v->frame, A._NET_WM_STATE, XCB_ATOM_ATOM, 0, 64), NULL);
			x_after_blocking();
			bool maxv = false, maxh = false, full = false, hidden = false;
			if (r) {
				xcb_atom_t *a = xcb_get_property_value(r);
				int n = xcb_get_property_value_length(r) / 4;
				for (int i = 0; i < n; i++) {
					maxv |= a[i] == A._NET_WM_STATE_MAXIMIZED_VERT;
					maxh |= a[i] == A._NET_WM_STATE_MAXIMIZED_HORZ;
					full |= a[i] == A._NET_WM_STATE_FULLSCREEN;
					hidden |= a[i] == A._NET_WM_STATE_HIDDEN;
				}
				free(r);
			}
			wlr_xdg_toplevel_set_maximized(v->toplevel, maxv && maxh);
			wlr_xdg_toplevel_set_fullscreen(v->toplevel, full);
			if (v->xdg->client->shell->version >= 6)
				wlr_xdg_toplevel_set_suspended(v->toplevel, hidden);
		}
		break;
	}
	case XCB_CLIENT_MESSAGE: {
		xcb_client_message_event_t *ev = (void *)e;
		if (dnd_client_message(ev))
			break;
		struct view *v = view_by_window(ev->window);
		if (v && v->kind == VIEW_TOPLEVEL && ev->type == A.WM_PROTOCOLS && ev->data.data32[0] == A.WM_DELETE_WINDOW)
			wlr_xdg_toplevel_send_close(v->toplevel);
		break;
	}
	case XCB_SELECTION_NOTIFY:
		handle_selection_notify((void *)e);
		break;
	case XCB_SELECTION_REQUEST:
		handle_selection_request((void *)e);
		break;
	case XCB_SELECTION_CLEAR: {
		xcb_selection_clear_event_t *ev = (void *)e;
		int sel = sel_index(ev->selection);
		if (sel >= 0)
			S.sel_we_own[sel] = false;
		break;
	}
	case 0:
		break;  /* errors: windows of programs that went away, etc. */
	case XCB_GE_GENERIC: {
		xcb_ge_generic_event_t *ge = (void *)e;
		if (S.xinput_opcode && ge->extension == S.xinput_opcode && ge->event_type == XCB_INPUT_RAW_BUTTON_PRESS)
			raw_button_press();
		break;
	}
	default:
		if (S.xfixes_event && type == S.xfixes_event + XCB_XFIXES_SELECTION_NOTIFY) {
			xcb_xfixes_selection_notify_event_t *ev = (void *)e;
			S.last_time = ev->timestamp;
			int sel = sel_index(ev->selection);
			if (sel >= 0)
				x_owner_changed(sel, ev->owner);
		}
		break;
	}
}

static void x_drain(void) {
	xcb_generic_event_t *e;
	while ((e = xcb_poll_for_event(S.x))) {
		x_event(e);
		free(e);
	}
	if (xcb_connection_has_error(S.x)) {
		diag("the X server connection closed; exiting");
		wl_display_terminate(S.display);
	}
}

static int x_readable(int fd, uint32_t mask, void *data) {
	x_drain();
	return 0;
}

static void x_idle_drain(void *data) {
	S.x_idle = NULL;
	x_drain();
}

/* After a reply was waited for, events may sit in xcb's queue with nothing
 * to wake the loop: look at them as soon as the loop is idle. */
static void x_after_blocking(void) {
	if (!S.x_idle && S.loop)
		S.x_idle = wl_event_loop_add_idle(S.loop, x_idle_drain, NULL);
}

static bool x_setup(void) {
	int scr;
	S.x = xcb_connect(NULL, &scr);
	if (xcb_connection_has_error(S.x))
		return false;
	xcb_screen_iterator_t it = xcb_setup_roots_iterator(xcb_get_setup(S.x));
	for (int i = 0; i < scr; i++)
		xcb_screen_next(&it);
	S.screen = it.data;
	S.root = S.screen->root;
#define ATOM(n) A.n = intern(#n)
	ATOM(WM_PROTOCOLS); ATOM(WM_DELETE_WINDOW); ATOM(WM_STATE); ATOM(WM_CHANGE_STATE); ATOM(WM_CLIENT_MACHINE);
	ATOM(_NET_WM_NAME); ATOM(_NET_WM_PID); ATOM(_NET_WM_ICON); ATOM(_NET_WM_STATE);
	ATOM(_NET_WM_STATE_MAXIMIZED_VERT); ATOM(_NET_WM_STATE_MAXIMIZED_HORZ); ATOM(_NET_WM_STATE_FULLSCREEN);
	ATOM(_NET_WM_STATE_HIDDEN); ATOM(_NET_WM_MOVERESIZE); ATOM(_NET_WM_WINDOW_TYPE); ATOM(_NET_WM_WINDOW_TYPE_NORMAL);
	ATOM(_NET_WM_WINDOW_TYPE_DIALOG); ATOM(_NET_WM_WINDOW_TYPE_POPUP_MENU); ATOM(_NET_WM_WINDOW_TYPE_TOOLTIP);
	ATOM(_NET_ACTIVE_WINDOW); ATOM(_NET_WORKAREA); ATOM(_MOTIF_WM_HINTS); ATOM(_GTK_SHOW_WINDOW_MENU);
	ATOM(UTF8_STRING); ATOM(CLIPBOARD); ATOM(TARGETS); ATOM(TIMESTAMP); ATOM(MULTIPLE); ATOM(TEXT); ATOM(INCR);
	ATOM(_SYLUXIUM_WAYLAND); ATOM(RESOURCE_MANAGER);
#undef ATOM
	/* The X11 backend looks this atom up without creating it (only if it
	 * exists) and set a property with atom 0 otherwise: an X error per window. */
	intern("_VARIABLE_REFRESH");
	A.MIME_UTF8 = intern("text/plain;charset=utf-8");
	A.MIME_TEXT = intern("text/plain");
	A.SYLUXIUM_SELECTION = intern("_SYLUXIUM_SELECTION");
	XdndAware = intern("XdndAware");
	S.sel_atom[SEL_CLIPBOARD] = A.CLIPBOARD;
	S.sel_atom[SEL_PRIMARY] = XCB_ATOM_PRIMARY;
	dnd_atoms();

	/* ARGB32 for cursors */
	xcb_render_query_version_reply_t *rv = xcb_render_query_version_reply(S.x,
		xcb_render_query_version(S.x, 0, 11), NULL);
	free(rv);
	xcb_render_query_pict_formats_reply_t *pf = xcb_render_query_pict_formats_reply(S.x,
		xcb_render_query_pict_formats(S.x), NULL);
	if (pf) {
		xcb_render_pictforminfo_iterator_t fi = xcb_render_query_pict_formats_formats_iterator(pf);
		for (; fi.rem; xcb_render_pictforminfo_next(&fi)) {
			xcb_render_pictforminfo_t *f = fi.data;
			if (f->type == XCB_RENDER_PICT_TYPE_DIRECT && f->depth == 32 && f->direct.alpha_mask == 0xff &&
			    f->direct.alpha_shift == 24 && f->direct.red_shift == 16 && f->direct.blue_shift == 0) {
				S.argb32 = f->id;
				break;
			}
		}
		free(pf);
	}

	/* the selection window */
	S.selwin = xcb_generate_id(S.x);
	uint32_t m = XCB_EVENT_MASK_PROPERTY_CHANGE;
	xcb_create_window(S.x, XCB_COPY_FROM_PARENT, S.selwin, S.root, -10, -10, 1, 1, 0,
		XCB_WINDOW_CLASS_INPUT_ONLY, XCB_COPY_FROM_PARENT, XCB_CW_EVENT_MASK, &m);
	set_text(S.selwin, A._NET_WM_NAME, A.UTF8_STRING, "Syluxium Wayland host");
	const xcb_query_extension_reply_t *xf = xcb_get_extension_data(S.x, &xcb_xfixes_id);
	if (xf && xf->present) {
		free(xcb_xfixes_query_version_reply(S.x, xcb_xfixes_query_version(S.x, 5, 0), NULL));
		S.xfixes_event = xf->first_event;
		uint32_t mask = XCB_XFIXES_SELECTION_EVENT_MASK_SET_SELECTION_OWNER |
			XCB_XFIXES_SELECTION_EVENT_MASK_SELECTION_WINDOW_DESTROY |
			XCB_XFIXES_SELECTION_EVENT_MASK_SELECTION_CLIENT_CLOSE;
		xcb_xfixes_select_selection_input(S.x, S.selwin, A.CLIPBOARD, mask);
		xcb_xfixes_select_selection_input(S.x, S.selwin, XCB_ATOM_PRIMARY, mask);
	} else {
		diag("clipboard: no XFIXES, X programs' copies are not seen");
	}
	/* Clicks anywhere (XInput 2 raw events reach every client, whoever has the
	 * pointer): a click outside our windows closes an open menu, as on X11 (the
	 * taskbar takes no focus, so the focus doesn't say so). */
	const xcb_query_extension_reply_t *xi = xcb_get_extension_data(S.x, &xcb_input_id);
	if (xi && xi->present) {
		xcb_input_xi_query_version_reply_t *v = xcb_input_xi_query_version_reply(S.x,
			xcb_input_xi_query_version(S.x, 2, 2), NULL);
		if (v && (v->major_version > 2 || (v->major_version == 2 && v->minor_version >= 0))) {
			S.xinput_opcode = xi->major_opcode;
			struct {
				xcb_input_event_mask_t head;
				uint32_t mask;
			} raw = { { XCB_INPUT_DEVICE_ALL_MASTER, 1 }, XCB_INPUT_XI_EVENT_MASK_RAW_BUTTON_PRESS };
			xcb_input_xi_select_events(S.x, S.root, 1, &raw.head);
		}
		free(v);
	}
	xcb_flush(S.x);
	return true;
}

/* The X session's scale, from XSETTINGS (xfsettingsd): Gdk/WindowScalingFactor
 * (Xfce's "Window Scaling", whole numbers) and Xft/DPI (fonts; a DPI of 144
 * is 1.5). The larger of the two, to 1/8. SYLUXIUM_WAYLAND_HOST_SCALE overrides. */
static bool xsettings_int(const uint8_t *d, size_t len, const char *want, int32_t *out) {
	if (len < 12)
		return false;
	bool big = d[0] == 1;  /* MSBFirst */
#define RD32(o) (big ? (uint32_t)d[o] << 24 | d[(o) + 1] << 16 | d[(o) + 2] << 8 | d[(o) + 3] : \
	(uint32_t)d[(o) + 3] << 24 | d[(o) + 2] << 16 | d[(o) + 1] << 8 | d[o])
#define RD16(o) (big ? (uint16_t)(d[o] << 8 | d[(o) + 1]) : (uint16_t)(d[(o) + 1] << 8 | d[o]))
	uint32_t n = RD32(8);
	size_t o = 12;
	for (uint32_t i = 0; i < n && o + 8 <= len; i++) {
		uint8_t type = d[o];
		uint16_t nlen = RD16(o + 2);
		size_t name = o + 4;
		o = name + ((nlen + 3) & ~3u) + 4;  /* name, padding, last-change serial */
		if (o > len)
			return false;
		if (type == 0) {
			if (o + 4 > len)
				return false;
			if (nlen == strlen(want) && memcmp(d + name, want, nlen) == 0) {
				*out = (int32_t)RD32(o);
				return true;
			}
			o += 4;
		} else if (type == 1) {
			if (o + 4 > len)
				return false;
			o += 4 + ((RD32(o) + 3) & ~3u);
		} else if (type == 2) {
			o += 8;
		} else {
			return false;
		}
	}
	return false;
#undef RD32
#undef RD16
}

static void read_scale(void) {
	int32_t factor = 1, dpi = 0;
	char sel[32];
	snprintf(sel, sizeof(sel), "_XSETTINGS_S%d", 0);
	xcb_get_selection_owner_reply_t *o = xcb_get_selection_owner_reply(S.x,
		xcb_get_selection_owner(S.x, intern(sel)), NULL);
	xcb_window_t owner = o ? o->owner : 0;
	free(o);
	if (owner && owner != S.xsettings_owner) {
		uint32_t m = XCB_EVENT_MASK_PROPERTY_CHANGE | XCB_EVENT_MASK_STRUCTURE_NOTIFY;
		xcb_change_window_attributes(S.x, owner, XCB_CW_EVENT_MASK, &m);
	}
	S.xsettings_owner = owner;
	if (owner) {
		xcb_get_property_reply_t *r = xcb_get_property_reply(S.x, xcb_get_property(S.x, 0, owner,
			intern("_XSETTINGS_SETTINGS"), XCB_GET_PROPERTY_TYPE_ANY, 0, 65536), NULL);
		if (r) {
			xsettings_int(xcb_get_property_value(r), xcb_get_property_value_length(r), "Gdk/WindowScalingFactor", &factor);
			xsettings_int(xcb_get_property_value(r), xcb_get_property_value_length(r), "Xft/DPI", &dpi);
			free(r);
		}
	}
	x_after_blocking();
	double by_dpi = dpi > 0 ? dpi / 1024.0 / 96.0 : 1;
	double scale = factor > 1 ? factor : 1;
	if (by_dpi > scale + 0.05)
		scale = by_dpi;
	scale = round(scale * 8) / 8;
	const char *e = getenv("SYLUXIUM_WAYLAND_HOST_SCALE");
	if (e && atof(e) >= 0.5 && atof(e) <= 4)
		scale = atof(e);
	if (scale < 1)
		scale = 1;
	S.scale = scale;
	snprintf(S.scale_desc, sizeof(S.scale_desc), "scale %.3g (window scaling %d, font DPI %.0f)", scale, (int)factor,
		dpi > 0 ? dpi / 1024.0 : 96.0);
}

/* The session's scale changed (Xfce settings): windows keep their size on
 * the screen and get the new scale. */
static void scale_changed(void) {
	double old = S.scale;
	read_scale();
	if (S.scale == old)
		return;
	diag("scale changed: %s", S.scale_desc);
	if (S.xcursor)
		wlr_xcursor_manager_load(S.xcursor, S.scale);
	xcb_cursor_t def = theme_cursor("default");
	if (def) {
		S.cursor_default = def;
		set_current_cursor(def, false);
	}
	if (S.screen_output) {
		struct wlr_output_state st;
		wlr_output_state_init(&st);
		wlr_output_state_set_scale(&st, S.scale);
		wlr_output_commit_state(S.screen_output, &st);
		wlr_output_state_finish(&st);
	}
	dismiss_popups();
	struct view *v;
	wl_list_for_each(v, &S.views, link)
		if (v->kind == VIEW_TOPLEVEL && v->x_mapped) {
			v->scale = S.scale;
			view_render_resize(v, v->w, v->h);
			v->size_serial = wlr_xdg_toplevel_set_size(v->toplevel, units(v, v->w), units(v, v->h));
			view_update_size_hints(v);
		}
}

/* The cursor theme and size the X desktop uses (Xcursor.* resources). */
static void cursor_theme(char *theme, size_t tlen, int *size) {
	*size = 24;
	theme[0] = 0;
	xcb_get_property_reply_t *r = xcb_get_property_reply(S.x,
		xcb_get_property(S.x, 0, S.root, A.RESOURCE_MANAGER, XCB_ATOM_STRING, 0, 65536), NULL);
	if (r) {
		char *s = strndup(xcb_get_property_value(r), xcb_get_property_value_length(r));
		for (char *line = strtok(s, "\n"); line; line = strtok(NULL, "\n")) {
			char *v = strchr(line, ':');
			if (!v)
				continue;
			*v++ = 0;
			while (*v == ' ' || *v == '\t')
				v++;
			if (!strcmp(line, "Xcursor.theme"))
				snprintf(theme, tlen, "%s", v);
			else if (!strcmp(line, "Xcursor.size") && atoi(v) > 0)
				*size = atoi(v);
		}
		free(s);
		free(r);
	}
	if (!theme[0] && getenv("XCURSOR_THEME"))
		snprintf(theme, tlen, "%s", getenv("XCURSOR_THEME"));
	if (getenv("XCURSOR_SIZE") && atoi(getenv("XCURSOR_SIZE")) > 0)
		*size = atoi(getenv("XCURSOR_SIZE"));
}

/* wl_output for programs: the X screen (never drawn to). */
static void make_screen_output(void) {
	S.headless = wlr_headless_backend_create(S.loop);
	if (!S.headless || !wlr_backend_start(S.headless))
		return;
	struct wlr_output *o = wlr_headless_add_output(S.headless, S.screen->width_in_pixels, S.screen->height_in_pixels);
	if (!o)
		return;
	wlr_output_init_render(o, S.allocator, S.renderer);
	wlr_output_set_name(o, "X11-screen");
	wlr_output_set_description(o, "Xfce desktop");
	struct wlr_output_state st;
	wlr_output_state_init(&st);
	wlr_output_state_set_enabled(&st, true);
	wlr_output_state_set_custom_mode(&st, S.screen->width_in_pixels, S.screen->height_in_pixels, 0);
	wlr_output_state_set_scale(&st, S.scale);
	wlr_output_commit_state(o, &st);
	wlr_output_state_finish(&st);
	wlr_output_create_global(o, S.display);
	S.screen_output = o;
}

static void open_diag(void) {
	struct stat st;
	if (stat("/run/syluxium-diag", &st) == 0 && access("/run/syluxium-diag", W_OK) == 0) {
		S.diag = fopen("/run/syluxium-diag/wayland-host.txt", "a");
		if (S.diag)
			fchmod(fileno(S.diag), 0666);
	}
}

static struct wl_listener l_new_output = { .notify = server_new_output }, l_new_input = { .notify = server_new_input },
	l_new_toplevel = { .notify = new_toplevel }, l_new_popup = { .notify = new_popup },
	l_req_cursor = { .notify = seat_request_cursor }, l_cursor_shape = { .notify = cursor_shape_request },
	l_req_sel = { .notify = seat_request_set_selection }, l_req_prim = { .notify = seat_request_set_primary },
	l_set_sel = { .notify = seat_set_selection }, l_set_prim = { .notify = seat_set_primary },
	l_new_deco = { .notify = new_xdg_decoration }, l_new_kde = { .notify = new_kde_decoration },
	l_activate = { .notify = request_activate };

/* How windows are drawn, in words for wayland-host.txt: GPU (GLES2 on a
 * render node, dma-bufs to the X server through DRI3, programs offered
 * linux-dmabuf) or software (pixman, MIT-SHM). */
static void describe_gpu_path(void) {
	int bfd = wlr_backend_get_drm_fd(S.backend);
	const char *forced = getenv("WLR_RENDERER");
	if (wlr_renderer_is_gles2(S.renderer)) {
		int fd = wlr_renderer_get_drm_fd(S.renderer);
		char *node = fd >= 0 ? drmGetDeviceNameFromFd2(fd) : NULL;
		drmVersion *ver = fd >= 0 ? drmGetVersion(fd) : NULL;
		bool dmabuf = wlr_renderer_get_texture_formats(S.renderer, WLR_BUFFER_CAP_DMABUF) != NULL;
		snprintf(S.gpu_desc, sizeof(S.gpu_desc), "GPU rendering (GLES2 on %s, driver %s), %s to the X server, "
			"programs %s dma-bufs", node ? node : "?", ver && ver->name ? ver->name : "?",
			bfd >= 0 ? "dma-bufs (DRI3)" : "shared memory", dmabuf ? "can draw into" : "can't use");
		free(node);
		if (ver)
			drmFreeVersion(ver);
	} else {
		snprintf(S.gpu_desc, sizeof(S.gpu_desc), "software rendering (pixman), shared memory (MIT-SHM) to the X "
			"server (%s)", forced && *forced ? "forced" : bfd >= 0 ? "no OpenGL on the GPU" :
			"the X server has no DRI3 / GPU");
	}
}

/* Before programs connect: one frame through the chosen path into a
 * window nobody sees, and the X server must say it was shown (Present).
 * A GPU path that fails here (DRI3 import, modifiers, a broken driver)
 * would leave every window black. */
static bool gpu_check(void) {
	if (wlr_renderer_is_pixman(S.renderer) || getenv("SYLUXIUM_WAYLAND_HOST_NOCHECK"))
		return true;
	xcb_window_t hidden = xcb_generate_id(S.x);
	xcb_create_window(S.x, XCB_COPY_FROM_PARENT, hidden, S.root, -200, -200, 64, 64, 0,
		XCB_WINDOW_CLASS_INPUT_OUTPUT, S.screen->root_visual, 0, NULL);
	xcb_flush(S.x);
	S.test_output = true;
	struct wlr_output *out = wlr_x11_output_create(S.backend);
	S.test_output = false;
	if (!out || out != S.test_out) {
		xcb_destroy_window(S.x, hidden);
		return false;
	}
	xcb_window_t child = ((struct x11_output_prefix *)out)->win;
	bool ok = child_window_ok(child);
	if (ok) {
		xcb_reparent_window(S.x, child, hidden, 0, 0);
		free(xcb_get_input_focus_reply(S.x, xcb_get_input_focus(S.x), NULL));
		struct wlr_scene *scene = wlr_scene_create();
		const float c[4] = { 0.1f, 0.2f, 0.4f, 1 };
		wlr_scene_rect_create(&scene->tree, 64, 64, c);
		struct wlr_scene_output *so = wlr_scene_output_create(scene, out);
		wl_signal_add(&out->events.present, &l_test_present);
		S.test_presented = false;
		struct wlr_output_state st;
		wlr_output_state_init(&st);
		wlr_output_state_set_enabled(&st, true);
		wlr_output_state_set_custom_mode(&st, 64, 64, 0);
		ok = wlr_scene_output_build_state(so, &st, NULL) && wlr_output_commit_state(out, &st);
		wlr_output_state_finish(&st);
		for (int i = 0; ok && !S.test_presented && i < 40; i++)
			wl_event_loop_dispatch(S.loop, 50);
		ok = ok && S.test_presented;
		wl_list_remove(&l_test_present.link);
		wlr_scene_output_destroy(so);
		wlr_scene_node_destroy(&scene->tree.node);
	}
	wlr_output_destroy(out);
	S.test_out = NULL;
	xcb_destroy_window(S.x, hidden);
	xcb_flush(S.x);
	return ok;
}

static int on_signal(int sig, void *data) {
	wl_display_terminate(S.display);
	return 0;
}

int main(int argc, char **argv) {
	signal(SIGPIPE, SIG_IGN);
	signal(SIGCHLD, SIG_DFL);
	const char *dpy = getenv("DISPLAY");
	if (!dpy) {
		fprintf(stderr, "syluxium-wayland-host: no X display (DISPLAY)\n");
		return 1;
	}
	char defname[64];
	snprintf(defname, sizeof(defname), "syluxium-x11-%d", dpy[0] == ':' ? atoi(dpy + 1) : 0);
	S.socket = argc > 1 ? argv[1] : defname;
	gethostname(S.hostname, sizeof(S.hostname) - 1);
	wl_list_init(&S.views);
	wl_list_init(&S.pointers);
	wl_list_init(&S.kde_decos);
	wl_list_init(&S.incoming);
	wl_list_init(&S.outgoing);
	open_diag();
	wlr_log_init(getenv("SYLUXIUM_WAYLAND_HOST_DEBUG") ? WLR_DEBUG : WLR_ERROR, NULL);

	if (!x_setup()) {
		diag("ERROR: cannot connect to the X display %s", dpy);
		return 1;
	}
	/* SYLUXIUM_WAYLAND_HOST_RENDERER=pixman|gles2 (the starter sets it for
	 * the software fallback and the whostsoft boot option) */
	const char *want = getenv("SYLUXIUM_WAYLAND_HOST_RENDERER");
	if (want && *want)
		setenv("WLR_RENDERER", want, 1);
	S.display = wl_display_create();
	S.loop = wl_display_get_event_loop(S.display);
	S.backend = wlr_x11_backend_create(S.loop, NULL);
	if (!S.backend) {
		diag("ERROR: the wlroots X11 backend failed (X extensions: Present, XInput 2, MIT-SHM or DRI3)");
		return 1;
	}
	S.bx = (struct x11_backend_prefix *)S.backend;
	S.renderer = wlr_renderer_autocreate(S.backend);
	if (!S.renderer) {
		diag("ERROR: no renderer");
		return 1;
	}
	wlr_renderer_init_wl_display(S.renderer, S.display);
	S.allocator = wlr_allocator_autocreate(S.backend, S.renderer);
	if (!S.allocator) {
		diag("ERROR: no buffer allocator");
		return 1;
	}
	describe_gpu_path();
	wl_signal_add(&S.backend->events.new_output, &l_new_output);
	wl_signal_add(&S.backend->events.new_input, &l_new_input);

	struct wlr_compositor *comp = wlr_compositor_create(S.display, 6, S.renderer);
	(void)comp;
	wlr_subcompositor_create(S.display);
	wlr_data_device_manager_create(S.display);
	wlr_primary_selection_v1_device_manager_create(S.display);
	wlr_data_control_manager_v1_create(S.display);
	wlr_viewporter_create(S.display);
	wlr_presentation_create(S.display, S.backend);
	wlr_single_pixel_buffer_manager_v1_create(S.display);
	wlr_fractional_scale_manager_v1_create(S.display, 1);
	S.xdg_shell = wlr_xdg_shell_create(S.display, 6);
	wl_signal_add(&S.xdg_shell->events.new_toplevel, &l_new_toplevel);
	wl_signal_add(&S.xdg_shell->events.new_popup, &l_new_popup);
	struct wlr_xdg_decoration_manager_v1 *dm = wlr_xdg_decoration_manager_v1_create(S.display);
	wl_signal_add(&dm->events.new_toplevel_decoration, &l_new_deco);
	S.kde_deco_mgr = wlr_server_decoration_manager_create(S.display);
	wlr_server_decoration_manager_set_default_mode(S.kde_deco_mgr, WLR_SERVER_DECORATION_MANAGER_MODE_SERVER);
	wl_signal_add(&S.kde_deco_mgr->events.new_decoration, &l_new_kde);
	struct wlr_xdg_activation_v1 *act = wlr_xdg_activation_v1_create(S.display);
	wl_signal_add(&act->events.request_activate, &l_activate);
	struct wlr_cursor_shape_manager_v1 *shape = wlr_cursor_shape_manager_v1_create(S.display, 1);
	wl_signal_add(&shape->events.request_set_shape, &l_cursor_shape);

	S.seat = wlr_seat_create(S.display, "seat0");
	wl_signal_add(&S.seat->events.request_set_cursor, &l_req_cursor);
	wl_signal_add(&S.seat->events.request_set_selection, &l_req_sel);
	wl_signal_add(&S.seat->events.request_set_primary_selection, &l_req_prim);
	wl_signal_add(&S.seat->events.set_selection, &l_set_sel);
	wl_signal_add(&S.seat->events.set_primary_selection, &l_set_prim);
	wl_signal_add(&S.seat->events.request_start_drag, &l_req_drag);
	wl_signal_add(&S.seat->events.start_drag, &l_start_drag);
	wlr_seat_set_capabilities(S.seat, WL_SEAT_CAPABILITY_POINTER);

	char theme[128];
	int csize;
	cursor_theme(theme, sizeof(theme), &csize);
	S.cursor_size = csize;
	read_scale();
	S.xcursor = wlr_xcursor_manager_create(theme[0] ? theme : NULL, csize);
	if (S.xcursor)
		wlr_xcursor_manager_load(S.xcursor, S.scale);
	S.cursor_default = theme_cursor("default");
	uint32_t blank = 0;
	S.cursor_hidden = make_cursor(&blank, 1, 1, 1, 0, 0);
	S.cursor_current = S.cursor_default;

	if (!wlr_backend_start(S.backend)) {
		diag("ERROR: the X11 backend did not start");
		return 1;
	}
	if (S.bx->xcb == NULL || xcb_connection_has_error(S.bx->xcb) ||
	    xcb_get_setup(S.bx->xcb)->protocol_major_version != 11) {
		diag("ERROR: unexpected wlroots X11 backend layout (not wlroots 0.18?)");
		return 1;
	}
	make_screen_output();
	read_refresh_rate();

	S.x_src = wl_event_loop_add_fd(S.loop, xcb_get_file_descriptor(S.x), WL_EVENT_READABLE, x_readable, NULL);
	S.incoming_timer = wl_event_loop_add_timer(S.loop, incoming_timeout, NULL);
	wl_event_loop_add_signal(S.loop, SIGTERM, on_signal, NULL);
	wl_event_loop_add_signal(S.loop, SIGINT, on_signal, NULL);

	/* an X program already owns the clipboard: offer it */
	for (int sel = 0; sel < SEL_N; sel++) {
		xcb_get_selection_owner_reply_t *r = xcb_get_selection_owner_reply(S.x,
			xcb_get_selection_owner(S.x, S.sel_atom[sel]), NULL);
		if (r && r->owner)
			x_owner_changed(sel, r->owner);
		free(r);
	}
	x_after_blocking();

	if (!gpu_check()) {
		diag("ERROR: drawing through %s failed (no frame shown in a test window); exiting so the starter "
			"runs the host in software (pixman)", S.gpu_desc);
		return 3;
	}

	if (wl_display_add_socket(S.display, S.socket) != 0) {
		diag("socket %s is in use (another host is running?)", S.socket);
		return 2;
	}
	diag("started on %s as WAYLAND_DISPLAY=%s: %s; screen %dx%d at %.1f Hz, %s, cursor theme %s %d",
		dpy, S.socket, S.gpu_desc, S.screen->width_in_pixels, S.screen->height_in_pixels, 1e9 / S.frame_ns,
		S.scale_desc, theme[0] ? theme : "default", csize);
	/* tell the starter we are ready */
	if (getenv("SYLUXIUM_WAYLAND_HOST_READY_FD")) {
		int fd = atoi(getenv("SYLUXIUM_WAYLAND_HOST_READY_FD"));
		if (write(fd, "ok\n", 3) < 0) { }
		close(fd);
		unsetenv("SYLUXIUM_WAYLAND_HOST_READY_FD");
	}
	static struct wl_event_source *stats;
	if (getenv("SYLUXIUM_WAYLAND_HOST_DEBUG")) {
		stats = wl_event_loop_add_timer(S.loop, debug_stats, &stats);
		wl_event_source_timer_update(stats, 3000);
	}
	wl_display_run(S.display);
	diag("stopped (%d windows hosted)", S.hosted);
	wl_display_destroy_clients(S.display);
	wl_display_destroy(S.display);
	return 0;
}
