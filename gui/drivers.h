#ifndef DRIVERS_H
#define DRIVERS_H

enum {
	drv_kind_internal,
	drv_kind_storage,
	drv_kind_filesystem
};

typedef struct drv_driver {
	char *id;
	char *name;
	char **deps;
	int dep_count;
	char **modules;
	int module_count;
	int kind;
	int hidden;
} drv_driver;

typedef struct drv_set {
	drv_driver *items;
	int count;
} drv_set;

int drv_add(drv_set *set, const char *id, const char *text);
void drv_finish(drv_set *set);
void drv_free(drv_set *set);
int drv_find(const drv_set *set, const char *id);
int drv_resolve(const drv_set *set, const int *selected, int selected_count, const char **out, int out_max);

#endif
