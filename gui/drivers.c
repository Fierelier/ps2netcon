#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "drivers.h"

static const char *skip_modules[] = {"fileXio.irx", "dev9_hidden.irx", "dev9_ns.irx", "smap.irx", NULL};
static const char *conflict_modules[] = {"ministack.irx", NULL};

static char *dup_n(const char *s, size_t n)
{
	char *d = malloc(n + 1);
	if (!d) return NULL;
	memcpy(d, s, n);
	d[n] = 0;
	return d;
}

static int list_has(const char **list, const char *name)
{
	int i;
	for (i = 0; list[i]; i++) if (strcmp(list[i], name) == 0) return 1;
	return 0;
}

static int push_string(char ***list, int *count, char *value)
{
	char **grown;
	if (!value) return -1;
	grown = realloc(*list, sizeof(char *) * (size_t)(*count + 1));
	if (!grown) {
		free(value);
		return -1;
	}
	*list = grown;
	(*list)[(*count)++] = value;
	return 0;
}

static const char *parse_string(const char *s, char **out)
{
	char buf[512];
	size_t n = 0;
	if (*s != '"') return NULL;
	s++;
	while (*s && *s != '"' && n < sizeof buf - 1) {
		if (*s == '\\' && s[1]) s++;
		buf[n++] = *s++;
	}
	if (*s != '"') return NULL;
	*out = dup_n(buf, n);
	return s + 1;
}

static int parse_array(const char *s, char ***items, int *count)
{
	*items = NULL;
	*count = 0;
	if (*s != '[') return -1;
	s++;
	for (;;) {
		char *value = NULL;
		while (*s == ' ' || *s == '\t' || *s == ',') s++;
		if (*s == ']') return 0;
		s = parse_string(s, &value);
		if (!s) return -1;
		if (push_string(items, count, value)) return -1;
	}
}

static void free_list(char **list, int count)
{
	int i;
	for (i = 0; i < count; i++) free(list[i]);
	free(list);
}

static int list_contains(char **list, int count, const char *value)
{
	int i;
	for (i = 0; i < count; i++) if (strcmp(list[i], value) == 0) return 1;
	return 0;
}

typedef struct module_state {
	char *file;
	char **env;
	int env_count;
	int active;
} module_state;

static void module_flush(drv_driver *d, module_state *m)
{
	if (m->active && m->file) {
		if (m->env_count == 0 || list_contains(m->env, m->env_count, "LE")) {
			push_string(&d->modules, &d->module_count, m->file);
			m->file = NULL;
		}
	}
	free(m->file);
	free_list(m->env, m->env_count);
	memset(m, 0, sizeof *m);
}

int drv_add(drv_set *set, const char *id, const char *text)
{
	drv_driver *grown, *d;
	module_state m;
	const char *line = text;
	int in_other = 0;
	grown = realloc(set->items, sizeof(drv_driver) * (size_t)(set->count + 1));
	if (!grown) return -1;
	set->items = grown;
	d = &set->items[set->count];
	memset(d, 0, sizeof *d);
	d->id = dup_n(id, strlen(id));
	memset(&m, 0, sizeof m);
	while (*line) {
		const char *end = strchr(line, '\n');
		size_t n = end ? (size_t)(end - line) : strlen(line);
		char buf[1024], *eq, *key, *value;
		size_t kl;
		if (n >= sizeof buf) n = sizeof buf - 1;
		memcpy(buf, line, n);
		buf[n] = 0;
		line = end ? end + 1 : line + n;
		key = buf;
		while (*key == ' ' || *key == '\t' || *key == '\r') key++;
		if (*key == 0 || *key == '#') continue;
		if (*key == '[') {
			module_flush(d, &m);
			in_other = 0;
			if (strncmp(key, "[[module]]", 10) == 0) m.active = 1;
			else in_other = 1;
			continue;
		}
		eq = strchr(key, '=');
		if (!eq) continue;
		value = eq + 1;
		while (*value == ' ' || *value == '\t') value++;
		kl = (size_t)(eq - key);
		while (kl > 0 && (key[kl - 1] == ' ' || key[kl - 1] == '\t')) kl--;
		key[kl] = 0;
		if (in_other) continue;
		if (m.active) {
			if (strcmp(key, "file") == 0) {
				char *s = NULL;
				if (parse_string(value, &s)) {
					free(m.file);
					m.file = s;
				}
			} else if (strcmp(key, "env") == 0) {
				free_list(m.env, m.env_count);
				m.env = NULL;
				m.env_count = 0;
				parse_array(value, &m.env, &m.env_count);
			}
		} else if (strcmp(key, "name") == 0) {
			char *s = NULL;
			if (parse_string(value, &s)) {
				free(d->name);
				d->name = s;
			}
		} else if (strcmp(key, "depends") == 0) {
			free_list(d->deps, d->dep_count);
			d->deps = NULL;
			d->dep_count = 0;
			parse_array(value, &d->deps, &d->dep_count);
		}
	}
	module_flush(d, &m);
	if (!d->name) d->name = dup_n(id, strlen(id));
	set->count++;
	return 0;
}

int drv_find(const drv_set *set, const char *id)
{
	int i;
	for (i = 0; i < set->count; i++) if (strcmp(set->items[i].id, id) == 0) return i;
	return -1;
}

static int conflicts(const drv_set *set, int index, int depth)
{
	const drv_driver *d = &set->items[index];
	int i;
	if (depth > 8) return 0;
	for (i = 0; i < d->module_count; i++) if (list_has(conflict_modules, d->modules[i])) return 1;
	for (i = 0; i < d->dep_count; i++) {
		int dep = drv_find(set, d->deps[i]);
		if (dep >= 0 && conflicts(set, dep, depth + 1)) return 1;
	}
	return 0;
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

static int driver_compare(const void *a, const void *b)
{
	const drv_driver *x = a, *y = b;
	if (x->kind != y->kind) return x->kind - y->kind;
	return ci_compare(x->name, y->name);
}

void drv_finish(drv_set *set)
{
	int i;
	for (i = 0; i < set->count; i++) {
		drv_driver *d = &set->items[i];
		if (strncmp(d->id, "bsdfs-", 6) == 0) d->kind = drv_kind_filesystem;
		else if (strncmp(d->id, "bsd-", 4) == 0) d->kind = drv_kind_storage;
		else d->kind = drv_kind_internal;
	}
	for (i = 0; i < set->count; i++) set->items[i].hidden = conflicts(set, i, 0);
	if (set->count > 1) qsort(set->items, (size_t)set->count, sizeof(drv_driver), driver_compare);
}

void drv_free(drv_set *set)
{
	int i;
	for (i = 0; i < set->count; i++) {
		free(set->items[i].id);
		free(set->items[i].name);
		free_list(set->items[i].deps, set->items[i].dep_count);
		free_list(set->items[i].modules, set->items[i].module_count);
	}
	free(set->items);
	set->items = NULL;
	set->count = 0;
}

static void add_driver(const drv_set *set, int index, char *visited, const char **out, int *count, int out_max)
{
	const drv_driver *d = &set->items[index];
	int i, j;
	if (visited[index]) return;
	visited[index] = 1;
	for (i = 0; i < d->dep_count; i++) {
		int dep = drv_find(set, d->deps[i]);
		if (dep >= 0) add_driver(set, dep, visited, out, count, out_max);
	}
	for (i = 0; i < d->module_count; i++) {
		int seen = 0;
		if (list_has(skip_modules, d->modules[i])) continue;
		for (j = 0; j < *count; j++) if (strcmp(out[j], d->modules[i]) == 0) seen = 1;
		if (!seen && *count < out_max) out[(*count)++] = d->modules[i];
	}
}

int drv_resolve(const drv_set *set, const int *selected, int selected_count, const char **out, int out_max)
{
	char *visited = calloc((size_t)set->count + 1, 1), *chosen = calloc((size_t)set->count + 1, 1);
	int count = 0, i;
	if (!visited || !chosen) {
		free(visited);
		free(chosen);
		return 0;
	}
	for (i = 0; i < selected_count; i++) if (selected[i] >= 0 && selected[i] < set->count) chosen[selected[i]] = 1;
	for (i = 0; i < set->count; i++) if (chosen[i]) add_driver(set, i, visited, out, &count, out_max);
	for (i = 1; i < count; i++) {
		if (strcmp(out[i], "iomanX.irx") == 0) {
			const char *tmp = out[i];
			int j;
			for (j = i; j > 0; j--) out[j] = out[j - 1];
			out[0] = tmp;
			break;
		}
	}
	free(visited);
	free(chosen);
	return count;
}
