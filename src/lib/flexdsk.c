/* flexdsk.c — FLEX .DSK image library */
#include "flexdsk.h"
#include "util.h"
#include "vpath.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define SIR(b, f) ((b)[FLEX_SIR_OFFSET + (f)])

uint16_t flex_cksum(const uint8_t buf[FLEX_SECTOR_SIZE])
{
    unsigned sum = 0;
    for (unsigned i = 0; i < FLEX_SECTOR_SIZE; i++)
        sum += buf[i];
    return (uint16_t)(sum & 0xffffu);
}

int flexdsk_geom(const uint8_t sir[FLEX_SECTOR_SIZE], uint64_t size,
                 flex_geom_t *g, const char **why)
{
    const char *dummy;
    uint64_t blocks, data;
    unsigned end_trk = SIR(sir, 22), spt = SIR(sir, 23);

    if (!why)
        why = &dummy;
    if (size % FLEX_SECTOR_SIZE) {
        *why = "image size is not a multiple of 256";
        return -1;
    }
    if (size < (uint64_t)FLEX_MIN_SECTORS * FLEX_SECTOR_SIZE) {
        *why = "image too small";
        return -1;
    }
    if (size > FLEX_MAX_IMAGE) {
        *why = "image larger than 256x255 sectors";
        return -1;
    }
    if (spt < FLEX_MIN_SECTORS) {
        *why = "SIR sectors-per-track invalid";
        return -1;
    }
    blocks = size / FLEX_SECTOR_SIZE;
    data = (uint64_t)end_trk * spt;
    if (data >= blocks || blocks - data < FLEX_MIN_SECTORS || blocks - data > spt) {
        *why = "image size inconsistent with SIR geometry";
        return -1;
    }
    g->max_trk = (uint8_t)end_trk;
    g->spt = (uint8_t)spt;
    g->t0_spt = (uint8_t)(blocks - data);
    g->blocks = (uint32_t)blocks;
    return 0;
}

int flexdsk_ts2blk(const flex_geom_t *g, unsigned trk, unsigned sec, uint32_t *blk)
{
    if (trk > g->max_trk)
        return -1;
    if (trk == 0) {
        if (sec == 0) {
            *blk = 0;
            return 0;
        }
        if (sec > g->t0_spt)
            return -1;
        *blk = sec - 1;
        return 0;
    }
    if (sec == 0 || sec > g->spt)
        return -1;
    *blk = g->t0_spt + (uint32_t)(trk - 1) * g->spt + (sec - 1);
    return 0;
}

static int lock_writer(int fd)
{
    struct flock fl;

    memset(&fl, 0, sizeof fl);
    fl.l_type = F_WRLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start = 0;
    fl.l_len = 1;
    return fcntl(fd, F_OFD_SETLK, &fl);
}

int flexdsk_open(int dirfd, const char *rel, bool want_rw, flexdsk_t **out,
                 unsigned *flags, const char **why)
{
    const char *dummy;
    uint8_t sir[FLEX_SECTOR_SIZE];
    struct stat st;
    flexdsk_t *d;
    bool rw = false;
    unsigned fl = 0;
    int fd = -1;

    if (!why)
        why = &dummy;
    *why = NULL;
    if (want_rw) {
        fd = open_beneath(dirfd, rel, O_RDWR, 0);
        if (fd >= 0)
            rw = true;
        else if (errno == EACCES || errno == EROFS || errno == EPERM)
            fl |= FLEXDSK_RO_FILE;
        else {
            *why = strerror(errno);
            return -1;
        }
    }
    if (fd < 0) {
        fd = open_beneath(dirfd, rel, O_RDONLY, 0);
        if (fd < 0) {
            *why = strerror(errno);
            return -1;
        }
    }
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode)) {
        *why = "not a regular file";
        close(fd);
        errno = EINVAL;
        return -1;
    }
    if (pread(fd, sir, sizeof sir, (off_t)FLEX_SIR_BLOCK * FLEX_SECTOR_SIZE) != (ssize_t)sizeof sir) {
        *why = "cannot read SIR";
        close(fd);
        errno = EINVAL;
        return -1;
    }
    d = calloc(1, sizeof *d);
    if (!d) {
        close(fd);
        return -1;
    }
    if (flexdsk_geom(sir, (uint64_t)st.st_size, &d->g, why) < 0) {
        free(d);
        close(fd);
        errno = EINVAL;
        return -1;
    }
    if (rw && lock_writer(fd) < 0) {
        rw = false;
        fl |= FLEXDSK_RO_LOCKED;
    }
    d->fd = fd;
    d->rw = rw;
    d->dev = st.st_dev;
    d->ino = st.st_ino;
    if (flags)
        *flags = fl;
    *out = d;
    return 0;
}

int flexdsk_read(flexdsk_t *d, unsigned trk, unsigned sec, uint8_t buf[FLEX_SECTOR_SIZE])
{
    uint32_t blk;

    if (flexdsk_ts2blk(&d->g, trk, sec, &blk) < 0) {
        errno = EINVAL;
        return -1;
    }
    if (pread(d->fd, buf, FLEX_SECTOR_SIZE, (off_t)blk * FLEX_SECTOR_SIZE) != FLEX_SECTOR_SIZE) {
        if (errno == 0)
            errno = EIO;
        return -1;
    }
    return 0;
}

int flexdsk_write(flexdsk_t *d, unsigned trk, unsigned sec, const uint8_t buf[FLEX_SECTOR_SIZE])
{
    uint32_t blk;

    if (!d->rw) {
        errno = EROFS;
        return -1;
    }
    if (flexdsk_ts2blk(&d->g, trk, sec, &blk) < 0) {
        errno = EINVAL;
        return -1;
    }
    errno = 0;
    if (pwrite(d->fd, buf, FLEX_SECTOR_SIZE, (off_t)blk * FLEX_SECTOR_SIZE) != FLEX_SECTOR_SIZE) {
        if (errno == 0)
            errno = EIO;
        return -1;
    }
    d->dirty = true;
    return 0;
}

int flexdsk_sync(flexdsk_t *d)
{
    if (!d->dirty)
        return 0;
    d->dirty = false;
    return fdatasync(d->fd);
}

void flexdsk_close(flexdsk_t *d)
{
    if (!d)
        return;
    flexdsk_sync(d);
    close(d->fd);
    free(d);
}

int flexdsk_create_check(const flex_create_t *p, const char **why)
{
    unsigned t0 = p->t0_spt ? p->t0_spt : p->spt;

    if (p->tracks < 2 || p->tracks > FLEX_MAX_TRACKS) {
        *why = "tracks must be 2..256";
        return -1;
    }
    if (p->spt < FLEX_MIN_SECTORS || p->spt > FLEX_MAX_SECTORS) {
        *why = "sectors must be 5..255";
        return -1;
    }
    if (t0 < FLEX_MIN_SECTORS || t0 > p->spt) {
        *why = "track-0 sectors must be 5..sectors";
        return -1;
    }
    if (p->volume > 65535) {
        *why = "volume number must be 0..65535";
        return -1;
    }
    return 0;
}

static void link_next(uint8_t *s, unsigned t, unsigned sec)
{
    s[0] = (uint8_t)t;
    s[1] = (uint8_t)sec;
}

int flexdsk_create(int dirfd, const char *rel, const flex_create_t *p, const char **why)
{
    const char *dummy;
    unsigned t0, end_trk, free_cnt, mon = p->month, day = p->day, yr = p->year;
    uint8_t sec[FLEX_SECTOR_SIZE];
    off_t off = 0;
    int fd;

    if (!why)
        why = &dummy;
    if (flexdsk_create_check(p, why) < 0) {
        errno = EINVAL;
        return -1;
    }
    t0 = p->t0_spt ? p->t0_spt : p->spt;
    end_trk = p->tracks - 1;
    free_cnt = end_trk * p->spt;
    if (!mon) {
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        mon = (unsigned)tm.tm_mon + 1;
        day = (unsigned)tm.tm_mday;
        yr = (unsigned)tm.tm_year % 100;
    }

    fd = open_beneath(dirfd, rel, O_WRONLY | O_CREAT | O_EXCL, 0664);
    if (fd < 0) {
        *why = strerror(errno);
        return -1;
    }

    /* Track 0 */
    for (unsigned s = 1; s <= t0; s++) {
        memset(sec, 0, sizeof sec);
        if (s == 3) {
            size_t ll = strlen(p->label);
            memcpy(&SIR(sec, 0), p->label, ll > FLEX_LABEL_LEN ? FLEX_LABEL_LEN : ll);
            SIR(sec, 11) = (uint8_t)(p->volume >> 8);
            SIR(sec, 12) = (uint8_t)p->volume;
            SIR(sec, 13) = 1;
            SIR(sec, 14) = 1;
            SIR(sec, 15) = (uint8_t)end_trk;
            SIR(sec, 16) = (uint8_t)p->spt;
            SIR(sec, 17) = (uint8_t)(free_cnt >> 8);
            SIR(sec, 18) = (uint8_t)free_cnt;
            SIR(sec, 19) = (uint8_t)mon;
            SIR(sec, 20) = (uint8_t)day;
            SIR(sec, 21) = (uint8_t)yr;
            SIR(sec, 22) = (uint8_t)end_trk;
            SIR(sec, 23) = (uint8_t)p->spt;
        } else if (s >= FLEX_DIR_SECTOR && s < t0) {
            link_next(sec, 0, s + 1);
        }
        if (pwrite(fd, sec, sizeof sec, off) != (ssize_t)sizeof sec)
            goto io_fail;
        off += FLEX_SECTOR_SIZE;
    }

    /* Data tracks: one free chain in physical order */
    for (unsigned t = 1; t <= end_trk; t++) {
        for (unsigned s = 1; s <= p->spt; s++) {
            memset(sec, 0, sizeof sec);
            if (s < p->spt)
                link_next(sec, t, s + 1);
            else if (t < end_trk)
                link_next(sec, t + 1, 1);
            if (pwrite(fd, sec, sizeof sec, off) != (ssize_t)sizeof sec)
                goto io_fail;
            off += FLEX_SECTOR_SIZE;
        }
    }
    if (fsync(fd) < 0)
        goto io_fail;
    close(fd);
    return 0;

io_fail:
    *why = strerror(errno ? errno : EIO);
    close(fd);
    unlinkat(dirfd, rel, 0);
    return -1;
}

int flexdsk_read_sir(flexdsk_t *d, flex_sir_t *s)
{
    uint8_t b[FLEX_SECTOR_SIZE];

    if (flexdsk_read(d, 0, 3, b) < 0)
        return -1;
    memset(s, 0, sizeof *s);
    for (unsigned i = 0; i < FLEX_LABEL_LEN; i++) {
        uint8_t c = SIR(b, i);
        if (!c)
            break;
        s->label[i] = (c >= 0x20 && c < 0x7f) ? (char)c : '?';
    }
    s->volume = (unsigned)SIR(b, 11) << 8 | SIR(b, 12);
    s->ff_trk = SIR(b, 13);
    s->ff_sec = SIR(b, 14);
    s->lf_trk = SIR(b, 15);
    s->lf_sec = SIR(b, 16);
    s->free_sectors = (unsigned)SIR(b, 17) << 8 | SIR(b, 18);
    s->month = SIR(b, 19);
    s->day = SIR(b, 20);
    s->year = SIR(b, 21);
    s->end_trk = SIR(b, 22);
    s->end_sec = SIR(b, 23);
    return 0;
}

static void decode_dirent(const uint8_t *e, flex_dirent_t *o)
{
    size_t n = 0;

    memset(o, 0, sizeof *o);
    o->deleted = (e[0] & 0x80) != 0;
    for (unsigned i = 0; i < 8 && e[i]; i++) {
        uint8_t c = (uint8_t)(e[i] & 0x7f);
        o->name[n++] = (c >= 0x20 && c < 0x7f) ? (char)c : '?';
    }
    if (e[8]) {
        o->name[n++] = '.';
        for (unsigned i = 8; i < 11 && e[i]; i++)
            o->name[n++] = (e[i] >= 0x20 && e[i] < 0x7f) ? (char)e[i] : '?';
    }
    o->name[n] = '\0';
    o->st = e[13];
    o->ss = e[14];
    o->et = e[15];
    o->es = e[16];
    o->sectors = (unsigned)e[17] << 8 | e[18];
    o->random = e[19];
    o->month = e[21];
    o->day = e[22];
    o->year = e[23];
}

int flexdsk_dir(flexdsk_t *d, bool include_deleted, flex_dir_cb cb, void *arg)
{
    uint8_t b[FLEX_SECTOR_SIZE];
    unsigned t = 0, s = FLEX_DIR_SECTOR, steps = 0;

    while ((t || s) && steps++ < d->g.blocks) {
        if (flexdsk_read(d, t, s, b) < 0)
            return -1;
        for (unsigned i = 0; i < FLEX_DIR_PER_SEC; i++) {
            const uint8_t *e = b + 16 + i * FLEX_DIR_ENTRY;
            flex_dirent_t de;
            int r;

            if (e[0] == 0)
                return 0;               /* never-used entry: end of directory */
            decode_dirent(e, &de);
            if (de.deleted && !include_deleted)
                continue;
            if ((r = cb(&de, arg)) != 0)
                return r;
        }
        t = b[0];
        s = b[1];
    }
    return 0;
}

/* ---- consistency check (REQ-123) ---- */

enum { OWN_NONE, OWN_SYS, OWN_DIR, OWN_FREE, OWN_FILE };

struct check_ctx {
    flexdsk_t     *d;
    uint8_t       *own;
    int            errors;
    flex_report_cb report;
    void          *arg;
};

static void creport(struct check_ctx *c, bool is_error, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

static void creport(struct check_ctx *c, bool is_error, const char *fmt, ...)
{
    char msg[256];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if (is_error)
        c->errors++;
    if (c->report)
        c->report(msg, c->arg);
}

/* Follow a chain, marking blocks. Returns the number of sectors walked. */
static unsigned walk_chain(struct check_ctx *c, const char *what, unsigned t, unsigned s,
                           unsigned limit, uint8_t owner, unsigned *last_t, unsigned *last_s)
{
    uint8_t b[FLEX_SECTOR_SIZE];
    unsigned n = 0;

    *last_t = *last_s = 0;
    while ((t || s) && n < limit) {
        uint32_t blk;

        if (flexdsk_ts2blk(&c->d->g, t, s, &blk) < 0) {
            creport(c, true, "%s: link to invalid T/S %u/%u", what, t, s);
            break;
        }
        if (c->own[blk] != OWN_NONE) {
            creport(c, true, "%s: sector %u/%u already in use (%s)", what, t, s,
                    c->own[blk] == owner ? "cycle" : "cross-link");
            break;
        }
        c->own[blk] = owner;
        n++;
        *last_t = t;
        *last_s = s;
        if (flexdsk_read(c->d, t, s, b) < 0) {
            creport(c, true, "%s: read error at %u/%u", what, t, s);
            break;
        }
        t = b[0];
        s = b[1];
    }
    return n;
}

struct file_walk {
    struct check_ctx *c;
    unsigned files;
};

static int check_file(const flex_dirent_t *e, void *arg)
{
    struct file_walk *fw = arg;
    struct check_ctx *c = fw->c;
    unsigned lt, ls, n;

    fw->files++;
    n = walk_chain(c, e->name, e->st, e->ss, c->d->g.blocks,
                   OWN_FILE, &lt, &ls);
    if (n != e->sectors)
        creport(c, true, "%s: directory says %u sectors, chain has %u", e->name, e->sectors, n);
    else if (lt != e->et || ls != e->es)
        creport(c, true, "%s: chain ends at %u/%u, directory says %u/%u",
                e->name, lt, ls, e->et, e->es);
    return 0;
}

int flexdsk_check(flexdsk_t *d, flex_report_cb report, void *arg)
{
    struct check_ctx c = { .d = d, .report = report, .arg = arg };
    struct file_walk fw = { .c = &c };
    flex_sir_t sir;
    unsigned lt, ls, nfree, lost = 0;

    c.own = calloc(d->g.blocks, 1);
    if (!c.own)
        return -1;
    if (flexdsk_read_sir(d, &sir) < 0) {
        free(c.own);
        return -1;
    }
    for (unsigned s = 1; s < FLEX_DIR_SECTOR && s <= d->g.t0_spt; s++)
        c.own[s - 1] = OWN_SYS;

    walk_chain(&c, "directory", 0, FLEX_DIR_SECTOR, d->g.blocks, OWN_DIR, &lt, &ls);

    nfree = walk_chain(&c, "free chain", sir.ff_trk, sir.ff_sec, d->g.blocks, OWN_FREE, &lt, &ls);
    if (nfree != sir.free_sectors)
        creport(&c, true, "free chain has %u sectors, SIR says %u", nfree, sir.free_sectors);
    if (nfree && (lt != sir.lf_trk || ls != sir.lf_sec))
        creport(&c, true, "free chain ends at %u/%u, SIR says %u/%u", lt, ls, sir.lf_trk, sir.lf_sec);

    if (flexdsk_dir(d, false, check_file, &fw) < 0)
        creport(&c, true, "directory read error");

    for (uint32_t b = 0; b < d->g.blocks; b++)
        if (c.own[b] == OWN_NONE)
            lost++;
    if (lost)
        creport(&c, true, "%u sector(s) not in any chain", lost);

    creport(&c, false, "%u file(s), %u free sector(s), %d error(s)", fw.files, nfree, c.errors);
    free(c.own);
    return c.errors;
}
