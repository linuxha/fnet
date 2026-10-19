/* fnetmock — mock FLEX target: FlexNet client and paper tape reader/punch.
 *
 * Plays the target side of an fnetd port, so fnetd can be exercised without
 * hardware. Transports:
 *   --connect HOST:PORT    act as a terminal server dialling an fnetd listen port
 *   --listen [ADDR:]PORT   act as a terminal server accepting an fnetd rfc2217 port
 *   --tty DEVICE           a serial line (null-modem to fnetd)
 *   --pty[=LINK]           a new pty for an fnetd tty port
 * Commands come from -e, -s FILE, the remaining arguments, or stdin.
 */
#include "flexdsk.h"
#include "srec.h"
#include "telnet.h"
#include "util.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <libgen.h>
#include <limits.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define ACK 0x06
#define NAK 0x15
#define CR  0x0d
#define LF  0x0a
#define ESC 0x1b
#define SP  0x20
#define DC1 0x11
#define DC2 0x12
#define DC3 0x13
#define DC4 0x14

#define MAX_ARGS   24
#define MAX_TAPE   (16u * 1024u * 1024u)

/* ---------------------------------------------------------------- state */

static struct {
    int      fd;
    bool     telnet;
    struct telnet tn;
    uint8_t  will[32], dont_sent[32], doo[32];
    uint8_t  buf[16384];        /* decoded receive data */
    size_t   len;
    bool     eof;
    unsigned baud;              /* last RFC-2217 SET-BAUDRATE from fnetd */
    unsigned comport_cmds;
    char     desc[NI_MAXHOST + NI_MAXSERV + 64];
} L = { .fd = -1 };

static int  g_timeout_ms = 3000;
static int  g_verbose;
static bool g_keep_going;
static int  g_pty_slave = -1;
static char g_pty_link[512];

static void vlog(int lvl, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void vlog(int lvl, const char *fmt, ...)
{
    va_list ap;

    if (g_verbose < lvl)
        return;
    va_start(ap, fmt);
    fputs("fnetmock: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

static int fail(const char *cmd, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static int fail(const char *cmd, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    fprintf(stderr, "fnetmock: %s: ", cmd);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    return 1;
}

static void hexdump(FILE *f, const char *pfx, const uint8_t *b, size_t n)
{
    for (size_t i = 0; i < n; i += 16) {
        fprintf(f, "%s%04zx ", pfx, i);
        for (size_t j = 0; j < 16; j++) {
            if (i + j < n)
                fprintf(f, " %02x", b[i + j]);
            else
                fputs("   ", f);
        }
        fputs("  ", f);
        for (size_t j = 0; j < 16 && i + j < n; j++)
            fputc(isprint(b[i + j]) ? b[i + j] : '.', f);
        fputc('\n', f);
    }
}

static void sleep_us(uint64_t us)
{
    struct timespec ts = { .tv_sec = (time_t)(us / 1000000u), .tv_nsec = (long)(us % 1000000u) * 1000 };

    while (nanosleep(&ts, &ts) < 0 && errno == EINTR)
        ;
}

/* ---------------------------------------------------------------- transmit */

static int write_all(const uint8_t *b, size_t n)
{
    while (n) {
        ssize_t w = write(L.fd, b, n);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN) {
                struct pollfd pf = { .fd = L.fd, .events = POLLOUT };
                poll(&pf, 1, 1000);
                continue;
            }
            fprintf(stderr, "fnetmock: write: %s\n", strerror(errno));
            L.eof = true;
            return -1;
        }
        b += w;
        n -= (size_t)w;
    }
    return 0;
}

static int tx(const void *data, size_t n)
{
    const uint8_t *b = data;
    uint8_t esc[1024];

    if (g_verbose >= 2)
        hexdump(stderr, "  > ", b, n);
    if (!L.telnet)
        return write_all(b, n);
    while (n) {
        size_t chunk = n < sizeof esc / 2 ? n : sizeof esc / 2;
        if (write_all(esc, telnet_escape(b, chunk, esc)) < 0)
            return -1;
        b += chunk;
        n -= chunk;
    }
    return 0;
}

static int tx_byte(uint8_t b) { return tx(&b, 1); }

static int tx_str(const char *s) { return tx(s, strlen(s)); }

/* Send data one byte at a time with pacing (a slow tape punch). */
static int tx_paced(const uint8_t *b, size_t n, unsigned char_us, unsigned line_ms)
{
    if (!char_us && !line_ms)
        return tx(b, n);
    for (size_t i = 0; i < n; i++) {
        if (tx(b + i, 1) < 0)
            return -1;
        if (char_us)
            sleep_us(char_us);
        if (line_ms && b[i] == LF)
            sleep_us((uint64_t)line_ms * 1000u);
    }
    return 0;
}

/* ---------------------------------------------------------------- telnet (terminal-server side) */

static bool bit(const uint8_t *s, uint8_t o) { return s[o >> 3] & (1u << (o & 7)); }
static void set_bit(uint8_t *s, uint8_t o) { s[o >> 3] = (uint8_t)(s[o >> 3] | (1u << (o & 7))); }
static void clr_bit(uint8_t *s, uint8_t o) { s[o >> 3] = (uint8_t)(s[o >> 3] & ~(1u << (o & 7))); }

static const char *tn_name(uint8_t cmd)
{
    return cmd == TN_WILL ? "WILL" : cmd == TN_WONT ? "WONT" : cmd == TN_DO ? "DO" : "DONT";
}

static void tn_send(uint8_t cmd, uint8_t opt)
{
    uint8_t b[3] = { TN_IAC, cmd, opt };

    vlog(1, "telnet > %s %u", tn_name(cmd), opt);
    write_all(b, 3);
}

static bool tn_ours(uint8_t opt)
{
    return opt == TNO_BINARY || opt == TNO_SGA || opt == TNO_COMPORT;
}

static void on_tn_cmd(void *arg, uint8_t cmd, uint8_t opt)
{
    (void)arg;
    vlog(1, "telnet < %s %u", tn_name(cmd), opt);
    switch (cmd) {
    case TN_DO:
        if (!tn_ours(opt)) {
            if (!bit(L.dont_sent, opt)) {
                set_bit(L.dont_sent, opt);
                tn_send(TN_WONT, opt);
            }
        } else if (!bit(L.will, opt)) {
            set_bit(L.will, opt);
            tn_send(TN_WILL, opt);
        }
        break;
    case TN_WILL:
        if (!tn_ours(opt)) {
            if (!bit(L.dont_sent, opt)) {
                set_bit(L.dont_sent, opt);
                tn_send(TN_DONT, opt);
            }
        } else if (!bit(L.doo, opt)) {
            set_bit(L.doo, opt);
            tn_send(TN_DO, opt);
        }
        break;
    case TN_DONT:
        if (bit(L.will, opt)) {
            clr_bit(L.will, opt);
            tn_send(TN_WONT, opt);
        }
        break;
    case TN_WONT:
        if (bit(L.doo, opt)) {
            clr_bit(L.doo, opt);
            tn_send(TN_DONT, opt);
        }
        break;
    }
}

/* fnetd is the RFC-2217 client: acknowledge each setting with cmd+100, same value. */
static void on_tn_sb(void *arg, const uint8_t *sb, size_t len)
{
    uint8_t out[80];

    (void)arg;
    if (len < 2 || sb[0] != TNO_COMPORT || sb[1] >= CPO_SERVER_OFFSET || len - 2 > 32)
        return;
    L.comport_cmds++;
    switch (sb[1]) {
    case CPO_SET_BAUDRATE:
        if (len >= 6) {
            L.baud = (unsigned)sb[2] << 24 | (unsigned)sb[3] << 16 | (unsigned)sb[4] << 8 | sb[5];
            vlog(1, "rfc2217 < SET-BAUDRATE %u", L.baud);
        }
        break;
    case CPO_SET_DATASIZE: vlog(1, "rfc2217 < SET-DATASIZE %u", len > 2 ? sb[2] : 0); break;
    case CPO_SET_PARITY:   vlog(1, "rfc2217 < SET-PARITY %u", len > 2 ? sb[2] : 0); break;
    case CPO_SET_STOPSIZE: vlog(1, "rfc2217 < SET-STOPSIZE %u", len > 2 ? sb[2] : 0); break;
    case CPO_SET_CONTROL:  vlog(1, "rfc2217 < SET-CONTROL %u", len > 2 ? sb[2] : 0); break;
    default:               vlog(1, "rfc2217 < command %u", sb[1]); break;
    }
    write_all(out, telnet_comport_sb(out, (uint8_t)(sb[1] + CPO_SERVER_OFFSET), sb + 2, len - 2));
}

static const struct telnet_cb tn_cb = { on_tn_cmd, on_tn_sb, NULL };

/* ---------------------------------------------------------------- receive */

/* Wait up to ms for input and decode it. 1 = got something, 0 = timeout, -1 = closed. */
static int fill(int ms)
{
    struct pollfd pf = { .fd = L.fd, .events = POLLIN };
    uint8_t raw[4096];
    size_t room = sizeof L.buf - L.len, n;
    ssize_t r;

    if (L.eof)
        return -1;
    if (room < sizeof raw)
        return 1;                       /* caller must consume first */
    if (poll(&pf, 1, ms < 0 ? 0 : ms) <= 0)
        return 0;
    r = read(L.fd, raw, sizeof raw);
    if (r < 0 && (errno == EAGAIN || errno == EINTR))
        return 0;
    if (r <= 0) {
        if (!L.eof)
            fprintf(stderr, "fnetmock: connection closed%s%s\n", r < 0 ? ": " : "",
                    r < 0 ? strerror(errno) : "");
        L.eof = true;
        return -1;
    }
    n = L.telnet ? telnet_rx(&L.tn, raw, (size_t)r, L.buf + L.len, &tn_cb) : (size_t)r;
    if (!L.telnet)
        memcpy(L.buf + L.len, raw, n);
    if (g_verbose >= 2 && n)
        hexdump(stderr, "  < ", L.buf + L.len, n);
    L.len += n;
    return 1;
}

static void consume(uint8_t *out, size_t n)
{
    if (out)
        memcpy(out, L.buf, n);
    memmove(L.buf, L.buf + n, L.len - n);
    L.len -= n;
}

static int ms_left(uint64_t deadline)
{
    uint64_t t = now_us();
    return t >= deadline ? 0 : (int)((deadline - t + 999) / 1000);
}

/* Receive exactly n bytes within ms. Returns the count received. */
static size_t rx_n(uint8_t *out, size_t n, int ms)
{
    uint64_t deadline = now_us() + (uint64_t)ms * 1000u;
    size_t got = 0;

    while (got < n) {
        if (L.len) {
            size_t take = L.len < n - got ? L.len : n - got;
            consume(out + got, take);
            got += take;
            continue;
        }
        if (!ms_left(deadline) || fill(ms_left(deadline)) < 0)
            break;
    }
    return got;
}

static int rx_byte(int ms)
{
    uint8_t b;
    return rx_n(&b, 1, ms) == 1 ? b : -1;
}

/* Receive up to and including term. Returns the count, *found tells if term arrived. */
static size_t rx_until(uint8_t *out, size_t cap, uint8_t term, int ms, bool *found)
{
    uint64_t deadline = now_us() + (uint64_t)ms * 1000u;
    size_t got = 0;

    *found = false;
    while (got < cap) {
        if (L.len) {
            uint8_t b = L.buf[0];
            consume(NULL, 1);
            out[got++] = b;
            if (b == term) {
                *found = true;
                break;
            }
            continue;
        }
        if (!ms_left(deadline) || fill(ms_left(deadline)) < 0)
            break;
    }
    return got;
}

/* Collect whatever arrives until the line is quiet for ms. */
static size_t rx_idle(uint8_t *out, size_t cap, int ms)
{
    size_t got = 0;

    for (;;) {
        if (L.len) {
            size_t take = L.len < cap - got ? L.len : cap - got;
            consume(out ? out + got : NULL, take);
            got += take;
            if (got == cap)
                break;
            continue;
        }
        if (fill(ms) <= 0)
            break;
    }
    return got;
}

/* Throw away bytes left over from an earlier exchange. */
static void flush_stray(void)
{
    uint8_t junk[256];
    size_t n;

    fill(0);
    if (!L.len)
        return;
    n = L.len < sizeof junk ? L.len : sizeof junk;
    memcpy(junk, L.buf, n);
    fprintf(stderr, "fnetmock: discarding %zu stray byte(s)\n", L.len);
    if (g_verbose)
        hexdump(stderr, "  ? ", junk, n);
    L.len = 0;
}

/* ---------------------------------------------------------------- arguments */

struct args {
    const char *cmd;
    int   npos;
    char *pos[MAX_ARGS];
    int   nkv;
    char *key[MAX_ARGS];
    char *val[MAX_ARGS];
};

static const char *opt(const struct args *a, const char *k)
{
    for (int i = 0; i < a->nkv; i++)
        if (!strcmp(a->key[i], k))
            return a->val[i];
    return NULL;
}

static bool flag(const struct args *a, const char *k)
{
    return opt(a, k) != NULL;
}

/* Decimal, 0x hex or $hex. */
static int num(const char *s, unsigned long max, unsigned long *out)
{
    char *end;
    int base = 10;

    if (!s || !*s)
        return -1;
    if (*s == '$') {
        s++;
        base = 16;
    } else if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
        base = 16;
    }
    if (!isxdigit((unsigned char)*s))
        return -1;
    errno = 0;
    *out = strtoul(s, &end, base);
    return errno || *end || *out > max ? -1 : 0;
}

static int num_opt(const struct args *a, const char *k, unsigned long max, unsigned long def,
                   unsigned long *out)
{
    const char *v = opt(a, k);

    *out = def;
    if (!v)
        return 0;
    if (num(v, max, out) < 0)
        return fail(a->cmd, "bad value for %s: '%s'", k, v);
    return 0;
}

static int pos_num(const struct args *a, int i, const char *what, unsigned long max, uint8_t *out)
{
    unsigned long v;

    if (num(a->pos[i], max, &v) < 0)
        return fail(a->cmd, "bad %s '%s'", what, a->pos[i]);
    *out = (uint8_t)v;
    return 0;
}

/* Send a FlexNet command byte, optional binary prefix, then CR-terminated text fields. */
static int send_cmd(uint8_t c, const uint8_t *bin, size_t nbin, char *const *fields, int nf)
{
    uint8_t b[512];
    size_t n = 0;

    b[n++] = c;
    for (size_t i = 0; i < nbin; i++)
        b[n++] = bin[i];
    for (int i = 0; i < nf; i++) {
        size_t l = strlen(fields[i]);
        if (n + l + 1 > sizeof b)
            return -1;
        memcpy(b + n, fields[i], l);
        n += l;
        b[n++] = CR;
    }
    return tx(b, n);
}

static int expect_ack(const struct args *a, int ms)
{
    int r = rx_byte(ms);

    if (r == ACK)
        return 0;
    if (r == NAK)
        return fail(a->cmd, "NAK");
    if (r < 0)
        return fail(a->cmd, "timeout");
    return fail(a->cmd, "unexpected reply 0x%02x", r);
}

/* ---------------------------------------------------------------- FlexNet commands */

static int c_sync(struct args *a)
{
    static const uint8_t seq[2] = { 0x55, 0xaa };

    for (int i = 0; i < 2; i++) {
        int r;
        if (tx_byte(seq[i]) < 0)
            return 1;
        r = rx_byte(g_timeout_ms);
        if (r != seq[i])
            return r < 0 ? fail(a->cmd, "no echo of $%02X", seq[i])
                         : fail(a->cmd, "sent $%02X, got $%02X", seq[i], r);
    }
    puts("sync: ok");
    return 0;
}

static int c_ping(struct args *a)
{
    uint64_t t0 = now_us();

    if (tx_byte('Q') < 0 || expect_ack(a, g_timeout_ms))
        return 1;
    printf("ping: ACK in %.1f ms\n", (double)(now_us() - t0) / 1000.0);
    return 0;
}

static int c_pwd(struct args *a)
{
    uint8_t b[300];
    bool found;
    size_t n;

    if (tx_byte('?') < 0)
        return 1;
    n = rx_until(b, sizeof b - 1, ACK, g_timeout_ms, &found);
    if (!found)
        return fail(a->cmd, "timeout");
    n--;                                /* ACK */
    if (n && b[n - 1] == CR)
        n--;
    b[n] = '\0';
    printf("%s\n", (char *)b);
    return 0;
}

static int c_cd(struct args *a)
{
    if (send_cmd('P', NULL, 0, a->pos, 1) < 0 || expect_ack(a, g_timeout_ms))
        return 1;
    printf("cd: ok\n");
    return 0;
}

static int c_mount(struct args *a)
{
    uint8_t drv = 0;
    int r;

    if (a->npos == 2) {
        if (pos_num(a, 0, "drive", 3, &drv) || send_cmd('m', &drv, 1, a->pos + 1, 1) < 0)
            return 1;
    } else if (send_cmd('M', NULL, 0, a->pos, 1) < 0) {
        return 1;
    }
    if (expect_ack(a, g_timeout_ms))
        return 1;
    r = rx_byte(g_timeout_ms);
    if (r != 'W' && r != 'R')
        return fail(a->cmd, "ACK not followed by W/R (got %d)", r);
    if (a->npos == 2)
        printf("mount: drive %u %s (%c)\n", drv, a->pos[1], r);
    else
        printf("mount: legacy drive %s (%c)\n", a->pos[0], r);
    return 0;
}

static int c_drive(struct args *a)
{
    uint8_t drv, b[300];
    bool found;
    size_t n;

    if (pos_num(a, 0, "drive", 255, &drv) || send_cmd('d', &drv, 1, NULL, 0) < 0)
        return 1;
    n = rx_until(b, sizeof b - 1, ACK, g_timeout_ms, &found);
    if (!found)
        return fail(a->cmd, "timeout");
    b[n - 1] = '\0';
    printf("drive %u: %s\n", drv, n > 1 ? (char *)b : "(none)");
    return 0;
}

/* Walk an RDIR/RLIST listing: SP for each line, ESC to stop early. */
static int listing(struct args *a)
{
    unsigned long max;
    unsigned count = 0;
    uint8_t b[300];

    if (num_opt(a, "max", 100000, ULONG_MAX, &max))
        return 1;
    for (;;) {
        bool found;
        size_t n;
        int r;

        if (count == max) {
            if (tx_byte(ESC) < 0 || expect_ack(a, g_timeout_ms))
                return 1;
            printf("(stopped after %u)\n", count);
            return 0;
        }
        if (tx_byte(SP) < 0)
            return 1;
        r = rx_byte(g_timeout_ms);
        if (r == ACK)
            break;
        if (r < 0)
            return fail(a->cmd, "timeout after %u entries", count);
        b[0] = (uint8_t)r;
        n = 1 + rx_until(b + 1, sizeof b - 2, LF, g_timeout_ms, &found);
        if (!found)
            return fail(a->cmd, "unterminated entry");
        n -= n >= 2 && b[n - 2] == CR ? 2 : 1;
        b[n] = '\0';
        printf("%s\n", (char *)b);
        count++;
    }
    printf("(%u)\n", count);
    return 0;
}

static int expect_crlf(struct args *a)
{
    uint8_t h[2];

    if (rx_n(h, 2, g_timeout_ms) != 2 || h[0] != CR || h[1] != LF)
        return fail(a->cmd, "listing did not start with CR LF");
    return 0;
}

static int c_dir(struct args *a)
{
    char *pat[1] = { a->npos ? a->pos[0] : "" };

    if (send_cmd('A', NULL, 0, pat, 1) < 0 || expect_crlf(a))
        return 1;
    return listing(a);
}

static int c_dirs(struct args *a)
{
    char *empty[1] = { "" };

    if (send_cmd('I', NULL, 0, empty, 1) < 0 || tx_byte(SP) < 0 || expect_crlf(a))
        return 1;
    return listing(a);
}

static int c_create(struct args *a)
{
    uint8_t drv;
    int off = a->npos == 6;

    if (off) {
        if (pos_num(a, 0, "drive", 3, &drv) || send_cmd('c', &drv, 1, a->pos + 1, 5) < 0)
            return 1;
    } else if (send_cmd('C', NULL, 0, a->pos, 5) < 0) {
        return 1;
    }
    if (expect_ack(a, g_timeout_ms * 4))
        return 1;
    printf("create: %s ok\n", a->pos[off + 1]);
    return 0;
}

static int c_delete(struct args *a)
{
    if (send_cmd('D', NULL, 0, a->pos, 1) < 0 || expect_ack(a, g_timeout_ms))
        return 1;
    printf("delete: %s ok\n", a->pos[0]);
    return 0;
}

static int c_rexit(struct args *a)
{
    if (tx_byte('E') < 0 || expect_ack(a, g_timeout_ms))
        return 1;
    puts("rexit: ok");
    return 0;
}

/* One sector read. 0 ok, 1 transport/protocol failure, 2 server-reported read error. */
static int read_sector(const char *cmd, uint8_t drv, uint8_t trk, uint8_t sec, uint8_t *data,
                       uint8_t reply, bool quiet)
{
    uint8_t req[4] = { 's', drv, trk, sec }, b[FLEX_SECTOR_SIZE + 2];
    uint16_t ck, want;
    size_t n;

    if (tx(req, 4) < 0)
        return 1;
    n = rx_n(b, sizeof b, g_timeout_ms);
    if (n != sizeof b) {
        fail(cmd, "T%u S%u: short reply (%zu of 258 bytes)", trk, sec, n);
        return 1;
    }
    ck = (uint16_t)(b[256] << 8 | b[257]);
    want = flex_cksum(b);
    if (ck != want && reply == ACK)
        reply = NAK;                    /* what NETDRV does on a bad sector */
    if (reply && tx_byte(reply) < 0)
        return 1;
    memcpy(data, b, FLEX_SECTOR_SIZE);
    if (ck == want)
        return 0;
    if (!quiet) {
        bool zero = want == 0 && ck == 1;
        fail(cmd, "T%u S%u: %s (checksum $%04X, data $%04X)", trk, sec,
             zero ? "read error reported by server" : "checksum mismatch", ck, want);
    }
    return 2;
}

static int write_sector(const char *cmd, uint8_t drv, uint8_t trk, uint8_t sec,
                        const uint8_t *data, bool bad_ck)
{
    uint8_t b[4 + FLEX_SECTOR_SIZE + 2] = { 'r', drv, trk, sec };
    uint16_t ck = flex_cksum(data);
    int r;

    if (bad_ck)
        ck ^= 0x5a5a;
    memcpy(b + 4, data, FLEX_SECTOR_SIZE);
    b[260] = (uint8_t)(ck >> 8);
    b[261] = (uint8_t)ck;
    if (tx(b, sizeof b) < 0)
        return 1;
    r = rx_byte(g_timeout_ms);
    if (r == ACK)
        return 0;
    if (r == NAK)
        return fail(cmd, "T%u S%u: NAK", trk, sec);
    return fail(cmd, "T%u S%u: %s", trk, sec, r < 0 ? "timeout" : "unexpected reply");
}

static int c_read(struct args *a)
{
    uint8_t drv, trk, sec, data[FLEX_SECTOR_SIZE], reply = ACK;
    const char *out = opt(a, "out");
    int rc;

    if (pos_num(a, 0, "drive", 255, &drv) || pos_num(a, 1, "track", 255, &trk) ||
        pos_num(a, 2, "sector", 255, &sec))
        return 1;
    if (flag(a, "nak"))
        reply = NAK;
    if (flag(a, "noack"))
        reply = 0;
    rc = read_sector(a->cmd, drv, trk, sec, data, reply, false);
    if (rc == 1)
        return 1;
    if (out) {
        FILE *f = fopen(out, "wb");
        if (!f || fwrite(data, 1, sizeof data, f) != sizeof data || fclose(f)) {
            if (f)
                fclose(f);
            return fail(a->cmd, "%s: %s", out, strerror(errno));
        }
    } else if (!flag(a, "quiet")) {
        hexdump(stdout, "", data, sizeof data);
    }
    if (rc == 0)
        printf("read: drive %u T%u S%u ok\n", drv, trk, sec);
    return rc ? 1 : 0;
}

static int c_write(struct args *a)
{
    uint8_t drv, trk, sec, data[FLEX_SECTOR_SIZE] = { 0 };
    const char *fill_v = opt(a, "fill");

    if (pos_num(a, 0, "drive", 255, &drv) || pos_num(a, 1, "track", 255, &trk) ||
        pos_num(a, 2, "sector", 255, &sec))
        return 1;
    if (a->npos == 4) {
        FILE *f = fopen(a->pos[3], "rb");
        if (!f)
            return fail(a->cmd, "%s: %s", a->pos[3], strerror(errno));
        if (fread(data, 1, sizeof data, f) == 0 && ferror(f)) {
            fclose(f);
            return fail(a->cmd, "%s: read error", a->pos[3]);
        }
        fclose(f);
    } else if (fill_v) {
        unsigned long v;
        if (num(fill_v, 255, &v) < 0)
            return fail(a->cmd, "bad fill value '%s'", fill_v);
        memset(data, (int)v, sizeof data);
    } else {
        return fail(a->cmd, "need FILE or fill=XX");
    }
    if (write_sector(a->cmd, drv, trk, sec, data, flag(a, "badck")))
        return 1;
    printf("write: drive %u T%u S%u ok\n", drv, trk, sec);
    return 0;
}

/* Copy a whole mounted image to a local file, geometry from its SIR. */
static int c_fetch(struct args *a)
{
    uint8_t drv, sir[FLEX_SECTOR_SIZE], data[FLEX_SECTOR_SIZE];
    unsigned max_trk, spt, t0 = 0, blocks = 0;
    uint64_t t0us = now_us();
    double secs;
    FILE *f;

    if (pos_num(a, 0, "drive", 3, &drv))
        return 1;
    if (read_sector(a->cmd, drv, 0, 3, sir, ACK, false))
        return 1;
    max_trk = sir[FLEX_SIR_OFFSET + 22];
    spt = sir[FLEX_SIR_OFFSET + 23];
    if (spt < FLEX_MIN_SECTORS)
        return fail(a->cmd, "SIR geometry invalid (%u tracks x %u sectors)", max_trk + 1, spt);
    if (!(f = fopen(a->pos[1], "wb")))
        return fail(a->cmd, "%s: %s", a->pos[1], strerror(errno));
    for (unsigned t = 0; t <= max_trk; t++) {
        for (unsigned s = 1; s <= spt; s++) {
            int rc = read_sector(a->cmd, drv, (uint8_t)t, (uint8_t)s, data, ACK, t == 0 && s > 4);
            if (rc == 2 && t == 0 && s > 4)
                break;                  /* end of a short (single-density) track 0 */
            if (rc) {
                fclose(f);
                return 1;
            }
            if (fwrite(data, 1, sizeof data, f) != sizeof data) {
                fclose(f);
                return fail(a->cmd, "%s: %s", a->pos[1], strerror(errno));
            }
            if (t == 0)
                t0++;
            blocks++;
        }
        vlog(2, "fetch: track %u", t);
    }
    if (fclose(f))
        return fail(a->cmd, "%s: %s", a->pos[1], strerror(errno));
    secs = (double)(now_us() - t0us) / 1e6;
    printf("fetch: %u tracks x %u sectors (track 0: %u), %u sectors in %.2f s (%.0f B/s)\n",
           max_trk + 1, spt, t0, blocks, secs, secs > 0 ? blocks * 256.0 / secs : 0.0);
    return 0;
}

/* Write a local image over the one mounted on DRV. */
static int c_put(struct args *a)
{
    char p1[4096], p2[4096];
    uint8_t drv, data[FLEX_SECTOR_SIZE];
    const char *why = NULL;
    flexdsk_t *d;
    unsigned n = 0;
    uint64_t t0us = now_us();
    int dfd;

    if (pos_num(a, 0, "drive", 3, &drv))
        return 1;
    str_copy(p1, a->pos[1], sizeof p1);
    str_copy(p2, a->pos[1], sizeof p2);
    if ((dfd = open(dirname(p1), O_RDONLY | O_DIRECTORY | O_CLOEXEC)) < 0)
        return fail(a->cmd, "%s: %s", a->pos[1], strerror(errno));
    if (flexdsk_open(dfd, basename(p2), false, &d, NULL, &why) < 0) {
        close(dfd);
        return fail(a->cmd, "%s: %s", a->pos[1], why ? why : strerror(errno));
    }
    close(dfd);
    for (unsigned t = 0; t <= d->g.max_trk; t++) {
        unsigned last = t ? d->g.spt : d->g.t0_spt;
        for (unsigned s = 1; s <= last; s++) {
            if (flexdsk_read(d, t, s, data) < 0 ||
                write_sector(a->cmd, drv, (uint8_t)t, (uint8_t)s, data, false)) {
                flexdsk_close(d);
                return 1;
            }
            n++;
        }
        vlog(2, "put: track %u", t);
    }
    flexdsk_close(d);
    printf("put: %u sectors in %.2f s\n", n, (double)(now_us() - t0us) / 1e6);
    return 0;
}

/* ---------------------------------------------------------------- paper tape */

static int c_select(struct args *a)
{
    const char *term = opt(a, "term");
    char s[600];
    uint8_t t = CR;

    if (term) {
        if (!strcasecmp(term, "cr"))       t = CR;
        else if (!strcasecmp(term, "lf"))  t = LF;
        else if (!strcasecmp(term, "eot")) t = 0x04;
        else if (!strcasecmp(term, "nul")) t = 0x00;
        else return fail(a->cmd, "term must be cr, lf, eot or nul");
    }
    snprintf(s, sizeof s, "|%s%s%s", a->npos ? a->pos[0] : "", a->npos > 1 ? "|" : "",
             a->npos > 1 ? a->pos[1] : "");
    if (tx_str(s) < 0 || tx_byte(t) < 0)
        return 1;
    printf("select: %s\n", s);
    return 0;
}

struct srec_tally {
    char     line[1024];
    size_t   len;
    unsigned lines, records, errors;
    bool     terminated, overlong;
};

static void tally_line(struct srec_tally *st)
{
    srec_info_t info;
    enum srec_err e;

    if (st->len == 0 && !st->overlong)
        return;
    st->lines++;
    e = st->overlong ? SREC_ESYNTAX : srec_check_line(st->line, st->len, &info);
    if (e == SREC_OK) {
        st->records++;
        if (info.terminator)
            st->terminated = true;
    } else if (e != SREC_EMPTY) {
        st->errors++;
        if (st->errors <= 5)
            fprintf(stderr, "fnetmock: load: line %u: %s\n", st->lines, srec_strerror(e));
    }
    st->len = 0;
    st->overlong = false;
}

static void tally(struct srec_tally *st, const uint8_t *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (b[i] == CR || b[i] == LF) {
            tally_line(st);
        } else if (st->len < sizeof st->line) {
            st->line[st->len++] = (char)b[i];
        } else {
            st->overlong = true;
        }
    }
}

/* DC1, then read the tape like a monitor's loader: until the terminator record
 * (or quiet for idle ms), optionally pausing with DC3/DC1 every N bytes. */
static int c_load(struct args *a)
{
    unsigned long idle, every, pause_ms, stop_after, max_late = 0;
    const char *out = opt(a, "out"), *eof = opt(a, "eof");
    bool until_idle = eof && !strcmp(eof, "idle");
    struct srec_tally st = { .len = 0 };
    uint8_t *data;
    size_t len = 0, next_pause;
    uint64_t t0;
    double secs;
    unsigned pauses = 0;
    int rc = 0;

    if (num_opt(a, "idle", 600000, 1000, &idle) || num_opt(a, "pause-every", MAX_TAPE, 0, &every) ||
        num_opt(a, "pause-ms", 600000, 500, &pause_ms) ||
        num_opt(a, "stop-after", MAX_TAPE, 0, &stop_after))
        return 1;
    if (eof && strcmp(eof, "idle") && strcmp(eof, "record"))
        return fail(a->cmd, "eof must be record or idle");
    if (!(data = malloc(MAX_TAPE)))
        return fail(a->cmd, "out of memory");
    next_pause = every ? every : SIZE_MAX;

    if (tx_byte(DC1) < 0) {
        free(data);
        return 1;
    }
    t0 = now_us();
    for (;;) {
        size_t want = MAX_TAPE - len, n;
        int wait = len ? (int)idle : g_timeout_ms;

        if (stop_after && stop_after - len < want)
            want = stop_after - len;
        if (next_pause - len < want)
            want = next_pause - len;
        if (!want && len == MAX_TAPE)
            break;
        if (!L.len && fill(wait) <= 0)
            break;
        n = L.len < want ? L.len : want;
        consume(data + len, n);
        tally(&st, data + len, n);
        len += n;
        if (stop_after && len >= stop_after) {
            tx_byte(DC3);
            n = rx_idle(data + len, MAX_TAPE - len, 300);
            if (n > max_late)
                max_late = n;
            tally(&st, data + len, n);
            len += n;
            printf("load: stopped with DC3 after %lu bytes (%zu late)\n", stop_after, n);
            break;
        }
        if (len >= next_pause) {
            tx_byte(DC3);
            n = rx_idle(data + len, MAX_TAPE - len, (int)pause_ms);
            if (n > max_late)
                max_late = n;
            tally(&st, data + len, n);
            len += n;
            pauses++;
            next_pause = len + every;
            tx_byte(DC1);
        }
        if (st.terminated && !until_idle) {
            /* take the rest of the terminator's line ending */
            n = rx_idle(data + len, MAX_TAPE - len, 150);
            tally(&st, data + len, n);
            len += n;
            break;
        }
    }
    secs = (double)(now_us() - t0) / 1e6;
    if (!stop_after)
        tally_line(&st);                /* a DC3 stop leaves a partial line: don't judge it */

    if (out) {
        FILE *f = strcmp(out, "-") ? fopen(out, "wb") : stdout;
        if (!f || fwrite(data, 1, len, f) != len || (f != stdout && fclose(f))) {
            rc = fail(a->cmd, "%s: %s", out, strerror(errno));
            if (f && f != stdout)
                fclose(f);
        }
    }
    printf("load: %zu bytes, %u records, %u errors, %s, %.2f s (%.0f cps)",
           len, st.records, st.errors, st.terminated ? "terminated" : "no terminator",
           secs, secs > 0 ? (double)len / secs : 0.0);
    if (pauses)
        printf(", %u pauses, max %lu byte(s) after DC3", pauses, max_late);
    putchar('\n');
    free(data);
    if (!len)
        return fail(a->cmd, "no data");
    if (rc || st.errors || (!st.terminated && !stop_after && !until_idle))
        return 1;
    return 0;
}

static int c_punch(struct args *a)
{
    unsigned long leader, trailer, char_us, line_ms;
    const char *eolname = opt(a, "eol"), *eol = "\r\n";
    const char *path = a->pos[0];
    uint8_t *in, *out;
    size_t n = 0, o = 0;
    FILE *f;
    int rc = 1;

    if (num_opt(a, "leader", 4096, 0, &leader) || num_opt(a, "trailer", 4096, 0, &trailer) ||
        num_opt(a, "char-us", 1000000, 0, &char_us) || num_opt(a, "line-ms", 60000, 0, &line_ms))
        return 1;
    if (eolname) {
        if (!strcasecmp(eolname, "crlf"))     eol = "\r\n";
        else if (!strcasecmp(eolname, "cr"))  eol = "\r";
        else if (!strcasecmp(eolname, "lf"))  eol = "\n";
        else return fail(a->cmd, "eol must be crlf, cr or lf");
    }
    if (!strcmp(path, "-")) {
        f = stdin;
    } else if (!(f = fopen(path, "rb"))) {
        return fail(a->cmd, "%s: %s", path, strerror(errno));
    }
    in = malloc(MAX_TAPE);
    out = malloc(MAX_TAPE * 2 + 8192);
    if (!in || !out) {
        fail(a->cmd, "out of memory");
        goto done;
    }
    n = fread(in, 1, MAX_TAPE, f);
    if (ferror(f)) {
        fail(a->cmd, "%s: read error", path);
        goto done;
    }
    for (unsigned long i = 0; i < leader; i++)
        out[o++] = 0;
    for (size_t i = 0; i < n; i++) {
        if (in[i] == CR || in[i] == LF) {
            if (in[i] == CR && i + 1 < n && in[i + 1] == LF)
                i++;
            memcpy(out + o, eol, strlen(eol));
            o += strlen(eol);
        } else {
            out[o++] = in[i];
        }
    }
    for (unsigned long i = 0; i < trailer; i++)
        out[o++] = 0;

    if (tx_byte(DC2) < 0 || tx_paced(out, o, (unsigned)char_us, (unsigned)line_ms) < 0)
        goto done;
    if (!flag(a, "nodc4") && tx_byte(DC4) < 0)
        goto done;
    printf("punch: %zu bytes from %s%s\n", o, path, flag(a, "nodc4") ? " (no DC4)" : "");
    rc = 0;
done:
    if (f != stdin)
        fclose(f);
    free(in);
    free(out);
    return rc;
}

/* ---------------------------------------------------------------- raw / misc */

static int c_dc(struct args *a)
{
    return tx_byte((uint8_t)(DC1 + (a->cmd[2] - '1')));
}

/* Tokens: $XX or 0xXX is one byte; anything else is text with \r \n \t \\ \xHH. */
static int c_send(struct args *a)
{
    uint8_t b[1024];
    size_t n = 0;

    for (int i = 0; i < a->npos; i++) {
        const char *s = a->pos[i];
        unsigned long v;

        if ((s[0] == '$' || (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))) && num(s, 255, &v) == 0) {
            if (n < sizeof b)
                b[n++] = (uint8_t)v;
            continue;
        }
        for (; *s && n < sizeof b; s++) {
            if (*s != '\\' || !s[1]) {
                b[n++] = (uint8_t)*s;
                continue;
            }
            switch (*++s) {
            case 'r': b[n++] = CR; break;
            case 'n': b[n++] = LF; break;
            case 't': b[n++] = '\t'; break;
            case 'x':
                if (isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
                    char h[3] = { s[1], s[2], 0 };
                    b[n++] = (uint8_t)strtoul(h, NULL, 16);
                    s += 2;
                    break;
                }
                /* fall through */
            default: b[n++] = (uint8_t)*s; break;
            }
        }
    }
    return tx(b, n) < 0;
}

static int c_recv(struct args *a)
{
    unsigned long ms = 500;
    uint8_t b[8192];
    size_t n;

    if (a->npos && num(a->pos[0], 600000, &ms) < 0)
        return fail(a->cmd, "bad time '%s'", a->pos[0]);
    n = rx_idle(b, sizeof b, (int)ms);
    hexdump(stdout, "", b, n);
    printf("recv: %zu bytes\n", n);
    return 0;
}

static int c_sleep(struct args *a)
{
    unsigned long ms;

    if (num(a->pos[0], 3600000, &ms) < 0)
        return fail(a->cmd, "bad time '%s'", a->pos[0]);
    sleep_us((uint64_t)ms * 1000u);
    return 0;
}

static int c_echo(struct args *a)
{
    for (int i = 0; i < a->npos; i++)
        printf("%s%s", i ? " " : "", a->pos[i]);
    putchar('\n');
    return 0;
}

static int c_status(struct args *a)
{
    (void)a;
    printf("link: %s, %s", L.desc, L.eof ? "closed" : "open");
    if (L.telnet) {
        printf(", telnet BINARY %s/%s SGA %s/%s",
               bit(L.will, TNO_BINARY) ? "will" : "-", bit(L.doo, TNO_BINARY) ? "do" : "-",
               bit(L.will, TNO_SGA) ? "will" : "-", bit(L.doo, TNO_SGA) ? "do" : "-");
        if (L.comport_cmds)
            printf(", rfc2217 baud %u", L.baud);
    }
    putchar('\n');
    return 0;
}

static int c_help(struct args *a);

/* ---------------------------------------------------------------- command table */

struct command {
    const char *name;
    int  (*fn)(struct args *);
    int   minpos, maxpos;
    const char *keys;           /* allowed key=value / flag names, comma separated */
    bool  flexnet;              /* flush stray input first */
    const char *usage;
};

static const struct command commands[] = {
    { "sync",   c_sync,   0, 0, "",     true,  "                         $55/$AA handshake" },
    { "ping",   c_ping,   0, 0, "",     true,  "                         Q, expect ACK" },
    { "pwd",    c_pwd,    0, 0, "",     true,  "                         ? (RWHICH)" },
    { "cd",     c_cd,     1, 1, "",     true,  "PATH                     P (RCD)" },
    { "mount",  c_mount,  1, 2, "",     true,  "[DRV] NAME               M (legacy) or m DRV" },
    { "drive",  c_drive,  1, 1, "",     true,  "DRV                      d: image on DRV" },
    { "dir",    c_dir,    0, 1, "max",  true,  "[PATTERN] [max=N]        A (RDIR), ESC after N" },
    { "dirs",   c_dirs,   0, 0, "max",  true,  "[max=N]                  I (RLIST)" },
    { "create", c_create, 5, 6, "",     true,  "[DRV] DIR NAME VOL TRK SEC  C or c DRV" },
    { "delete", c_delete, 1, 1, "",     true,  "NAME.DSK                 D" },
    { "read",   c_read,   3, 3, "out,nak,noack,quiet", true,
                                              "DRV TRK SEC [out=F] [nak|noack] [quiet]  s" },
    { "write",  c_write,  3, 4, "fill,badck", true,
                                              "DRV TRK SEC FILE|fill=XX [badck]  r" },
    { "fetch",  c_fetch,  2, 2, "",     true,  "DRV FILE                 read whole image to FILE" },
    { "put",    c_put,    2, 2, "",     true,  "DRV FILE                 write local image FILE to DRV" },
    { "rexit",  c_rexit,  0, 0, "",     true,  "                         E (REXIT)" },
    { "select", c_select, 0, 2, "term", true,  "[LOAD] [PUNCH] [term=cr|lf|eot|nul]  |CPU:FILE|CPU:FILE" },
    { "load",   c_load,   0, 0, "out,idle,eof,pause-every,pause-ms,stop-after", true,
                                              "[out=F|-] [idle=MS] [eof=record|idle] [pause-every=N"
                                              " pause-ms=MS] [stop-after=N]  DC1 ... DC3" },
    { "punch",  c_punch,  1, 1, "eol,leader,trailer,char-us,line-ms,nodc4", true,
                                              "FILE|- [eol=crlf|cr|lf] [leader=N] [trailer=N]"
                                              " [char-us=N] [line-ms=N] [nodc4]  DC2 ... DC4" },
    { "dc1",    c_dc,     0, 0, "",     false, "                         send DC1 (also dc2, dc3, dc4)" },
    { "dc2",    c_dc,     0, 0, "",     false, NULL },
    { "dc3",    c_dc,     0, 0, "",     false, NULL },
    { "dc4",    c_dc,     0, 0, "",     false, NULL },
    { "send",   c_send,   1, MAX_ARGS, "", false, "TOKEN...                 raw bytes: $XX, 0xXX, or text with \\r \\n \\xHH" },
    { "recv",   c_recv,   0, 1, "",     false, "[MS]                     hex dump input until quiet for MS" },
    { "sleep",  c_sleep,  1, 1, "",     false, "MS" },
    { "echo",   c_echo,   0, MAX_ARGS, "", false, "TEXT..." },
    { "status", c_status, 0, 0, "",     false, "                         link and negotiation state" },
    { "help",   c_help,   0, 0, "",     false, NULL },
};

static int c_help(struct args *a)
{
    (void)a;
    puts("commands (prefix with ! to expect failure):");
    for (size_t i = 0; i < ARRAY_LEN(commands); i++)
        if (commands[i].usage)
            printf("  %-6s %s\n", commands[i].name, commands[i].usage);
    puts("  quit");
    return 0;
}

static bool key_allowed(const char *keys, const char *k)
{
    size_t kl = strlen(k);

    for (const char *p = keys; *p;) {
        const char *e = strchr(p, ',');
        size_t l = e ? (size_t)(e - p) : strlen(p);
        if (l == kl && !strncmp(p, k, l))
            return true;
        p += l + (e != NULL);
    }
    return false;
}

/* Run one tokenised command. 0 ok, 1 failed, -1 quit. */
static int run(int argc, char **argv)
{
    const struct command *c = NULL;
    struct args a = { .npos = 0 };
    bool invert = false;
    const char *name = argv[0];
    int rc;

    if (*name == '!') {
        invert = true;
        name++;
    }
    if (!strcmp(name, "quit") || !strcmp(name, "exit"))
        return -1;
    for (size_t i = 0; i < ARRAY_LEN(commands); i++)
        if (!strcmp(commands[i].name, name))
            c = &commands[i];
    if (!c)
        return fail(name, "unknown command (try help)");
    a.cmd = c->name;
    for (int i = 1; i < argc; i++) {
        char *eq = strchr(argv[i], '=');
        if (c->keys[0] && (eq || key_allowed(c->keys, argv[i]))) {
            if (a.nkv == MAX_ARGS)
                return fail(name, "too many options");
            a.key[a.nkv] = argv[i];
            a.val[a.nkv] = eq ? eq + 1 : "";
            if (eq)
                *eq = '\0';
            if (!key_allowed(c->keys, a.key[a.nkv]))
                return fail(name, "unknown option '%s'", a.key[a.nkv]);
            a.nkv++;
        } else {
            if (a.npos == MAX_ARGS)
                return fail(name, "too many arguments");
            a.pos[a.npos++] = argv[i];
        }
    }
    if (a.npos < c->minpos || a.npos > c->maxpos)
        return fail(name, "usage: %s %s", c->name, c->usage ? c->usage : "");
    if (L.fd < 0 && c->fn != c_help && c->fn != c_echo && c->fn != c_sleep)
        return fail(name, "no link");
    if (L.eof && c->fn != c_help && c->fn != c_echo && c->fn != c_status)
        return fail(name, "link is closed");
    if (c->flexnet)
        flush_stray();
    rc = c->fn(&a);
    fflush(stdout);
    if (invert) {
        if (rc == 0)
            fprintf(stderr, "fnetmock: %s: succeeded but failure was expected\n", name);
        else
            printf("%s: failed as expected\n", name);
        return rc == 0;
    }
    return rc;
}

/* Split a line into commands (';') and tokens; quotes group, '#' starts a comment. */
static int run_line(char *line)
{
    char *argv[MAX_ARGS * 2 + 1], tok[4096];
    char *store[MAX_ARGS * 2 + 1];
    int argc = 0, rc = 0;
    size_t tl = 0;
    bool in_tok = false;
    char q = 0;

    for (char *p = line;; p++) {
        char ch = *p;
        bool end_cmd = !ch || (!q && (ch == ';' || (ch == '#' && !in_tok)));

        if (q && ch && ch != q) {
            if (tl < sizeof tok - 1)
                tok[tl++] = ch;
            continue;
        }
        if (q && ch == q) {
            q = 0;
            continue;
        }
        if (!end_cmd && (ch == '"' || ch == '\'')) {
            q = ch;
            in_tok = true;
            continue;
        }
        if (end_cmd || isspace((unsigned char)ch)) {
            if (in_tok) {
                tok[tl] = '\0';
                if (argc < (int)ARRAY_LEN(argv) - 1 && (store[argc] = strdup(tok)))
                    argv[argc] = store[argc], argc++;
                tl = 0;
                in_tok = false;
            }
            if (end_cmd) {
                if (argc) {
                    int r = run(argc, argv);
                    for (int i = 0; i < argc; i++)
                        free(store[i]);
                    argc = 0;
                    if (r < 0)
                        return -1;
                    if (r) {
                        rc = 1;
                        if (!g_keep_going)
                            return 1;
                    }
                }
                if (!ch || ch == '#')
                    break;
            }
            continue;
        }
        in_tok = true;
        if (tl < sizeof tok - 1)
            tok[tl++] = ch;
    }
    for (int i = 0; i < argc; i++)
        free(store[i]);
    return rc;
}

static int run_stream(FILE *f, bool prompt)
{
    char line[4096];
    int rc = 0;

    for (;;) {
        int r;
        if (prompt) {
            fputs("fnetmock> ", stdout);
            fflush(stdout);
        }
        if (!fgets(line, sizeof line, f))
            break;
        r = run_line(line);
        if (r < 0)
            return rc;
        if (r) {
            rc = 1;
            if (!g_keep_going && !prompt)
                return 1;
        }
    }
    if (prompt)
        putchar('\n');
    return rc;
}

/* ---------------------------------------------------------------- transports */

static int split_hostport(const char *s, char *host, size_t hsz, const char **port)
{
    const char *colon;

    if (*s == '[') {
        const char *rb = strchr(s, ']');
        if (!rb || rb[1] != ':')
            return -1;
        snprintf(host, hsz, "%.*s", (int)(rb - s - 1), s + 1);
        *port = rb + 2;
        return 0;
    }
    colon = strrchr(s, ':');
    if (!colon) {
        host[0] = '\0';
        *port = s;
        return 0;
    }
    snprintf(host, hsz, "%.*s", (int)(colon - s), s);
    *port = colon + 1;
    return 0;
}

static void tcp_opts(int fd)
{
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
}

static int open_connect(const char *spec)
{
    struct addrinfo hints = { .ai_socktype = SOCK_STREAM }, *res, *ai;
    char host[256];
    const char *port;
    int fd = -1, e;

    if (split_hostport(spec, host, sizeof host, &port) < 0 || !host[0]) {
        fprintf(stderr, "fnetmock: --connect needs HOST:PORT\n");
        return -1;
    }
    if ((e = getaddrinfo(host, port, &hints, &res))) {
        fprintf(stderr, "fnetmock: %s: %s\n", spec, gai_strerror(e));
        return -1;
    }
    for (ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC, ai->ai_protocol);
        if (fd >= 0 && connect(fd, ai->ai_addr, ai->ai_addrlen) == 0)
            break;
        e = errno;
        if (fd >= 0)
            close(fd);
        fd = -1;
        errno = e;
    }
    freeaddrinfo(res);
    if (fd < 0) {
        fprintf(stderr, "fnetmock: connect %s: %s\n", spec, strerror(errno));
        return -1;
    }
    tcp_opts(fd);
    snprintf(L.desc, sizeof L.desc, "tcp to %s", spec);
    return fd;
}

static int open_listen(const char *spec, int accept_ms)
{
    struct addrinfo hints = { .ai_socktype = SOCK_STREAM, .ai_flags = AI_PASSIVE }, *res;
    struct sockaddr_storage peer;
    socklen_t plen = sizeof peer;
    char host[256], phost[NI_MAXHOST], pserv[NI_MAXSERV];
    const char *port;
    struct pollfd pf;
    int lfd, fd, one = 1, e;

    if (split_hostport(spec, host, sizeof host, &port) < 0) {
        fprintf(stderr, "fnetmock: bad --listen address '%s'\n", spec);
        return -1;
    }
    if ((e = getaddrinfo(host[0] ? host : NULL, port, &hints, &res))) {
        fprintf(stderr, "fnetmock: %s: %s\n", spec, gai_strerror(e));
        return -1;
    }
    lfd = socket(res->ai_family, res->ai_socktype | SOCK_CLOEXEC, res->ai_protocol);
    if (lfd < 0 || setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one) < 0 ||
        bind(lfd, res->ai_addr, res->ai_addrlen) < 0 || listen(lfd, 1) < 0) {
        fprintf(stderr, "fnetmock: listen %s: %s\n", spec, strerror(errno));
        freeaddrinfo(res);
        if (lfd >= 0)
            close(lfd);
        return -1;
    }
    freeaddrinfo(res);
    fprintf(stderr, "fnetmock: listening on %s, waiting for fnetd\n", spec);
    pf = (struct pollfd){ .fd = lfd, .events = POLLIN };
    if (poll(&pf, 1, accept_ms ? accept_ms : -1) <= 0) {
        fprintf(stderr, "fnetmock: no connection on %s\n", spec);
        close(lfd);
        return -1;
    }
    fd = accept4(lfd, (struct sockaddr *)&peer, &plen, SOCK_CLOEXEC);
    close(lfd);
    if (fd < 0) {
        fprintf(stderr, "fnetmock: accept: %s\n", strerror(errno));
        return -1;
    }
    tcp_opts(fd);
    if (getnameinfo((struct sockaddr *)&peer, plen, phost, sizeof phost, pserv, sizeof pserv,
                    NI_NUMERICHOST | NI_NUMERICSERV))
        str_copy(phost, "?", sizeof phost), str_copy(pserv, "?", sizeof pserv);
    snprintf(L.desc, sizeof L.desc, "tcp from %s:%s", phost, pserv);
    fprintf(stderr, "fnetmock: connection from %s:%s\n", phost, pserv);
    return fd;
}

static speed_t baud_const(unsigned long b)
{
    switch (b) {
    case 300: return B300;       case 600: return B600;       case 1200: return B1200;
    case 2400: return B2400;     case 4800: return B4800;     case 9600: return B9600;
    case 19200: return B19200;   case 38400: return B38400;   case 57600: return B57600;
    case 115200: return B115200; case 230400: return B230400;
    default: return 0;
    }
}

static int set_raw(int fd, unsigned long baud)
{
    struct termios tio;

    if (tcgetattr(fd, &tio) < 0)
        return -1;
    cfmakeraw(&tio);
    tio.c_cflag |= CLOCAL | CREAD;
    tio.c_cflag &= (tcflag_t)~CRTSCTS;
    tio.c_cc[VMIN] = 1;
    tio.c_cc[VTIME] = 0;
    if (baud) {
        cfsetispeed(&tio, baud_const(baud));
        cfsetospeed(&tio, baud_const(baud));
    }
    return tcsetattr(fd, TCSANOW, &tio);
}

static int open_tty(const char *dev, unsigned long baud)
{
    int fd = open(dev, O_RDWR | O_NOCTTY | O_CLOEXEC);

    if (fd < 0 || set_raw(fd, baud) < 0) {
        fprintf(stderr, "fnetmock: %s: %s\n", dev, strerror(errno));
        if (fd >= 0)
            close(fd);
        return -1;
    }
    snprintf(L.desc, sizeof L.desc, "tty %s %lu", dev, baud);
    return fd;
}

static void remove_link(void)
{
    if (g_pty_link[0])
        unlink(g_pty_link);
}

static int open_pty(const char *link)
{
    const char *name;
    int fd = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);

    if (fd < 0 || grantpt(fd) < 0 || unlockpt(fd) < 0 || !(name = ptsname(fd))) {
        fprintf(stderr, "fnetmock: pty: %s\n", strerror(errno));
        if (fd >= 0)
            close(fd);
        return -1;
    }
    /* Hold the slave open so the master never sees a hangup between fnetd opens. */
    g_pty_slave = open(name, O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (g_pty_slave < 0 || set_raw(g_pty_slave, 0) < 0) {
        fprintf(stderr, "fnetmock: %s: %s\n", name, strerror(errno));
        close(fd);
        return -1;
    }
    if (link && *link) {
        unlink(link);
        if (symlink(name, link) < 0) {
            fprintf(stderr, "fnetmock: symlink %s: %s\n", link, strerror(errno));
            close(fd);
            return -1;
        }
        str_copy(g_pty_link, link, sizeof g_pty_link);
        atexit(remove_link);
    }
    snprintf(L.desc, sizeof L.desc, "pty %s", name);
    printf("pty: %s%s%s\n", name, link && *link ? " <- " : "", link && *link ? link : "");
    fflush(stdout);
    return fd;
}

static void on_signal(int sig)
{
    remove_link();
    signal(sig, SIG_DFL);
    raise(sig);
}

/* ---------------------------------------------------------------- main */

static void usage(FILE *f)
{
    fprintf(f,
        "usage: fnetmock TRANSPORT [options] [COMMAND [ARGS...]]\n"
        "Mock FLEX target for fnetd: FlexNet client and paper tape reader/punch.\n"
        "\n"
        "transport (one):\n"
        "  -C, --connect HOST:PORT   dial an fnetd 'listen' port (terminal server role)\n"
        "  -L, --listen [ADDR:]PORT  accept an fnetd 'rfc2217' port (terminal server role)\n"
        "  -T, --tty DEVICE          serial device\n"
        "  -P, --pty[=LINK]          new pty for an fnetd 'tty' port; LINK gets a symlink\n"
        "options:\n"
        "  -r, --raw                 no Telnet/RFC-2217 on TCP\n"
        "  -b, --baud N              tty speed (default 9600)\n"
        "  -t, --timeout MS          reply timeout (default 3000)\n"
        "  -w, --wait MS             listen: give up after MS; pty/connect: pause before commands\n"
        "  -e, --exec 'CMD; CMD'     run commands (repeatable)\n"
        "  -s, --script FILE         run commands from FILE (- = stdin)\n"
        "  -k, --keep-going          continue after a failed command\n"
        "  -v, --verbose             telnet/RFC-2217 negotiation; -vv adds a wire hex dump\n"
        "  -V, --version\n"
        "With no -e, -s or COMMAND, commands are read from stdin. 'help' lists them.\n");
}

int main(int argc, char **argv)
{
    static const struct option lo[] = {
        { "connect", required_argument, NULL, 'C' },
        { "listen",  required_argument, NULL, 'L' },
        { "tty",     required_argument, NULL, 'T' },
        { "pty",     optional_argument, NULL, 'P' },
        { "raw",     no_argument,       NULL, 'r' },
        { "baud",    required_argument, NULL, 'b' },
        { "timeout", required_argument, NULL, 't' },
        { "wait",    required_argument, NULL, 'w' },
        { "exec",    required_argument, NULL, 'e' },
        { "script",  required_argument, NULL, 's' },
        { "keep-going", no_argument,    NULL, 'k' },
        { "verbose", no_argument,       NULL, 'v' },
        { "version", no_argument,       NULL, 'V' },
        { "help",    no_argument,       NULL, 'h' },
        { NULL, 0, NULL, 0 },
    };
    struct { char kind; const char *s; } src[64];
    int nsrc = 0, c, rc = 0;
    char mode = 0;
    const char *target = NULL;
    unsigned long baud = 9600, wait_ms = 0, v;
    bool raw = false;

    while ((c = getopt_long(argc, argv, "+C:L:T:P::rb:t:w:e:s:kvVh", lo, NULL)) != -1) {
        switch (c) {
        case 'C': case 'L': case 'T': case 'P':
            if (mode) {
                fprintf(stderr, "fnetmock: only one transport\n");
                return 2;
            }
            mode = (char)c;
            target = optarg;
            break;
        case 'r': raw = true; break;
        case 'b':
            if (parse_ulong(optarg, 4000000, &baud) < 0 || !baud_const(baud)) {
                fprintf(stderr, "fnetmock: unsupported baud '%s'\n", optarg);
                return 2;
            }
            break;
        case 't':
        case 'w':
            if (parse_ulong(optarg, 3600000, &v) < 0) {
                fprintf(stderr, "fnetmock: bad milliseconds '%s'\n", optarg);
                return 2;
            }
            if (c == 't')
                g_timeout_ms = (int)v;
            else
                wait_ms = v;
            break;
        case 'e': case 's':
            if (nsrc == (int)ARRAY_LEN(src)) {
                fprintf(stderr, "fnetmock: too many -e/-s\n");
                return 2;
            }
            src[nsrc].kind = (char)c;
            src[nsrc++].s = optarg;
            break;
        case 'k': g_keep_going = true; break;
        case 'v': g_verbose++; break;
        case 'V': printf("fnetmock %s\n", FNET_VERSION); return 0;
        case 'h': usage(stdout); return 0;
        default:  usage(stderr); return 2;
        }
    }
    if (!mode) {
        usage(stderr);
        return 2;
    }
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    switch (mode) {
    case 'C': L.fd = open_connect(target); L.telnet = !raw; break;
    case 'L': L.fd = open_listen(target, (int)wait_ms); L.telnet = !raw; wait_ms = 0; break;
    case 'T': L.fd = open_tty(target, baud); break;
    case 'P': L.fd = open_pty(target); break;
    }
    if (L.fd < 0)
        return 2;
    set_nonblock(L.fd);
    telnet_reset(&L.tn);

    /* Let fnetd open the pty / finish negotiating before the first command. */
    if (wait_ms) {
        uint64_t end = now_us() + wait_ms * 1000u;
        while (!L.eof && ms_left(end))
            if (fill(ms_left(end)) > 0)
                L.len = 0;
    } else if (L.telnet) {
        fill(200);
        while (fill(50) > 0)
            ;
        L.len = 0;
    }

    for (int i = 0; i < nsrc; i++) {
        int r;
        if (src[i].kind == 'e') {
            char *dup = strdup(src[i].s);
            r = dup ? run_line(dup) : 2;
            free(dup);
        } else {
            FILE *f = strcmp(src[i].s, "-") ? fopen(src[i].s, "r") : stdin;
            if (!f) {
                fprintf(stderr, "fnetmock: %s: %s\n", src[i].s, strerror(errno));
                return 2;
            }
            r = run_stream(f, false);
            if (f != stdin)
                fclose(f);
        }
        if (r < 0)
            goto out;
        if (r) {
            rc = 1;
            if (!g_keep_going)
                goto out;
        }
    }
    if (optind < argc) {
        int r = run(argc - optind, argv + optind);
        if (r > 0)
            rc = 1;
    } else if (!nsrc) {
        rc = run_stream(stdin, isatty(STDIN_FILENO));
    }
out:
    if (L.fd >= 0)
        close(L.fd);
    if (g_pty_slave >= 0)
        close(g_pty_slave);
    return rc;
}
