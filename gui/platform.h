#ifndef PLATFORM_H
#define PLATFORM_H

#include <stdint.h>

typedef struct pf_file pf_file;

typedef struct pf_entry {
	char *name;
	int is_dir;
	uint64_t size;
} pf_entry;

pf_file *pf_open(const char *path);
void pf_close(pf_file *f);
uint64_t pf_size(pf_file *f);
int pf_seek(pf_file *f, uint64_t pos);
long pf_read(pf_file *f, void *buf, long n);

int pf_list_dir(const char *path, pf_entry **out, int *count);
int pf_list_roots(pf_entry **out, int *count);
void pf_free_entries(pf_entry *entries, int count);
int pf_has_roots(void);
void pf_start_dir(char *out, int size);

uint32_t pf_ticks_ms(void);
void pf_u64_to_str(uint64_t value, char *out);
void pf_format_size(uint64_t value, char *out, int size);

#endif
