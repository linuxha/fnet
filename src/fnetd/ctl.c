/* ctl.c — control socket for fnetctl (REQ-100..REQ-105, §4.2.4) */
#include "daemon.h"
#include "util.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#define CTL_MSG_MAX 65536
#define CTL_PROTO   1

struct ctl_client {
    struct evh         ev;
    struct ctl_client *next;
    bool               dead;
    struct ucred       cred;
};

static struct evh g_lev = { .fd = -1 };
static char g_path[108];
static struct ctl_client *g_clients;
static const struct log_ctx ctl_log = { "ctl", -1 };

struct tbuf {
    char   s[32768];
    size_t len;
};

static void tprintf(struct tbuf *t, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void tprintf(struct tbuf *t, const char *fmt, ...)
{
    va_list ap;
    int n;

    if (t->len >= sizeof t->s - 1)
        return;
    va_start(ap, fmt);
    n = vsnprintf(t->s + t->len, sizeof t->s - t->len, fmt, ap);
    va_end(ap);
    if (n > 0)
        t->len += (size_t)n < sizeof t->s - t->len ? (size_t)n : sizeof t->s - t->len - 1;
}

struct req {
    struct jpair kv[32];
    int          n;
    struct jb   *data;      /* open object for command-specific data */
    struct tbuf *text;
    char         err[512];
    char         ecode[16];
};

static const char *rget(struct req *r, const char *k)
{
    return jp_get(r->kv, r->n, k);
}

static int fail(struct req *r, const char *code, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

static int fail(struct req *r, const char *code, const char *fmt, ...)
{
    va_list ap;
    str_copy(r->ecode, code, sizeof r->ecode);
    va_start(ap, fmt);
    vsnprintf(r->err, sizeof r->err, fmt, ap);
    va_end(ap);
    return -1;
}

static struct port *need_port(struct req *r)
{
    const char *name = rget(r, "port");
    struct port *p;

    if (!name || !*name) {
        fail(r, "EINVAL", "missing port name");
        return NULL;
    }
    if (!(p = daemon_port_find(name)))
        fail(r, "ENOENT", "no such port '%s'", name);
    return p;
}

static int need_drive(struct req *r, unsigned *drv)
{
    long d = jp_long(r->kv, r->n, "drv", -1);
    if (d < 0 || d >= FLEX_DRIVES)
        return fail(r, "EINVAL", "drive must be 0..3");
    *drv = (unsigned)d;
    return 0;
}

/* ---------------------------------------------------------------- commands */

static void describe_port(struct req *r, const struct port *p)
{
    struct jb *j = r->data;

    jb_obj(j);
    jb_kstr(j, "name", p->cfg.name);
    jb_kstr(j, "type", port_type_name(p->cfg.type));
    jb_kbool(j, "enabled", p->cfg.enabled);
    jb_kstr(j, "link", port_link_name(p->link));
    jb_kstr(j, "peer", p->peer);
    jb_kstr(j, "mode", port_mode_name(p->mode));
    jb_ku64(j, "baud", p->cfg.baud);
    jb_ku64(j, "legacy_drive", p->cfg.legacy_drive);
    jb_kstr(j, "log_level", p->cfg.log_level < 0 ? "inherit" : log_level_name(p->cfg.log_level));
    jb_obj_end(j);
}

static int cmd_ports(struct req *r)
{
    jb_key(r->data, "ports");
    jb_arr(r->data);
    tprintf(r->text, "%-12s %-8s %-10s %-12s %-24s %s\n", "PORT", "TYPE", "LINK", "MODE", "PEER", "BAUD");
    for (struct port *p = ports_first(); p; p = p->next) {
        describe_port(r, p);
        tprintf(r->text, "%-12s %-8s %-10s %-12s %-24s %u\n", p->cfg.name, port_type_name(p->cfg.type),
                port_link_name(p->link), port_mode_name(p->mode), p->peer, p->cfg.baud);
    }
    jb_arr_end(r->data);
    return 0;
}

static void drives_text(struct req *r, const struct port *p)
{
    for (unsigned i = 0; i < FLEX_DRIVES; i++) {
        const struct mount *m = &p->fn.drv[i];
        tprintf(r->text, "  drive %u: %s%s\n", i, m->d ? m->vpath : "-",
                m->d ? (m->d->rw ? " (W)" : " (R)") : "");
    }
}

static void drives_json(struct req *r, const struct port *p)
{
    jb_key(r->data, "drives");
    jb_arr(r->data);
    for (unsigned i = 0; i < FLEX_DRIVES; i++) {
        const struct mount *m = &p->fn.drv[i];
        jb_obj(r->data);
        jb_ku64(r->data, "drive", i);
        if (m->d) {
            jb_kstr(r->data, "image", m->vpath);
            jb_kbool(r->data, "rw", m->d->rw);
        } else {
            jb_key(r->data, "image");
            jb_null(r->data);
        }
        jb_obj_end(r->data);
    }
    jb_arr_end(r->data);
}

static int cmd_sessions(struct req *r)
{
    jb_key(r->data, "sessions");
    jb_arr(r->data);
    for (struct port *p = ports_first(); p; p = p->next) {
        jb_obj(r->data);
        jb_kstr(r->data, "port", p->cfg.name);
        jb_kbool(r->data, "active", p->fn.active);
        jb_kstr(r->data, "mode", port_mode_name(p->mode));
        jb_kstr(r->data, "cwd", p->fn.cwd);
        jb_kstr(r->data, "load", p->tp.load_vpath);
        jb_kstr(r->data, "punch", p->tp.punch_vpath);
        drives_json(r, p);
        jb_obj_end(r->data);
        tprintf(r->text, "%s: %s, mode %s, cwd %s\n", p->cfg.name,
                p->fn.active ? "FlexNet session active" : "no FlexNet session",
                port_mode_name(p->mode), p->fn.cwd);
        drives_text(r, p);
        tprintf(r->text, "  tape load: %s\n  tape punch: %s\n",
                p->tp.load_vpath[0] ? p->tp.load_vpath : "-",
                p->tp.punch_vpath[0] ? p->tp.punch_vpath : "-");
    }
    jb_arr_end(r->data);
    return 0;
}

static int cmd_drives(struct req *r)
{
    struct port *p = need_port(r);
    if (!p)
        return -1;
    drives_json(r, p);
    tprintf(r->text, "%s:\n", p->cfg.name);
    drives_text(r, p);
    return 0;
}

static int cmd_mount(struct req *r)
{
    struct port *p = need_port(r);
    const char *path = rget(r, "path"), *why = NULL;
    char vp[VPATH_MAX], rw;
    unsigned drv;

    if (!p || need_drive(r, &drv) < 0)
        return -1;
    if (!path || !*path)
        return fail(r, "EINVAL", "missing image path");
    if (vpath_join("/", path, vp, sizeof vp) < 0)
        return fail(r, "EINVAL", "invalid path");
    if (port_busy(p))
        return fail(r, "EBUSY", "port %s is busy (%s)", p->cfg.name, port_mode_name(p->mode));
    if (flexnet_mount(p, drv, vp, !jp_bool(r->kv, r->n, "ro"), &rw, &why) < 0)
        return fail(r, "EIO", "mount %s: %s", vp, why ? why : strerror(errno));
    tprintf(r->text, "%s drive %u: %s (%c)\n", p->cfg.name, drv, vp, rw);
    jb_kstr(r->data, "image", vp);
    jb_kbool(r->data, "rw", rw == 'W');
    return 0;
}

static int cmd_umount(struct req *r)
{
    struct port *p = need_port(r);
    const char *d = rget(r, "drv");
    unsigned drv;

    if (!p)
        return -1;
    if (port_busy(p))
        return fail(r, "EBUSY", "port %s is busy (%s)", p->cfg.name, port_mode_name(p->mode));
    if (d && !strcmp(d, "all")) {
        for (unsigned i = 0; i < FLEX_DRIVES; i++)
            flexnet_umount(p, i);
        tprintf(r->text, "%s: all drives unmounted\n", p->cfg.name);
        return 0;
    }
    if (need_drive(r, &drv) < 0)
        return -1;
    flexnet_umount(p, drv);
    tprintf(r->text, "%s drive %u: unmounted\n", p->cfg.name, drv);
    return 0;
}

static int cmd_tape_set(struct req *r)
{
    struct port *p = need_port(r);
    const char *load = rget(r, "load"), *punch = rget(r, "punch");
    char spec[2 * VPATH_MAX + 4], msg[512];

    if (!p)
        return -1;
    if (p->mode == MODE_LOAD || p->mode == MODE_PUNCH)
        return fail(r, "EBUSY", "tape is running on %s", p->cfg.name);
    snprintf(spec, sizeof spec, "%s|%s", load ? load : "", punch ? punch : "");
    if (strlen(spec) > TEXT_MAX)
        return fail(r, "EINVAL", "selection too long");
    if (tape_select(p, spec, msg, sizeof msg) > 0)
        return fail(r, "EINVAL", "%s", msg);
    tprintf(r->text, "%s", msg);
    return 0;
}

static int cmd_tape_show(struct req *r)
{
    struct port *p = need_port(r);
    if (!p)
        return -1;
    jb_kstr(r->data, "mode", port_mode_name(p->mode));
    jb_kstr(r->data, "load", p->tp.load_vpath);
    jb_ku64(r->data, "load_pos", p->tp.pos);
    jb_ku64(r->data, "load_len", p->tp.len);
    jb_kstr(r->data, "punch", p->tp.punch_vpath);
    tprintf(r->text, "%s: mode %s\n  load:  %s", p->cfg.name, port_mode_name(p->mode),
            p->tp.load_vpath[0] ? p->tp.load_vpath : "-");
    if (p->tp.buf)
        tprintf(r->text, " (at byte %zu of %zu)", p->tp.pos, p->tp.len);
    tprintf(r->text, "\n  punch: %s\n", p->tp.punch_vpath[0] ? p->tp.punch_vpath : "-");
    return 0;
}

static int cmd_port_add(struct req *r)
{
    const char *name = rget(r, "port"), *kv = rget(r, "kv");
    struct port_cfg pc = *daemon_defaults();
    char buf[1024], *save = NULL, *tok, err[256];

    if (!name || !*name || strlen(name) >= CFG_NAME_MAX)
        return fail(r, "EINVAL", "invalid port name");
    if (daemon_port_find(name))
        return fail(r, "EEXIST", "port '%s' already exists", name);
    str_copy(pc.name, name, sizeof pc.name);
    str_copy(buf, kv ? kv : "", sizeof buf);
    for (tok = strtok_r(buf, "\n", &save); tok; tok = strtok_r(NULL, "\n", &save)) {
        char *eq = strchr(tok, '=');
        if (!eq)
            return fail(r, "EINVAL", "expected key=value, got '%s'", tok);
        *eq = '\0';
        if (config_port_kv(&pc, str_trim(tok), str_trim(eq + 1), err, sizeof err) < 0)
            return fail(r, "EINVAL", "%s", err);
    }
    if (config_port_validate(&pc, err, sizeof err) < 0 || daemon_port_add(&pc, err, sizeof err) < 0)
        return fail(r, "EINVAL", "%s", err);
    tprintf(r->text, "port %s added (not persistent: add it to the config file to keep it)\n", name);
    return 0;
}

static int cmd_port_set(struct req *r)
{
    struct port *p = need_port(r);
    const char *kv = rget(r, "kv");
    struct port_cfg pc;
    char buf[1024], *save = NULL, *tok, err[256];
    bool relink = false;

    if (!p)
        return -1;
    pc = p->cfg;
    str_copy(buf, kv ? kv : "", sizeof buf);
    for (tok = strtok_r(buf, "\n", &save); tok; tok = strtok_r(NULL, "\n", &save)) {
        char *eq = strchr(tok, '='), *k;
        if (!eq)
            return fail(r, "EINVAL", "expected key=value, got '%s'", tok);
        *eq = '\0';
        k = str_trim(tok);
        if (config_port_kv(&pc, k, str_trim(eq + 1), err, sizeof err) < 0)
            return fail(r, "EINVAL", "%s", err);
        relink |= config_key_is_link(k);
    }
    if (config_port_validate(&pc, err, sizeof err) < 0)
        return fail(r, "EINVAL", "%s", err);
    p->cfg = pc;
    p->log.level = pc.log_level;
    if (relink)
        port_restart(p);
    tprintf(r->text, "port %s updated%s (not persistent)\n", p->cfg.name, relink ? " and restarted" : "");
    return 0;
}

static int cmd_port_simple(struct req *r, const char *what)
{
    struct port *p;
    char err[256];

    if (!strcmp(what, "remove")) {
        const char *name = rget(r, "port");
        if (daemon_port_remove(name ? name : "", err, sizeof err) < 0)
            return fail(r, "ENOENT", "%s", err);
        tprintf(r->text, "port %s removed\n", name);
        return 0;
    }
    if (!(p = need_port(r)))
        return -1;
    if (!strcmp(what, "enable")) {
        p->cfg.enabled = true;
        port_restart(p);
    } else if (!strcmp(what, "disable")) {
        p->cfg.enabled = false;
        port_stop(p);
    } else {
        port_restart(p);
    }
    tprintf(r->text, "port %s %s%s\n", p->cfg.name, what, !strcmp(what, "restart") ? "ed" : "d");
    return 0;
}

static int cmd_log_level(struct req *r)
{
    const char *lv = rget(r, "level"), *name = rget(r, "port");
    int l;

    if (!lv)
        return fail(r, "EINVAL", "missing level");
    if (name && *name) {
        struct port *p = need_port(r);
        if (!p)
            return -1;
        l = strcmp(lv, "inherit") ? log_level_parse(lv) : -1;
        if (l < 0 && strcmp(lv, "inherit"))
            return fail(r, "EINVAL", "invalid level '%s'", lv);
        p->cfg.log_level = l;
        p->log.level = l;
        tprintf(r->text, "port %s log level %s\n", p->cfg.name, lv);
        return 0;
    }
    l = log_level_parse(lv);
    if (l < 0)
        return fail(r, "EINVAL", "invalid level '%s'", lv);
    log_set_level(l);
    tprintf(r->text, "log level %s\n", log_level_name(l));
    return 0;
}

static int cmd_stats(struct req *r)
{
    const char *name = rget(r, "port");
    const struct config *c = daemon_config();

    if (name && *name && !daemon_port_find(name))
        return fail(r, "ENOENT", "no such port '%s'", name);
    if (jp_bool(r->kv, r->n, "flush") && stats_write_file(c->stats_file) < 0)
        return fail(r, "EIO", "cannot write %s: %s", c->stats_file, strerror(errno));
    jb_key(r->data, "stats");
    stats_emit(r->data, name && *name ? name : NULL);
    for (struct port *p = ports_first(); p; p = p->next) {
        const struct port_stats *s = &p->st;
        if (name && *name && strcmp(name, p->cfg.name))
            continue;
        tprintf(r->text,
                "%s: link %s, rx %llu tx %llu, sectors r/w %llu/%llu, cksum err rx/tx %llu/%llu,\n"
                "    NAK sent/recv %llu/%llu, timeouts %llu, sync losses %llu, discarded %llu,\n"
                "    tape loaded %llu punched %llu, sessions %llu, connects %llu\n",
                p->cfg.name, port_link_name(p->link),
                (unsigned long long)s->rx_bytes, (unsigned long long)s->tx_bytes,
                (unsigned long long)s->sectors_read, (unsigned long long)s->sectors_written,
                (unsigned long long)s->cksum_err_rx, (unsigned long long)s->cksum_err_tx,
                (unsigned long long)s->naks_sent, (unsigned long long)s->naks_received,
                (unsigned long long)s->xfer_timeouts, (unsigned long long)s->sync_losses,
                (unsigned long long)s->discarded, (unsigned long long)s->tape_loaded,
                (unsigned long long)s->tape_punched, (unsigned long long)s->sessions,
                (unsigned long long)s->connects);
    }
    if (jp_bool(r->kv, r->n, "reset")) {
        for (struct port *p = ports_first(); p; p = p->next)
            if (!name || !*name || !strcmp(name, p->cfg.name))
                stats_reset(p);
        tprintf(r->text, "counters reset\n");
    }
    return 0;
}

static bool state_changing(const char *cmd)
{
    static const char *const ro[] = {
        "hello", "status", "version", "ports", "sessions", "drives", "tape_show", "stats", NULL,
    };
    for (int i = 0; ro[i]; i++)
        if (!strcmp(cmd, ro[i]))
            return false;
    return true;
}

static int dispatch(struct req *r, const char *cmd)
{
    if (!strcmp(cmd, "hello")) {
        jb_ku64(r->data, "proto", CTL_PROTO);
        return 0;
    }
    if (!strcmp(cmd, "version")) {
        jb_kstr(r->data, "version", FNET_VERSION);
        tprintf(r->text, "fnetd %s\n", FNET_VERSION);
        return 0;
    }
    if (!strcmp(cmd, "status")) {
        unsigned n = 0;
        for (struct port *p = ports_first(); p; p = p->next)
            n++;
        jb_kstr(r->data, "version", FNET_VERSION);
        jb_ku64(r->data, "uptime_s", daemon_uptime_s());
        jb_kstr(r->data, "config", daemon_config_path());
        jb_ku64(r->data, "ports", n);
        jb_kstr(r->data, "log_level", log_level_name(log_get_level()));
        tprintf(r->text, "fnetd %s, up %llu s, %u port(s), config %s, log level %s\n", FNET_VERSION,
                (unsigned long long)daemon_uptime_s(), n, daemon_config_path(),
                log_level_name(log_get_level()));
        return 0;
    }
    if (!strcmp(cmd, "reload")) {
        char err[512];
        if (daemon_reload(err, sizeof err) < 0)
            return fail(r, "EINVAL", "%s", err);
        tprintf(r->text, "configuration reloaded\n");
        return 0;
    }
    if (!strcmp(cmd, "ports"))        return cmd_ports(r);
    if (!strcmp(cmd, "sessions"))     return cmd_sessions(r);
    if (!strcmp(cmd, "drives"))       return cmd_drives(r);
    if (!strcmp(cmd, "mount"))        return cmd_mount(r);
    if (!strcmp(cmd, "umount"))       return cmd_umount(r);
    if (!strcmp(cmd, "tape_set"))     return cmd_tape_set(r);
    if (!strcmp(cmd, "tape_show"))    return cmd_tape_show(r);
    if (!strcmp(cmd, "tape_rewind") || !strcmp(cmd, "tape_cancel")) {
        struct port *p = need_port(r);
        if (!p)
            return -1;
        if (cmd[5] == 'r')
            tape_rewind(p);
        else
            tape_cancel(p);
        tprintf(r->text, "%s: tape %s\n", p->cfg.name, cmd + 5);
        return 0;
    }
    if (!strcmp(cmd, "port_add"))     return cmd_port_add(r);
    if (!strcmp(cmd, "port_set"))     return cmd_port_set(r);
    if (!strncmp(cmd, "port_", 5) &&
        (!strcmp(cmd + 5, "remove") || !strcmp(cmd + 5, "enable") ||
         !strcmp(cmd + 5, "disable") || !strcmp(cmd + 5, "restart")))
        return cmd_port_simple(r, cmd + 5);
    if (!strcmp(cmd, "log_level"))    return cmd_log_level(r);
    if (!strcmp(cmd, "log_reopen")) {
        if (log_reopen() < 0)
            return fail(r, "EIO", "cannot reopen log file: %s", strerror(errno));
        tprintf(r->text, "log reopened\n");
        return 0;
    }
    if (!strcmp(cmd, "stats"))        return cmd_stats(r);
    return fail(r, "EINVAL", "unknown command '%s'", cmd);
}

static void handle_request(struct ctl_client *c, const char *msg)
{
    static struct tbuf text;
    struct jb data, out;
    struct req r;
    const char *cmd;
    int rc;

    memset(&r, 0, sizeof r);
    text.len = 0;
    text.s[0] = '\0';
    jb_init(&data);
    jb_init(&out);
    r.data = &data;
    r.text = &text;
    jb_obj(&data);

    r.n = json_parse_flat(msg, r.kv, (int)ARRAY_LEN(r.kv));
    if (r.n < 0 || !(cmd = rget(&r, "cmd"))) {
        rc = fail(&r, "EINVAL", "malformed request");
        cmd = "?";
    } else {
        if (state_changing(cmd))
            flog(FL_NOTICE, &ctl_log, "ctl", "%s from uid %u pid %d", cmd,
                 (unsigned)c->cred.uid, (int)c->cred.pid);
        rc = dispatch(&r, cmd);
    }
    jb_obj_end(&data);

    jb_obj(&out);
    jb_kbool(&out, "ok", rc == 0);
    if (rc) {
        jb_kstr(&out, "err", r.ecode);
        jb_kstr(&out, "msg", r.err);
    }
    jb_kstr(&out, "text", text.s);
    jb_key(&out, "data");
    /* splice the data object in raw */
    if (!data.oom && data.s) {
        out.after_key = false;
        size_t need = out.len + data.len + 2;
        if (need > out.cap) {
            char *ns = realloc(out.s, need);
            if (ns) {
                out.s = ns;
                out.cap = need;
            } else {
                out.oom = true;
            }
        }
        if (!out.oom) {
            memcpy(out.s + out.len, data.s, data.len);
            out.len += data.len;
            out.s[out.len] = '\0';
        }
    } else {
        jb_null(&out);
    }
    jb_obj_end(&out);

    if (out.oom || out.len > CTL_MSG_MAX) {
        const char *e = "{\"ok\":false,\"err\":\"ENOMEM\",\"msg\":\"reply too large\",\"text\":\"\",\"data\":null}";
        send(c->ev.fd, e, strlen(e), MSG_NOSIGNAL);
    } else {
        send(c->ev.fd, out.s, out.len, MSG_NOSIGNAL);
    }
    jb_free(&data);
    jb_free(&out);
}

static void on_client(struct evh *h, uint32_t events)
{
    struct ctl_client *c = h->arg;
    static char buf[CTL_MSG_MAX + 1];
    ssize_t n;

    if (c->dead)
        return;
    if (events & EPOLLIN) {
        n = recv(c->ev.fd, buf, CTL_MSG_MAX, 0);
        if (n > 0) {
            buf[n] = '\0';
            handle_request(c, buf);
            return;
        }
        if (n < 0 && errno == EAGAIN)
            return;
    }
    loop_del(&c->ev);
    close(c->ev.fd);
    c->ev.fd = -1;
    c->dead = true;
}

static void on_listen(struct evh *h, uint32_t events)
{
    struct ctl_client *c;
    socklen_t sl;
    int fd;

    (void)h;
    (void)events;
    fd = accept4(g_lev.fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (fd < 0)
        return;
    c = calloc(1, sizeof *c);
    if (!c) {
        close(fd);
        return;
    }
    sl = sizeof c->cred;
    getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &c->cred, &sl);
    c->ev.fd = fd;
    c->ev.cb = on_client;
    c->ev.arg = c;
    c->next = g_clients;
    g_clients = c;
    loop_add(&c->ev, EPOLLIN);
}

int ctl_open(const char *path)
{
    struct sockaddr_un sa = { .sun_family = AF_UNIX };
    int fd;

    if (strlen(path) >= sizeof sa.sun_path) {
        errno = ENAMETOOLONG;
        return -1;
    }
    str_copy(sa.sun_path, path, sizeof sa.sun_path);
    str_copy(g_path, path, sizeof g_path);
    fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;
    unlink(path);
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) < 0 || listen(fd, 8) < 0) {
        close(fd);
        return -1;
    }
    chmod(path, 0660);
    g_lev.fd = fd;
    g_lev.cb = on_listen;
    g_lev.arg = NULL;
    return loop_add(&g_lev, EPOLLIN);
}

void ctl_reap(void)
{
    for (struct ctl_client **pp = &g_clients; *pp;) {
        struct ctl_client *c = *pp;
        if (c->dead) {
            *pp = c->next;
            free(c);
        } else {
            pp = &c->next;
        }
    }
}

void ctl_close(void)
{
    for (struct ctl_client *c = g_clients; c; c = c->next) {
        if (c->ev.fd >= 0) {
            loop_del(&c->ev);
            close(c->ev.fd);
            c->ev.fd = -1;
        }
        c->dead = true;
    }
    ctl_reap();
    if (g_lev.fd >= 0) {
        loop_del(&g_lev);
        close(g_lev.fd);
        g_lev.fd = -1;
        unlink(g_path);
    }
}
