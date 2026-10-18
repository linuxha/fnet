/* tape.c — S-Record paper tape reader/punch (§2.6, REQ-070..REQ-089) */
#include "port.h"
#include "srec.h"
#include "util.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define TAPE_MAX_FILE (16u * 1024u * 1024u)
static const char s9_record[] = "S9030000FC\r\n";

static void on_tick(struct ftimer *t);

void tape_init(struct port *p)
{
    p->tp.pdir = -1;
    p->tp.pfd = -1;
    timer_init(&p->tp.tick, on_tick, p);
}

static void free_load(struct tape *tp)
{
    free(tp->buf);
    tp->buf = NULL;
    tp->len = tp->pos = 0;
    tp->synthetic = false;
}

/* ---------------------------------------------------------------- selection */

static bool valid_tag(const char *s, size_t n)
{
    if (n < 1 || n > 16)
        return false;
    for (size_t i = 0; i < n; i++)
        if (!isalnum((unsigned char)s[i]) && s[i] != '_' && s[i] != '-')
            return false;
    return true;
}

/* Map a processor tag to an existing directory name under tape_root (REQ-071). */
static int resolve_cpu(struct port *p, const char *tag, char *dir, size_t dirsz, bool create)
{
    const struct config *c = daemon_config();
    const char *want = tag;
    struct dirent *de;
    DIR *d;
    int fd, rc = -1;

    if (!valid_tag(tag, strlen(tag)) || p->tape_root < 0)
        return -1;
    for (unsigned i = 0; c && i < c->nalias; i++)
        if (!strcasecmp(c->alias[i].tag, tag))
            want = c->alias[i].dir;

    fd = dup(p->tape_root);
    if (fd < 0 || !(d = fdopendir(fd))) {
        if (fd >= 0)
            close(fd);
        return -1;
    }
    rewinddir(d);
    while ((de = readdir(d))) {
        struct stat st;
        if (strcasecmp(de->d_name, want))
            continue;
        if (fstatat(p->tape_root, de->d_name, &st, 0) == 0 && S_ISDIR(st.st_mode)) {
            str_copy(dir, de->d_name, dirsz);
            rc = 0;
            break;
        }
    }
    closedir(d);
    if (rc < 0 && create && mkdirat(p->tape_root, want, 0775) == 0) {
        str_copy(dir, want, dirsz);
        rc = 0;
    }
    return rc;
}

/* "[CPU:]path" -> vpath under tape_root confined to the CPU directory (REQ-073). */
static int parse_field(struct port *p, const char *field, char *vp, size_t vpsz, const char **why)
{
    char tag[CFG_TAG_MAX], dir[CFG_TAG_MAX], base[VPATH_MAX];
    const char *colon = strchr(field, ':'), *path;
    size_t blen;

    if (colon) {
        size_t n = (size_t)(colon - field);
        if (!valid_tag(field, n)) {
            *why = "invalid processor tag";
            return -1;
        }
        memcpy(tag, field, n);
        tag[n] = '\0';
        path = colon + 1;
    } else {
        str_copy(tag, p->cfg.default_cpu, sizeof tag);
        path = field;
    }
    while (*path == '/')
        path++;
    if (!*path) {
        *why = "empty file name";
        return -1;
    }
    if (resolve_cpu(p, tag, dir, sizeof dir, false) < 0) {
        *why = "no directory for processor tag";
        return -1;
    }
    snprintf(base, sizeof base, "/%s", dir);
    if (vpath_join(base, path, vp, vpsz) < 0) {
        *why = "path escapes the tape directory";
        return -1;
    }
    blen = strlen(base);
    if (strncmp(vp, base, blen) || vp[blen] != '/') {
        *why = "path escapes the processor directory";
        return -1;
    }
    return 0;
}

static int read_file(struct port *p, const char *vpath, uint8_t **out, size_t *outlen)
{
    struct stat st;
    uint8_t *buf;
    size_t got = 0;
    int fd = open_beneath(p->tape_root, vpath_rel(vpath), O_RDONLY, 0);

    if (fd < 0)
        return -1;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || (uint64_t)st.st_size > TAPE_MAX_FILE) {
        close(fd);
        errno = EFBIG;
        return -1;
    }
    buf = malloc((size_t)st.st_size + 1);
    if (!buf) {
        close(fd);
        return -1;
    }
    while (got < (size_t)st.st_size) {
        ssize_t n = read(fd, buf + got, (size_t)st.st_size - got);
        if (n <= 0)
            break;
        got += (size_t)n;
    }
    close(fd);
    *out = buf;
    *outlen = got;
    return 0;
}

struct vctx { struct port *p; const char *what; unsigned shown; };

static void report_bad(unsigned line, enum srec_err e, void *arg)
{
    struct vctx *v = arg;
    if (v->shown++ < 5)
        flog(FL_WARN, &v->p->log, "tape", "%s line %u: %s", v->what, line, srec_strerror(e));
}

/* REQ-082: returns -1 if the file must be rejected. */
static int validate_load(struct port *p, const char *vpath)
{
    struct vctx v = { p, vpath, 0 };
    srec_summary_t sum;
    uint8_t *buf;
    size_t len;

    if (p->cfg.load_validate == VALID_OFF)
        return 0;
    if (read_file(p, vpath, &buf, &len) < 0) {
        flog(FL_WARN, &p->log, "tape", "load file %s: %s", vpath, strerror(errno));
        return 0;
    }
    srec_check_buf((const char *)buf, len, &sum, report_bad, &v);
    free(buf);
    if (!sum.terminated)
        flog(FL_WARN, &p->log, "tape", "load file %s has no S7/S8/S9 terminator", vpath);
    if (sum.errors && p->cfg.load_validate == VALID_REJECT) {
        flog(FL_WARN, &p->log, "tape", "load file %s rejected: %u bad line(s)", vpath, sum.errors);
        return -1;
    }
    return 0;
}

static void appendf(char *msg, size_t n, const char *fmt, const char *a, const char *b)
{
    size_t l = msg ? strlen(msg) : 0;
    if (msg && l < n)
        snprintf(msg + l, n - l, fmt, a, b);
}

int tape_select(struct port *p, const char *spec, char *msg, size_t msgsz)
{
    char buf[TEXT_MAX + 1], *fields[2] = { NULL, NULL }, *bar;
    int errors = 0;

    if (msg && msgsz)
        msg[0] = '\0';
    str_copy(buf, spec, sizeof buf);
    fields[0] = buf;
    if ((bar = strchr(buf, '|'))) {
        *bar = '\0';
        fields[1] = bar + 1;
    }
    for (int i = 0; i < 2; i++) {
        char *f = fields[i], vp[VPATH_MAX];
        char *dst = i == 0 ? p->tp.load_vpath : p->tp.punch_vpath;
        const char *what = i == 0 ? "load" : "punch", *why = NULL;

        if (!f || !*f)
            continue;                        /* REQ-072: keep current selection */
        if (!strcmp(f, "-")) {
            dst[0] = '\0';
            if (i == 0)
                free_load(&p->tp);
            flog(FL_INFO, &p->log, "tape", "%s file cleared", what);
            appendf(msg, msgsz, "%s cleared%s\n", what, "");
            continue;
        }
        if (parse_field(p, f, vp, sizeof vp, &why) < 0) {
            flog(FL_WARN, &p->log, "tape", "%s selection '%s' rejected: %s", what, f, why);
            appendf(msg, msgsz, "%s rejected: %s\n", what, why);
            errors++;
            continue;
        }
        if (i == 0 && validate_load(p, vp) < 0) {
            appendf(msg, msgsz, "%s rejected: %s\n", what, "S-Record validation failed");
            errors++;
            continue;
        }
        str_copy(dst, vp, VPATH_MAX);
        if (i == 0)
            free_load(&p->tp);               /* REQ-074: new selection starts at 0 */
        flog(FL_INFO, &p->log, "tape", "%s file %s", what, vp);
        appendf(msg, msgsz, "%s %s\n", what, vp);
    }
    return errors;
}

void tape_sel_start(struct port *p)
{
    p->tp.sel_len = 0;
    p->tp.sel_overflow = false;
    p->mode = MODE_TAPE_SEL;
    port_arm_xfer(p);
}

void tape_sel_byte(struct port *p, uint8_t b)
{
    struct tape *tp = &p->tp;

    if (b == CR || b == LF || b == EOT || b == 0) {
        p->mode = MODE_TOP;
        timer_cancel(&p->xfer_timer);
        if (tp->sel_overflow) {
            flog(FL_WARN, &p->log, "tape", "file selection longer than %d bytes ignored", TEXT_MAX);
            return;
        }
        tp->sel[tp->sel_len] = '\0';
        tape_select(p, tp->sel, NULL, 0);
        return;
    }
    if (tp->sel_len < TEXT_MAX)
        tp->sel[tp->sel_len++] = (char)b;
    else
        tp->sel_overflow = true;
    port_arm_xfer(p);
}

/* ---------------------------------------------------------------- load */

static int transform(const uint8_t *in, size_t n, int mode, uint8_t **out, size_t *outlen, char *eol_last)
{
    const char *eol = mode == EOL_CR ? "\r" : mode == EOL_LF ? "\n" : "\r\n";
    size_t el = strlen(eol), o = 0;
    uint8_t *buf;

    if (mode == EOL_RAW) {
        buf = malloc(n ? n : 1);
        if (!buf)
            return -1;
        memcpy(buf, in, n);
        *out = buf;
        *outlen = n;
        *eol_last = '\n';
        return 0;
    }
    buf = malloc(2 * n + 1);
    if (!buf)
        return -1;
    for (size_t i = 0; i < n; i++) {
        if (in[i] == '\r' || in[i] == '\n') {
            if (in[i] == '\r' && i + 1 < n && in[i + 1] == '\n')
                i++;
            memcpy(buf + o, eol, el);
            o += el;
        } else {
            buf[o++] = in[i];
        }
    }
    *out = buf;
    *outlen = o;
    *eol_last = eol[el - 1];
    return 0;
}

static int load_file(struct port *p)
{
    struct tape *tp = &p->tp;
    uint8_t *raw;
    size_t rlen;
    int rc;

    if (read_file(p, tp->load_vpath, &raw, &rlen) < 0)
        return -1;
    rc = transform(raw, rlen, p->cfg.load_eol, &tp->buf, &tp->len, &tp->eol_last);
    free(raw);
    tp->pos = 0;
    tp->synthetic = false;
    return rc;
}

void tape_load_start(struct port *p)
{
    struct tape *tp = &p->tp;

    if (!tp->buf || tp->pos >= tp->len) {
        free_load(tp);
        if (!tp->load_vpath[0] || load_file(p) < 0) {
            const char *why = tp->load_vpath[0] ? strerror(errno) : "no load file selected";
            if (p->cfg.load_missing == MISS_NONE) {
                flog(FL_WARN, &p->log, "tape", "DC1: %s; nothing sent", why);
                return;
            }
            flog(FL_WARN, &p->log, "tape", "DC1: %s; sending S9 terminator", why);
            tp->buf = malloc(sizeof s9_record);
            if (!tp->buf)
                return;
            memcpy(tp->buf, s9_record, sizeof s9_record - 1);
            tp->len = sizeof s9_record - 1;
            tp->pos = 0;
            tp->synthetic = true;
            tp->eol_last = '\n';
        } else {
            flog(FL_INFO, &p->log, "tape", "load %s started (%zu bytes)", tp->load_vpath, tp->len);
        }
    } else {
        flog(FL_INFO, &p->log, "tape", "load %s resumed at byte %zu", tp->load_vpath, tp->pos);
    }
    p->mode = MODE_LOAD;
    timer_arm(&tp->tick, 0);
}

void tape_load_pause(struct port *p)
{
    if (p->mode != MODE_LOAD)
        return;
    timer_cancel(&p->tp.tick);
    p->mode = MODE_TOP;
    flog(FL_INFO, &p->log, "tape", "load paused at byte %zu of %zu", p->tp.pos, p->tp.len);
}

static void on_tick(struct ftimer *t)
{
    struct port *p = t->arg;
    struct tape *tp = &p->tp;
    unsigned cus = port_char_us(p);
    uint64_t delay;
    uint8_t b;

    if (p->mode != MODE_LOAD || !tp->buf)
        return;
    if (p->link != LINK_UP) {
        timer_arm(&tp->tick, 100000);
        return;
    }
    /* REQ-076: at most one byte queued beyond the pacing point */
    if (port_pending_tx(p) > 1) {
        timer_arm(&tp->tick, cus);
        return;
    }
    b = tp->buf[tp->pos++];
    port_send_byte(p, b);
    p->st.tape_loaded++;
    if (tp->pos >= tp->len) {
        flog(FL_INFO, &p->log, "tape", "load %s complete (%zu bytes)",
             tp->synthetic ? "(terminator)" : tp->load_vpath, tp->len);
        p->mode = MODE_TOP;
        if (tp->synthetic)
            free_load(tp);
        return;
    }
    delay = (uint64_t)cus + p->cfg.char_delay_us;
    if ((char)b == tp->eol_last)
        delay += (uint64_t)p->cfg.line_delay_ms * 1000u;
    timer_arm(&tp->tick, delay);
}

void tape_rewind(struct port *p)
{
    if (p->mode == MODE_LOAD)
        tape_load_pause(p);
    free_load(&p->tp);
}

/* ---------------------------------------------------------------- punch */

static void punch_write(struct port *p, const char *buf, size_t n)
{
    struct tape *tp = &p->tp;

    if (tp->pfd < 0)
        return;
    while (n) {
        ssize_t w = write(tp->pfd, buf, n);
        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0) {
            flog(FL_ERROR, &p->log, "tape", "punch write %s: %s; discarding the rest",
                 tp->pvpath, strerror(errno));
            close(tp->pfd);
            tp->pfd = -1;
            return;
        }
        buf += w;
        n -= (size_t)w;
    }
}

static void punch_line_done(struct port *p)
{
    struct tape *tp = &p->tp;
    srec_info_t info;
    enum srec_err e;

    if (!tp->plen)
        return;
    tp->pline_no++;
    e = srec_check_line(tp->pline, tp->plen, &info);
    if (e == SREC_OK) {
        if (info.terminator)
            tp->pterminated = true;
    } else if (e != SREC_EMPTY) {
        tp->perrors++;
        flog(FL_WARN, &p->log, "tape", "punch %s line %u: %s", tp->pvpath, tp->pline_no, srec_strerror(e));
    }
    tp->pline[tp->plen++] = '\n';
    punch_write(p, tp->pline, tp->plen);
    tp->plen = 0;
}

static int auto_punch_path(struct port *p, char *vp, size_t n)
{
    char dir[CFG_TAG_MAX], sub[VPATH_MAX], ts[32];
    struct tm tm;
    time_t now = time(NULL);
    int dfd;

    if (resolve_cpu(p, p->cfg.default_cpu, dir, sizeof dir, true) < 0)
        return -1;
    snprintf(sub, sizeof sub, "%s/punch", dir);
    dfd = open_beneath(p->tape_root, dir, O_RDONLY | O_DIRECTORY, 0);
    if (dfd < 0)
        return -1;
    if (mkdirat(dfd, "punch", 0775) < 0 && errno != EEXIST) {
        close(dfd);
        return -1;
    }
    close(dfd);
    localtime_r(&now, &tm);
    strftime(ts, sizeof ts, "%Y%m%d-%H%M%S", &tm);
    if ((size_t)snprintf(vp, n, "/%s/%s-%s.s19", sub, p->cfg.name, ts) >= n)
        return -1;
    return 0;
}

static int open_punch(struct port *p)
{
    struct tape *tp = &p->tp;
    char base[VPATH_MAX];

    tp->prename = false;
    if (tp->punch_vpath[0]) {
        str_copy(tp->pvpath, tp->punch_vpath, sizeof tp->pvpath);
    } else if (auto_punch_path(p, tp->pvpath, sizeof tp->pvpath) < 0) {
        return -1;
    } else {
        flog(FL_INFO, &p->log, "tape", "no punch file selected; capturing to %s", tp->pvpath);
    }
    tp->pdir = open_parent_beneath(p->tape_root, tp->pvpath, base, sizeof base);
    if (tp->pdir < 0)
        return -1;
    str_copy(tp->pfinal, base, sizeof tp->pfinal);

    switch (p->cfg.punch_mode) {
    case PUNCH_APPEND:
        tp->pfd = openat(tp->pdir, base, O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW | O_CLOEXEC, 0664);
        break;
    case PUNCH_VERSION: {
        char name[VPATH_MAX], *dot;
        tp->pfd = openat(tp->pdir, base, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0664);
        str_copy(name, base, sizeof name);
        dot = strrchr(name, '.');
        if (dot)
            *dot++ = '\0';
        for (unsigned v = 1; tp->pfd < 0 && errno == EEXIST && v < 10000; v++) {
            char cand[VPATH_MAX + 16];
            if (dot)
                snprintf(cand, sizeof cand, "%s.%u.%s", name, v, dot);
            else
                snprintf(cand, sizeof cand, "%s.%u", name, v);
            tp->pfd = openat(tp->pdir, cand, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0664);
            if (tp->pfd >= 0) {
                char *slash = strrchr(tp->pvpath, '/');
                str_copy(tp->pfinal, cand, sizeof tp->pfinal);
                if (slash)
                    snprintf(slash + 1, sizeof tp->pvpath - (size_t)(slash + 1 - tp->pvpath), "%s", cand);
            }
        }
        break;
    }
    default: /* PUNCH_OVERWRITE: temp file + rename (REQ-089) */
        if ((size_t)snprintf(tp->ptmp, sizeof tp->ptmp, ".%s.%ld.tmp", base, (long)getpid()) >=
            sizeof tp->ptmp) {
            errno = ENAMETOOLONG;
            return -1;
        }
        unlinkat(tp->pdir, tp->ptmp, 0);
        tp->pfd = openat(tp->pdir, tp->ptmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0664);
        tp->prename = tp->pfd >= 0;
        break;
    }
    return tp->pfd < 0 ? -1 : 0;
}

void tape_punch_start(struct port *p)
{
    struct tape *tp = &p->tp;

    tp->plen = 0;
    tp->pline_no = tp->perrors = 0;
    tp->pterminated = false;
    tp->pbytes = 0;
    tp->pvpath[0] = '\0';
    if (p->tape_root < 0 || open_punch(p) < 0) {
        flog(FL_ERROR, &p->log, "tape", "DC2: cannot open punch file %s: %s; data will be discarded",
             tp->pvpath[0] ? tp->pvpath : "(auto)", strerror(errno));
        if (tp->pdir >= 0)
            close(tp->pdir);
        tp->pdir = -1;
        tp->pfd = -1;
    } else {
        flog(FL_INFO, &p->log, "tape", "punch to %s started", tp->pvpath);
    }
    /* Enter PUNCH even without a file so the data isn't read as commands. */
    p->mode = MODE_PUNCH;
    timer_arm(&tp->ptimer, (uint64_t)p->cfg.punch_timeout * 1000000u);
}

void tape_punch_byte(struct port *p, uint8_t b)
{
    struct tape *tp = &p->tp;

    if (b == DC4) {
        tape_punch_end(p, "DC4");
        return;
    }
    timer_arm(&tp->ptimer, (uint64_t)p->cfg.punch_timeout * 1000000u);
    if (b == 0x00 || b == 0x7f || b == 0xff)        /* REQ-085: leader/trailer */
        return;
    p->st.tape_punched++;
    tp->pbytes++;
    if (b == CR || b == LF) {
        punch_line_done(p);
        return;
    }
    if (tp->plen >= sizeof tp->pline - 2)
        punch_line_done(p);                          /* over-long line: split */
    tp->pline[tp->plen++] = (char)b;
}

void tape_punch_end(struct port *p, const char *why)
{
    struct tape *tp = &p->tp;

    if (p->mode != MODE_PUNCH)
        return;
    timer_cancel(&tp->ptimer);
    punch_line_done(p);
    p->mode = MODE_TOP;
    if (tp->pfd >= 0) {
        if (!tp->pterminated)
            flog(FL_WARN, &p->log, "tape", "punch %s has no S7/S8/S9 terminator", tp->pvpath);
        if (fsync(tp->pfd) < 0)
            flog(FL_ERROR, &p->log, "tape", "fsync %s: %s", tp->pvpath, strerror(errno));
        close(tp->pfd);
        tp->pfd = -1;
        if (tp->prename && renameat(tp->pdir, tp->ptmp, tp->pdir, tp->pfinal) < 0)
            flog(FL_ERROR, &p->log, "tape", "rename %s: %s", tp->pvpath, strerror(errno));
        flog(FL_INFO, &p->log, "tape", "punch %s complete (%s): %u line(s), %u error(s)",
             tp->pvpath, why, tp->pline_no, tp->perrors);
    }
    if (tp->pdir >= 0)
        close(tp->pdir);
    tp->pdir = -1;
}

void tape_timeout(struct port *p)
{
    if (p->mode != MODE_PUNCH)
        return;
    flog(FL_WARN, &p->log, "tape", "punch idle for %u s; closing file", p->cfg.punch_timeout);
    tape_punch_end(p, "timeout");
}

void tape_cancel(struct port *p)
{
    struct tape *tp = &p->tp;

    if (p->mode == MODE_LOAD) {
        tape_load_pause(p);
        free_load(tp);
        flog(FL_INFO, &p->log, "tape", "load cancelled");
    } else if (p->mode == MODE_PUNCH) {
        timer_cancel(&tp->ptimer);
        if (tp->pfd >= 0) {
            close(tp->pfd);
            tp->pfd = -1;
            if (tp->prename)
                unlinkat(tp->pdir, tp->ptmp, 0);
        }
        if (tp->pdir >= 0)
            close(tp->pdir);
        tp->pdir = -1;
        p->mode = MODE_TOP;
        flog(FL_INFO, &p->log, "tape", "punch cancelled");
    }
}

void tape_shutdown(struct port *p)
{
    if (p->mode == MODE_PUNCH)
        tape_punch_end(p, "shutdown");
    timer_cancel(&p->tp.tick);
    free_load(&p->tp);
}
