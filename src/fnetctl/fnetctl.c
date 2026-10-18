/* fnetctl — runtime control of fnetd (REQ-100..REQ-105, §4.2.3) */
#include "json.h"
#include "util.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define EXIT_DAEMON 1
#define EXIT_USAGE  2
#define REPLY_MAX   (65536 + 1)

static const char *g_socket = "/run/fnetd/ctl";
static bool g_json;

static void usage(FILE *f)
{
    fprintf(f,
        "usage: fnetctl [--socket path] [--json] <command>\n"
        "  status | version | reload\n"
        "  ports\n"
        "  port add <name> type=tty|rfc2217|listen [key=val ...]\n"
        "  port remove|enable|disable|restart <name>\n"
        "  port set <name> key=val [key=val ...]\n"
        "  sessions\n"
        "  drives <port>\n"
        "  mount  <port> <drv> <path> [--ro] [--wait[=secs]]\n"
        "  umount <port> <drv|all> [--wait[=secs]]\n"
        "  tape set <port> [load=<cpu:file>] [punch=<cpu:file>]\n"
        "  tape show|rewind|cancel <port>\n"
        "  log level [--port P] <level|inherit>\n"
        "  log reopen\n"
        "  stats [--port P] [--flush] [--reset]\n");
}

static int connect_ctl(void)
{
    struct sockaddr_un sa = { .sun_family = AF_UNIX };
    int fd;

    if (strlen(g_socket) >= sizeof sa.sun_path) {
        fprintf(stderr, "fnetctl: socket path too long\n");
        return -1;
    }
    str_copy(sa.sun_path, g_socket, sizeof sa.sun_path);
    fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (fd < 0 || connect(fd, (struct sockaddr *)&sa, sizeof sa) < 0) {
        fprintf(stderr, "fnetctl: cannot connect to %s: %s\n", g_socket, strerror(errno));
        if (fd >= 0)
            close(fd);
        return -1;
    }
    return fd;
}

/* Send one request; returns exit code. If busy_retry, retry EBUSY for that many ms. */
static int transact(struct jb *req, long busy_retry_ms)
{
    static char reply[REPLY_MAX];
    struct jpair kv[8];
    char text[REPLY_MAX];
    int fd, n;
    ssize_t r;
    long waited = 0;

    if (req->oom)
        return EXIT_USAGE;
    for (;;) {
        fd = connect_ctl();
        if (fd < 0)
            return EXIT_USAGE;
        if (send(fd, req->s, req->len, MSG_NOSIGNAL) < 0) {
            fprintf(stderr, "fnetctl: send: %s\n", strerror(errno));
            close(fd);
            return EXIT_USAGE;
        }
        r = recv(fd, reply, sizeof reply - 1, 0);
        close(fd);
        if (r <= 0) {
            fprintf(stderr, "fnetctl: no reply from fnetd\n");
            return EXIT_USAGE;
        }
        reply[r] = '\0';
        n = json_parse_until(reply, kv, (int)ARRAY_LEN(kv), "text");
        if (n < 0) {
            fprintf(stderr, "fnetctl: malformed reply\n");
            return EXIT_USAGE;
        }
        if (!jp_bool(kv, n, "ok") && busy_retry_ms > waited) {
            const char *e = jp_get(kv, n, "err");
            if (e && !strcmp(e, "EBUSY")) {
                struct timespec ts = { 0, 100 * 1000000L };
                nanosleep(&ts, NULL);
                waited += 100;
                continue;
            }
        }
        break;
    }

    if (g_json) {
        printf("%s\n", reply);
    } else {
        if (json_find_str(reply, "text", text, sizeof text) == 0)
            fputs(text, stdout);
        if (!jp_bool(kv, n, "ok")) {
            const char *m = jp_get(kv, n, "msg");
            fprintf(stderr, "fnetctl: %s\n", m ? m : "error");
        }
    }
    return jp_bool(kv, n, "ok") ? 0 : EXIT_DAEMON;
}

static void kv_join(struct jb *j, const char *key, int argc, char **argv)
{
    char buf[2048];
    size_t len = 0;

    buf[0] = '\0';
    for (int i = 0; i < argc; i++) {
        int w = snprintf(buf + len, sizeof buf - len, "%s%s", i ? "\n" : "", argv[i]);
        if (w < 0 || (size_t)w >= sizeof buf - len)
            break;
        len += (size_t)w;
    }
    jb_kstr(j, key, buf);
}

static bool all_kv(int argc, char **argv)
{
    for (int i = 0; i < argc; i++)
        if (!strchr(argv[i], '=') || argv[i][0] == '=')
            return false;
    return true;
}

/* Parse --ro / --wait[=s] flags out of argv; returns remaining positional count. */
static int strip_flags(int argc, char **argv, bool *ro, long *wait_ms)
{
    int o = 0;
    for (int i = 0; i < argc; i++) {
        if (ro && !strcmp(argv[i], "--ro")) {
            *ro = true;
        } else if (wait_ms && !strncmp(argv[i], "--wait", 6) && (argv[i][6] == '\0' || argv[i][6] == '=')) {
            unsigned long s = 5;
            if (argv[i][6] == '=' && parse_ulong(argv[i] + 7, 3600, &s) < 0)
                return -1;
            *wait_ms = (long)s * 1000;
        } else {
            argv[o++] = argv[i];
        }
    }
    return o;
}

static int bad(void)
{
    usage(stderr);
    return EXIT_USAGE;
}

int main(int argc, char **argv)
{
    struct jb j;
    const char *env = getenv("FNETCTL_SOCKET");
    char **av;
    int ac, i = 1, rc;
    long wait_ms = 0;
    bool ro = false;

    if (env && *env)
        g_socket = env;
    while (i < argc && argv[i][0] == '-' && argv[i][1] == '-') {
        if (!strcmp(argv[i], "--json")) {
            g_json = true;
        } else if (!strcmp(argv[i], "--socket") && i + 1 < argc) {
            g_socket = argv[++i];
        } else if (!strcmp(argv[i], "--help")) {
            usage(stdout);
            return 0;
        } else {
            return bad();
        }
        i++;
    }
    if (i < argc && !strcmp(argv[i], "-h")) {
        usage(stdout);
        return 0;
    }
    if (i >= argc)
        return bad();
    av = argv + i;
    ac = argc - i;

    jb_init(&j);
    jb_obj(&j);

    if (ac == 1 && (!strcmp(av[0], "status") || !strcmp(av[0], "version") || !strcmp(av[0], "reload") ||
                    !strcmp(av[0], "ports") || !strcmp(av[0], "sessions"))) {
        jb_kstr(&j, "cmd", av[0]);
    } else if (!strcmp(av[0], "port") && ac >= 3) {
        const char *sub = av[1];
        if (!strcmp(sub, "add") || !strcmp(sub, "set")) {
            if (!all_kv(ac - 3, av + 3) || (!strcmp(sub, "set") && ac < 4))
                return bad();
            jb_kstr(&j, "cmd", !strcmp(sub, "add") ? "port_add" : "port_set");
            jb_kstr(&j, "port", av[2]);
            kv_join(&j, "kv", ac - 3, av + 3);
            if (!strcmp(sub, "add"))
                fprintf(stderr, "fnetctl: note: runtime changes are not written to the config file\n");
        } else if (ac == 3 && (!strcmp(sub, "remove") || !strcmp(sub, "enable") ||
                               !strcmp(sub, "disable") || !strcmp(sub, "restart"))) {
            char cmd[32];
            snprintf(cmd, sizeof cmd, "port_%s", sub);
            jb_kstr(&j, "cmd", cmd);
            jb_kstr(&j, "port", av[2]);
        } else {
            return bad();
        }
    } else if (!strcmp(av[0], "drives") && ac == 2) {
        jb_kstr(&j, "cmd", "drives");
        jb_kstr(&j, "port", av[1]);
    } else if (!strcmp(av[0], "mount")) {
        unsigned long drv;
        ac = strip_flags(ac, av, &ro, &wait_ms);
        if (ac != 4 || parse_ulong(av[2], 3, &drv) < 0)
            return bad();
        jb_kstr(&j, "cmd", "mount");
        jb_kstr(&j, "port", av[1]);
        jb_ku64(&j, "drv", drv);
        jb_kstr(&j, "path", av[3]);
        jb_kbool(&j, "ro", ro);
    } else if (!strcmp(av[0], "umount")) {
        unsigned long drv;
        ac = strip_flags(ac, av, NULL, &wait_ms);
        if (ac != 3)
            return bad();
        jb_kstr(&j, "cmd", "umount");
        jb_kstr(&j, "port", av[1]);
        if (!strcmp(av[2], "all"))
            jb_kstr(&j, "drv", "all");
        else if (parse_ulong(av[2], 3, &drv) == 0)
            jb_ku64(&j, "drv", drv);
        else
            return bad();
    } else if (!strcmp(av[0], "tape") && ac >= 3) {
        if (!strcmp(av[1], "set")) {
            jb_kstr(&j, "cmd", "tape_set");
            jb_kstr(&j, "port", av[2]);
            for (int k = 3; k < ac; k++) {
                if (!strncmp(av[k], "load=", 5))
                    jb_kstr(&j, "load", av[k] + 5);
                else if (!strncmp(av[k], "punch=", 6))
                    jb_kstr(&j, "punch", av[k] + 6);
                else
                    return bad();
            }
        } else if (ac == 3 && (!strcmp(av[1], "show") || !strcmp(av[1], "rewind") || !strcmp(av[1], "cancel"))) {
            char cmd[32];
            snprintf(cmd, sizeof cmd, "tape_%s", av[1]);
            jb_kstr(&j, "cmd", cmd);
            jb_kstr(&j, "port", av[2]);
        } else {
            return bad();
        }
    } else if (!strcmp(av[0], "log") && ac >= 2) {
        if (!strcmp(av[1], "reopen") && ac == 2) {
            jb_kstr(&j, "cmd", "log_reopen");
        } else if (!strcmp(av[1], "level")) {
            jb_kstr(&j, "cmd", "log_level");
            if (ac == 5 && !strcmp(av[2], "--port")) {
                jb_kstr(&j, "port", av[3]);
                jb_kstr(&j, "level", av[4]);
            } else if (ac == 3) {
                jb_kstr(&j, "level", av[2]);
            } else {
                return bad();
            }
        } else {
            return bad();
        }
    } else if (!strcmp(av[0], "stats")) {
        jb_kstr(&j, "cmd", "stats");
        for (int k = 1; k < ac; k++) {
            if (!strcmp(av[k], "--flush"))
                jb_kbool(&j, "flush", true);
            else if (!strcmp(av[k], "--reset"))
                jb_kbool(&j, "reset", true);
            else if (!strcmp(av[k], "--port") && k + 1 < ac)
                jb_kstr(&j, "port", av[++k]);
            else
                return bad();
        }
    } else {
        return bad();
    }
    jb_obj_end(&j);

    rc = transact(&j, wait_ms);
    jb_free(&j);
    return rc;
}
