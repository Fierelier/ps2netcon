#ifndef ZIPFS_H
#define ZIPFS_H

#include <stdint.h>
#include "platform.h"

typedef struct zip_entry {
	char *name;
	uint64_t compressed_size;
	uint64_t size;
	uint64_t header_offset;
	uint32_t crc;
	int method;
	int flags;
	int is_dir;
} zip_entry;

typedef struct zip_archive {
	pf_file *file;
	zip_entry *entries;
	int count;
} zip_archive;

typedef struct zip_stream zip_stream;

zip_archive *zip_open(const char *path, char *err, int err_size);
void zip_close(zip_archive *z);
zip_stream *zip_stream_open(zip_archive *z, int index, char *err, int err_size);
long zip_stream_read(zip_stream *s, void *buf, long n);
const char *zip_stream_error(zip_stream *s);
void zip_stream_close(zip_stream *s);

#endif
