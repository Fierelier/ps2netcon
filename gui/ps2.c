#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#ifndef _WIN32
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#endif

#include <unisocket/main.c>
#include "ps2.h"
#include "platform.h"

#ifdef _WIN32
typedef SOCKET sock_t;
#define sock_invalid INVALID_SOCKET
#define sock_close closesocket
#else
typedef int sock_t;
#define sock_invalid (-1)
#define sock_close close
#endif

#define send_piece 131072
#define default_timeout 20000
#define pump_timeout 900000
#define socket_buffer 524288

struct ps2 {
	sock_t sock;
	char *buf;
	size_t len;
	size_t cap;
	int skip_prompt;
	unsigned sync_id;
	ps2_pump_fn pump;
	void *pump_user;
	int timeout_ms;
	char ip[64];
};

int ps2_init(void)
{
#ifndef _WIN32
	signal(SIGPIPE, SIG_IGN);
#endif
	return unisocket_init();
}

static void set_nonblocking(sock_t s, int on)
{
#ifdef _WIN32
	u_long mode = on ? 1 : 0;
	ioctlsocket(s, FIONBIO, &mode);
#else
	int flags = fcntl(s, F_GETFL, 0);
	if (flags < 0) return;
	fcntl(s, F_SETFL, on ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK));
#endif
}

static void set_err(char *err, int size, const char *msg)
{
	if (err && size > 0) snprintf(err, (size_t)size, "%s", msg);
}

static int wait_socket(ps2 *p, int for_write, int timeout_ms)
{
	uint32_t start = pf_ticks_ms();
	for (;;) {
		fd_set set, except;
		struct timeval tv;
		int r, slice = p->pump ? 20 : timeout_ms;
		uint32_t elapsed = pf_ticks_ms() - start;
		if (p->pump && p->pump(p->pump_user)) return ps2_cancel;
		if ((int)elapsed >= timeout_ms) return ps2_timeout;
		if (!p->pump && slice > timeout_ms - (int)elapsed) slice = timeout_ms - (int)elapsed;
		FD_ZERO(&set);
		FD_ZERO(&except);
		FD_SET(p->sock, &set);
		FD_SET(p->sock, &except);
		tv.tv_sec = slice / 1000;
		tv.tv_usec = (slice % 1000) * 1000;
		r = select((int)p->sock + 1, for_write ? NULL : &set, for_write ? &set : NULL, &except, &tv);
		if (r < 0) return ps2_err;
		if (r > 0) return ps2_ok;
	}
}

static int read_more(ps2 *p, int timeout_ms)
{
	int w = wait_socket(p, 0, timeout_ms);
	int r;
	if (w != ps2_ok) return w;
	if (p->cap - p->len < 4097) {
		size_t ncap = p->cap ? p->cap * 2 : 16384;
		char *grown = realloc(p->buf, ncap);
		if (!grown) return ps2_err;
		p->buf = grown;
		p->cap = ncap;
	}
	r = (int)recv(p->sock, p->buf + p->len, (int)(p->cap - p->len - 1), 0);
	if (r <= 0) return ps2_err;
	p->len += (size_t)r;
	p->buf[p->len] = 0;
	return ps2_ok;
}

static int send_all(ps2 *p, const char *data, size_t n)
{
	size_t done = 0;
	while (done < n) {
		size_t piece = n - done;
		int w, r;
		if (piece > send_piece) piece = send_piece;
		w = wait_socket(p, 1, p->timeout_ms);
		if (w != ps2_ok) return w;
		r = (int)send(p->sock, data + done, (int)piece, 0);
		if (r <= 0) return ps2_err;
		done += (size_t)r;
	}
	return ps2_ok;
}

static void consume(ps2 *p, size_t n)
{
	if (n >= p->len) {
		p->len = 0;
	} else {
		memmove(p->buf, p->buf + n, p->len - n);
		p->len -= n;
	}
	if (p->buf) p->buf[p->len] = 0;
}

static int wait_for(ps2 *p, const char *needle, char **at)
{
	for (;;) {
		char *found = p->buf ? strstr(p->buf, needle) : NULL;
		int r;
		if (found) {
			*at = found;
			return ps2_ok;
		}
		r = read_more(p, p->timeout_ms);
		if (r != ps2_ok) return r;
	}
}

static int sync_body(ps2 *p, char **body)
{
	char sentinel[40], marker[80], *at;
	size_t start = 0, end;
	int r;
	p->sync_id++;
	snprintf(sentinel, sizeof sentinel, "zz_sync_%u", p->sync_id);
	snprintf(marker, sizeof marker, "command '%s' not found!\n", sentinel);
	{
		char line[48];
		snprintf(line, sizeof line, "%s\n", sentinel);
		r = send_all(p, line, strlen(line));
		if (r != ps2_ok) return r;
	}
	r = wait_for(p, marker, &at);
	if (r != ps2_ok) return r;
	end = (size_t)(at - p->buf);
	if (p->skip_prompt && end > 0 && p->buf[0] == '>') start = 1;
	if (end > start && p->buf[end - 1] == '>') end--;
	if (body) {
		*body = malloc(end - start + 1);
		if (!*body) return ps2_err;
		memcpy(*body, p->buf + start, end - start);
		(*body)[end - start] = 0;
	}
	consume(p, (size_t)(at - p->buf) + strlen(marker));
	p->skip_prompt = 1;
	return ps2_ok;
}

void ps2_quote(const char *in, char *out, int out_size)
{
	int n = 0;
	if (out_size < 3) return;
	out[n++] = '\'';
	while (*in && n < out_size - 3) {
		if (*in == '\\' || *in == '\'') out[n++] = '\\';
		out[n++] = *in++;
	}
	out[n++] = '\'';
	out[n] = 0;
}

int ps2_connect(ps2 **out, const char *ip, int port, ps2_pump_fn pump, void *user, char *err, int err_size)
{
	ps2 *p;
	struct sockaddr_in addr;
	sock_t s;
	int r, one = 1, bufsize = socket_buffer;
	char *at;

	memset(&addr, 0, sizeof addr);
	addr.sin_family = AF_INET;
	if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) {
		set_err(err, err_size, "invalid IP address");
		return ps2_err;
	}
	addr.sin_port = htons((unsigned short)port);
	s = socket(AF_INET, SOCK_STREAM, 0);
	if (s == sock_invalid) {
		set_err(err, err_size, "cannot create socket");
		return ps2_err;
	}
	setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
	setsockopt(s, SOL_SOCKET, SO_SNDBUF, (const char *)&bufsize, sizeof bufsize);
	setsockopt(s, SOL_SOCKET, SO_RCVBUF, (const char *)&bufsize, sizeof bufsize);
	p = calloc(1, sizeof *p);
	if (!p) {
		sock_close(s);
		set_err(err, err_size, "out of memory");
		return ps2_err;
	}
	p->sock = s;
	p->pump = pump;
	p->pump_user = user;
	p->timeout_ms = 8000;
	snprintf(p->ip, sizeof p->ip, "%s", ip);
	set_nonblocking(s, 1);
	connect(s, (struct sockaddr *)&addr, sizeof addr);
	r = wait_socket(p, 1, p->timeout_ms);
	if (r == ps2_ok) {
		int so_err = 0;
		socklen_t len = sizeof so_err;
		getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&so_err, &len);
		if (so_err != 0) r = ps2_err;
	}
	if (r != ps2_ok) {
		sock_close(s);
		free(p);
		set_err(err, err_size, r == ps2_cancel ? "cancelled" : r == ps2_timeout ? "connection timed out" : "connection failed");
		return r;
	}
	set_nonblocking(s, 0);
	p->timeout_ms = 5000;
	r = wait_for(p, ">", &at);
	if (r != ps2_ok) {
		sock_close(s);
		free(p->buf);
		free(p);
		set_err(err, err_size, r == ps2_cancel ? "cancelled" : "no response (is another session open?)");
		return r;
	}
	consume(p, (size_t)(at - p->buf) + 1);
	p->skip_prompt = 0;
	p->timeout_ms = default_timeout;
	p->pump = NULL;
	p->pump_user = NULL;
	*out = p;
	return ps2_ok;
}

void ps2_close(ps2 *p)
{
	if (!p) return;
	sock_close(p->sock);
	free(p->buf);
	free(p);
}

void ps2_set_pump(ps2 *p, ps2_pump_fn pump, void *user)
{
	p->pump = pump;
	p->pump_user = user;
	p->timeout_ms = pump ? pump_timeout : default_timeout;
}

const char *ps2_ip(ps2 *p)
{
	return p->ip;
}

int ps2_exec(ps2 *p, const char *command, char **body)
{
	char *line;
	size_t n = strlen(command);
	int r;
	line = malloc(n + 2);
	if (!line) return ps2_err;
	memcpy(line, command, n);
	line[n] = '\n';
	line[n + 1] = 0;
	r = send_all(p, line, n + 1);
	free(line);
	if (r != ps2_ok) return r;
	return sync_body(p, body);
}

int ps2_list(ps2 *p, const char *path, char **body)
{
	char quoted[1100], cmd[1200];
	ps2_quote(path, quoted, sizeof quoted);
	snprintf(cmd, sizeof cmd, "ls %s", quoted);
	return ps2_exec(p, cmd, body);
}

int ps2_rmdir(ps2 *p, const char *path, char **body)
{
	char quoted[1100], cmd[1200], *at;
	int r;
	ps2_quote(path, quoted, sizeof quoted);
	snprintf(cmd, sizeof cmd, "rmdir %s\n", quoted);
	r = send_all(p, cmd, strlen(cmd));
	if (r != ps2_ok) return r;
	r = wait_for(p, "[Y/N]: ", &at);
	if (r != ps2_ok) return r;
	r = send_all(p, "Y\n", 2);
	if (r != ps2_ok) return r;
	return sync_body(p, body);
}

int ps2_recv_begin(ps2 *p, const char *path, uint64_t size)
{
	char quoted[1100], size_text[24], cmd[1200];
	ps2_quote(path, quoted, sizeof quoted);
	pf_u64_to_str(size, size_text);
	snprintf(cmd, sizeof cmd, "recv %s '%s'\n", quoted, size_text);
	return send_all(p, cmd, strlen(cmd));
}

int ps2_send_data(ps2 *p, const void *data, long size)
{
	return send_all(p, (const char *)data, (size_t)size);
}

int ps2_recv_end(ps2 *p, char **body)
{
	return sync_body(p, body);
}

int ps2_len(ps2 *p, const char *path, uint64_t *length)
{
	char quoted[1100], cmd[1200], *body = NULL, *s;
	uint64_t value = 0;
	int r;
	ps2_quote(path, quoted, sizeof quoted);
	snprintf(cmd, sizeof cmd, "len %s", quoted);
	r = ps2_exec(p, cmd, &body);
	if (r != ps2_ok) {
		free(body);
		return r;
	}
	s = body;
	if (!isdigit((unsigned char)*s)) {
		free(body);
		return ps2_err - 100;
	}
	while (isdigit((unsigned char)*s)) value = value * 10u + (uint64_t)(*s++ - '0');
	free(body);
	*length = value;
	return ps2_ok;
}

int ps2_cat(ps2 *p, const char *path, uint64_t start, uint64_t count, char **data)
{
	char quoted[1100], a[24], b[24], cmd[1300], *out;
	int r;
	if (count > 0x1000000u) return ps2_err;
	ps2_quote(path, quoted, sizeof quoted);
	pf_u64_to_str(start, a);
	pf_u64_to_str(start + count, b);
	snprintf(cmd, sizeof cmd, "cat %s '%s' '%s'\n", quoted, a, b);
	r = send_all(p, cmd, strlen(cmd));
	if (r != ps2_ok) return r;
	while (p->len < 1) {
		r = read_more(p, p->timeout_ms);
		if (r != ps2_ok) return r;
	}
	if (p->skip_prompt && p->buf[0] == '>') consume(p, 1);
	p->skip_prompt = 0;
	while (p->len < (size_t)count) {
		r = read_more(p, p->timeout_ms);
		if (r != ps2_ok) return r;
	}
	out = malloc((size_t)count + 1);
	if (!out) return ps2_err;
	memcpy(out, p->buf, (size_t)count);
	out[count] = 0;
	consume(p, (size_t)count);
	r = sync_body(p, NULL);
	if (r != ps2_ok) {
		free(out);
		return r;
	}
	*data = out;
	return ps2_ok;
}

int ps2_fire(ps2 *p, const char *command)
{
	char *line;
	size_t n = strlen(command);
	int r;
	line = malloc(n + 2);
	if (!line) return ps2_err;
	memcpy(line, command, n);
	line[n] = '\n';
	line[n + 1] = 0;
	p->pump = NULL;
	r = send_all(p, line, n + 1);
	free(line);
	if (r != ps2_ok) return r;
	while (read_more(p, 3000) == ps2_ok);
	return ps2_ok;
}
