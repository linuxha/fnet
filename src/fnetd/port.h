/* port.h — one fnetd endpoint and its session (REQ-011..REQ-026) */
#ifndef FNETD_PORT_H
#define FNETD_PORT_H

#include "config.h"
#include "flexdsk.h"
#include "log.h"
#include "loop.h"
#include "telnet.h"
#include "vpath.h"

#include <stdbool.h>
#include <stdint.h>

#define ACK  0x06
#define NAK  0x15
#define CR   0x0d
#define LF   0x0a
#define ESC  0x1b
#define SP   0x20
#define EOT  0x04
#define DC1  0x11
#define DC2  0x12
#define DC3  0x13
#define DC4  0x14

#define FLEX_DRIVES   4
#define TEXT_MAX      127
#define TXBUF_SIZE    16384

enum disp_mode {
    MODE_TOP,           /* waiting for a command byte */
    MODE_FLEXNET,       /* FlexNet transaction in progress */
    MODE_TAPE_SEL,      /* collecting a '|' file selection */
    MODE_LOAD,          /* paper tape reader running */
    MODE_PUNCH,         /* paper tape punch capturing */
};

enum link_state { LINK_DOWN, LINK_CONNECTING, LINK_UP, LINK_LISTENING, LINK_DISABLED };

struct port_stats {
    uint64_t rx_bytes, tx_bytes, connects;
    uint64_t sectors_read, sectors_written;
    uint64_t cksum_err_rx;      /* write with bad checksum from client */
    uint64_t cksum_err_tx;      /* client NAKed our read */
    uint64_t naks_sent, naks_received;
    uint64_t xfer_timeouts, sync_losses, discarded;
    uint64_t sessions;
    uint64_t tape_loaded, tape_punched;
};

struct mount {
    flexdsk_t *d;
    char       vpath[VPATH_MAX];
};

/* FlexNet transaction / session state */
enum fn_state { FN_IDLE, FN_BIN, FN_TEXT, FN_READACK, FN_LIST_FIRST_SP, FN_LIST };

struct flexnet {
    int       state;
    uint8_t   cmd;
    uint8_t   bin[262];
    size_t    bin_need, bin_have;
    char      text[5][TEXT_MAX + 1];
    unsigned  text_need, text_have;
    size_t    tlen;
    bool      overflow;
    char    **list;
    size_t    nlist, ilist;
    bool      active;           /* session started (REQ-023) */
    char      cwd[VPATH_MAX];
    struct mount drv[FLEX_DRIVES];
    unsigned  consec_timeouts, consec_garbage;
};

struct tape {
    char     load_vpath[VPATH_MAX];      /* relative to tape_root, "" = none */
    char     punch_vpath[VPATH_MAX];
    uint8_t *buf;                        /* transformed load data */
    size_t   len, pos;
    bool     synthetic;
    char     eol_last;
    struct ftimer tick;
    char     sel[TEXT_MAX + 1];
    size_t   sel_len;
    bool     sel_overflow;
    /* punch */
    int      pdir;                       /* parent dir fd */
    int      pfd;
    char     ptmp[VPATH_MAX];
    char     pfinal[VPATH_MAX];          /* base name in pdir */
    char     pvpath[VPATH_MAX];
    bool     prename;
    char     pline[600];
    size_t   plen;
    unsigned pline_no, perrors;
    bool     pterminated;
    uint64_t pbytes;
    struct ftimer ptimer;
};

struct port {
    struct port     *next;
    struct port_cfg  cfg;
    struct log_ctx   log;
    int              link;
    struct evh       ev;            /* data fd (tty or tcp) */
    struct evh       lev;           /* listening socket */
    bool             want_out;
    bool             dead;          /* reap after this loop iteration */
    char             peer[CFG_PATH_MAX + 8];
    /* telnet */
    bool             use_telnet;
    struct telnet    tn;
    uint8_t          tn_will[32], tn_do[32];   /* bitsets of options we sent */
    uint8_t          tn_refused[32];
    bool             tn_comport_sent;
    /* output */
    uint8_t          tx[TXBUF_SIZE];
    size_t           txlen;
    /* roots */
    int              flex_root;
    int              tape_root;
    /* dispatcher */
    int              mode;
    struct flexnet   fn;
    struct tape      tp;
    struct ftimer    xfer_timer;
    struct ftimer    link_timer;    /* reconnect / reopen */
    unsigned         backoff_s;
    uint64_t         last_rx_us;
    uint64_t         last_sync_us;
    struct port_stats st;
};

/* port.c */
struct port *port_new(const struct port_cfg *cfg);
void port_start(struct port *p);
void port_stop(struct port *p);          /* closes link; keeps session */
void port_free(struct port *p);          /* ends session and frees */
void port_restart(struct port *p);
void port_send(struct port *p, const void *buf, size_t n);
void port_send_byte(struct port *p, uint8_t b);
size_t port_pending_tx(struct port *p);
unsigned port_char_us(const struct port *p);
void port_arm_xfer(struct port *p);
bool port_busy(const struct port *p);
const char *port_mode_name(int mode);
const char *port_link_name(int link);

/* flexnet.c */
bool flexnet_is_cmd(uint8_t b);
void flexnet_start(struct port *p, uint8_t cmd);
void flexnet_byte(struct port *p, uint8_t b);
void flexnet_timeout(struct port *p);
void flexnet_abort(struct port *p);              /* drop transaction, keep session */
void flexnet_end(struct port *p, const char *why);
void flexnet_garbage(struct port *p);
int  flexnet_mount(struct port *p, unsigned drv, const char *vpath, bool want_rw,
                   char *rw, const char **why);
void flexnet_umount(struct port *p, unsigned drv);
void flexnet_sync_images(struct port *p);
bool flexnet_image_in_use(dev_t dev, ino_t ino);

/* tape.c */
void tape_init(struct port *p);
void tape_sel_start(struct port *p);
void tape_sel_byte(struct port *p, uint8_t b);
int  tape_select(struct port *p, const char *spec, char *msg, size_t msgsz);
void tape_load_start(struct port *p);
void tape_load_pause(struct port *p);
void tape_punch_start(struct port *p);
void tape_punch_byte(struct port *p, uint8_t b);
void tape_punch_end(struct port *p, const char *why);
void tape_rewind(struct port *p);
void tape_cancel(struct port *p);
void tape_shutdown(struct port *p);
void tape_timeout(struct port *p);

/* main.c — daemon-wide state */
struct port *ports_first(void);
const struct config *daemon_config(void);

#endif
