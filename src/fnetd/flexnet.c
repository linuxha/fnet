/* flexnet.c — FlexNet / NetPC server state machine (§2.4, t/flexnet-protocol.md) */
#include "port.h"
#include "util.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

static const char flex_cmds[] = "sSrRQ?VPAIMmdcCDE";

bool flexnet_is_cmd(uint8_t b)
{
    return b && strchr(flex_cmds, b) != NULL;
}

static void send_ack(struct port *p) { port_send_byte(p, ACK); }

static void send_nak(struct port *p)
{
    p->st.naks_sent++;
    port_send_byte(p, NAK);
}

static void free_list(struct flexnet *fn)
{
    for (size_t i = 0; i < fn->nlist; i++)
        free(fn->list[i]);
    free(fn->list);
    fn->list = NULL;
    fn->nlist = fn->ilist = 0;
}

/* Transaction finished normally: back to top of loop. */
static void done(struct port *p)
{
    free_list(&p->fn);
    p->fn.state = FN_IDLE;
    p->fn.consec_timeouts = 0;
    p->mode = MODE_TOP;
    timer_cancel(&p->xfer_timer);
}

void flexnet_abort(struct port *p)
{
    free_list(&p->fn);
    p->fn.state = FN_IDLE;
    p->mode = MODE_TOP;
    timer_cancel(&p->xfer_timer);
}

void flexnet_umount(struct port *p, unsigned drv)
{
    struct mount *m = &p->fn.drv[drv];
    if (m->d) {
        flexdsk_close(m->d);
        flog(FL_INFO, &p->log, "flexnet", "drive %u: unmounted %s", drv, m->vpath);
    }
    m->d = NULL;
    m->vpath[0] = '\0';
}

void flexnet_end(struct port *p, const char *why)
{
    bool was = p->fn.active;

    flexnet_abort(p);
    for (unsigned i = 0; i < FLEX_DRIVES; i++)
        flexnet_umount(p, i);
    str_copy(p->fn.cwd, "/", sizeof p->fn.cwd);
    p->fn.active = false;
    p->fn.consec_timeouts = p->fn.consec_garbage = 0;
    if (was)
        flog(FL_INFO, &p->log, "flexnet", "session ended: %s", why);
}

static void sync_loss(struct port *p, const char *why)
{
    p->st.sync_losses++;
    flog(FL_WARN, &p->log, "flexnet", "sync lost (%s)", why);
    flexnet_end(p, "sync lost");
}

void flexnet_garbage(struct port *p)
{
    if (++p->fn.consec_garbage >= p->cfg.sync_loss_garbage && p->cfg.sync_loss_garbage) {
        p->fn.consec_garbage = 0;
        if (p->fn.active)
            sync_loss(p, "too many unexpected bytes");
    }
}

void flexnet_timeout(struct port *p)
{
    p->st.xfer_timeouts++;
    flog(FL_WARN, &p->log, "flexnet", "transaction '%c' timed out", isprint(p->fn.cmd) ? p->fn.cmd : '?');
    flexnet_abort(p);
    if (++p->fn.consec_timeouts >= p->cfg.sync_loss_count && p->cfg.sync_loss_count) {
        p->fn.consec_timeouts = 0;
        sync_loss(p, "repeated transaction timeouts");
    }
}

void flexnet_sync_images(struct port *p)
{
    for (unsigned i = 0; i < FLEX_DRIVES; i++)
        if (p->fn.drv[i].d && flexdsk_sync(p->fn.drv[i].d) < 0)
            flog(FL_ERROR, &p->log, "image", "fdatasync %s: %s", p->fn.drv[i].vpath, strerror(errno));
}

bool flexnet_image_in_use(dev_t dev, ino_t ino)
{
    for (struct port *q = ports_first(); q; q = q->next)
        for (unsigned i = 0; i < FLEX_DRIVES; i++)
            if (q->fn.drv[i].d && q->fn.drv[i].d->dev == dev && q->fn.drv[i].d->ino == ino)
                return true;
    return false;
}

/* ---------------------------------------------------------------- mount */

int flexnet_mount(struct port *p, unsigned drv, const char *vpath, bool want_rw,
                  char *rw, const char **why)
{
    flexdsk_t *d;
    unsigned flags = 0;

    if (drv >= FLEX_DRIVES) {
        *why = "invalid drive";
        errno = EINVAL;
        return -1;
    }
    if (p->flex_root < 0) {
        *why = "flexnet_root not available";
        errno = ENOENT;
        return -1;
    }
    if (flexdsk_open(p->flex_root, vpath_rel(vpath), want_rw && !p->cfg.read_only, &d, &flags, why) < 0)
        return -1;
    if (flags & FLEXDSK_RO_LOCKED)
        flog(FL_WARN, &p->log, "flexnet", "%s is open for writing elsewhere; mounted read-only", vpath);
    flexnet_umount(p, drv);           /* only after the new image opened (REQ-042) */
    p->fn.drv[drv].d = d;
    str_copy(p->fn.drv[drv].vpath, vpath, sizeof p->fn.drv[drv].vpath);
    *rw = d->rw ? 'W' : 'R';
    flog(FL_INFO, &p->log, "flexnet", "drive %u: mounted %s (%c, %u tracks x %u sectors)",
         drv, vpath, *rw, d->g.max_trk + 1u, d->g.spt);
    return 0;
}

/* M / m: try NAME.DSK, NAME.dsk, NAME in the cwd. */
static void mount_by_name(struct port *p, unsigned drv)
{
    static const char *const ext[] = { ".DSK", ".dsk", "" };
    const char *name = p->fn.text[0], *why = "not found";
    char cand[VPATH_MAX], vp[VPATH_MAX], rw;

    if (p->fn.overflow || !name[0] || drv >= FLEX_DRIVES || strchr(name, '/')) {
        send_nak(p);
        return;
    }
    for (size_t i = 0; i < ARRAY_LEN(ext); i++) {
        snprintf(cand, sizeof cand, "%s%s", name, ext[i]);
        if (vpath_join(p->fn.cwd, cand, vp, sizeof vp) < 0)
            break;
        if (flexnet_mount(p, drv, vp, true, &rw, &why) == 0) {
            uint8_t r[2] = { ACK, (uint8_t)rw };
            port_send(p, r, 2);
            return;
        }
        if (errno != ENOENT)
            break;
    }
    flog(FL_INFO, &p->log, "flexnet", "mount %s on drive %u failed: %s", name, drv, why);
    send_nak(p);
}

/* ---------------------------------------------------------------- sectors */

static void do_read(struct port *p)
{
    unsigned drv = p->fn.bin[0], trk = p->fn.bin[1], sec = p->fn.bin[2];
    flexdsk_t *d = drv < FLEX_DRIVES ? p->fn.drv[drv].d : NULL;
    uint8_t out[FLEX_SECTOR_SIZE + 2];
    uint16_t ck;

    if (d && flexdsk_read(d, trk, sec, out) == 0) {
        ck = flex_cksum(out);
        p->st.sectors_read++;
        flog(FL_DEBUG, &p->log, "flexnet", "read drive %u T%u S%u", drv, trk, sec);
    } else {
        /* REQ-036: 256 zeros + deliberately wrong checksum, exactly 258 bytes */
        memset(out, 0, FLEX_SECTOR_SIZE);
        ck = 0x0001;
        flog(FL_INFO, &p->log, "flexnet", "read drive %u T%u S%u failed: %s", drv, trk, sec,
             !d ? "no image mounted" : errno == EINVAL ? "invalid track/sector" : strerror(errno));
    }
    out[FLEX_SECTOR_SIZE] = (uint8_t)(ck >> 8);
    out[FLEX_SECTOR_SIZE + 1] = (uint8_t)ck;
    port_send(p, out, sizeof out);
    p->fn.state = FN_READACK;
    p->mode = MODE_FLEXNET;
    port_arm_xfer(p);
}

static void do_write(struct port *p)
{
    unsigned drv = p->fn.bin[0], trk = p->fn.bin[1], sec = p->fn.bin[2];
    const uint8_t *data = p->fn.bin + 3;
    uint16_t ck = (uint16_t)(p->fn.bin[259] << 8 | p->fn.bin[260]);
    flexdsk_t *d = drv < FLEX_DRIVES ? p->fn.drv[drv].d : NULL;

    if (flex_cksum(data) != ck) {
        p->st.cksum_err_rx++;
        flog(FL_WARN, &p->log, "flexnet", "write drive %u T%u S%u: checksum error", drv, trk, sec);
        send_nak(p);
        return;
    }
    if (!d) {
        flog(FL_INFO, &p->log, "flexnet", "write drive %u T%u S%u: no image mounted", drv, trk, sec);
        send_nak(p);
        return;
    }
    if (flexdsk_write(d, trk, sec, data) < 0) {
        flog(FL_INFO, &p->log, "flexnet", "write drive %u T%u S%u failed: %s", drv, trk, sec,
             errno == EINVAL ? "invalid track/sector" : errno == EROFS ? "read-only" : strerror(errno));
        send_nak(p);
        return;
    }
    p->st.sectors_written++;
    flog(FL_DEBUG, &p->log, "flexnet", "write drive %u T%u S%u", drv, trk, sec);
    send_ack(p);
}

/* ---------------------------------------------------------------- directories */

static int cmp_ci(const void *a, const void *b)
{
    return strcasecmp(*(const char *const *)a, *(const char *const *)b);
}

static bool name_matches(const char *name, const char *pat)
{
    char stem[256];
    size_t sl;

    if (!*pat)
        return true;
    if (strpbrk(pat, "*?")) {
        str_copy(stem, name, sizeof stem);
        sl = strlen(stem);
        if (sl > 4)
            stem[sl - 4] = '\0';
        return fnmatch(pat, name, FNM_CASEFOLD) == 0 || fnmatch(pat, stem, FNM_CASEFOLD) == 0;
    }
    return strncasecmp(name, pat, strlen(pat)) == 0;
}

static void build_list(struct port *p, bool dirs, const char *pattern)
{
    struct flexnet *fn = &p->fn;
    struct dirent *de;
    size_t cap = 0;
    DIR *dir;
    int dfd;

    free_list(fn);
    if (p->flex_root < 0)
        return;
    dfd = open_beneath(p->flex_root, vpath_rel(fn->cwd), O_RDONLY | O_DIRECTORY, 0);
    if (dfd < 0 || !(dir = fdopendir(dfd))) {
        if (dfd >= 0)
            close(dfd);
        return;
    }
    while ((de = readdir(dir))) {
        struct stat st;
        bool ok;

        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;
        if (fstatat(dirfd(dir), de->d_name, &st, 0) < 0)
            continue;
        if (dirs)
            ok = S_ISDIR(st.st_mode);
        else
            ok = S_ISREG(st.st_mode) && strlen(de->d_name) > 4 &&
                 str_endswith_ci(de->d_name, ".dsk") && name_matches(de->d_name, pattern);
        if (!ok)
            continue;
        if (fn->nlist == cap) {
            size_t ncap = cap ? cap * 2 : 32;
            char **nl = realloc(fn->list, ncap * sizeof *nl);
            if (!nl)
                break;
            fn->list = nl;
            cap = ncap;
        }
        if (!(fn->list[fn->nlist] = strdup(de->d_name)))
            break;
        fn->nlist++;
    }
    closedir(dir);
    if (fn->nlist)
        qsort(fn->list, fn->nlist, sizeof *fn->list, cmp_ci);
}

static void list_next(struct port *p)
{
    struct flexnet *fn = &p->fn;

    if (fn->ilist < fn->nlist) {
        const char *s = fn->list[fn->ilist++];
        port_send(p, s, strlen(s));
        port_send(p, "\r\n", 2);
        return;
    }
    send_ack(p);
    done(p);
}

static void do_cd(struct port *p)
{
    char vp[VPATH_MAX];
    int fd;

    if (p->fn.overflow || p->flex_root < 0 || vpath_join(p->fn.cwd, p->fn.text[0], vp, sizeof vp) < 0) {
        send_nak(p);
        return;
    }
    fd = open_beneath(p->flex_root, vpath_rel(vp), O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0) {
        flog(FL_INFO, &p->log, "flexnet", "cd %s: %s", vp, strerror(errno));
        send_nak(p);
        return;
    }
    close(fd);
    str_copy(p->fn.cwd, vp, sizeof p->fn.cwd);
    flog(FL_DEBUG, &p->log, "flexnet", "cwd %s", vp);
    send_ack(p);
}

static int field_num(const char *s, unsigned long max, unsigned *out)
{
    char buf[TEXT_MAX + 1];
    unsigned long v;

    str_copy(buf, s, sizeof buf);
    if (parse_ulong(str_trim(buf), max, &v) < 0)
        return -1;
    *out = (unsigned)v;
    return 0;
}

static void do_create(struct port *p, int drv)
{
    struct flexnet *fn = &p->fn;
    flex_create_t cp;
    char dir[VPATH_MAX], fname[TEXT_MAX + 5], file[VPATH_MAX], name[TEXT_MAX + 1], *dot, rw;
    const char *why = "invalid parameters";

    memset(&cp, 0, sizeof cp);
    str_copy(name, fn->text[1], sizeof name);
    str_copy(name, str_trim(name), sizeof name);
    if (p->cfg.read_only) {
        why = "port is read-only";
        goto nak;
    }
    if (fn->overflow || p->flex_root < 0 || !name[0] || strchr(name, '/') ||
        field_num(fn->text[2], 65535, &cp.volume) < 0 ||
        field_num(fn->text[3], 256, &cp.tracks) < 0 ||
        field_num(fn->text[4], 255, &cp.spt) < 0 ||
        (drv >= 0 && drv >= FLEX_DRIVES))
        goto nak;
    if (vpath_join(fn->cwd, fn->text[0][0] ? fn->text[0] : ".", dir, sizeof dir) < 0)
        goto nak;
    snprintf(fname, sizeof fname, "%s%s", name, str_endswith_ci(name, ".dsk") ? "" : ".DSK");
    if (vpath_join(dir, fname, file, sizeof file) < 0)
        goto nak;
    str_copy(cp.label, name, sizeof cp.label);
    if ((dot = strchr(cp.label, '.')))
        *dot = '\0';
    if (flexdsk_create(p->flex_root, vpath_rel(file), &cp, &why) < 0)
        goto nak;
    flog(FL_INFO, &p->log, "flexnet", "created %s (%u tracks x %u sectors, volume %u)",
         file, cp.tracks, cp.spt, cp.volume);
    if (drv >= 0 && flexnet_mount(p, (unsigned)drv, file, true, &rw, &why) < 0)
        goto nak;
    send_ack(p);
    return;

nak:
    flog(FL_INFO, &p->log, "flexnet", "create '%s' failed: %s", name, why);
    send_nak(p);
}

static void do_delete(struct port *p)
{
    char vp[VPATH_MAX], base[VPATH_MAX];
    const char *why = "invalid name";
    struct stat st;
    int dfd = -1;

    if (p->fn.overflow || p->flex_root < 0 || !str_endswith_ci(p->fn.text[0], ".dsk") ||
        strchr(p->fn.text[0], '/') || vpath_join(p->fn.cwd, p->fn.text[0], vp, sizeof vp) < 0)
        goto nak;
    if (p->cfg.read_only) {
        why = "port is read-only";
        goto nak;
    }
    dfd = open_parent_beneath(p->flex_root, vp, base, sizeof base);
    if (dfd < 0 || fstatat(dfd, base, &st, AT_SYMLINK_NOFOLLOW) < 0) {
        why = "not found";
        goto nak;
    }
    if (!S_ISREG(st.st_mode)) {
        why = "not a regular file";
        goto nak;
    }
    if (flexnet_image_in_use(st.st_dev, st.st_ino)) {
        why = "image is mounted";
        goto nak;
    }
    if (faccessat(dfd, base, W_OK, AT_EACCESS) < 0) {
        why = "write-protected";
        goto nak;
    }
    if (unlinkat(dfd, base, 0) < 0) {
        why = strerror(errno);
        goto nak;
    }
    close(dfd);
    flog(FL_INFO, &p->log, "flexnet", "deleted %s", vp);
    send_ack(p);
    return;

nak:
    if (dfd >= 0)
        close(dfd);
    flog(FL_INFO, &p->log, "flexnet", "delete '%s' failed: %s", p->fn.text[0], why);
    send_nak(p);
}

/* ---------------------------------------------------------------- dispatch */

/* Run a fully received command. */
static void execute(struct port *p)
{
    struct flexnet *fn = &p->fn;
    unsigned drv;

    switch (fn->cmd) {
    case 0x55:
    case 0xaa:
        port_send_byte(p, fn->cmd);
        break;
    case 'Q':
    case 'V':
        send_ack(p);
        break;
    case '?':
        port_send(p, fn->cwd, strlen(fn->cwd));
        port_send_byte(p, CR);
        send_ack(p);
        break;
    case 's':
    case 'S':
        do_read(p);
        return;                         /* waits for ACK/NAK */
    case 'r':
    case 'R':
        do_write(p);
        break;
    case 'M':
        mount_by_name(p, p->cfg.legacy_drive);
        break;
    case 'm':
        mount_by_name(p, fn->bin[0]);
        break;
    case 'd':
        drv = fn->bin[0];
        if (drv < FLEX_DRIVES && fn->drv[drv].d) {
            const char *b = vpath_base(fn->drv[drv].vpath);
            port_send(p, b, strlen(b));
        }
        send_ack(p);
        break;
    case 'P':
        do_cd(p);
        break;
    case 'A': {
        char pat[TEXT_MAX + 1];
        if (fn->overflow) {
            send_ack(p);
            break;
        }
        str_copy(pat, fn->text[0], sizeof pat);
        build_list(p, false, str_trim(pat));
        port_send(p, "\r\n", 2);
        fn->state = FN_LIST;
        p->mode = MODE_FLEXNET;
        timer_cancel(&p->xfer_timer);   /* the user may pause the listing */
        return;
    }
    case 'I':
        build_list(p, true, "");
        fn->state = FN_LIST_FIRST_SP;
        p->mode = MODE_FLEXNET;
        port_arm_xfer(p);
        return;
    case 'C':
        do_create(p, -1);
        break;
    case 'c':
        do_create(p, fn->bin[0]);
        break;
    case 'D':
        do_delete(p);
        break;
    case 'E':
        send_ack(p);
        done(p);
        flexnet_end(p, "REXIT");
        return;
    }
    done(p);
}

void flexnet_start(struct port *p, uint8_t cmd)
{
    struct flexnet *fn = &p->fn;

    if (!fn->active) {
        fn->active = true;
        p->st.sessions++;
        flog(FL_INFO, &p->log, "flexnet", "session started");
    }
    fn->cmd = cmd;
    fn->bin_need = fn->bin_have = 0;
    fn->text_need = fn->text_have = 0;
    fn->tlen = 0;
    fn->overflow = false;
    for (unsigned i = 0; i < 5; i++)
        fn->text[i][0] = '\0';

    switch (cmd) {
    case 's': case 'S': fn->bin_need = 3; break;
    case 'r': case 'R': fn->bin_need = 3 + FLEX_SECTOR_SIZE + 2; break;
    case 'm':           fn->bin_need = 1; fn->text_need = 1; break;
    case 'd':           fn->bin_need = 1; break;
    case 'c':           fn->bin_need = 1; fn->text_need = 5; break;
    case 'C':           fn->text_need = 5; break;
    case 'M': case 'P': case 'V': case 'A': case 'D': case 'I':
                        fn->text_need = 1; break;
    default:            break;
    }
    if (!fn->bin_need && !fn->text_need) {
        execute(p);
        return;
    }
    fn->state = fn->bin_need ? FN_BIN : FN_TEXT;
    p->mode = MODE_FLEXNET;
    port_arm_xfer(p);
}

void flexnet_byte(struct port *p, uint8_t b)
{
    struct flexnet *fn = &p->fn;

    switch (fn->state) {
    case FN_BIN:
        fn->bin[fn->bin_have++] = b;
        if (fn->bin_have < fn->bin_need) {
            port_arm_xfer(p);
            return;
        }
        if (!fn->text_need) {
            execute(p);
            return;
        }
        fn->state = FN_TEXT;
        port_arm_xfer(p);
        return;

    case FN_TEXT:
        if (b == CR) {
            fn->text[fn->text_have][fn->tlen] = '\0';
            fn->tlen = 0;
            if (++fn->text_have == fn->text_need) {
                execute(p);
                return;
            }
        } else if (fn->tlen < TEXT_MAX) {
            fn->text[fn->text_have][fn->tlen++] = (char)b;
        } else {
            fn->overflow = true;
        }
        port_arm_xfer(p);
        return;

    case FN_READACK:
        if (b == NAK) {
            p->st.naks_received++;
            p->st.cksum_err_tx++;
            flog(FL_WARN, &p->log, "flexnet", "client NAKed sector read");
        } else if (b != ACK) {
            flog(FL_DEBUG, &p->log, "flexnet", "expected ACK/NAK after read, got 0x%02x", b);
        }
        done(p);
        return;

    case FN_LIST_FIRST_SP:
        if (b == SP) {
            port_send(p, "\r\n", 2);
            fn->state = FN_LIST;
            timer_cancel(&p->xfer_timer);
            return;
        }
        send_ack(p);
        done(p);
        return;

    case FN_LIST:
        if (b == SP) {
            list_next(p);
            return;
        }
        if (b != ESC)
            flog(FL_DEBUG, &p->log, "flexnet", "unexpected 0x%02x during listing, aborting it", b);
        send_ack(p);
        done(p);
        return;
    }
    /* FN_IDLE should not happen in MODE_FLEXNET */
    done(p);
}
