/* stats.c — per-port counters and stats.json (REQ-114, REQ-115) */
#include "daemon.h"
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void emit_port(struct jb *j, const struct port *p)
{
    const struct port_stats *s = &p->st;

    jb_obj(j);
    jb_kstr(j, "name", p->cfg.name);
    jb_kstr(j, "type", port_type_name(p->cfg.type));
    jb_kstr(j, "link", port_link_name(p->link));
    jb_kstr(j, "peer", p->peer);
    jb_kstr(j, "mode", port_mode_name(p->mode));
    jb_kbool(j, "session", p->fn.active);
    jb_ku64(j, "connects", s->connects);
    jb_ku64(j, "rx_bytes", s->rx_bytes);
    jb_ku64(j, "tx_bytes", s->tx_bytes);
    jb_ku64(j, "sectors_read", s->sectors_read);
    jb_ku64(j, "sectors_written", s->sectors_written);
    jb_ku64(j, "cksum_err_rx", s->cksum_err_rx);
    jb_ku64(j, "cksum_err_tx", s->cksum_err_tx);
    jb_ku64(j, "naks_sent", s->naks_sent);
    jb_ku64(j, "naks_received", s->naks_received);
    jb_ku64(j, "xfer_timeouts", s->xfer_timeouts);
    jb_ku64(j, "sync_losses", s->sync_losses);
    jb_ku64(j, "discarded", s->discarded);
    jb_ku64(j, "sessions", s->sessions);
    jb_ku64(j, "tape_loaded", s->tape_loaded);
    jb_ku64(j, "tape_punched", s->tape_punched);
    jb_key(j, "mounts");
    jb_arr(j);
    for (unsigned i = 0; i < FLEX_DRIVES; i++) {
        if (!p->fn.drv[i].d)
            continue;
        jb_obj(j);
        jb_ku64(j, "drive", i);
        jb_kstr(j, "image", p->fn.drv[i].vpath);
        jb_kbool(j, "rw", p->fn.drv[i].d->rw);
        jb_obj_end(j);
    }
    jb_arr_end(j);
    jb_obj_end(j);
}

void stats_emit(struct jb *j, const char *port_filter)
{
    jb_obj(j);
    jb_kstr(j, "version", FNET_VERSION);
    jb_ku64(j, "uptime_s", daemon_uptime_s());
    jb_key(j, "ports");
    jb_arr(j);
    for (struct port *p = ports_first(); p; p = p->next)
        if (!port_filter || !strcmp(port_filter, p->cfg.name))
            emit_port(j, p);
    jb_arr_end(j);
    jb_obj_end(j);
}

int stats_write_file(const char *path)
{
    char tmp[CFG_PATH_MAX + 16];
    struct jb j;
    int fd, rc = 0;

    if (!path || !path[0])
        return 0;
    jb_init(&j);
    stats_emit(&j, NULL);
    if (j.oom) {
        jb_free(&j);
        return -1;
    }
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        jb_free(&j);
        return -1;
    }
    if (write(fd, j.s, j.len) != (ssize_t)j.len || write(fd, "\n", 1) != 1)
        rc = -1;
    close(fd);
    if (rc == 0 && rename(tmp, path) < 0)
        rc = -1;
    if (rc < 0)
        unlink(tmp);
    jb_free(&j);
    return rc;
}

void stats_reset(struct port *p)
{
    memset(&p->st, 0, sizeof p->st);
}
