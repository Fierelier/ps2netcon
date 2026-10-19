#define _FILE_OFFSET_BITS 64

#include "platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32

#include <windows.h>

struct pf_file {
	HANDLE handle;
	uint64_t size;
};

pf_file *pf_open(const char *path)
{
	DWORD high = 0, low;
	pf_file *f;
	HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (h == INVALID_HANDLE_VALUE) return NULL;
	low = GetFileSize(h, &high);
	if (low == 0xFFFFFFFFu && GetLastError() != NO_ERROR) {
		CloseHandle(h);
		return NULL;
	}
	f = malloc(sizeof *f);
	if (!f) {
		CloseHandle(h);
		return NULL;
	}
	f->handle = h;
	f->size = ((uint64_t)high << 32) | low;
	return f;
}

void pf_close(pf_file *f)
{
	if (!f) return;
	CloseHandle(f->handle);
	free(f);
}

uint64_t pf_size(pf_file *f)
{
	return f->size;
}

int pf_seek(pf_file *f, uint64_t pos)
{
	LONG high = (LONG)(pos >> 32);
	DWORD r = SetFilePointer(f->handle, (LONG)(pos & 0xFFFFFFFFu), &high, FILE_BEGIN);
	if (r == 0xFFFFFFFFu && GetLastError() != NO_ERROR) return -1;
	return 0;
}

long pf_read(pf_file *f, void *buf, long n)
{
	DWORD got = 0;
	if (!ReadFile(f->handle, buf, (DWORD)n, &got, NULL)) return -1;
	return (long)got;
}

static int entries_push(pf_entry **list, int *count, int *cap, const char *name, int is_dir, uint64_t size)
{
	pf_entry *grown;
	if (*count >= *cap) {
		*cap = *cap ? *cap * 2 : 64;
		grown = realloc(*list, sizeof(pf_entry) * (size_t)*cap);
		if (!grown) return -1;
		*list = grown;
	}
	(*list)[*count].name = malloc(strlen(name) + 1);
	if (!(*list)[*count].name) return -1;
	strcpy((*list)[*count].name, name);
	(*list)[*count].is_dir = is_dir;
	(*list)[*count].size = size;
	(*count)++;
	return 0;
}

int pf_list_dir(const char *path, pf_entry **out, int *count)
{
	char pattern[1100];
	WIN32_FIND_DATAA fd;
	HANDLE h;
	pf_entry *list = NULL;
	int n = 0, cap = 0;
	snprintf(pattern, sizeof pattern, "%s*", path);
	h = FindFirstFileA(pattern, &fd);
	if (h == INVALID_HANDLE_VALUE) {
		if (GetLastError() == ERROR_FILE_NOT_FOUND) {
			*out = NULL;
			*count = 0;
			return 0;
		}
		return -1;
	}
	do {
		if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
		if (entries_push(&list, &n, &cap, fd.cFileName, (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0, ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow)) break;
	} while (FindNextFileA(h, &fd));
	FindClose(h);
	*out = list;
	*count = n;
	return 0;
}

int pf_list_roots(pf_entry **out, int *count)
{
	DWORD mask = GetLogicalDrives();
	pf_entry *list = NULL;
	int n = 0, cap = 0, i;
	char name[4];
	for (i = 0; i < 26; i++) {
		if (!(mask & (1u << i))) continue;
		name[0] = (char)('A' + i);
		name[1] = ':';
		name[2] = 0;
		if (entries_push(&list, &n, &cap, name, 1, 0)) break;
	}
	*out = list;
	*count = n;
	return 0;
}

int pf_has_roots(void)
{
	return 1;
}

void pf_start_dir(char *out, int size)
{
	char buf[520];
	DWORD len = GetCurrentDirectoryA(sizeof buf - 2, buf);
	int i;
	if (len == 0 || len >= sizeof buf - 2) {
		snprintf(out, (size_t)size, "C:/");
		return;
	}
	for (i = 0; buf[i]; i++) if (buf[i] == '\\') buf[i] = '/';
	if (buf[len - 1] != '/') {
		buf[len] = '/';
		buf[len + 1] = 0;
	}
	snprintf(out, (size_t)size, "%s", buf);
}

uint32_t pf_ticks_ms(void)
{
	return (uint32_t)GetTickCount();
}

#else

#include <dirent.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

struct pf_file {
	FILE *fp;
	uint64_t size;
};

pf_file *pf_open(const char *path)
{
	pf_file *f;
	FILE *fp = fopen(path, "rb");
	if (!fp) return NULL;
	if (fseeko(fp, 0, SEEK_END) != 0) {
		fclose(fp);
		return NULL;
	}
	f = malloc(sizeof *f);
	if (!f) {
		fclose(fp);
		return NULL;
	}
	f->fp = fp;
	f->size = (uint64_t)ftello(fp);
	fseeko(fp, 0, SEEK_SET);
	return f;
}

void pf_close(pf_file *f)
{
	if (!f) return;
	fclose(f->fp);
	free(f);
}

uint64_t pf_size(pf_file *f)
{
	return f->size;
}

int pf_seek(pf_file *f, uint64_t pos)
{
	return fseeko(f->fp, (off_t)pos, SEEK_SET) == 0 ? 0 : -1;
}

long pf_read(pf_file *f, void *buf, long n)
{
	size_t got = fread(buf, 1, (size_t)n, f->fp);
	if (got == 0 && ferror(f->fp)) return -1;
	return (long)got;
}

static int entries_push(pf_entry **list, int *count, int *cap, const char *name, int is_dir, uint64_t size)
{
	pf_entry *grown;
	if (*count >= *cap) {
		*cap = *cap ? *cap * 2 : 64;
		grown = realloc(*list, sizeof(pf_entry) * (size_t)*cap);
		if (!grown) return -1;
		*list = grown;
	}
	(*list)[*count].name = malloc(strlen(name) + 1);
	if (!(*list)[*count].name) return -1;
	strcpy((*list)[*count].name, name);
	(*list)[*count].is_dir = is_dir;
	(*list)[*count].size = size;
	(*count)++;
	return 0;
}

int pf_list_dir(const char *path, pf_entry **out, int *count)
{
	DIR *d = opendir(path);
	struct dirent *de;
	struct stat st;
	char full[2200];
	pf_entry *list = NULL;
	int n = 0, cap = 0;
	if (!d) return -1;
	while ((de = readdir(d)) != NULL) {
		int is_dir = 0;
		uint64_t size = 0;
		if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
		snprintf(full, sizeof full, "%s%s", path, de->d_name);
		if (stat(full, &st) == 0) {
			is_dir = S_ISDIR(st.st_mode) != 0;
			size = is_dir ? 0 : (uint64_t)st.st_size;
		}
		if (entries_push(&list, &n, &cap, de->d_name, is_dir, size)) break;
	}
	closedir(d);
	*out = list;
	*count = n;
	return 0;
}

int pf_list_roots(pf_entry **out, int *count)
{
	*out = NULL;
	*count = 0;
	return 0;
}

int pf_has_roots(void)
{
	return 0;
}

void pf_start_dir(char *out, int size)
{
	const char *home = getenv("HOME");
	size_t len;
	if (!home || !*home) home = "/";
	snprintf(out, (size_t)size, "%s", home);
	len = strlen(out);
	if (len == 0 || out[len - 1] != '/') {
		if ((int)len + 1 < size) {
			out[len] = '/';
			out[len + 1] = 0;
		}
	}
}

uint32_t pf_ticks_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)((uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u);
}

#endif

void pf_free_entries(pf_entry *entries, int count)
{
	int i;
	if (!entries) return;
	for (i = 0; i < count; i++) free(entries[i].name);
	free(entries);
}

void pf_u64_to_str(uint64_t value, char *out)
{
	char tmp[24];
	int n = 0, i;
	if (value == 0) tmp[n++] = '0';
	while (value > 0) {
		tmp[n++] = (char)('0' + (int)(value % 10u));
		value /= 10u;
	}
	for (i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
	out[n] = 0;
}

void pf_format_size(uint64_t value, char *out, int size)
{
	static const char *units[] = {"B", "KB", "MB", "GB", "TB"};
	int unit = 0;
	uint64_t whole = value, tenth = 0;
	while (whole >= 1024u && unit < 4) {
		tenth = ((whole % 1024u) * 10u) / 1024u;
		whole /= 1024u;
		unit++;
	}
	if (unit == 0) snprintf(out, (size_t)size, "%u B", (unsigned)whole);
	else snprintf(out, (size_t)size, "%u.%u %s", (unsigned)whole, (unsigned)tenth, units[unit]);
}
