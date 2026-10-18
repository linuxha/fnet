/* port.c — port lifecycle, transports (tty, RFC-2217, listen, raw TCP) and dispatcher */
#include "port.h"
#include "util.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/sockios.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <termios.h>
#include <unistd.h>

#define TTY_RETRY_US   2000000u
#define BACKOFF_MAX_S  30u

static void link_close(struct port *p, const char *why, bool reschedule);
static void port_rx_byte(struct port *p, uint8_t b);

const char *port_mode_name(int mode)
{
    switch (mode) {
    case MODE_TOP:      return "idle";
    case MODE_FLEXNET:  return "flexnet";
    case MODE_TAPE_SEL: return "tape-select";
    case MODE_LOAD:     return "tape-load";
    case MODE_PUNCH:    return "tape-punch";
    }
    return "?";
}

const char *port_link_name(int link)
{
    switch (link) {
    case LINK_DOWN:       return "down";
    case LINK_CONNECTING: return "connecting";
    case LINK_UP:         return "up";
    case LINK_LISTENING:  return "listening";
    case LINK_DISABLED:   return "disabled";
    }
    return "?";
}

unsigned port_char_us(const struct port *p)
{
    unsigned baud = p->cfg.baud ? p->cfg.baud : 9600;
    return 10000000u / baud;
}

/* ---------------------------------------------------------------- output */

static void update_events(struct port *p)
{
    bool want = p->txlen > 0;
    if (p->ev.fd < 0 || want == p->want_out || p->link != LINK_UP)
        return;
    p->want_out = want;
    loop_mod(&p->ev, EPOLLIN | EPOLLRDHUP | (want ? EPOLLOUT : 0u));
}

static void flush_tx(struct port *p)
{
    while (p->txlen && p->ev.fd >= 0) {
        ssize_t w = write(p->ev.fd, p->tx, p->txlen);
        if (w > 0) {
            memmove(p->tx, p->tx + w, p->txlen - (size_t)w);
            p->txlen -= (size_t)w;
            continue;
        }
        if (w < 0 && errno == EINTR)
            continue;
        if (w < 0 && errno != EAGAIN)
            flog(FL_DEBUG, &p->log, "xport", "write: %s", strerror(errno));
        break;
    }
    update_events(p);
}

static void tx_raw(struct port *p, const uint8_t *buf, size_t n)
{
    if (p->ev.fd < 0)
        return;
    if (n > sizeof p->tx - p->txlen) {
        flog(FL_WARN, &p->log, "xport", "transmit buffer full, %zu bytes dropped", n);
        return;
    }
    memcpy(p->tx + p->txlen, buf, n);
    p->txlen += n;
}

void port_send(struct port *p, const void *buf, size_t n)
{
    const uint8_t *b = buf;

    if (p->ev.fd < 0 || p->link != LINK_UP) {
        flog(FL_DEBUG, &p->log, "xport", "link down, %zu bytes not sent", n);
        return;
    }
    flog_hex(&p->log, "wire", "tx", b, n);
    p->st.tx_bytes += n;
    if (p->use_telnet) {
        uint8_t esc[1024];
        while (n) {
            size_t chunk = n > sizeof esc / 2 ? sizeof esc / 2 : n;
            size_t m = telnet_escape(b, chunk, esc);
            tx_raw(p, esc, m);
            b += chunk;
            n -= chunk;
        }
    } else {
        tx_raw(p, b, n);
    }
    flush_tx(p);
}

void port_send_byte(struct port *p, uint8_t b)
{
    port_send(p, &b, 1);
}

size_t port_pending_tx(struct port *p)
{
    int q = 0;
    if (p->ev.fd >= 0)
        ioctl(p->ev.fd, p->cfg.type == PT_TTY ? TIOCOUTQ : SIOCOUTQ, &q);
    return p->txlen + (q > 0 ? (size_t)q : 0);
}

void port_arm_xfer(struct port *p)
{
    uint64_t us = (uint64_t)p->cfg.xfer_timeout_ms * 1000u +
                  (uint64_t)port_pending_tx(p) * port_char_us(p);
    timer_arm(&p->xfer_timer, us);
}

bool port_busy(const struct port *p)
{
    return p->mode != MODE_TOP;
}

/* ---------------------------------------------------------------- telnet */

static bool bit_get(const uint8_t *bs, uint8_t o) { return bs[o / 8] & (1u << (o % 8)); }
static void bit_set(uint8_t *bs, uint8_t o)       { bs[o / 8] = (uint8_t)(bs[o / 8] | (1u << (o % 8))); }
static void bit_clr(uint8_t *bs, uint8_t o)       { bs[o / 8] = (uint8_t)(bs[o / 8] & ~(1u << (o % 8))); }

static void tn_cmd(struct port *p, uint8_t cmd, uint8_t opt)
{
    uint8_t b[3] = { TN_IAC, cmd, opt };
    tx_raw(p, b, 3);
}

static void tn_send_comport(struct port *p)
{
    uint8_t sb[32], v[4];
    size_t n;

    v[0] = (uint8_t)(p->cfg.baud >> 24);
    v[1] = (uint8_t)(p->cfg.baud >> 16);
    v[2] = (uint8_t)(p->cfg.baud >> 8);
    v[3] = (uint8_t)p->cfg.baud;
    n = telnet_comport_sb(sb, CPO_SET_BAUDRATE, v, 4);
    tx_raw(p, sb, n);
    v[0] = 8;
    n = telnet_comport_sb(sb, CPO_SET_DATASIZE, v, 1);
    tx_raw(p, sb, n);
    v[0] = 1;                                   /* NONE */
    n = telnet_comport_sb(sb, CPO_SET_PARITY, v, 1);
    tx_raw(p, sb, n);
    v[0] = 1;                                   /* 1 stop bit */
    n = telnet_comport_sb(sb, CPO_SET_STOPSIZE, v, 1);
    tx_raw(p, sb, n);
    v[0] = 1;                                   /* no flow control */
    n = telnet_comport_sb(sb, CPO_SET_CONTROL, v, 1);
    tx_raw(p, sb, n);
    p->tn_comport_sent = true;
    flog(FL_DEBUG, &p->log, "rfc2217", "sent COM-PORT settings %u 8N1 no-flow", p->cfg.baud);
}

static void on_tn_cmd(void *arg, uint8_t cmd, uint8_t opt)
{
    struct port *p = arg;
    bool ours = opt == TNO_BINARY || opt == TNO_SGA || opt == TNO_COMPORT;

    flog(FL_DEBUG, &p->log, "rfc2217", "recv %s %u",
         cmd == TN_WILL ? "WILL" : cmd == TN_WONT ? "WONT" : cmd == TN_DO ? "DO" : "DONT", opt);
    switch (cmd) {
    case TN_DO:
        if (ours) {
            if (!bit_get(p->tn_will, opt)) {
                bit_set(p->tn_will, opt);
                tn_cmd(p, TN_WILL, opt);
            }
            if (opt == TNO_COMPORT && !p->tn_comport_sent)
                tn_send_comport(p);
        } else if (!bit_get(p->tn_refused, opt)) {
            bit_set(p->tn_refused, opt);
            tn_cmd(p, TN_WONT, opt);
        }
        break;
    case TN_DONT:
        if (opt == TNO_COMPORT)
            flog(FL_WARN, &p->log, "rfc2217",
                 "terminal server refused COM-PORT-OPTION; using its preset serial settings");
        if (opt == TNO_BINARY)
            flog(FL_WARN, &p->log, "rfc2217", "terminal server refused BINARY; link may not be 8-bit clean");
        if (bit_get(p->tn_will, opt)) {
            bit_clr(p->tn_will, opt);
            tn_cmd(p, TN_WONT, opt);
        }
        break;
    case TN_WILL:
        if (opt == TNO_BINARY || opt == TNO_SGA) {
            if (!bit_get(p->tn_do, opt)) {
                bit_set(p->tn_do, opt);
                tn_cmd(p, TN_DO, opt);
            }
        } else if (!bit_get(p->tn_refused, opt)) {
            bit_set(p->tn_refused, opt);
            tn_cmd(p, TN_DONT, opt);
        }
        break;
    case TN_WONT:
        if (opt == TNO_BINARY)
            flog(FL_WARN, &p->log, "rfc2217", "terminal server won't send BINARY; link may not be 8-bit clean");
        bit_clr(p->tn_do, opt);
        break;
    }
}

static void on_tn_sb(void *arg, const uint8_t *sb, size_t len)
{
    struct port *p = arg;
    static const uint8_t want1[] = { 0, 0, 8, 1, 1, 1 };   /* index by cmd-100 for 2..5 */
    uint8_t c;

    if (len < 2 || sb[0] != TNO_COMPORT)
        return;
    c = sb[1];
    if (c == CPO_SERVER_OFFSET + CPO_SET_BAUDRATE && len >= 6) {
        unsigned b = (unsigned)sb[2] << 24 | (unsigned)sb[3] << 16 | (unsigned)sb[4] << 8 | sb[5];
        if (b != p->cfg.baud)
            flog(FL_WARN, &p->log, "rfc2217", "terminal server set baud %u (wanted %u)", b, p->cfg.baud);
        else
            flog(FL_DEBUG, &p->log, "rfc2217", "terminal server confirmed baud %u", b);
    } else if (c >= CPO_SERVER_OFFSET + CPO_SET_DATASIZE &&
               c <= CPO_SERVER_OFFSET + CPO_SET_CONTROL && len >= 3) {
        unsigned idx = c - CPO_SERVER_OFFSET;
        if (sb[2] != want1[idx])
            flog(FL_WARN, &p->log, "rfc2217", "terminal server answered setting %u with %u (wanted %u)",
                 idx, sb[2], want1[idx]);
    } else {
        flog(FL_DEBUG, &p->log, "rfc2217", "COM-PORT notification %u (%zu bytes)", c, len);
    }
}

static void tn_start(struct port *p)
{
    telnet_reset(&p->tn);
    memset(p->tn_will, 0, sizeof p->tn_will);
    memset(p->tn_do, 0, sizeof p->tn_do);
    memset(p->tn_refused, 0, sizeof p->tn_refused);
    p->tn_comport_sent = false;
    if (!p->use_telnet)
        return;
    bit_set(p->tn_will, TNO_BINARY);
    tn_cmd(p, TN_WILL, TNO_BINARY);
    bit_set(p->tn_do, TNO_BINARY);
    tn_cmd(p, TN_DO, TNO_BINARY);
    bit_set(p->tn_will, TNO_SGA);
    tn_cmd(p, TN_WILL, TNO_SGA);
    bit_set(p->tn_do, TNO_SGA);
    tn_cmd(p, TN_DO, TNO_SGA);
    bit_set(p->tn_will, TNO_COMPORT);
    tn_cmd(p, TN_WILL, TNO_COMPORT);
    flush_tx(p);
}

/* ---------------------------------------------------------------- input */

static void on_data(struct evh *h, uint32_t events);

static void handle_input(struct port *p)
{
    uint8_t raw[512], dat[512];
    int fd = p->ev.fd;
    const struct telnet_cb cb = { on_tn_cmd, on_tn_sb, p };

    for (;;) {
        ssize_t n = read(fd, raw, sizeof raw);
        size_t m;

        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && errno == EAGAIN)
            return;
        if (n <= 0) {
            link_close(p, n == 0 ? "connection closed" : strerror(errno), true);
            return;
        }
        if (p->use_telnet) {
            m = telnet_rx(&p->tn, raw, (size_t)n, dat, &cb);
            flush_tx(p);                       /* negotiation replies */
        } else {
            memcpy(dat, raw, (size_t)n);
            m = (size_t)n;
        }
        if (!m)
            continue;
        flog_hex(&p->log, "wire", "rx", dat, m);
        p->st.rx_bytes += m;
        p->last_rx_us = now_us();
        for (size_t i = 0; i < m; i++) {
            port_rx_byte(p, dat[i]);
            if (p->ev.fd != fd)
                return;
        }
    }
}

static void tcp_connected(struct port *p);

static void on_data(struct evh *h, uint32_t events)
{
    struct port *p = h->arg;

    if (p->dead || p->ev.fd < 0)
        return;
    if (p->link == LINK_CONNECTING) {
        int err = 0;
        socklen_t len = sizeof err;
        getsockopt(p->ev.fd, SOL_SOCKET, SO_ERROR, &err, &len);
        if (err) {
            link_close(p, strerror(err), true);
            return;
        }
        if (events & EPOLLOUT)
            tcp_connected(p);
        return;
    }
    if (events & EPOLLIN)
        handle_input(p);
    if (p->ev.fd < 0)
        return;
    if (events & EPOLLOUT)
        flush_tx(p);
    if (events & (EPOLLHUP | EPOLLERR | EPOLLRDHUP)) {
        /* drain anything left, then close */
        handle_input(p);
        if (p->ev.fd >= 0)
            link_close(p, "hangup", true);
    }
}

/* ---------------------------------------------------------------- dispatcher (§2.3) */

static void port_rx_byte(struct port *p, uint8_t b)
{
    switch (p->mode) {
    case MODE_FLEXNET:
        flexnet_byte(p, b);
        return;
    case MODE_TAPE_SEL:
        tape_sel_byte(p, b);
        return;
    case MODE_LOAD:
        if (b == DC3) {
            tape_load_pause(p);
        } else {
            p->st.discarded++;
            flog(FL_DEBUG, &p->log, "tape", "discarded 0x%02x during load", b);
        }
        return;
    case MODE_PUNCH:
        tape_punch_byte(p, b);
        return;
    }

    /* MODE_TOP */
    if (b == 0x55 || b == 0xaa || flexnet_is_cmd(b)) {
        p->fn.consec_garbage = 0;
        flexnet_start(p, b);
        return;
    }
    switch (b) {
    case '|':
        tape_sel_start(p);
        return;
    case DC1:
        tape_load_start(p);
        return;
    case DC2:
        tape_punch_start(p);
        return;
    case DC3:
    case DC4:
        flog(FL_DEBUG, &p->log, "tape", "DC%d with no tape active, ignored", b - DC1 + 1);
        return;
    }
    p->st.discarded++;
    flog(FL_DEBUG, &p->log, "core", "discarded 0x%02x at top of loop", b);
    flexnet_garbage(p);
}

static void on_xfer_timeout(struct ftimer *t)
{
    struct port *p = t->arg;

    if (p->mode == MODE_FLEXNET) {
        flexnet_timeout(p);
    } else if (p->mode == MODE_TAPE_SEL) {
        flog(FL_WARN, &p->log, "tape", "file selection string timed out");
        p->st.xfer_timeouts++;
        p->mode = MODE_TOP;
    }
}

static void on_tape_ptimer(struct ftimer *t)
{
    tape_timeout(t->arg);
}

/* ---------------------------------------------------------------- tty */

static speed_t baud_const(unsigned b)
{
    switch (b) {
    case 50: return B50;          case 75: return B75;          case 110: return B110;
    case 134: return B134;        case 150: return B150;        case 200: return B200;
    case 300: return B300;        case 600: return B600;        case 1200: return B1200;
    case 1800: return B1800;      case 2400: return B2400;      case 4800: return B4800;
    case 9600: return B9600;      case 19200: return B19200;    case 38400: return B38400;
    case 57600: return B57600;    case 115200: return B115200;  case 230400: return B230400;
    case 460800: return B460800;
    }
    return B0;
}

static void schedule_link(struct port *p, uint64_t us)
{
    timer_arm(&p->link_timer, us);
}

static void link_up(struct port *p, int fd)
{
    p->ev.fd = fd;
    p->ev.cb = on_data;
    p->ev.arg = p;
    p->want_out = false;
    p->link = LINK_UP;
    p->txlen = 0;
    p->st.connects++;
    loop_add(&p->ev, EPOLLIN | EPOLLRDHUP);
}

static void tty_open(struct port *p)
{
    struct termios tio;
    int fd = open(p->cfg.device, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);

    if (fd < 0) {
        if (p->link != LINK_DOWN || !p->backoff_s)
            flog(FL_WARN, &p->log, "xport", "%s: %s (retrying every 2 s)", p->cfg.device, strerror(errno));
        p->backoff_s = 1;
        p->link = LINK_DOWN;
        schedule_link(p, TTY_RETRY_US);
        return;
    }
    if (tcgetattr(fd, &tio) == 0) {
        speed_t sp = baud_const(p->cfg.baud);
        cfmakeraw(&tio);
        tio.c_cflag |= CLOCAL | CREAD;
        tio.c_cflag &= ~(tcflag_t)(CRTSCTS | CSTOPB | PARENB | CSIZE);
        tio.c_cflag |= CS8;
        tio.c_iflag &= ~(tcflag_t)(IXON | IXOFF | IXANY);
        tio.c_cc[VMIN] = 1;
        tio.c_cc[VTIME] = 0;
        cfsetispeed(&tio, sp);
        cfsetospeed(&tio, sp);
        if (tcsetattr(fd, TCSANOW, &tio) < 0)
            flog(FL_WARN, &p->log, "xport", "%s: tcsetattr: %s", p->cfg.device, strerror(errno));
        ioctl(fd, TIOCEXCL);
        tcflush(fd, TCIOFLUSH);
    }
    p->backoff_s = 0;
    p->use_telnet = false;
    str_copy(p->peer, p->cfg.device, sizeof p->peer);
    link_up(p, fd);
    flog(FL_INFO, &p->log, "xport", "opened %s at %u baud", p->cfg.device, p->cfg.baud);
}

/* ---------------------------------------------------------------- tcp */

static void tcp_tune(int fd)
{
    int one = 1, idle = 30, intvl = 10, cnt = 3;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof one);
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof idle);
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof intvl);
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof cnt);
}

static void tcp_connected(struct port *p)
{
    p->link = LINK_UP;
    p->backoff_s = 0;
    p->st.connects++;
    p->want_out = true;               /* registered with EPOLLOUT while connecting */
    p->txlen = 0;
    tcp_tune(p->ev.fd);
    flog(FL_INFO, &p->log, "xport", "connected to %s", p->peer);
    tn_start(p);
    flush_tx(p);                      /* rewrites the event mask */
}

static void tcp_connect(struct port *p)
{
    struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM }, *ai = NULL;
    char port_s[8];
    int fd, rc;

    snprintf(port_s, sizeof port_s, "%u", p->cfg.port);
    snprintf(p->peer, sizeof p->peer, "%s:%u", p->cfg.host, p->cfg.port);
    rc = getaddrinfo(p->cfg.host, port_s, &hints, &ai);
    if (rc) {
        flog(FL_WARN, &p->log, "xport", "%s: %s", p->peer, gai_strerror(rc));
        goto retry;
    }
    fd = socket(ai->ai_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        flog(FL_WARN, &p->log, "xport", "socket: %s", strerror(errno));
        freeaddrinfo(ai);
        goto retry;
    }
    rc = connect(fd, ai->ai_addr, ai->ai_addrlen);
    freeaddrinfo(ai);
    if (rc < 0 && errno != EINPROGRESS) {
        flog(FL_WARN, &p->log, "xport", "connect %s: %s", p->peer, strerror(errno));
        close(fd);
        goto retry;
    }
    p->use_telnet = p->cfg.telnet;
    p->ev.fd = fd;
    p->ev.cb = on_data;
    p->ev.arg = p;
    p->link = LINK_CONNECTING;
    loop_add(&p->ev, EPOLLOUT | EPOLLIN | EPOLLRDHUP);
    return;

retry:
    p->link = LINK_DOWN;
    p->backoff_s = p->backoff_s ? (p->backoff_s * 2 > BACKOFF_MAX_S ? BACKOFF_MAX_S : p->backoff_s * 2) : 1;
    schedule_link(p, (uint64_t)p->backoff_s * 1000000u);
}

static bool peer_allowed(const struct port *p, const struct sockaddr_storage *ss)
{
    const void *addr;
    int fam = ss->ss_family;

    if (fam == AF_INET)
        addr = &((const struct sockaddr_in *)ss)->sin_addr;
    else if (fam == AF_INET6)
        addr = &((const struct sockaddr_in6 *)ss)->sin6_addr;
    else
        return false;
    for (unsigned i = 0; i < p->cfg.nallow; i++)
        if (cidr_match(&p->cfg.allow[i], fam, addr))
            return true;
    return false;
}

static void fmt_peer(const struct sockaddr_storage *ss, char *out, size_t n)
{
    char a[INET6_ADDRSTRLEN] = "?";
    unsigned port = 0;

    if (ss->ss_family == AF_INET) {
        const struct sockaddr_in *s4 = (const struct sockaddr_in *)ss;
        inet_ntop(AF_INET, &s4->sin_addr, a, sizeof a);
        port = ntohs(s4->sin_port);
    } else if (ss->ss_family == AF_INET6) {
        const struct sockaddr_in6 *s6 = (const struct sockaddr_in6 *)ss;
        inet_ntop(AF_INET6, &s6->sin6_addr, a, sizeof a);
        port = ntohs(s6->sin6_port);
    }
    snprintf(out, n, "%s:%u", a, port);
}

static void on_accept(struct evh *h, uint32_t events)
{
    struct port *p = h->arg;
    struct sockaddr_storage ss;
    socklen_t sl = sizeof ss;
    char peer[80];
    int fd;

    (void)events;
    if (p->dead)
        return;
    fd = accept4(p->lev.fd, (struct sockaddr *)&ss, &sl, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (fd < 0)
        return;
    fmt_peer(&ss, peer, sizeof peer);
    if (!peer_allowed(p, &ss)) {
        flog(FL_WARN, &p->log, "xport", "rejected connection from %s (not in allow list)", peer);
        close(fd);
        return;
    }
    if (p->ev.fd >= 0) {
        if (!p->cfg.on_new_replace) {
            flog(FL_WARN, &p->log, "xport", "refused connection from %s: %s already connected",
                 peer, p->peer);
            close(fd);
            return;
        }
        flog(FL_WARN, &p->log, "xport", "connection from %s replaces %s", peer, p->peer);
        link_close(p, "replaced", false);
    }
    flog(FL_INFO, &p->log, "xport", "accepted connection from %s", peer);
    str_copy(p->peer, peer, sizeof p->peer);
    p->use_telnet = p->cfg.telnet;
    tcp_tune(fd);
    link_up(p, fd);
    tn_start(p);
}

static void listen_open(struct port *p)
{
    struct sockaddr_storage ss;
    socklen_t sl;
    int fd, one = 1;

    memset(&ss, 0, sizeof ss);
    if (inet_pton(AF_INET, p->cfg.bind, &((struct sockaddr_in *)&ss)->sin_addr) == 1) {
        struct sockaddr_in *s4 = (struct sockaddr_in *)&ss;
        s4->sin_family = AF_INET;
        s4->sin_port = htons((uint16_t)p->cfg.port);
        sl = sizeof *s4;
    } else if (inet_pton(AF_INET6, p->cfg.bind, &((struct sockaddr_in6 *)&ss)->sin6_addr) == 1) {
        struct sockaddr_in6 *s6 = (struct sockaddr_in6 *)&ss;
        s6->sin6_family = AF_INET6;
        s6->sin6_port = htons((uint16_t)p->cfg.port);
        sl = sizeof *s6;
    } else {
        flog(FL_ERROR, &p->log, "xport", "bind address '%s' is not an IP address", p->cfg.bind);
        p->link = LINK_DOWN;
        return;
    }
    fd = socket(ss.ss_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0)
        goto fail;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (ss.ss_family == AF_INET6)
        setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &one, sizeof one);
    if (bind(fd, (struct sockaddr *)&ss, sl) < 0 || listen(fd, 4) < 0) {
        close(fd);
        goto fail;
    }
    p->lev.fd = fd;
    p->lev.cb = on_accept;
    p->lev.arg = p;
    loop_add(&p->lev, EPOLLIN);
    p->link = LINK_LISTENING;
    p->backoff_s = 0;
    flog(FL_INFO, &p->log, "xport", "listening on %s:%u", p->cfg.bind, p->cfg.port);
    if (!p->cfg.nallow)
        flog(FL_WARN, &p->log, "xport", "allow list is empty: all connections will be refused");
    return;

fail:
    flog(FL_ERROR, &p->log, "xport", "listen %s:%u: %s (retrying)", p->cfg.bind, p->cfg.port,
         strerror(errno));
    p->link = LINK_DOWN;
    schedule_link(p, 5000000u);
}

/* ---------------------------------------------------------------- lifecycle */

static void link_close(struct port *p, const char *why, bool reschedule)
{
    if (p->ev.fd >= 0) {
        loop_del(&p->ev);
        if (p->cfg.type == PT_TTY)
            ioctl(p->ev.fd, TIOCNXCL);      /* exclusivity outlives us if others hold the tty */
        close(p->ev.fd);
        p->ev.fd = -1;
        flog(FL_INFO, &p->log, "xport", "link closed: %s", why);
    }
    p->txlen = 0;
    p->want_out = false;
    timer_cancel(&p->xfer_timer);

    /* Abort what was in flight; keep the session (REQ-015, REQ-016). */
    switch (p->mode) {
    case MODE_FLEXNET:
        flexnet_abort(p);
        break;
    case MODE_TAPE_SEL:
        p->mode = MODE_TOP;
        break;
    case MODE_LOAD:
        tape_load_pause(p);
        break;
    default:
        break;
    }

    if (!reschedule)
        return;
    switch (p->cfg.type) {
    case PT_TTY:
        p->link = LINK_DOWN;
        p->backoff_s = 0;
        schedule_link(p, TTY_RETRY_US);
        break;
    case PT_RFC2217:
        p->link = LINK_DOWN;
        p->backoff_s = p->backoff_s ? (p->backoff_s * 2 > BACKOFF_MAX_S ? BACKOFF_MAX_S : p->backoff_s * 2) : 1;
        flog(FL_INFO, &p->log, "xport", "reconnecting in %u s", p->backoff_s);
        schedule_link(p, (uint64_t)p->backoff_s * 1000000u);
        break;
    case PT_LISTEN:
        p->link = p->lev.fd >= 0 ? LINK_LISTENING : LINK_DOWN;
        break;
    }
}

static void on_link_timer(struct ftimer *t)
{
    struct port *p = t->arg;

    if (p->dead || !p->cfg.enabled)
        return;
    switch (p->cfg.type) {
    case PT_TTY:     if (p->ev.fd < 0) tty_open(p); break;
    case PT_RFC2217: if (p->ev.fd < 0) tcp_connect(p); break;
    case PT_LISTEN:  if (p->lev.fd < 0) listen_open(p); break;
    }
}

static void open_roots(struct port *p)
{
    if (p->flex_root < 0) {
        p->flex_root = open(p->cfg.flexnet_root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (p->flex_root < 0)
            flog(FL_ERROR, &p->log, "core", "flexnet_root %s: %s", p->cfg.flexnet_root, strerror(errno));
    }
    if (p->tape_root < 0) {
        p->tape_root = open(p->cfg.tape_root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (p->tape_root < 0)
            flog(FL_ERROR, &p->log, "core", "tape_root %s: %s", p->cfg.tape_root, strerror(errno));
    }
}

static void close_roots(struct port *p)
{
    if (p->flex_root >= 0)
        close(p->flex_root);
    if (p->tape_root >= 0)
        close(p->tape_root);
    p->flex_root = p->tape_root = -1;
}

struct port *port_new(const struct port_cfg *cfg)
{
    struct port *p = calloc(1, sizeof *p);

    if (!p)
        return NULL;
    p->cfg = *cfg;
    p->log.name = p->cfg.name;
    p->log.level = p->cfg.log_level;
    p->ev.fd = -1;
    p->lev.fd = -1;
    p->flex_root = -1;
    p->tape_root = -1;
    p->link = LINK_DOWN;
    p->mode = MODE_TOP;
    str_copy(p->fn.cwd, "/", sizeof p->fn.cwd);
    timer_init(&p->xfer_timer, on_xfer_timeout, p);
    timer_init(&p->link_timer, on_link_timer, p);
    timer_init(&p->tp.ptimer, on_tape_ptimer, p);
    tape_init(p);
    return p;
}

void port_start(struct port *p)
{
    if (!p->cfg.enabled) {
        p->link = LINK_DISABLED;
        flog(FL_INFO, &p->log, "core", "port disabled");
        return;
    }
    open_roots(p);
    p->backoff_s = 0;
    switch (p->cfg.type) {
    case PT_TTY:     tty_open(p); break;
    case PT_RFC2217: tcp_connect(p); break;
    case PT_LISTEN:  listen_open(p); break;
    }
}

void port_stop(struct port *p)
{
    timer_cancel(&p->link_timer);
    link_close(p, "port stopped", false);
    if (p->lev.fd >= 0) {
        loop_del(&p->lev);
        close(p->lev.fd);
        p->lev.fd = -1;
    }
    p->link = LINK_DISABLED;
}

void port_restart(struct port *p)
{
    port_stop(p);
    close_roots(p);
    p->log.level = p->cfg.log_level;
    port_start(p);
}

void port_free(struct port *p)
{
    port_stop(p);
    flexnet_end(p, "port removed");
    tape_shutdown(p);
    timer_cancel(&p->tp.tick);
    timer_cancel(&p->tp.ptimer);
    close_roots(p);
    free(p);
}
