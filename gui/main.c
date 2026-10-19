#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "stdgui.h"
#include "platform.h"
#include "zipfs.h"
#include "ps2.h"
#include "drivers.h"

#define path_max 1100
#define ps2_port 1234
#define double_click_ms 450

typedef struct item {
	char *label;
	char *name;
	int is_dir;
	int is_up;
	uint64_t size;
	int zip_index;
} item;

typedef struct pane {
	sg_object *list;
	sg_object *path_edit;
	sg_object *status;
	item *items;
	int count;
	int cap;
	char dir[path_max];
	int last_row;
	uint32_t last_click;
} pane;

typedef struct job {
	char *rel;
	char *local;
	int zip_index;
	int is_dir;
	uint64_t size;
} job;

typedef struct joblist {
	job *jobs;
	int count;
	int cap;
	uint64_t total;
} joblist;

typedef struct progress {
	sg_object *win;
	sg_object *name_label;
	sg_object *stat_label;
	sg_object *bar_file;
	sg_object *bar_all;
	int cancel;
	uint32_t last_draw;
	uint32_t last_speed_time;
	uint64_t last_speed_bytes;
	uint64_t speed;
} progress;

typedef struct dialog {
	sg_object *win;
	sg_object *edit;
	int done;
	int result;
} dialog;

static pane local_pane;
static pane ps2_pane;
static ps2 *conn;
static char conn_ip[64] = "192.168.0.10";
static sg_object *main_win;
static sg_object *top_win;
static sg_object *status_label;
static sg_object *connect_win;
static sg_object *ip_edit;
static sg_object *connect_label;
static sg_object *connect_btn;
static int connecting;
static int connect_cancel;
static int want_quit;
static zip_archive *zip;
static char zip_path[path_max];
static char zip_dir[path_max];
static char neutrino_dir[path_max] = "mc0:/APPS/neutrino";
static char loaded_modules[40][40];
static int loaded_count;
static pane *pending_pane;
static int pending_row;
static int pending_disconnect;
static char launched_message[300];

static const char *ps2_devices[] = {"mc0:/", "mc1:/", "mass0:/", "mass1:/", "mass2:/", "mass3:/", "mmce0:/", "mmce1:/", "cdrom0:/", "rom0:/"};

static sg_object *layout_new(sg_object *parent, int vertical)
{
	sg_object *l = sg_layout_create(parent, vertical, 0);
	sg_layout_set_spacing(l, 3, sg_get_kind(parent) == SG_KIND_WINDOW ? 3 : 0);
	return l;
}

static char *xdup(const char *s)
{
	char *d = malloc(strlen(s) + 1);
	if (d) strcpy(d, s);
	return d;
}

static int ci_compare(const char *a, const char *b)
{
	while (*a && *b) {
		int ca = tolower((unsigned char)*a), cb = tolower((unsigned char)*b);
		if (ca != cb) return ca - cb;
		a++;
		b++;
	}
	return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}

static int ends_with_ci(const char *s, const char *suffix)
{
	size_t a = strlen(s), b = strlen(suffix);
	return a >= b && ci_compare(s + a - b, suffix) == 0;
}

static void items_free(pane *p)
{
	int i;
	for (i = 0; i < p->count; i++) {
		free(p->items[i].label);
		free(p->items[i].name);
	}
	free(p->items);
	p->items = NULL;
	p->count = 0;
	p->cap = 0;
}

static int item_add(pane *p, const char *name, int is_dir, int is_up, uint64_t size, int zip_index)
{
	item *it;
	char label[path_max + 2];
	if (p->count >= p->cap) {
		item *grown;
		int ncap = p->cap ? p->cap * 2 : 128;
		grown = realloc(p->items, sizeof(item) * (size_t)ncap);
		if (!grown) return -1;
		p->items = grown;
		p->cap = ncap;
	}
	snprintf(label, sizeof label, "%s%s", name, is_dir && !is_up && name[strlen(name) - 1] != '/' ? "/" : "");
	it = &p->items[p->count];
	it->label = xdup(label);
	it->name = xdup(name);
	it->is_dir = is_dir;
	it->is_up = is_up;
	it->size = size;
	it->zip_index = zip_index;
	if (!it->label || !it->name) return -1;
	p->count++;
	return 0;
}

static int item_compare(const void *a, const void *b)
{
	const item *x = a, *y = b;
	if (x->is_dir != y->is_dir) return y->is_dir - x->is_dir;
	return ci_compare(x->name, y->name);
}

static void items_sort(pane *p, int start)
{
	int i, out;
	if (p->count - start < 2) return;
	qsort(p->items + start, (size_t)(p->count - start), sizeof(item), item_compare);
	out = start;
	for (i = start; i < p->count; i++) {
		if (out > start && p->items[i].is_dir && p->items[out - 1].is_dir && strcmp(p->items[i].name, p->items[out - 1].name) == 0) {
			free(p->items[i].label);
			free(p->items[i].name);
			continue;
		}
		p->items[out++] = p->items[i];
	}
	p->count = out;
}

static const char *list_text(sg_object *list, int index, void *user)
{
	pane *p = user;
	(void)list;
	if (index < 0 || index >= p->count) return "";
	return p->items[index].label;
}

static void pane_update_status(pane *p)
{
	int i, selected = 0;
	uint64_t total = 0;
	char size_text[32], text[128];
	if (!p->status) return;
	for (i = 0; i < p->count; i++) {
		if (p->items[i].is_up || !sg_list_is_selected(p->list, i)) continue;
		selected++;
		total += p->items[i].size;
	}
	if (selected == 0) {
		snprintf(text, sizeof text, "%d items", p->count - (p->count > 0 && p->items[0].is_up ? 1 : 0));
	} else if (total > 0) {
		pf_format_size(total, size_text, sizeof size_text);
		snprintf(text, sizeof text, "%d selected (%s)", selected, size_text);
	} else {
		snprintf(text, sizeof text, "%d selected", selected);
	}
	sg_set_text(p->status, text);
}

static void pane_show(pane *p)
{
	sg_list_set_virtual(p->list, p->count, list_text, p);
	sg_set_scroll(p->list, 0, 0);
	p->last_row = -1;
	pane_update_status(p);
}

static void set_status(const char *text)
{
	if (status_label) sg_set_text(status_label, text);
}

static int run_dialog(const char *title, const char *text, const char *yes, const char *no, int with_edit, char *edit_text, int edit_size);

static void message_box(const char *text)
{
	run_dialog("ps2netcon", text, "OK", NULL, 0, NULL, 0);
}

static int confirm_box(const char *text)
{
	return run_dialog("ps2netcon", text, "Yes", "No", 0, NULL, 0);
}

static int dialog_event(sg_object *self, const sg_event *ev, void *user)
{
	dialog *d = user;
	(void)self;
	if (ev->type == SG_EVENT_CLOSE) {
		d->result = 0;
		d->done = 1;
	}
	if (ev->type == SG_EVENT_KEY_DOWN && ev->key == SG_KEY_ENTER && d->edit && ev->target == d->edit) {
		d->result = 1;
		d->done = 1;
		return SG_CANCEL;
	}
	return 0;
}

static void dialog_yes(sg_object *b, void *user)
{
	dialog *d = user;
	(void)b;
	d->result = 1;
	d->done = 1;
}

static void dialog_no(sg_object *b, void *user)
{
	dialog *d = user;
	(void)b;
	d->result = 0;
	d->done = 1;
}

static int run_dialog(const char *title, const char *text, const char *yes, const char *no, int with_edit, char *edit_text, int edit_size)
{
	dialog d;
	sg_object *col, *row, *blocked = top_win;
	memset(&d, 0, sizeof d);
	d.win = sg_window_create(title, 400, 0);
	if (!d.win) return 0;
	sg_set_event_cb(d.win, dialog_event, &d);
	col = layout_new(d.win, 1);
	sg_label_create(col, text, 1);
	if (with_edit) {
		d.edit = sg_edit_create(col, 0);
		sg_set_text(d.edit, edit_text);
	}
	row = layout_new(col, 0);
	sg_button_create(row, yes, dialog_yes, &d);
	if (no) sg_button_create(row, no, dialog_no, &d);
	if (blocked) sg_window_set_blocker(blocked, d.win);
	if (d.edit) sg_set_focus(d.edit);
	while (!d.done) if (!sg_poll(1)) break;
	if (with_edit && d.result) snprintf(edit_text, (size_t)edit_size, "%s", sg_get_text(d.edit));
	if (d.win) sg_destroy(d.win);
	return d.result;
}

static void draw_bar(sg_object *bar, uint64_t done, uint64_t total)
{
	int w, h, x, y, fill;
	uint32_t *px = sg_surface_pixels(bar, &w, &h);
	if (!px) return;
	fill = total ? (int)((done * (uint64_t)(w - 2)) / total) : 0;
	if (fill > w - 2) fill = w - 2;
	for (y = 0; y < h; y++) {
		for (x = 0; x < w; x++) {
			uint32_t c = 0xFFFFFFFFu;
			if (x == 0 || y == 0 || x == w - 1 || y == h - 1) c = 0xFF808080u;
			else if (x - 1 < fill) c = 0xFF000080u;
			px[y * w + x] = c;
		}
	}
	sg_surface_update(bar);
}

static void format_time(uint64_t seconds, char *out, int size)
{
	unsigned s = (unsigned)seconds;
	if (s >= 3600) snprintf(out, (size_t)size, "%u:%02u:%02u", s / 3600, (s / 60) % 60, s % 60);
	else snprintf(out, (size_t)size, "%u:%02u", s / 60, s % 60);
}

static int progress_event(sg_object *self, const sg_event *ev, void *user)
{
	progress *pr = user;
	(void)self;
	if (ev->type == SG_EVENT_CLOSE) {
		pr->cancel = 1;
		return SG_CANCEL;
	}
	return 0;
}

static void progress_cancel(sg_object *b, void *user)
{
	progress *pr = user;
	(void)b;
	pr->cancel = 1;
}

static int progress_pump(void *user)
{
	progress *pr = user;
	if (!sg_poll(0)) return 1;
	return pr->cancel;
}

static progress *progress_open(const char *title, int bars)
{
	progress *pr = calloc(1, sizeof *pr);
	sg_object *col;
	if (!pr) return NULL;
	pr->win = sg_window_create(title, 440, 0);
	sg_set_event_cb(pr->win, progress_event, pr);
	col = layout_new(pr->win, 1);
	pr->name_label = sg_label_create(col, "", 0);
	if (bars) {
		pr->bar_file = sg_surface_create(col, 400, 16, SG_ALPHA_NONE);
		pr->stat_label = sg_label_create(col, "", 0);
		pr->bar_all = sg_surface_create(col, 400, 16, SG_ALPHA_NONE);
		draw_bar(pr->bar_file, 0, 1);
		draw_bar(pr->bar_all, 0, 1);
	}
	sg_button_create(col, "Cancel", progress_cancel, pr);
	if (top_win) sg_window_set_blocker(top_win, pr->win);
	pr->last_speed_time = pf_ticks_ms();
	top_win = pr->win;
	sg_poll(0);
	return pr;
}

static void progress_close(progress *pr)
{
	if (!pr) return;
	top_win = main_win;
	if (pr->win) sg_destroy(pr->win);
	free(pr);
}

static void progress_update(progress *pr, const char *name, uint64_t file_done, uint64_t file_total, uint64_t all_done, uint64_t all_total, int force)
{
	uint32_t now = pf_ticks_ms();
	char a[32], b[32], rate[32], eta[32], text[200];
	if (!force && now - pr->last_draw < 120) return;
	pr->last_draw = now;
	if (now - pr->last_speed_time >= 500) {
		uint64_t inst = (all_done - pr->last_speed_bytes) * 1000u / (now - pr->last_speed_time);
		pr->speed = pr->speed ? (pr->speed * 2u + inst) / 3u : inst;
		pr->last_speed_time = now;
		pr->last_speed_bytes = all_done;
	}
	sg_set_text(pr->name_label, name);
	if (pr->bar_file) {
		draw_bar(pr->bar_file, file_done, file_total);
		draw_bar(pr->bar_all, all_done, all_total);
		pf_format_size(all_done, a, sizeof a);
		pf_format_size(all_total, b, sizeof b);
		pf_format_size(pr->speed, rate, sizeof rate);
		if (pr->speed > 0 && all_total > all_done) format_time((all_total - all_done) / pr->speed, eta, sizeof eta);
		else snprintf(eta, sizeof eta, "--:--");
		snprintf(text, sizeof text, "%s / %s  -  %s/s  -  ETA %s", a, b, rate, eta);
		sg_set_text(pr->stat_label, text);
	}
	sg_poll(0);
}

static void join_path(char *out, int size, const char *dir, const char *name)
{
	snprintf(out, (size_t)size, "%s%s", dir, name);
}

static int local_is_root(const char *d)
{
	if (pf_has_roots()) return strlen(d) == 3 && d[1] == ':' && d[2] == '/';
	return strcmp(d, "/") == 0;
}

static void local_parent(char *d)
{
	size_t n;
	if (local_is_root(d)) {
		if (pf_has_roots()) d[0] = 0;
		return;
	}
	n = strlen(d);
	if (n > 0 && d[n - 1] == '/') d[--n] = 0;
	while (n > 0 && d[n - 1] != '/') d[--n] = 0;
	if (n == 0) snprintf(d, 2, "/");
}

static void zip_parent(char *d)
{
	size_t n = strlen(d);
	if (n > 0 && d[n - 1] == '/') d[--n] = 0;
	while (n > 0 && d[n - 1] != '/') d[--n] = 0;
}

static void local_set_path_text(void)
{
	char text[2 * path_max + 4];
	if (zip) snprintf(text, sizeof text, "%s!/%s", zip_path, zip_dir);
	else snprintf(text, sizeof text, "%s", local_pane.dir);
	sg_set_text(local_pane.path_edit, text);
}

static int local_load(void)
{
	pane *p = &local_pane;
	items_free(p);
	if (zip) {
		int i, start;
		size_t dl = strlen(zip_dir);
		item_add(p, "..", 1, 1, 0, -1);
		start = p->count;
		for (i = 0; i < zip->count; i++) {
			const zip_entry *e = &zip->entries[i];
			const char *rest;
			char *slash;
			if (strncmp(e->name, zip_dir, dl) != 0) continue;
			rest = e->name + dl;
			if (!*rest) continue;
			slash = strchr(rest, '/');
			if (slash) {
				char name[path_max];
				size_t n = (size_t)(slash - rest);
				if (n == 0 || n >= sizeof name) continue;
				memcpy(name, rest, n);
				name[n] = 0;
				item_add(p, name, 1, 0, 0, -1);
			} else {
				item_add(p, rest, 0, 0, e->size, i);
			}
		}
		items_sort(p, start);
	} else if (p->dir[0] == 0) {
		pf_entry *e;
		int n, i;
		pf_list_roots(&e, &n);
		for (i = 0; i < n; i++) item_add(p, e[i].name, 1, 0, 0, -1);
		pf_free_entries(e, n);
		items_sort(p, 0);
	} else {
		pf_entry *e;
		int n, i;
		if (pf_list_dir(p->dir, &e, &n)) return -1;
		item_add(p, "..", 1, 1, 0, -1);
		for (i = 0; i < n; i++) item_add(p, e[i].name, e[i].is_dir, 0, e[i].size, -1);
		pf_free_entries(e, n);
		items_sort(p, 1);
		if (local_is_root(p->dir) && !pf_has_roots()) {
			free(p->items[0].label);
			free(p->items[0].name);
			memmove(p->items, p->items + 1, sizeof(item) * (size_t)(p->count - 1));
			p->count--;
		}
	}
	local_set_path_text();
	pane_show(p);
	return 0;
}

static void local_close_zip(void)
{
	if (zip) zip_close(zip);
	zip = NULL;
	zip_path[0] = 0;
	zip_dir[0] = 0;
}

static int local_open_zip(const char *path, const char *inner)
{
	char err[128], text[256];
	zip_archive *z = zip_open(path, err, sizeof err);
	if (!z) {
		snprintf(text, sizeof text, "Cannot open zip: %s", err);
		set_status(text);
		return -1;
	}
	local_close_zip();
	zip = z;
	snprintf(zip_path, sizeof zip_path, "%s", path);
	snprintf(zip_dir, sizeof zip_dir, "%s", inner);
	return 0;
}

static void local_go_dir(const char *dir)
{
	char previous[path_max];
	snprintf(previous, sizeof previous, "%s", local_pane.dir);
	snprintf(local_pane.dir, sizeof local_pane.dir, "%s", dir);
	if (local_load()) {
		snprintf(local_pane.dir, sizeof local_pane.dir, "%s", previous);
		local_load();
		set_status("Cannot read folder");
	}
}

static void local_up(void)
{
	if (zip) {
		if (zip_dir[0]) {
			zip_parent(zip_dir);
		} else {
			local_close_zip();
		}
		local_load();
		return;
	}
	if (local_pane.dir[0] == 0) return;
	local_parent(local_pane.dir);
	local_load();
}

static void local_open_item(int row)
{
	item *it;
	char path[path_max];
	if (row < 0 || row >= local_pane.count) return;
	it = &local_pane.items[row];
	if (it->is_up) {
		local_up();
		return;
	}
	if (zip) {
		if (it->is_dir) {
			char next[path_max];
			snprintf(next, sizeof next, "%s%s/", zip_dir, it->name);
			snprintf(zip_dir, sizeof zip_dir, "%s", next);
			local_load();
		}
		return;
	}
	if (it->is_dir) {
		if (local_pane.dir[0] == 0) snprintf(path, sizeof path, "%s/", it->name);
		else snprintf(path, sizeof path, "%s%s/", local_pane.dir, it->name);
		local_go_dir(path);
		return;
	}
	if (ends_with_ci(it->name, ".zip")) {
		join_path(path, sizeof path, local_pane.dir, it->name);
		if (local_open_zip(path, "") == 0) local_load();
	}
}

static void local_go_text(const char *text)
{
	char path[path_max], *bang;
	size_t n;
	int i;
	snprintf(path, sizeof path, "%s", text);
	for (i = 0; path[i]; i++) if (path[i] == '\\') path[i] = '/';
	bang = strstr(path, "!/");
	if (bang) {
		char inner[path_max];
		size_t il;
		snprintf(inner, sizeof inner, "%s", bang + 2);
		*bang = 0;
		il = strlen(inner);
		if (il > 0 && inner[il - 1] != '/') {
			inner[il++] = '/';
			inner[il] = 0;
		}
		if (local_open_zip(path, inner) == 0) {
			if (local_load()) {
				zip_dir[0] = 0;
				local_load();
			}
		}
		return;
	}
	n = strlen(path);
	if (n > 0 && ends_with_ci(path, ".zip")) {
		if (local_open_zip(path, "") == 0) local_load();
		return;
	}
	if (n == 0 && !pf_has_roots()) snprintf(path, sizeof path, "/");
	else if (n > 0 && path[n - 1] != '/') {
		path[n++] = '/';
		path[n] = 0;
	}
	if (n == 2 && path[1] == ':') {
		path[n++] = '/';
		path[n] = 0;
	}
	local_close_zip();
	local_go_dir(path);
}

static int ps2_is_root(const char *d)
{
	size_t n = strlen(d);
	return n >= 2 && d[n - 1] == '/' && d[n - 2] == ':';
}

static int ps2_parse_list(const char *body, pane *p)
{
	const char *s = body;
	char line[path_max];
	if (strstr(body, "opendir() failed")) return -1;
	while (*s) {
		const char *e = strchr(s, '\n');
		size_t n = e ? (size_t)(e - s) : strlen(s);
		int is_dir = 0;
		if (n >= sizeof line) n = sizeof line - 1;
		memcpy(line, s, n);
		line[n] = 0;
		while (n > 0 && line[n - 1] == '\r') line[--n] = 0;
		if (n > 0 && line[n - 1] == '/') {
			is_dir = 1;
			line[--n] = 0;
		}
		if (n > 0) item_add(p, line, is_dir, 0, 0, -1);
		if (!e) break;
		s = e + 1;
	}
	return 0;
}

static void go_disconnected(const char *msg);
static void show_connect_window(const char *msg);

static int ps2_failed(int r)
{
	if (r == ps2_ok) return 0;
	if (r == ps2_cancel) return 1;
	pending_disconnect = 1;
	return 1;
}

static int ps2_load(void)
{
	pane *p = &ps2_pane;
	char *body = NULL, path[path_max];
	int r, i;
	items_free(p);
	if (p->dir[0] == 0) {
		for (i = 0; i < (int)(sizeof ps2_devices / sizeof ps2_devices[0]); i++) item_add(p, ps2_devices[i], 1, 0, 0, -1);
		sg_set_text(p->path_edit, "");
		pane_show(p);
		return 0;
	}
	snprintf(path, sizeof path, "%s", p->dir);
	if (!ps2_is_root(path) && path[strlen(path) - 1] == '/') path[strlen(path) - 1] = 0;
	r = ps2_list(conn, path, &body);
	if (r != ps2_ok) {
		free(body);
		ps2_failed(r);
		return -1;
	}
	item_add(p, "..", 1, 1, 0, -1);
	if (ps2_parse_list(body, p)) {
		free(body);
		return -1;
	}
	free(body);
	items_sort(p, 1);
	sg_set_text(p->path_edit, p->dir);
	pane_show(p);
	return 0;
}

static void ps2_go_dir(const char *dir)
{
	char previous[path_max];
	snprintf(previous, sizeof previous, "%s", ps2_pane.dir);
	snprintf(ps2_pane.dir, sizeof ps2_pane.dir, "%s", dir);
	if (ps2_load()) {
		snprintf(ps2_pane.dir, sizeof ps2_pane.dir, "%s", previous);
		if (!pending_disconnect) {
			ps2_load();
			set_status("Cannot read PS2 folder (is the device driver loaded?)");
		}
	}
}

static void ps2_up(void)
{
	char *d = ps2_pane.dir;
	size_t n;
	if (d[0] == 0) return;
	if (ps2_is_root(d)) {
		d[0] = 0;
		ps2_load();
		return;
	}
	n = strlen(d);
	if (n > 0 && d[n - 1] == '/') d[--n] = 0;
	while (n > 0 && d[n - 1] != '/') d[--n] = 0;
	ps2_load();
}

static void ps2_open_item(int row)
{
	item *it;
	char path[path_max];
	if (row < 0 || row >= ps2_pane.count) return;
	it = &ps2_pane.items[row];
	if (it->is_up) {
		ps2_up();
		return;
	}
	if (!it->is_dir) return;
	if (ps2_pane.dir[0] == 0) snprintf(path, sizeof path, "%s", it->name);
	else snprintf(path, sizeof path, "%s%s/", ps2_pane.dir, it->name);
	ps2_go_dir(path);
}

static void ps2_go_text(const char *text)
{
	char path[path_max];
	size_t n;
	int i;
	snprintf(path, sizeof path, "%s", text);
	for (i = 0; path[i]; i++) if (path[i] == '\\') path[i] = '/';
	n = strlen(path);
	if (n == 0) {
		ps2_go_dir("");
		return;
	}
	if (!strchr(path, ':')) {
		set_status("PS2 path must start with a device, like mass0:/");
		return;
	}
	if (path[n - 1] == ':') path[n++] = '/';
	else if (path[n - 1] != '/') path[n++] = '/';
	path[n] = 0;
	ps2_go_dir(path);
}

static int selected_rows(pane *p, int **out)
{
	int i, n = 0;
	int *rows = malloc(sizeof(int) * (size_t)(p->count + 1));
	if (!rows) return 0;
	for (i = 0; i < p->count; i++) if (!p->items[i].is_up && sg_list_is_selected(p->list, i)) rows[n++] = i;
	*out = rows;
	return n;
}

static void joblist_free(joblist *jl)
{
	int i;
	for (i = 0; i < jl->count; i++) {
		free(jl->jobs[i].rel);
		free(jl->jobs[i].local);
	}
	free(jl->jobs);
	memset(jl, 0, sizeof *jl);
}

static int job_add(joblist *jl, const char *rel, const char *local, int zip_index, int is_dir, uint64_t size)
{
	job *j;
	if (jl->count >= jl->cap) {
		job *grown;
		int ncap = jl->cap ? jl->cap * 2 : 128;
		grown = realloc(jl->jobs, sizeof(job) * (size_t)ncap);
		if (!grown) return -1;
		jl->jobs = grown;
		jl->cap = ncap;
	}
	j = &jl->jobs[jl->count];
	j->rel = xdup(rel);
	j->local = local ? xdup(local) : NULL;
	j->zip_index = zip_index;
	j->is_dir = is_dir;
	j->size = size;
	if (!j->rel) return -1;
	jl->count++;
	if (!is_dir) jl->total += size;
	return 0;
}

static void walk_local(const char *dir_path, const char *rel_prefix, joblist *jl, int depth)
{
	pf_entry *e;
	int n, i;
	char path[path_max], rel[path_max];
	if (depth > 40 || pf_list_dir(dir_path, &e, &n)) return;
	for (i = 0; i < n; i++) {
		snprintf(rel, sizeof rel, "%s%s", rel_prefix, e[i].name);
		if (e[i].is_dir) {
			job_add(jl, rel, NULL, -1, 1, 0);
			snprintf(path, sizeof path, "%s%s/", dir_path, e[i].name);
			snprintf(rel, sizeof rel, "%s%s/", rel_prefix, e[i].name);
			walk_local(path, rel, jl, depth + 1);
		} else {
			snprintf(path, sizeof path, "%s%s", dir_path, e[i].name);
			job_add(jl, rel, path, -1, 0, e[i].size);
		}
	}
	pf_free_entries(e, n);
}

static void walk_zip(const char *prefix, size_t strip, joblist *jl)
{
	int i;
	size_t pl = strlen(prefix);
	char last_parent[path_max];
	last_parent[0] = 0;
	for (i = 0; i < zip->count; i++) {
		const zip_entry *e = &zip->entries[i];
		char rel[path_max], *slash;
		size_t n;
		if (strncmp(e->name, prefix, pl) != 0 || strlen(e->name) <= pl) continue;
		snprintf(rel, sizeof rel, "%s", e->name + strip);
		n = strlen(rel);
		if (e->is_dir) {
			if (n > 0) rel[n - 1] = 0;
			job_add(jl, rel, NULL, -1, 1, 0);
			continue;
		}
		slash = strrchr(rel, '/');
		if (slash) {
			char parent[path_max], *c;
			size_t pn = (size_t)(slash - rel);
			memcpy(parent, rel, pn);
			parent[pn] = 0;
			if (strcmp(parent, last_parent) != 0) {
				snprintf(last_parent, sizeof last_parent, "%s", parent);
				for (c = parent; *c; c++) {
					if (*c == '/') {
						*c = 0;
						job_add(jl, parent, NULL, -1, 1, 0);
						*c = '/';
					}
				}
				job_add(jl, parent, NULL, -1, 1, 0);
			}
		}
		job_add(jl, rel, NULL, i, 0, e->size);
	}
}

static int reconnect(progress *pr)
{
	char err[128];
	char ip[64];
	snprintf(ip, sizeof ip, "%s", ps2_ip(conn));
	ps2_close(conn);
	conn = NULL;
	if (ps2_connect(&conn, ip, ps2_port, progress_pump, pr, err, sizeof err) != ps2_ok) {
		conn = NULL;
		return -1;
	}
	ps2_set_pump(conn, progress_pump, pr);
	return 0;
}

static int local_row_exists_on_ps2(const char *name)
{
	int i;
	for (i = 0; i < ps2_pane.count; i++) if (!ps2_pane.items[i].is_up && ci_compare(ps2_pane.items[i].name, name) == 0) return 1;
	return 0;
}

static void finish_failed(progress *pr, const char *text, int fatal)
{
	char msg[400];
	if (!fatal && conn && reconnect(pr) == 0) {
		progress_close(pr);
		message_box(text);
		return;
	}
	progress_close(pr);
	snprintf(msg, sizeof msg, "%s", text);
	message_box(msg);
	pending_disconnect = 1;
}

static void do_send(sg_object *b, void *user)
{
	int *rows = NULL, n, i, check = 0;
	joblist jl;
	progress *pr;
	uint8_t *buf;
	uint64_t done_all = 0;
	uint32_t t0;
	char dest[path_max + path_max], quoted[2 * path_max], cmd[3 * path_max], text[400], size_text[32], rate[32];
	(void)b;
	(void)user;
	if (!conn) return;
	if (ps2_pane.dir[0] == 0) {
		set_status("Open a PS2 device folder first");
		return;
	}
	n = selected_rows(&local_pane, &rows);
	if (n == 0) {
		free(rows);
		set_status("Select files on the computer side first");
		return;
	}
	memset(&jl, 0, sizeof jl);
	for (i = 0; i < n; i++) {
		item *it = &local_pane.items[rows[i]];
		char path[path_max];
		if (local_row_exists_on_ps2(it->name)) check++;
		if (zip) {
			if (it->is_dir) {
				char prefix[path_max];
				job_add(&jl, it->name, NULL, -1, 1, 0);
				snprintf(prefix, sizeof prefix, "%s%s/", zip_dir, it->name);
				walk_zip(prefix, strlen(zip_dir), &jl);
			} else {
				job_add(&jl, it->name, NULL, it->zip_index, 0, it->size);
			}
		} else if (local_pane.dir[0] == 0) {
			continue;
		} else if (it->is_dir) {
			char rel[path_max];
			job_add(&jl, it->name, NULL, -1, 1, 0);
			snprintf(path, sizeof path, "%s%s/", local_pane.dir, it->name);
			snprintf(rel, sizeof rel, "%s/", it->name);
			walk_local(path, rel, &jl, 0);
		} else {
			join_path(path, sizeof path, local_pane.dir, it->name);
			job_add(&jl, it->name, path, -1, 0, it->size);
		}
	}
	free(rows);
	if (jl.count == 0) {
		joblist_free(&jl);
		return;
	}
	if (check) {
		snprintf(text, sizeof text, "%d item(s) already exist on the PS2 and will be overwritten. Continue?", check);
		if (!confirm_box(text)) {
			joblist_free(&jl);
			return;
		}
	}
	buf = malloc(ps2_io_buffer);
	pr = progress_open("Sending to PS2", 1);
	if (!buf || !pr) {
		free(buf);
		progress_close(pr);
		joblist_free(&jl);
		return;
	}
	ps2_set_pump(conn, progress_pump, pr);
	t0 = pf_ticks_ms();
	for (i = 0; i < jl.count; i++) {
		job *j = &jl.jobs[i];
		char *body = NULL;
		int r;
		join_path(dest, sizeof dest, ps2_pane.dir, j->rel);
		progress_update(pr, j->rel, 0, j->size, done_all, jl.total, 1);
		if (j->is_dir) {
			ps2_quote(dest, quoted, sizeof quoted);
			snprintf(cmd, sizeof cmd, "mkdir %s", quoted);
			r = ps2_exec(conn, cmd, &body);
			free(body);
			if (r != ps2_ok) goto transfer_error;
			continue;
		} else {
			pf_file *pf = NULL;
			zip_stream *zs = NULL;
			uint64_t sent = 0;
			char err[128];
			if (j->zip_index >= 0) {
				zs = zip_stream_open(zip, j->zip_index, err, sizeof err);
				if (!zs) {
					snprintf(text, sizeof text, "Cannot read '%s' from the zip: %s", j->rel, err);
					finish_failed(pr, text, 0);
					goto transfer_end;
				}
			} else {
				pf = pf_open(j->local);
				if (!pf) {
					snprintf(text, sizeof text, "Cannot open '%s'", j->local);
					finish_failed(pr, text, 0);
					goto transfer_end;
				}
			}
			r = ps2_recv_begin(conn, dest, j->size);
			while (r == ps2_ok && sent < j->size) {
				uint64_t left = j->size - sent;
				long want = left < ps2_io_buffer ? (long)left : ps2_io_buffer, got = 0;
				if (zs) {
					while (got < want) {
						long g = zip_stream_read(zs, buf + got, want - got);
						if (g <= 0) {
							if (g < 0) {
								snprintf(text, sizeof text, "Reading '%s' failed: %s", j->rel, zip_stream_error(zs));
								zip_stream_close(zs);
								finish_failed(pr, text, 0);
								goto transfer_end;
							}
							break;
						}
						got += g;
					}
				} else {
					got = pf_read(pf, buf, want);
				}
				if (got <= 0) {
					snprintf(text, sizeof text, "Reading '%s' failed (file changed?)", j->rel);
					if (zs) zip_stream_close(zs);
					if (pf) pf_close(pf);
					finish_failed(pr, text, 0);
					goto transfer_end;
				}
				r = ps2_send_data(conn, buf, got);
				sent += (uint64_t)got;
				done_all += (uint64_t)got;
				progress_update(pr, j->rel, sent, j->size, done_all, jl.total, 0);
			}
			if (zs) zip_stream_close(zs);
			if (pf) pf_close(pf);
			if (r == ps2_ok) r = ps2_recv_end(conn, &body);
			if (r == ps2_ok && body && strstr(body, "failed")) {
				snprintf(text, sizeof text, "PS2 reported an error for '%s':\n%s", j->rel, body);
				free(body);
				finish_failed(pr, text, 0);
				goto transfer_end;
			}
			free(body);
			if (r != ps2_ok) goto transfer_error;
		}
		continue;
transfer_error:
		if (pr->cancel) snprintf(text, sizeof text, "Cancelled. The partially sent file remains on the PS2.");
		else snprintf(text, sizeof text, "Transfer of '%s' failed: the PS2 closed the connection.\nCheck that the target path exists and the driver for it is loaded.", j->rel);
		finish_failed(pr, text, 0);
		goto transfer_end;
	}
	{
		uint32_t ms = pf_ticks_ms() - t0;
		uint64_t avg = ms ? jl.total * 1000u / ms : 0;
		pf_format_size(jl.total, size_text, sizeof size_text);
		pf_format_size(avg, rate, sizeof rate);
		snprintf(text, sizeof text, "Sent %s in %u.%u s (%s/s)", size_text, (unsigned)(ms / 1000), (unsigned)((ms % 1000) / 100), rate);
	}
	ps2_set_pump(conn, NULL, NULL);
	progress_close(pr);
	pr = NULL;
	ps2_load();
	set_status(text);
transfer_end:
	if (conn) ps2_set_pump(conn, NULL, NULL);
	free(buf);
	joblist_free(&jl);
}

static void do_delete(sg_object *b, void *user)
{
	int *rows = NULL, n, i;
	progress *pr;
	char text[400], quoted[2 * path_max], cmd[3 * path_max], path[2 * path_max];
	(void)b;
	(void)user;
	if (!conn || ps2_pane.dir[0] == 0) return;
	n = selected_rows(&ps2_pane, &rows);
	if (n == 0) {
		free(rows);
		set_status("Select items on the PS2 side first");
		return;
	}
	snprintf(text, sizeof text, "Delete %d item(s) from the PS2? Folders are deleted with all their contents.", n);
	if (!confirm_box(text)) {
		free(rows);
		return;
	}
	pr = progress_open("Deleting", 0);
	if (!pr) {
		free(rows);
		return;
	}
	ps2_set_pump(conn, progress_pump, pr);
	for (i = 0; i < n; i++) {
		item *it = &ps2_pane.items[rows[i]];
		char *body = NULL;
		int r;
		join_path(path, sizeof path, ps2_pane.dir, it->name);
		sg_set_text(pr->name_label, it->name);
		sg_poll(0);
		if (it->is_dir) {
			r = ps2_rmdir(conn, path, &body);
		} else {
			ps2_quote(path, quoted, sizeof quoted);
			snprintf(cmd, sizeof cmd, "rm %s", quoted);
			r = ps2_exec(conn, cmd, &body);
		}
		if (r != ps2_ok) {
			free(body);
			finish_failed(pr, pr->cancel ? "Cancelled." : "Connection to the PS2 was lost.", pr->cancel ? 0 : 1);
			pr = NULL;
			break;
		}
		if (strstr(body, "failed")) {
			snprintf(text, sizeof text, "Could not delete '%s'.", it->name);
			free(body);
			ps2_set_pump(conn, NULL, NULL);
			progress_close(pr);
			pr = NULL;
			message_box(text);
			break;
		}
		free(body);
	}
	free(rows);
	if (pr) {
		ps2_set_pump(conn, NULL, NULL);
		progress_close(pr);
	}
	if (conn) {
		ps2_set_pump(conn, NULL, NULL);
		ps2_load();
	}
}

static void do_new_folder(sg_object *b, void *user)
{
	char name[256] = "", path[2 * path_max], quoted[2 * path_max], cmd[3 * path_max], *body = NULL;
	int r;
	(void)b;
	(void)user;
	if (!conn || ps2_pane.dir[0] == 0) return;
	if (!run_dialog("New folder", "Folder name:", "OK", "Cancel", 1, name, sizeof name) || !name[0]) return;
	join_path(path, sizeof path, ps2_pane.dir, name);
	ps2_quote(path, quoted, sizeof quoted);
	snprintf(cmd, sizeof cmd, "mkdir %s", quoted);
	r = ps2_exec(conn, cmd, &body);
	if (r != ps2_ok) {
		free(body);
		ps2_failed(r);
		return;
	}
	if (strstr(body, "failed")) set_status("Could not create folder");
	free(body);
	ps2_load();
}

typedef struct drvwin {
	sg_object *win;
	sg_object *list;
	sg_object *dir_edit;
	sg_object *log;
	char text[8192];
	int busy;
	int flat;
	drv_set set;
	int visible[256];
	int visible_count;
	char **names;
} drvwin;

static void drv_log(drvwin *d, const char *line)
{
	size_t n = strlen(d->text);
	if (n + strlen(line) + 2 >= sizeof d->text) {
		d->text[0] = 0;
		n = 0;
	}
	snprintf(d->text + n, sizeof d->text - n, "%s\n", line);
	sg_set_text(d->log, d->text);
	sg_poll(0);
}

static int drv_loaded(const char *name)
{
	int i;
	for (i = 0; i < loaded_count; i++) if (strcmp(loaded_modules[i], name) == 0) return 1;
	return 0;
}

static void drv_clear_list(drvwin *d)
{
	int i;
	sg_list_set_items(d->list, NULL, 0);
	for (i = 0; i < d->visible_count; i++) free(d->names[i]);
	free(d->names);
	d->names = NULL;
	d->visible_count = 0;
	drv_free(&d->set);
}

static int drv_fetch(drvwin *d, const char *dir, const char *file, char **text)
{
	char path[2 * path_max];
	uint64_t length = 0;
	int r;
	snprintf(path, sizeof path, "%s/%s", dir, file);
	r = ps2_len(conn, path, &length);
	if (r != ps2_ok) return r;
	if (length > 65536) return ps2_err - 100;
	r = ps2_cat(conn, path, 0, length, text);
	(void)d;
	return r;
}

static void drv_reload(drvwin *d)
{
	char dir[path_max], config_dir[path_max], *body = NULL, line[400];
	const char *s;
	int i, r, loaded = 0, shown = 0;
	drv_clear_list(d);
	snprintf(dir, sizeof dir, "%s", sg_get_text(d->dir_edit));
	i = (int)strlen(dir);
	while (i > 0 && (dir[i - 1] == '/' || dir[i - 1] == '\\')) dir[--i] = 0;
	snprintf(neutrino_dir, sizeof neutrino_dir, "%s", dir);
	snprintf(config_dir, sizeof config_dir, "%s/config", dir);
	d->flat = 0;
	r = ps2_list(conn, config_dir, &body);
	if (r == ps2_ok && strstr(body, "opendir() failed")) {
		free(body);
		body = NULL;
		snprintf(config_dir, sizeof config_dir, "%s", dir);
		d->flat = 1;
		r = ps2_list(conn, config_dir, &body);
	}
	if (r != ps2_ok) {
		free(body);
		pending_disconnect = 1;
		return;
	}
	if (strstr(body, "opendir() failed")) {
		free(body);
		snprintf(line, sizeof line, "Cannot read %s/config on the PS2.", dir);
		drv_log(d, line);
		return;
	}
	s = body;
	while (*s) {
		const char *e = strchr(s, '\n');
		size_t n = e ? (size_t)(e - s) : strlen(s);
		char file[300], id[300];
		if (n >= sizeof file) n = sizeof file - 1;
		memcpy(file, s, n);
		file[n] = 0;
		while (n > 0 && file[n - 1] == '\r') file[--n] = 0;
		if (n > 5 && ends_with_ci(file, ".toml") && (strncmp(file, "bsd", 3) == 0 || strncmp(file, "i_", 2) == 0)) {
			char *text = NULL;
			snprintf(id, sizeof id, "%s", file);
			id[n - 5] = 0;
			r = drv_fetch(d, config_dir, file, &text);
			if (r == ps2_ok) {
				drv_add(&d->set, id, text);
				loaded++;
				free(text);
			} else if (r == ps2_err - 100) {
				drv_log(d, "This ps2netcon is too old: it needs the len and cat commands. Update it first.");
				free(body);
				return;
			} else if (r != ps2_ok) {
				free(body);
				pending_disconnect = 1;
				return;
			}
		}
		if (!e) break;
		s = e + 1;
	}
	free(body);
	drv_finish(&d->set);
	d->names = calloc((size_t)d->set.count + 1, sizeof(char *));
	for (i = 0; i < d->set.count && d->names && d->visible_count < 256; i++) {
		drv_driver *dr = &d->set.items[i];
		char label[400];
		if (dr->kind == drv_kind_internal || dr->hidden) continue;
		snprintf(label, sizeof label, "%s (%s)", dr->name, dr->id);
		d->names[d->visible_count] = xdup(label);
		d->visible[d->visible_count++] = i;
		shown++;
	}
	sg_list_set_items(d->list, (const char *const *)d->names, d->visible_count);
	snprintf(line, sizeof line, "Read %d config files from %s, %d drivers listed.", loaded, config_dir, shown);
	drv_log(d, line);
}

static void drv_reload_btn(sg_object *b, void *user)
{
	drvwin *d = user;
	(void)b;
	if (!d->busy && conn) drv_reload(d);
}

static void drv_load(sg_object *b, void *user)
{
	drvwin *d = user;
	int selected[256], sel_count = 0, i, count;
	const char *mods[64];
	char line[300], path[2 * path_max], quoted[2 * path_max], cmd[3 * path_max];
	(void)b;
	if (d->busy || !conn) return;
	for (i = 0; i < d->visible_count; i++) if (sg_list_is_selected(d->list, i)) selected[sel_count++] = d->visible[i];
	if (sel_count == 0) {
		drv_log(d, "Select at least one driver.");
		return;
	}
	count = drv_resolve(&d->set, selected, sel_count, mods, 64);
	d->busy = 1;
	for (i = 0; i < count; i++) {
		char *body = NULL;
		int r;
		if (drv_loaded(mods[i])) {
			snprintf(line, sizeof line, "skip %s (already loaded)", mods[i]);
			drv_log(d, line);
			continue;
		}
		if (d->flat) snprintf(path, sizeof path, "%s/%s", neutrino_dir, mods[i]);
		else snprintf(path, sizeof path, "%s/modules/%s", neutrino_dir, mods[i]);
		ps2_quote(path, quoted, sizeof quoted);
		snprintf(cmd, sizeof cmd, "irx %s", quoted);
		snprintf(line, sizeof line, "load %s ...", mods[i]);
		drv_log(d, line);
		r = ps2_exec(conn, cmd, &body);
		if (r != ps2_ok) {
			free(body);
			d->busy = 0;
			pending_disconnect = 1;
			return;
		}
		if (strstr(body, "failed")) {
			snprintf(line, sizeof line, "FAILED %s: %s", mods[i], body);
			drv_log(d, line);
			free(body);
			d->busy = 0;
			return;
		}
		free(body);
		if (loaded_count < 40) snprintf(loaded_modules[loaded_count++], sizeof loaded_modules[0], "%s", mods[i]);
	}
	drv_log(d, "done. Devices may take a few seconds to appear.");
	d->busy = 0;
}

static void drv_close_btn(sg_object *b, void *user)
{
	drvwin *d = user;
	(void)b;
	sg_destroy(d->win);
	d->win = NULL;
}

static int drv_event(sg_object *self, const sg_event *ev, void *user)
{
	drvwin *d = user;
	(void)self;
	if (ev->type == SG_EVENT_CLOSE) {
		if (d->busy) return SG_CANCEL;
		d->win = NULL;
	}
	return 0;
}

static void open_drivers(sg_object *b, void *user)
{
	drvwin d;
	sg_object *col, *row;
	(void)b;
	(void)user;
	if (!conn) return;
	memset(&d, 0, sizeof d);
	d.win = sg_window_create("Load drivers", 520, 520);
	sg_set_event_cb(d.win, drv_event, &d);
	col = layout_new(d.win, 1);
	sg_label_create(col, "Neutrino folder on the PS2:", 0);
	d.dir_edit = sg_edit_create(col, 0);
	sg_set_text(d.dir_edit, neutrino_dir);
	sg_label_create(col, "Drivers (dependencies are added in load order):", 0);
	d.list = sg_list_create(col, 1);
	sg_set_min_size(d.list, 0, 150);
	row = layout_new(col, 0);
	sg_button_create(row, "Load selected", drv_load, &d);
	sg_button_create(row, "Reload list", drv_reload_btn, &d);
	sg_button_create(row, "Close", drv_close_btn, &d);
	d.log = sg_edit_create(col, 1);
	sg_set_weight(d.log, 1.0f);
	sg_set_min_size(d.log, 0, 0);
	if (main_win) sg_window_set_blocker(main_win, d.win);
	top_win = d.win;
	drv_reload(&d);
	while (d.win) if (!sg_poll(1)) break;
	top_win = main_win;
	drv_clear_list(&d);
	if (conn) ps2_load();
}

static void do_load_irx(sg_object *b, void *user)
{
	int *rows = NULL, n, i;
	char path[2 * path_max], quoted[2 * path_max], cmd[3 * path_max], text[500];
	(void)b;
	(void)user;
	if (!conn || ps2_pane.dir[0] == 0) {
		set_status("Open a PS2 folder and select IRX files first");
		return;
	}
	n = selected_rows(&ps2_pane, &rows);
	if (n == 0) {
		free(rows);
		set_status("Select IRX files on the PS2 side first");
		return;
	}
	for (i = 0; i < n; i++) {
		item *it = &ps2_pane.items[rows[i]];
		char *body = NULL;
		int r;
		if (it->is_dir) continue;
		join_path(path, sizeof path, ps2_pane.dir, it->name);
		ps2_quote(path, quoted, sizeof quoted);
		snprintf(cmd, sizeof cmd, "irx %s", quoted);
		r = ps2_exec(conn, cmd, &body);
		if (r != ps2_ok) {
			free(body);
			free(rows);
			ps2_failed(r);
			return;
		}
		if (strstr(body, "failed")) {
			snprintf(text, sizeof text, "Loading %s failed: %s", it->name, body);
			free(body);
			free(rows);
			set_status(text);
			return;
		}
		free(body);
		if (loaded_count < 40 && !drv_loaded(it->name)) snprintf(loaded_modules[loaded_count++], sizeof loaded_modules[0], "%s", it->name);
	}
	free(rows);
	set_status(n == 1 ? "Module loaded" : "Modules loaded");
}

static void do_reset(sg_object *b, void *user)
{
	(void)b;
	(void)user;
	if (!conn) return;
	if (!confirm_box("Restart the PS2? The connection will be closed.")) return;
	ps2_fire(conn, "reset");
	pending_disconnect = 2;
}

static void do_launch(sg_object *b, void *user)
{
	int *rows = NULL, n;
	char path[2 * path_max], quoted[2 * path_max], cmd[3 * path_max], text[400];
	item *it;
	(void)b;
	(void)user;
	if (!conn || ps2_pane.dir[0] == 0) {
		set_status("Open a PS2 folder and select an ELF first");
		return;
	}
	n = selected_rows(&ps2_pane, &rows);
	if (n != 1 || ps2_pane.items[rows[0]].is_dir) {
		free(rows);
		set_status("Select exactly one ELF file on the PS2 side");
		return;
	}
	it = &ps2_pane.items[rows[0]];
	free(rows);
	snprintf(text, sizeof text, "Launch '%s'? The connection will be closed.", it->name);
	if (!confirm_box(text)) return;
	join_path(path, sizeof path, ps2_pane.dir, it->name);
	ps2_quote(path, quoted, sizeof quoted);
	snprintf(cmd, sizeof cmd, "elf %s", quoted);
	ps2_fire(conn, cmd);
	snprintf(launched_message, sizeof launched_message, "Launched %s.", it->name);
	pending_disconnect = 3;
}


static void on_up(sg_object *b, void *user)
{
	pane *p = user;
	(void)b;
	if (p == &local_pane) local_up();
	else if (conn) ps2_up();
}

static void on_refresh(sg_object *b, void *user)
{
	pane *p = user;
	(void)b;
	if (p == &local_pane) local_load();
	else if (conn) ps2_load();
}

static void go_text_for(pane *p)
{
	if (p == &local_pane) local_go_text(sg_get_text(p->path_edit));
	else if (conn) ps2_go_text(sg_get_text(p->path_edit));
}

static void on_go(sg_object *b, void *user)
{
	(void)b;
	go_text_for(user);
}

static int on_path_event(sg_object *self, const sg_event *ev, void *user)
{
	(void)self;
	if (ev->type == SG_EVENT_KEY_DOWN && ev->key == SG_KEY_ENTER) {
		go_text_for(user);
		return SG_CANCEL;
	}
	return 0;
}

static int on_list_event(sg_object *self, const sg_event *ev, void *user)
{
	pane *p = user;
	if (ev->type == SG_EVENT_SELECT) {
		pane_update_status(p);
		return 0;
	}
	if (ev->type == SG_EVENT_CLICK && ev->button == SG_MOUSE_LEFT && !(ev->mods & (SG_MOD_CONTROL | SG_MOD_SHIFT))) {
		int sx, sy, rh = sg_list_get_row_height(self), row;
		uint32_t now = pf_ticks_ms();
		sg_get_scroll(self, &sx, &sy);
		row = rh > 0 ? (ev->y + sy) / rh : -1;
		if (row >= 0 && row < p->count && row == p->last_row && now - p->last_click < double_click_ms) {
			pending_pane = p;
			pending_row = row;
			p->last_row = -1;
		} else {
			p->last_row = row;
			p->last_click = now;
		}
		return 0;
	}
	if (ev->type == SG_EVENT_KEY_DOWN) {
		if (ev->key == SG_KEY_ENTER) {
			int i;
			for (i = 0; i < p->count; i++) {
				if (sg_list_is_selected(self, i)) {
					pending_pane = p;
					pending_row = i;
					break;
				}
			}
			return SG_CANCEL;
		}
		if (ev->key == SG_KEY_BACKSPACE) {
			pending_pane = p;
			pending_row = 0;
			if (p->count > 0 && p->items[0].is_up) return SG_CANCEL;
			pending_pane = NULL;
		}
	}
	return 0;
}

static void on_disconnect(sg_object *b, void *user)
{
	(void)b;
	(void)user;
	pending_disconnect = 1;
}

static int on_main_event(sg_object *self, const sg_event *ev, void *user)
{
	(void)self;
	(void)user;
	if (ev->type == SG_EVENT_CLOSE) {
		main_win = NULL;
		top_win = NULL;
		if (conn) ps2_close(conn);
		conn = NULL;
		local_close_zip();
		sg_quit();
	}
	return 0;
}

static void build_pane(pane *p, sg_object *parent, const char *title)
{
	sg_object *col = layout_new(parent, 1), *row, *title_label;
	sg_set_weight(col, 0.48f);
	row = layout_new(col, 0);
	title_label = sg_label_create(row, title, 0);
	sg_set_weight(title_label, 1.0f);
	sg_button_create(row, "Up", on_up, p);
	sg_button_create(row, "Refresh", on_refresh, p);
	row = layout_new(col, 0);
	p->path_edit = sg_edit_create(row, 0);
	sg_set_weight(p->path_edit, 1.0f);
	sg_set_event_cb(p->path_edit, on_path_event, p);
	sg_button_create(row, "Go", on_go, p);
	p->list = sg_list_create(col, 1);
	sg_set_weight(p->list, 1.0f);
	sg_set_min_size(p->list, 0, 0);
	sg_set_event_cb(p->list, on_list_event, p);
	p->status = sg_label_create(col, "", 0);
	p->last_row = -1;
	sg_list_set_virtual(p->list, 0, list_text, p);
}

static void build_main(void)
{
	sg_object *root, *panes, *mid, *bottom, *top_pad, *bottom_pad;
	char title[128];
	items_free(&local_pane);
	items_free(&ps2_pane);
	memset(&local_pane, 0, sizeof local_pane);
	memset(&ps2_pane, 0, sizeof ps2_pane);
	pf_start_dir(local_pane.dir, sizeof local_pane.dir);
	snprintf(title, sizeof title, "ps2netcon - %s", conn_ip);
	main_win = sg_window_create(title, 980, 600);
	top_win = main_win;
	sg_set_event_cb(main_win, on_main_event, NULL);
	root = layout_new(main_win, 1);
	panes = layout_new(root, 0);
	sg_set_weight(panes, 1.0f);
	build_pane(&local_pane, panes, "Computer");
	mid = layout_new(panes, 1);
	top_pad = sg_label_create(mid, "", 0);
	sg_set_weight(top_pad, 1.0f);
	sg_set_min_size(top_pad, 0, 0);
	sg_button_create(mid, "Send >>", do_send, NULL);
	bottom_pad = sg_label_create(mid, "", 0);
	sg_set_weight(bottom_pad, 1.0f);
	sg_set_min_size(bottom_pad, 0, 0);
	build_pane(&ps2_pane, panes, "PS2");
	bottom = layout_new(root, 0);
	status_label = sg_label_create(bottom, "", 0);
	sg_set_weight(status_label, 1.0f);
	sg_button_create(bottom, "Drivers...", open_drivers, NULL);
	sg_button_create(bottom, "Load IRX", do_load_irx, NULL);
	sg_button_create(bottom, "New folder", do_new_folder, NULL);
	sg_button_create(bottom, "Delete", do_delete, NULL);
	sg_button_create(bottom, "Launch ELF", do_launch, NULL);
	sg_button_create(bottom, "Reset", do_reset, NULL);
	sg_button_create(bottom, "Disconnect", on_disconnect, NULL);
	local_load();
	ps2_load();
	sg_set_text(ps2_pane.status, "choose a device");
}

static void go_disconnected(const char *msg)
{
	sg_object *old = main_win;
	if (conn) ps2_close(conn);
	conn = NULL;
	local_close_zip();
	show_connect_window(msg);
	main_win = NULL;
	top_win = connect_win;
	status_label = NULL;
	local_pane.status = NULL;
	ps2_pane.status = NULL;
	if (old) sg_destroy(old);
}

static int connect_pump(void *user)
{
	(void)user;
	if (!sg_poll(0)) return 1;
	return connect_cancel;
}

static void do_connect(void)
{
	char ip[64], err[128];
	int r, i = 0;
	const char *t = sg_get_text(ip_edit);
	if (connecting) {
		connect_cancel = 1;
		return;
	}
	while (*t == ' ') t++;
	while (*t && *t != ' ' && i < (int)sizeof ip - 1) ip[i++] = *t++;
	ip[i] = 0;
	connecting = 1;
	connect_cancel = 0;
	sg_set_text(connect_btn, "Cancel");
	sg_set_text(connect_label, "Connecting ...");
	sg_set_enabled(ip_edit, 0);
	r = ps2_connect(&conn, ip, ps2_port, connect_pump, NULL, err, sizeof err);
	connecting = 0;
	if (want_quit) {
		sg_quit();
		return;
	}
	sg_set_enabled(ip_edit, 1);
	sg_set_text(connect_btn, "Connect");
	if (r != ps2_ok) {
		conn = NULL;
		sg_set_text(connect_label, err);
		return;
	}
	snprintf(conn_ip, sizeof conn_ip, "%s", ip);
	loaded_count = 0;
	{
		sg_object *old = connect_win;
		connect_win = NULL;
		build_main();
		sg_destroy(old);
	}
}

static void on_connect_btn(sg_object *b, void *user)
{
	(void)b;
	(void)user;
	do_connect();
}

static void on_quit_btn(sg_object *b, void *user)
{
	(void)b;
	(void)user;
	if (connecting) {
		connect_cancel = 1;
		want_quit = 1;
		return;
	}
	sg_quit();
}

static int on_ip_event(sg_object *self, const sg_event *ev, void *user)
{
	(void)self;
	(void)user;
	if (ev->type == SG_EVENT_KEY_DOWN && ev->key == SG_KEY_ENTER) {
		do_connect();
		return SG_CANCEL;
	}
	return 0;
}

static int on_connect_win_event(sg_object *self, const sg_event *ev, void *user)
{
	(void)self;
	(void)user;
	if (ev->type == SG_EVENT_CLOSE) {
		if (connecting) {
			connect_cancel = 1;
			want_quit = 1;
			return SG_CANCEL;
		}
		connect_win = NULL;
		sg_quit();
	}
	return 0;
}

static void show_connect_window(const char *msg)
{
	sg_object *col, *row;
	connect_win = sg_window_create("ps2netcon", 340, 0);
	sg_set_event_cb(connect_win, on_connect_win_event, NULL);
	col = layout_new(connect_win, 1);
	sg_label_create(col, "PS2 IP address:", 0);
	ip_edit = sg_edit_create(col, 0);
	sg_set_text(ip_edit, conn_ip);
	sg_set_event_cb(ip_edit, on_ip_event, NULL);
	row = layout_new(col, 0);
	connect_btn = sg_button_create(row, "Connect", on_connect_btn, NULL);
	sg_button_create(row, "Quit", on_quit_btn, NULL);
	connect_label = sg_label_create(col, msg ? msg : "", 1);
	sg_set_focus(ip_edit);
	top_win = connect_win;
}

int main(void)
{
	if (ps2_init() != 0) return 1;
	if (sg_init()) return 1;
	show_connect_window(NULL);
	while (sg_poll(1)) {
		if (pending_pane) {
			pane *p = pending_pane;
			int row = pending_row;
			pending_pane = NULL;
			if (main_win) {
				if (p == &local_pane) local_open_item(row);
				else if (conn) ps2_open_item(row);
			}
		}
		if (pending_disconnect) {
			int why = pending_disconnect;
			pending_disconnect = 0;
			if (main_win) {
				if (why == 2) go_disconnected("PS2 is restarting.");
				else if (why == 3) go_disconnected(launched_message);
				else go_disconnected("Disconnected.");
			}
		}
	}
	if (conn) ps2_close(conn);
	local_close_zip();
	sg_shutdown();
	return 0;
}
