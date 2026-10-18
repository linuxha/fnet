/* daemon.h — daemon-wide operations used by the control socket */
#ifndef FNETD_DAEMON_H
#define FNETD_DAEMON_H

#include "config.h"
#include "json.h"
#include "port.h"

#include <stddef.h>
#include <stdint.h>

int  daemon_reload(char *err, size_t errsz);
struct port *daemon_port_find(const char *name);
int  daemon_port_add(const struct port_cfg *pc, char *err, size_t errsz);
int  daemon_port_remove(const char *name, char *err, size_t errsz);
const char *daemon_config_path(void);
uint64_t daemon_uptime_s(void);
const struct port_cfg *daemon_defaults(void);

/* stats.c */
void stats_emit(struct jb *j, const char *port_filter);
int  stats_write_file(const char *path);
void stats_reset(struct port *p);

/* ctl.c */
int  ctl_open(const char *path);
void ctl_close(void);
void ctl_reap(void);

#endif
