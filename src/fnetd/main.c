/* fnetd — FLEX disk image and S-Record paper tape server (docs/fnetd-fsd.md) */
#include "daemon.h"
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <grp.h>
#include <libgen.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/file.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#define SHUTDOWN_GRACE_US 2500000u

static struct config g_cfg;
static char g_cfg_path[CFG_PATH_MAX] = "/etc/fnetd/fnetd.conf";
static struct port *g_ports, *g_graveyard;
static uint64_t g_start_us;
static bool g_foreground;
static int g_level_override = -1;
static bool g_stopping;
static uint64_t g_stop_deadline;
static struct evh g_sigev = { .fd = -1 };
static struct ftimer g_house;
static uint64_t g_last_stats;
static const struct log_ctx core_log = { NULL, -1 };

struct port *ports_first(void)              { return g_ports; }
const struct config *daemon_config(void)    { return &g_cfg; }
const char *daemon_config_path(void)        { return g_cfg_path; }
const struct port_cfg *daemon_defaults(void) { return &g_cfg.defaults; }
uint64_t daemon_uptime_s(void)              { return (now_us() - g_start_us) / 1000000u; }

struct port *daemon_port_find(const char *name)
{
    for (struct port *p = g_ports; p; p = p->next)
        if (!strcmp(p->cfg.name, name))
            return p;
    return NULL;
}

static void port_append(struct port *p)
{
    struct port **pp = &g_ports;
    while (*pp)
        pp = &(*pp)->next;
    p->next = NULL;
    *pp = p;
}

int daemon_port_add(const struct port_cfg *pc, char *err, size_t errsz)
{
    struct port *p = port_new(pc);

    if (!p) {
        snprintf(err, errsz, "out of memory");
        return -1;
    }
    port_append(p);
    port_start(p);
    return 0;
}

/* Unlink and stop now; free after the current loop iteration. */
static void port_bury(struct port *p)
{
    for (struct port **pp = &g_ports; *pp; pp = &(*pp)->next) {
        if (*pp == p) {
            *pp = p->next;
            break;
        }
    }
    port_stop(p);
    flexnet_end(p, "port removed");
    tape_shutdown(p);
    p->dead = true;
    p->next = g_graveyard;
    g_graveyard = p;
}

int daemon_port_remove(const char *name, char *err, size_t errsz)
{
    struct port *p = daemon_port_find(name);
    if (!p) {
        snprintf(err, errsz, "no such port '%s'", name);
        return -1;
    }
    flog(FL_INFO, &p->log, "core", "port removed");
    port_bury(p);
    return 0;
}

static void reap(void)
{
    while (g_graveyard) {
        struct port *p = g_graveyard;
        g_graveyard = p->next;
        port_free(p);
    }
    ctl_reap();
}

/* ---------------------------------------------------------------- reload (REQ-004/005) */

static bool link_equal(const struct port_cfg *a, const struct port_cfg *b)
{
    return a->type == b->type && a->enabled == b->enabled &&
           !strcmp(a->device, b->device) && !strcmp(a->host, b->host) && a->port == b->port &&
           !strcmp(a->bind, b->bind) && a->nallow == b->nallow &&
           !memcmp(a->allow, b->allow, sizeof a->allow) &&
           a->on_new_replace == b->on_new_replace && a->telnet == b->telnet && a->baud == b->baud &&
           !strcmp(a->flexnet_root, b->flexnet_root) && !strcmp(a->tape_root, b->tape_root);
}

static void apply_logging(const struct config *c)
{
    if (!g_foreground && log_open((enum log_sink)c->log_sink, c->log_file) < 0)
        flog(FL_ERROR, &core_log, "core", "cannot open log file %s: %s", c->log_file, strerror(errno));
    log_set_level(g_level_override >= 0 ? g_level_override : c->log_level);
}

int daemon_reload(char *err, size_t errsz)
{
    struct config nc;
    struct port *p, *next;

    if (config_load(g_cfg_path, &nc, err, errsz) < 0) {
        flog(FL_ERROR, &core_log, "core", "reload failed, keeping current configuration: %s", err);
        return -1;
    }
    if (strcmp(nc.ctl_socket, g_cfg.ctl_socket))
        flog(FL_WARN, &core_log, "core", "ctl_socket change needs a restart; keeping %s", g_cfg.ctl_socket);
    str_copy(nc.ctl_socket, g_cfg.ctl_socket, sizeof nc.ctl_socket);
    if (nc.log_sink != g_cfg.log_sink || strcmp(nc.log_file, g_cfg.log_file) ||
        nc.log_level != g_cfg.log_level)
        apply_logging(&nc);

    /* removed or changed ports */
    for (p = g_ports; p; p = next) {
        const struct port_cfg *npc = NULL;
        next = p->next;
        for (unsigned i = 0; i < nc.nports; i++)
            if (!strcmp(nc.ports[i].name, p->cfg.name))
                npc = &nc.ports[i];
        if (!npc) {
            flog(FL_INFO, &p->log, "core", "removed by reload");
            port_bury(p);
        } else if (!port_cfg_equal(npc, &p->cfg)) {
            bool relink = !link_equal(npc, &p->cfg);
            p->cfg = *npc;
            p->log.level = npc->log_level;
            if (relink) {
                flog(FL_INFO, &p->log, "core", "settings changed, restarting port");
                port_restart(p);
            } else {
                flog(FL_INFO, &p->log, "core", "settings updated");
            }
        }
    }
    /* new ports */
    for (unsigned i = 0; i < nc.nports; i++) {
        if (!daemon_port_find(nc.ports[i].name)) {
            char e[64];
            daemon_port_add(&nc.ports[i], e, sizeof e);
        }
    }
    config_free(&g_cfg);
    g_cfg = nc;
    flog(FL_NOTICE, &core_log, "core", "configuration reloaded (%u port(s))", g_cfg.nports);
    return 0;
}

/* ---------------------------------------------------------------- systemd */

static void sd_notify(const char *msg)
{
    const char *path = getenv("NOTIFY_SOCKET");
    struct sockaddr_un sa = { .sun_family = AF_UNIX };
    socklen_t sl;
    int fd;

    if (!path || !*path || strlen(path) >= sizeof sa.sun_path)
        return;
    str_copy(sa.sun_path, path, sizeof sa.sun_path);
    if (sa.sun_path[0] == '@')
        sa.sun_path[0] = '\0';
    sl = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + strlen(path));
    fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return;
    sendto(fd, msg, strlen(msg), MSG_NOSIGNAL, (struct sockaddr *)&sa, sl);
    close(fd);
}

/* ---------------------------------------------------------------- signals & housekeeping */

static void on_signal(struct evh *h, uint32_t events)
{
    struct signalfd_siginfo si;
    char err[512];

    (void)events;
    while (read(h->fd, &si, sizeof si) == (ssize_t)sizeof si) {
        switch (si.ssi_signo) {
        case SIGHUP:
            sd_notify("RELOADING=1");
            daemon_reload(err, sizeof err);
            sd_notify("READY=1");
            break;
        case SIGUSR1:
            if (log_reopen() < 0)
                flog(FL_ERROR, &core_log, "core", "log reopen failed: %s", strerror(errno));
            break;
        case SIGTERM:
        case SIGINT:
            if (!g_stopping) {
                flog(FL_NOTICE, &core_log, "core", "shutting down");
                sd_notify("STOPPING=1");
                g_stopping = true;
                g_stop_deadline = now_us() + SHUTDOWN_GRACE_US;
            }
            break;
        }
    }
}

static void on_house(struct ftimer *t)
{
    uint64_t now = now_us();

    for (struct port *p = g_ports; p; p = p->next) {
        uint64_t iv = (uint64_t)(p->cfg.sync_interval ? p->cfg.sync_interval : 5) * 1000000u;
        if (now - p->last_sync_us >= iv) {
            flexnet_sync_images(p);
            p->last_sync_us = now;
        }
    }
    if (g_cfg.stats_interval && now - g_last_stats >= (uint64_t)g_cfg.stats_interval * 1000000u) {
        g_last_stats = now;
        if (stats_write_file(g_cfg.stats_file) < 0)
            flog(FL_DEBUG, &core_log, "core", "stats %s: %s", g_cfg.stats_file, strerror(errno));
    }
    timer_arm(t, 1000000u);
}

static bool all_idle(void)
{
    for (struct port *p = g_ports; p; p = p->next)
        if (p->mode == MODE_FLEXNET && p->fn.state != FN_LIST)
            return false;
    return true;
}

/* ---------------------------------------------------------------- startup */

static int make_rundir(const char *ctl, char *rundir, size_t n)
{
    char tmp[CFG_PATH_MAX];

    str_copy(tmp, ctl, sizeof tmp);
    str_copy(rundir, dirname(tmp), n);
    if (mkdir(rundir, 0755) < 0 && errno != EEXIST)
        return -1;
    return 0;
}

static int take_lock(const char *rundir)
{
    char path[CFG_PATH_MAX + 16];
    int fd;

    snprintf(path, sizeof path, "%s/fnetd.lock", rundir);
    fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0)
        return -1;
    if (flock(fd, LOCK_EX | LOCK_NB) < 0) {
        close(fd);
        errno = EWOULDBLOCK;
        return -2;
    }
    return fd;
}

static int drop_privileges(const char *user, const char *rundir)
{
    struct passwd *pw;
    char path[CFG_PATH_MAX + 16];

    if (geteuid() != 0 || !user[0])
        return 0;
    pw = getpwnam(user);
    if (!pw) {
        fprintf(stderr, "fnetd: user '%s' not found\n", user);
        return -1;
    }
    if (chown(rundir, pw->pw_uid, pw->pw_gid) < 0)
        fprintf(stderr, "fnetd: chown %s: %s\n", rundir, strerror(errno));
    snprintf(path, sizeof path, "%s/fnetd.lock", rundir);
    if (chown(path, pw->pw_uid, pw->pw_gid) < 0)
        fprintf(stderr, "fnetd: chown %s: %s\n", path, strerror(errno));
    if (chown(g_cfg.ctl_socket, pw->pw_uid, pw->pw_gid) < 0)
        fprintf(stderr, "fnetd: chown %s: %s\n", g_cfg.ctl_socket, strerror(errno));
    if (initgroups(pw->pw_name, pw->pw_gid) < 0 || setgid(pw->pw_gid) < 0 || setuid(pw->pw_uid) < 0) {
        fprintf(stderr, "fnetd: cannot drop privileges to %s: %s\n", user, strerror(errno));
        return -1;
    }
    return 0;
}

static void usage(FILE *f)
{
    fprintf(f, "usage: fnetd [-f] [-c config] [-l level] [-V]\n"
               "  -f         run in the foreground, log to stderr\n"
               "  -c config  configuration file (default /etc/fnetd/fnetd.conf)\n"
               "  -l level   log level override (error|warn|notice|info|debug|trace)\n"
               "  -V         print version\n");
}

int main(int argc, char **argv)
{
    char err[512], rundir[CFG_PATH_MAX];
    sigset_t mask;
    int c, lockfd;

    while ((c = getopt(argc, argv, "fc:l:Vh")) != -1) {
        switch (c) {
        case 'f': g_foreground = true; break;
        case 'c': str_copy(g_cfg_path, optarg, sizeof g_cfg_path); break;
        case 'l':
            g_level_override = log_level_parse(optarg);
            if (g_level_override < 0) {
                fprintf(stderr, "fnetd: invalid log level '%s'\n", optarg);
                return 2;
            }
            break;
        case 'V': printf("fnetd %s\n", FNET_VERSION); return 0;
        case 'h': usage(stdout); return 0;
        default:  usage(stderr); return 2;
        }
    }

    if (config_load(g_cfg_path, &g_cfg, err, sizeof err) < 0) {
        fprintf(stderr, "fnetd: %s\n", err);          /* REQ-003 */
        return 1;
    }
    g_start_us = now_us();
    if (g_foreground)
        log_open(LOG_SINK_STDERR, NULL);
    apply_logging(&g_cfg);

    if (make_rundir(g_cfg.ctl_socket, rundir, sizeof rundir) < 0) {
        fprintf(stderr, "fnetd: cannot create %s: %s\n", rundir, strerror(errno));
        return 1;
    }
    lockfd = take_lock(rundir);
    if (lockfd == -2) {
        fprintf(stderr, "fnetd: already running (lock %s/fnetd.lock held)\n", rundir);
        return 1;
    }
    if (lockfd < 0) {
        fprintf(stderr, "fnetd: lock %s/fnetd.lock: %s\n", rundir, strerror(errno));
        return 1;
    }

    signal(SIGPIPE, SIG_IGN);
    sigemptyset(&mask);
    sigaddset(&mask, SIGHUP);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGUSR1);
    sigprocmask(SIG_BLOCK, &mask, NULL);

    if (loop_init() < 0) {
        fprintf(stderr, "fnetd: epoll: %s\n", strerror(errno));
        return 1;
    }
    if (ctl_open(g_cfg.ctl_socket) < 0) {
        fprintf(stderr, "fnetd: control socket %s: %s\n", g_cfg.ctl_socket, strerror(errno));
        return 1;
    }
    if (drop_privileges(g_cfg.user, rundir) < 0)
        return 1;

    g_sigev.fd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    g_sigev.cb = on_signal;
    loop_add(&g_sigev, EPOLLIN);

    flog(FL_NOTICE, &core_log, "core", "fnetd %s starting with %s (%u port(s))", FNET_VERSION,
         g_cfg_path, g_cfg.nports);
    for (unsigned i = 0; i < g_cfg.nports; i++)
        daemon_port_add(&g_cfg.ports[i], err, sizeof err);

    timer_init(&g_house, on_house, NULL);
    timer_arm(&g_house, 1000000u);
    g_last_stats = now_us();
    stats_write_file(g_cfg.stats_file);
    sd_notify("READY=1");

    for (;;) {
        loop_once(1000000u);
        reap();
        if (g_stopping && (all_idle() || now_us() >= g_stop_deadline))
            break;
    }

    while (g_ports)
        port_bury(g_ports);
    reap();
    ctl_close();
    stats_write_file(g_cfg.stats_file);
    config_free(&g_cfg);
    flog(FL_NOTICE, &core_log, "core", "stopped");
    close(lockfd);
    return 0;
}
