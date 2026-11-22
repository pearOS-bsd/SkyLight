/* The CoreAnimation render server of WindowServer: the commits of the clients arrive from carender-stub (the Mach side, one per system) on a stream socket, are decoded and composed by
 * ca_scene.c into the backing store of a window of WindowServer, and that window goes through the compositor of WindowServer like any other.
 *
 * Wire (SOCK_STREAM): records  u32 type, u32 length, payload.
 *   type 1: a commit message (the bytes of the Mach message of the render server protocol, from the header on)
 *   type 2: out-of-line data of a message that follows: u32 message number, u32 descriptor, the bytes (sent before the message that names them)
 * The window: one for the whole scene for now (the screen's size).  WSD_CA_LEVEL sets its level (default 5). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include "ws.h"
#include "ca_scene.h"
#include "wsd_priv.h"

#define CA_MAXC 4
#define CA_OOLN 512
static struct { int fd; uint8_t *buf; size_t n, cap; } cl[CA_MAXC];
static int ool_pending = -1;		/* the number of the message whose out-of-line data came last: the commit that names it follows (carender-stub numbers them, WindowServer may have been restarted since) */
static struct { int used, msg, desc; uint8_t *data; size_t size; } ool[CA_OOLN];
static uint8_t *ool_take(void *ctx, int msg, int desc, size_t *sz)
{
	(void)ctx;
	if (getenv("CA_DEBUG")) fprintf(stderr, "wscarender: asked for out-of-line data %d/%d\n", msg, desc);
	for (int i = 0; i < CA_OOLN; i++)
		if (ool[i].used && ool[i].msg == msg && ool[i].desc == desc) { uint8_t *d = ool[i].data; *sz = ool[i].size; ool[i].used = 0; return d; }
	return NULL;
}
static void ool_put(int msg, int desc, const uint8_t *d, size_t n)
{
	int slot = -1;
	for (int i = 0; i < CA_OOLN; i++) if (!ool[i].used) { slot = i; break; }
	if (slot < 0) { slot = 0; for (int i = 1; i < CA_OOLN; i++) if (ool[i].msg < ool[slot].msg) slot = i; free(ool[slot].data); }	/* full: the oldest goes */
	uint8_t *c = malloc(n ? n : 1);
	if (getenv("CA_DEBUG")) fprintf(stderr, "wscarender: out-of-line data %d/%d: %zu bytes\n", msg, desc, n);
	if (!c) { ool[slot].used = 0; return; }
	memcpy(c, d, n);
	ool[slot].used = 1; ool[slot].msg = msg; ool[slot].desc = desc; ool[slot].data = c; ool[slot].size = n;
}

int wsd_ca_listen(const char *path)
{
	struct sockaddr_un sa;
	int s;

	memset(&sa, 0, sizeof sa);
	sa.sun_family = AF_UNIX; strlcpy(sa.sun_path, path, sizeof sa.sun_path);
	if ((s = socket(AF_UNIX, SOCK_STREAM, 0)) < 0) return -1;
	unlink(path);
	mode_t old = umask(0177);
	int b = bind(s, (struct sockaddr *)&sa, sizeof sa);
	umask(old);
	if (b < 0 || listen(s, 4) < 0) { close(s); return -1; }
	ca_set_ool_loader(ool_take, NULL);
	for (int i = 0; i < CA_MAXC; i++) cl[i].fd = -1;
	return s;
}

int wsd_ca_accept(int ls)
{
	int fd = accept(ls, NULL, NULL);
	if (fd < 0) return -1;
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
	for (int i = 0; i < CA_MAXC; i++)
		if (cl[i].fd < 0) { cl[i].fd = fd; cl[i].n = 0; return fd; }
	close(fd);
	return -1;
}

int wsd_ca_client(int fd) { for (int i = 0; i < CA_MAXC; i++) if (cl[i].fd == fd && fd >= 0) return 1; return 0; }

#define CA_WINS 16
static int ca_dirty[CA_WINS];


/* How Apple does it: every render context (CAContext) of a client has its own tree of layers, and the window server shows the tree in the window that the context was bound to
 * (SetWindowLayerContext), at the frame of that window, with its level and order.  So a window of WindowServer for each context with a root layer: the frame, level and order of the window that its
 * context is bound to; a context that is bound to none (the Dock's layers) fills the screen.  A client whose contexts have no root yet is painted as one scene on the screen. */
#define CA_CTXS 8
static ws_wid ca_cwins[CA_WINS][CA_CTXS + 1];		/* [scene][root index]; the last one: the whole scene (no roots known) */
static int ca_cw[CA_WINS][CA_CTXS + 1], ca_ch[CA_WINS][CA_CTXS + 1];

static int ca_glass_used[CA_WINS][CA_CTXS + 1];
static void ca_show(ws_server *srv, int sw, int sh, int scene, int slot, uint32_t ctx, int root)
{
	ws_surface *b;
	ws_rect fr = { 0, 0, sw, sh };
	int lvl = 5;
	ws_wid bound = ctx ? (ws_wid)wsd_layer_ctx_window(ctx) : 0;

	if (bound && ws_window_exists(srv, bound) && root >= 0) {
		/* the window server of the Mac takes the size of a window from the root layer of its context (a window made by AppKit has an empty shape and is 1x1 here).  The position is not sent to the window server
		 * by any message that was found (see docs/EVIDENCE.md, 34th pass): until it is known the window is centred on the screen, and ordered in. */
		double gx, gy, gw, gh; ws_rect cur;
		if (ws_window_frame(srv, bound, &cur) == 0 && cur.w <= 1 && cur.h <= 1 && ca_scene_root_geom(scene, root, &gx, &gy, &gw, &gh) && gw > 1 && gh > 1 && gw <= sw && gh <= sh) {
			ws_set_window_frame(srv, bound, (int)((sw - gw) / 2), (int)((sh - gh) / 2), (int)gw, (int)gh);
			if (gh > 40) { ws_order_window(srv, bound, WS_ABOVE, 0); wsd_window_visible_pid((int)ca_scene_ctx(scene), (uint32_t)bound); }			/* the thin full-width windows of AppKit (the title bar: 1920x30) stay out, as on the Mac (they are not on screen there); what orders the others on the Mac is not known (no message does) */
			fprintf(stderr, "WindowServer: window %u takes the size of its root layer: %gx%g, centred\n", (unsigned)bound, gw, gh);
		}
	}
	if (bound && ws_window_exists(srv, bound)) {
		ws_rect r;
		if (ws_window_frame(srv, bound, &r) == 0 && r.w > 1 && r.h > 1) fr = r;
		ws_window_level(srv, bound, &lvl);
	} else
		bound = 0;
	if (ca_cwins[scene][slot] && (ca_cw[scene][slot] != fr.w || ca_ch[scene][slot] != fr.h)) { ws_release_window(srv, ca_cwins[scene][slot]); ca_cwins[scene][slot] = 0; }	/* another size: a new window (its backing has the size) */
	if (!ca_cwins[scene][slot]) {
		const char *lv = getenv("WSD_CA_LEVEL");
		ws_wid w = ws_create_window(srv, fr);
		if (!w) return;
		ca_cwins[scene][slot] = w; ca_cw[scene][slot] = fr.w; ca_ch[scene][slot] = fr.h;
		ws_set_window_level(srv, w, bound ? lvl : (lv ? atoi(lv) : 5));
		ws_order_window(srv, w, WS_ABOVE, bound ? bound : 0);
		fprintf(stderr, "WindowServer: render context %u (client %u): window %u%s (%d,%d %dx%d)\n", (unsigned)ctx, (unsigned)ca_scene_ctx(scene), (unsigned)w, bound ? " at the window of the client" : "", fr.x, fr.y, fr.w, fr.h);
	} else if (bound) {
		ws_move_window(srv, ca_cwins[scene][slot], fr.x, fr.y);
		ws_set_window_level(srv, ca_cwins[scene][slot], lvl);
	}
	if ((b = ws_window_backing(srv, ca_cwins[scene][slot])) == NULL) return;
	if (root >= 0) ca_paint_root(scene, root, b->px, b->w, b->h, (int)b->stride);
	if (root >= 0 && getenv("WSD_CA_DEBUG")) { size_t nz = 0; for (int yy = 0; yy < b->h; yy++) for (int xx = 0; xx < b->w; xx++) if (((const uint8_t *)b->px)[yy * b->stride + xx * 4 + 3]) nz++; fprintf(stderr, "WindowServer: context %u painted into %dx%d: %zu non-transparent pixels\n", (unsigned)ctx, b->w, b->h, nz); }
	if (root >= 0 && bound) {		/* the Liquid Glass of the context (CASDFElementLayers): the glass of the window it is bound to, drawn by the compositor under the content */
		ca_glass gl[32]; int ng = ca_scene_glass(scene, root, b->h, gl, 32);
		for (int q = 0; q < 32; q++) { if (q < ng) ws_set_window_glass_slot(srv, bound, q, (ws_rect){ gl[q].x, gl[q].y, gl[q].w, gl[q].h }, gl[q].radius, gl[q].mode); else if (ca_glass_used[scene][slot] > q) ws_set_window_glass_slot(srv, bound, q, (ws_rect){ 0, 0, 0, 0 }, 0, 0); }
		ca_glass_used[scene][slot] = ng;
	}
	else ca_paint_scene(scene, b->px, b->w, b->h, (int)b->stride);
	if (!bound && !getenv("WSD_CA_LEVEL")) {
		/* a context that no window of the window server is bound to and that covers the whole screen without a hole is the desktop picture (the Dock draws it: "WallpaperLayer"): it goes below everything else */
		const uint8_t *p = b->px;
		size_t st = b->stride;
		int opaque = b->w > 0 && b->h > 0 && p[3] == 255 && p[(b->w - 1) * 4 + 3] == 255 && p[(b->h - 1) * st + 3] == 255 && p[(b->h - 1) * st + (b->w - 1) * 4 + 3] == 255 && p[(b->h / 2) * st + (b->w / 2) * 4 + 3] == 255;

		ws_set_window_level(srv, ca_cwins[scene][slot], opaque ? -2147483623 /* kCGDesktopWindowLevel: the icons of the Finder (-2147483603) are above */ : 5);
	}
}

static void repaint(ws_server *srv, int sw, int sh)
{
	for (int i = 0; i < ca_nscenes() && i < CA_WINS; i++) {
		int nr = ca_scene_nroots(i);

		if (!ca_dirty[i]) continue;
		ca_dirty[i] = 0;
		if (nr == 0) ca_show(srv, sw, sh, i, CA_CTXS, 0, -1);
		for (int k = 0; k < nr && k < CA_CTXS; k++) ca_show(srv, sw, sh, i, k, ca_scene_root_ctx(i, k), k);
		if (getenv("WSD_CA_DEBUG")) for (int k = 0; k < nr && k < CA_CTXS; k++) { double gx, gy, gw, gh; if (ca_scene_root_geom(i, k, &gx, &gy, &gw, &gh)) fprintf(stderr, "WindowServer:   root %d layer: position %g,%g size %gx%g\n", k, gx, gy, gw, gh); }
		if (getenv("WSD_CA_DEBUG")) for (int k = 0; k < nr && k < CA_CTXS; k++) { uint32_t cx = ca_scene_root_ctx(i, k); fprintf(stderr, "WindowServer:   scene %d root %d: context %u bound to window %u\n", i, k, (unsigned)cx, (unsigned)wsd_layer_ctx_window(cx)); }
		if (getenv("WSD_CA_DEBUG")) { int nl, ni; ca_scene_info(i, &nl, &ni); fprintf(stderr, "WindowServer: scene %d (client %u): %d layers, %d images, %d contexts\n", i, (unsigned)ca_scene_ctx(i), nl, ni, nr); }
	}
	ws_server_invalidate(srv);
}

/* data is readable on the client's descriptor: returns -1 when it closed (the caller drops it from its queue) */
int wsd_ca_read(int fd, ws_server *srv, int sw, int sh)
{
	int ci = -1, fed = 0;
	for (int i = 0; i < CA_MAXC; i++) if (cl[i].fd == fd) ci = i;
	if (ci < 0) return -1;
	for (;;) {
		if (cl[ci].cap - cl[ci].n < 65536) { size_t nc = cl[ci].cap ? cl[ci].cap * 2 : 262144; uint8_t *nb = realloc(cl[ci].buf, nc); if (!nb) return -1; cl[ci].buf = nb; cl[ci].cap = nc; }
		ssize_t r = read(fd, cl[ci].buf + cl[ci].n, cl[ci].cap - cl[ci].n);
		if (r <= 0) {
			if (r < 0 && (errno == EAGAIN || errno == EINTR)) break;
			close(fd); cl[ci].fd = -1; free(cl[ci].buf); cl[ci].buf = NULL; cl[ci].cap = cl[ci].n = 0;
			return -1;
		}
		cl[ci].n += (size_t)r;
		size_t p = 0;
		while (cl[ci].n - p >= 8) {
			uint32_t type, len;
			memcpy(&type, cl[ci].buf + p, 4); memcpy(&len, cl[ci].buf + p + 4, 4);
			if (len > (64u << 20)) { close(fd); cl[ci].fd = -1; return -1; }
			if (cl[ci].n - p - 8 < len) break;
			const uint8_t *pl = cl[ci].buf + p + 8;
			if (type == 3 && len > 4) { uint32_t pid; if (ool_pending >= 0) { ca_set_msg_number(ool_pending); ool_pending = -1; } memcpy(&pid, pl, 4); int si = ca_feed_key(pid, pl + 4, len - 4); if (si >= 0 && si < CA_WINS) { ca_dirty[si] = 1; fed = 1; } }
			else if (type == 1) { if (ool_pending >= 0) { ca_set_msg_number(ool_pending); ool_pending = -1; } int si = ca_feed(pl, len); if (si >= 0 && si < CA_WINS) { ca_dirty[si] = 1; fed = 1; } }
			else if (type == 2 && len >= 8) { uint32_t m, d; memcpy(&m, pl, 4); memcpy(&d, pl + 4, 4); ool_put((int)m, (int)d, pl + 8, len - 8); ool_pending = (int)m; }
			p += 8 + len;
		}
		if (p) { memmove(cl[ci].buf, cl[ci].buf + p, cl[ci].n - p); cl[ci].n -= p; }
		if (r < (ssize_t)(cl[ci].cap - 65536)) break;		/* a short read: nothing more now */
	}
	if (fed) repaint(srv, sw, sh);
	return 0;
}
