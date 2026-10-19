#ifndef PS2_H
#define PS2_H

#include <stdint.h>

#define ps2_ok 0
#define ps2_err (-1)
#define ps2_cancel (-2)
#define ps2_timeout (-3)

#define ps2_io_buffer 524288

typedef struct ps2 ps2;
typedef int (*ps2_pump_fn)(void *user);

int ps2_init(void);
int ps2_connect(ps2 **out, const char *ip, int port, ps2_pump_fn pump, void *user, char *err, int err_size);
void ps2_close(ps2 *p);
void ps2_set_pump(ps2 *p, ps2_pump_fn pump, void *user);
const char *ps2_ip(ps2 *p);

int ps2_exec(ps2 *p, const char *command, char **body);
int ps2_list(ps2 *p, const char *path, char **body);
int ps2_rmdir(ps2 *p, const char *path, char **body);
int ps2_len(ps2 *p, const char *path, uint64_t *length);
int ps2_cat(ps2 *p, const char *path, uint64_t start, uint64_t count, char **data);
int ps2_recv_begin(ps2 *p, const char *path, uint64_t size);
int ps2_send_data(ps2 *p, const void *data, long size);
int ps2_recv_end(ps2 *p, char **body);

void ps2_quote(const char *in, char *out, int out_size);

#endif
