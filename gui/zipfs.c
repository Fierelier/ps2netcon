#include "zipfs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define zip_in_size 32768
#define zip_window_size 32768

enum {
	st_header,
	st_stored,
	st_codes,
	st_done,
	st_raw,
	st_error
};

typedef struct huff {
	uint16_t count[16];
	uint16_t symbol[288];
} huff;

struct zip_stream {
	zip_archive *zip;
	zip_entry *entry;
	uint64_t in_left;
	uint8_t in_buf[zip_in_size];
	int in_pos;
	int in_len;
	uint32_t bit_buf;
	int bit_count;
	uint8_t window[zip_window_size];
	uint32_t window_pos;
	uint64_t out_total;
	uint32_t crc;
	int state;
	int last_block;
	huff lit;
	huff dist;
	uint32_t stored_left;
	int copy_len;
	int copy_dist;
	const char *error;
};

static uint32_t crc_table[256];
static int crc_ready;

static void crc_init(void)
{
	uint32_t i, j, c;
	if (crc_ready) return;
	for (i = 0; i < 256; i++) {
		c = i;
		for (j = 0; j < 8; j++) c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
		crc_table[i] = c;
	}
	crc_ready = 1;
}

static uint32_t crc_update(uint32_t crc, const uint8_t *p, long n)
{
	long i;
	crc = ~crc;
	for (i = 0; i < n; i++) crc = crc_table[(crc ^ p[i]) & 0xFFu] ^ (crc >> 8);
	return ~crc;
}

static uint16_t rd16(const uint8_t *p)
{
	return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t rd32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t rd64(const uint8_t *p)
{
	return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

static void set_err(char *err, int size, const char *msg)
{
	if (err && size > 0) snprintf(err, (size_t)size, "%s", msg);
}

static int read_at(pf_file *f, uint64_t pos, void *buf, long n)
{
	long got;
	if (pf_seek(f, pos)) return -1;
	got = pf_read(f, buf, n);
	return got == n ? 0 : -1;
}

static int parse_extra(const uint8_t *extra, int len, zip_entry *e, int need_size, int need_csize, int need_offset)
{
	int pos = 0;
	while (pos + 4 <= len) {
		int id = rd16(extra + pos), sz = rd16(extra + pos + 2), p = pos + 4;
		if (p + sz > len) break;
		if (id == 1) {
			if (need_size && p + 8 <= pos + 4 + sz) {
				e->size = rd64(extra + p);
				p += 8;
			}
			if (need_csize && p + 8 <= pos + 4 + sz) {
				e->compressed_size = rd64(extra + p);
				p += 8;
			}
			if (need_offset && p + 8 <= pos + 4 + sz) {
				e->header_offset = rd64(extra + p);
				p += 8;
			}
			return 0;
		}
		pos += 4 + sz;
	}
	return 0;
}

zip_archive *zip_open(const char *path, char *err, int err_size)
{
	zip_archive *z;
	pf_file *f = pf_open(path);
	uint64_t fsize, cd_offset, cd_size, total;
	uint8_t tail[65536 + 22], head[64];
	uint8_t *cd = NULL;
	long tail_len, i, eocd = -1;
	uint64_t tail_start, pos;
	int n = 0, cap = 0;
	zip_entry *entries = NULL;

	crc_init();
	if (!f) {
		set_err(err, err_size, "cannot open file");
		return NULL;
	}
	fsize = pf_size(f);
	if (fsize < 22) {
		pf_close(f);
		set_err(err, err_size, "not a zip file");
		return NULL;
	}
	tail_len = fsize < sizeof tail ? (long)fsize : (long)sizeof tail;
	tail_start = fsize - (uint64_t)tail_len;
	if (read_at(f, tail_start, tail, tail_len)) {
		pf_close(f);
		set_err(err, err_size, "read error");
		return NULL;
	}
	for (i = tail_len - 22; i >= 0; i--) {
		if (rd32(tail + i) == 0x06054b50u) {
			eocd = i;
			break;
		}
	}
	if (eocd < 0) {
		pf_close(f);
		set_err(err, err_size, "not a zip file");
		return NULL;
	}
	total = rd16(tail + eocd + 10);
	cd_size = rd32(tail + eocd + 12);
	cd_offset = rd32(tail + eocd + 16);
	if (total == 0xFFFFu || cd_size == 0xFFFFFFFFu || cd_offset == 0xFFFFFFFFu) {
		uint64_t loc_pos = tail_start + (uint64_t)eocd;
		if (loc_pos >= 20 && read_at(f, loc_pos - 20, head, 20) == 0 && rd32(head) == 0x07064b50u) {
			uint64_t rec = rd64(head + 8);
			if (read_at(f, rec, head, 56) == 0 && rd32(head) == 0x06064b50u) {
				total = rd64(head + 32);
				cd_size = rd64(head + 40);
				cd_offset = rd64(head + 48);
			} else {
				pf_close(f);
				set_err(err, err_size, "bad zip64 record");
				return NULL;
			}
		}
	}
	if (cd_size > 0x40000000u || cd_offset + cd_size > fsize) {
		pf_close(f);
		set_err(err, err_size, "bad central directory");
		return NULL;
	}
	cd = malloc((size_t)cd_size + 1);
	if (!cd || read_at(f, cd_offset, cd, (long)cd_size)) {
		free(cd);
		pf_close(f);
		set_err(err, err_size, "cannot read central directory");
		return NULL;
	}
	pos = 0;
	while (pos + 46 <= cd_size && rd32(cd + pos) == 0x02014b50u) {
		zip_entry *e;
		int name_len = rd16(cd + pos + 28), extra_len = rd16(cd + pos + 30), comment_len = rd16(cd + pos + 32);
		uint32_t csize32 = rd32(cd + pos + 20), size32 = rd32(cd + pos + 24), off32 = rd32(cd + pos + 42);
		if (pos + 46 + (uint64_t)name_len + extra_len + comment_len > cd_size) break;
		if (n >= cap) {
			zip_entry *grown;
			cap = cap ? cap * 2 : 256;
			grown = realloc(entries, sizeof(zip_entry) * (size_t)cap);
			if (!grown) break;
			entries = grown;
		}
		e = &entries[n];
		memset(e, 0, sizeof *e);
		e->name = malloc((size_t)name_len + 1);
		if (!e->name) break;
		memcpy(e->name, cd + pos + 46, (size_t)name_len);
		e->name[name_len] = 0;
		for (i = 0; i < name_len; i++) if (e->name[i] == '\\') e->name[i] = '/';
		e->flags = rd16(cd + pos + 8);
		e->method = rd16(cd + pos + 10);
		e->crc = rd32(cd + pos + 16);
		e->compressed_size = csize32;
		e->size = size32;
		e->header_offset = off32;
		parse_extra(cd + pos + 46 + name_len, extra_len, e, size32 == 0xFFFFFFFFu, csize32 == 0xFFFFFFFFu, off32 == 0xFFFFFFFFu);
		e->is_dir = name_len > 0 && e->name[name_len - 1] == '/';
		n++;
		pos += 46 + (uint64_t)name_len + extra_len + comment_len;
	}
	free(cd);
	z = malloc(sizeof *z);
	if (!z) {
		for (i = 0; i < n; i++) free(entries[i].name);
		free(entries);
		pf_close(f);
		set_err(err, err_size, "out of memory");
		return NULL;
	}
	z->file = f;
	z->entries = entries;
	z->count = n;
	return z;
}

void zip_close(zip_archive *z)
{
	int i;
	if (!z) return;
	for (i = 0; i < z->count; i++) free(z->entries[i].name);
	free(z->entries);
	pf_close(z->file);
	free(z);
}

static int refill(zip_stream *s)
{
	long want, got;
	if (s->in_left == 0) return -1;
	want = s->in_left < zip_in_size ? (long)s->in_left : zip_in_size;
	got = pf_read(s->zip->file, s->in_buf, want);
	if (got <= 0) return -1;
	s->in_len = (int)got;
	s->in_pos = 0;
	s->in_left -= (uint64_t)got;
	return 0;
}

static int need_bits(zip_stream *s, int n)
{
	while (s->bit_count < n) {
		if (s->in_pos >= s->in_len && refill(s)) return -1;
		s->bit_buf |= (uint32_t)s->in_buf[s->in_pos++] << s->bit_count;
		s->bit_count += 8;
	}
	return 0;
}

static int get_bits(zip_stream *s, int n, int *ok)
{
	int v;
	if (n == 0) return 0;
	if (need_bits(s, n)) {
		*ok = 0;
		return 0;
	}
	v = (int)(s->bit_buf & ((1u << n) - 1u));
	s->bit_buf >>= n;
	s->bit_count -= n;
	return v;
}

static int build_huff(huff *h, const short *length, int n)
{
	short offs[16];
	int sym, len, left;
	for (len = 0; len <= 15; len++) h->count[len] = 0;
	for (sym = 0; sym < n; sym++) h->count[length[sym]]++;
	if (h->count[0] == n) return 0;
	left = 1;
	for (len = 1; len <= 15; len++) {
		left <<= 1;
		left -= h->count[len];
		if (left < 0) return left;
	}
	offs[1] = 0;
	for (len = 1; len < 15; len++) offs[len + 1] = (short)(offs[len] + h->count[len]);
	for (sym = 0; sym < n; sym++) if (length[sym] != 0) h->symbol[offs[length[sym]]++] = (uint16_t)sym;
	return left;
}

static int decode_sym(zip_stream *s, const huff *h)
{
	int code = 0, first = 0, index = 0, len;
	for (len = 1; len <= 15; len++) {
		int ok = 1, count;
		code |= get_bits(s, 1, &ok);
		if (!ok) return -1;
		count = h->count[len];
		if (code - count < first) return h->symbol[index + (code - first)];
		index += count;
		first += count;
		first <<= 1;
		code <<= 1;
	}
	return -2;
}

static const short len_base[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const short len_extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const short dist_base[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const short dist_extra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

static int fail(zip_stream *s, const char *msg)
{
	s->state = st_error;
	s->error = msg;
	return -1;
}

static int read_dynamic(zip_stream *s)
{
	static const short order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
	short lengths[320];
	int ok = 1, nlen, ndist, ncode, idx, err;
	huff lencode;
	nlen = get_bits(s, 5, &ok) + 257;
	ndist = get_bits(s, 5, &ok) + 1;
	ncode = get_bits(s, 4, &ok) + 4;
	if (!ok) return fail(s, "unexpected end of data");
	if (nlen > 286 || ndist > 30) return fail(s, "bad deflate table sizes");
	for (idx = 0; idx < ncode; idx++) lengths[order[idx]] = (short)get_bits(s, 3, &ok);
	for (; idx < 19; idx++) lengths[order[idx]] = 0;
	if (!ok) return fail(s, "unexpected end of data");
	err = build_huff(&lencode, lengths, 19);
	if (err != 0) return fail(s, "bad deflate code lengths");
	idx = 0;
	while (idx < nlen + ndist) {
		int sym = decode_sym(s, &lencode);
		if (sym < 0) return fail(s, "bad deflate data");
		if (sym < 16) {
			lengths[idx++] = (short)sym;
		} else {
			int len = 0, rep;
			if (sym == 16) {
				if (idx == 0) return fail(s, "bad deflate repeat");
				len = lengths[idx - 1];
				rep = 3 + get_bits(s, 2, &ok);
			} else if (sym == 17) {
				rep = 3 + get_bits(s, 3, &ok);
			} else {
				rep = 11 + get_bits(s, 7, &ok);
			}
			if (!ok) return fail(s, "unexpected end of data");
			if (idx + rep > nlen + ndist) return fail(s, "bad deflate repeat");
			while (rep--) lengths[idx++] = (short)len;
		}
	}
	if (lengths[256] == 0) return fail(s, "missing end of block code");
	err = build_huff(&s->lit, lengths, nlen);
	if (err < 0 || (err > 0 && nlen - s->lit.count[0] != 1)) return fail(s, "bad literal codes");
	err = build_huff(&s->dist, lengths + nlen, ndist);
	if (err < 0 || (err > 0 && ndist - s->dist.count[0] != 1)) return fail(s, "bad distance codes");
	return 0;
}

static void build_fixed(zip_stream *s)
{
	short lengths[288];
	int i;
	for (i = 0; i < 144; i++) lengths[i] = 8;
	for (; i < 256; i++) lengths[i] = 9;
	for (; i < 280; i++) lengths[i] = 7;
	for (; i < 288; i++) lengths[i] = 8;
	build_huff(&s->lit, lengths, 288);
	for (i = 0; i < 30; i++) lengths[i] = 5;
	build_huff(&s->dist, lengths, 30);
}

static void emit(zip_stream *s, uint8_t *out, long at, uint8_t byte)
{
	out[at] = byte;
	s->window[s->window_pos & (zip_window_size - 1)] = byte;
	s->window_pos++;
	s->out_total++;
}

static long inflate_fill(zip_stream *s, uint8_t *out, long n)
{
	long produced = 0;
	int ok;
	while (produced < n && s->state != st_done && s->state != st_error) {
		if (s->copy_len > 0) {
			while (s->copy_len > 0 && produced < n) {
				uint8_t b = s->window[(s->window_pos - (uint32_t)s->copy_dist) & (zip_window_size - 1)];
				emit(s, out, produced++, b);
				s->copy_len--;
			}
			continue;
		}
		if (s->out_total >= s->entry->size) {
			s->state = st_done;
			break;
		}
		if (s->state == st_header) {
			int type;
			ok = 1;
			if (s->last_block) {
				s->state = st_done;
				break;
			}
			s->last_block = get_bits(s, 1, &ok);
			type = get_bits(s, 2, &ok);
			if (!ok) {
				fail(s, "unexpected end of data");
				break;
			}
			if (type == 0) {
				int len, nlen;
				s->bit_buf = 0;
				s->bit_count = 0;
				len = get_bits(s, 16, &ok);
				nlen = get_bits(s, 16, &ok);
				if (!ok || (len & 0xFFFF) != (~nlen & 0xFFFF)) {
					fail(s, "bad stored block");
					break;
				}
				s->stored_left = (uint32_t)len;
				s->state = st_stored;
			} else if (type == 1) {
				build_fixed(s);
				s->state = st_codes;
			} else if (type == 2) {
				if (read_dynamic(s)) break;
				s->state = st_codes;
			} else {
				fail(s, "bad deflate block type");
				break;
			}
		} else if (s->state == st_stored) {
			if (s->stored_left == 0) {
				s->state = st_header;
				continue;
			}
			ok = 1;
			{
				int b = get_bits(s, 8, &ok);
				if (!ok) {
					fail(s, "unexpected end of data");
					break;
				}
				emit(s, out, produced++, (uint8_t)b);
				s->stored_left--;
			}
		} else if (s->state == st_codes) {
			int sym = decode_sym(s, &s->lit);
			if (sym < 0) {
				fail(s, "bad deflate data");
				break;
			}
			if (sym < 256) {
				emit(s, out, produced++, (uint8_t)sym);
			} else if (sym == 256) {
				s->state = st_header;
			} else {
				int len, dsym, dist;
				ok = 1;
				sym -= 257;
				if (sym >= 29) {
					fail(s, "bad length code");
					break;
				}
				len = len_base[sym] + get_bits(s, len_extra[sym], &ok);
				dsym = decode_sym(s, &s->dist);
				if (!ok || dsym < 0 || dsym >= 30) {
					fail(s, "bad distance code");
					break;
				}
				dist = dist_base[dsym] + get_bits(s, dist_extra[dsym], &ok);
				if (!ok) {
					fail(s, "unexpected end of data");
					break;
				}
				if ((uint64_t)dist > s->out_total || dist > zip_window_size) {
					fail(s, "bad distance");
					break;
				}
				s->copy_len = len;
				s->copy_dist = dist;
			}
		} else {
			break;
		}
	}
	return produced;
}

zip_stream *zip_stream_open(zip_archive *z, int index, char *err, int err_size)
{
	zip_stream *s;
	zip_entry *e;
	uint8_t head[30];
	uint64_t data_pos;
	if (index < 0 || index >= z->count) {
		set_err(err, err_size, "bad entry");
		return NULL;
	}
	e = &z->entries[index];
	if (e->flags & 1) {
		set_err(err, err_size, "encrypted entries are not supported");
		return NULL;
	}
	if (e->method != 0 && e->method != 8) {
		set_err(err, err_size, "unsupported compression method");
		return NULL;
	}
	if (read_at(z->file, e->header_offset, head, 30) || rd32(head) != 0x04034b50u) {
		set_err(err, err_size, "bad local header");
		return NULL;
	}
	data_pos = e->header_offset + 30 + rd16(head + 26) + rd16(head + 28);
	s = calloc(1, sizeof *s);
	if (!s) {
		set_err(err, err_size, "out of memory");
		return NULL;
	}
	s->zip = z;
	s->entry = e;
	s->in_left = e->compressed_size;
	s->state = e->method == 0 ? st_raw : st_header;
	if (pf_seek(z->file, data_pos)) {
		free(s);
		set_err(err, err_size, "seek failed");
		return NULL;
	}
	return s;
}

long zip_stream_read(zip_stream *s, void *buf, long n)
{
	long got = 0;
	if (s->state == st_error) return -1;
	if (s->out_total >= s->entry->size) {
		if (s->crc != s->entry->crc) {
			s->state = st_error;
			s->error = "checksum mismatch";
			return -1;
		}
		return 0;
	}
	if (s->state == st_raw) {
		uint64_t left = s->entry->size - s->out_total;
		long want = left < (uint64_t)n ? (long)left : n;
		got = pf_read(s->zip->file, buf, want);
		if (got <= 0) {
			s->state = st_error;
			s->error = "unexpected end of data";
			return -1;
		}
		s->out_total += (uint64_t)got;
	} else {
		uint64_t left = s->entry->size - s->out_total;
		long want = left < (uint64_t)n ? (long)left : n;
		got = inflate_fill(s, (uint8_t *)buf, want);
		if (s->state == st_error) return -1;
		if (got == 0) {
			s->state = st_error;
			s->error = "unexpected end of data";
			return -1;
		}
	}
	s->crc = crc_update(s->crc, (const uint8_t *)buf, got);
	return got;
}

const char *zip_stream_error(zip_stream *s)
{
	return s->error ? s->error : "error";
}

void zip_stream_close(zip_stream *s)
{
	free(s);
}
