/* telnet.h — Telnet / RFC-2217 codec (REQ-012..REQ-014, REQ-131) */
#ifndef FNETD_TELNET_H
#define FNETD_TELNET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TN_SE    240
#define TN_NOP   241
#define TN_SB    250
#define TN_WILL  251
#define TN_WONT  252
#define TN_DO    253
#define TN_DONT  254
#define TN_IAC   255

#define TNO_BINARY   0
#define TNO_SGA      3
#define TNO_COMPORT  44

/* RFC-2217 client-to-server commands (server replies with +100) */
#define CPO_SET_BAUDRATE   1
#define CPO_SET_DATASIZE   2
#define CPO_SET_PARITY     3
#define CPO_SET_STOPSIZE   4
#define CPO_SET_CONTROL    5
#define CPO_NOTIFY_LINE    6
#define CPO_NOTIFY_MODEM   7
#define CPO_PURGE_DATA     12
#define CPO_SERVER_OFFSET  100

struct telnet_cb {
    void (*cmd)(void *arg, uint8_t cmd, uint8_t opt);            /* WILL/WONT/DO/DONT */
    void (*sb)(void *arg, const uint8_t *sb, size_t len);        /* option byte first */
    void *arg;
};

struct telnet {
    int     state;
    uint8_t cmd;
    uint8_t sb[64];
    size_t  sblen;
    bool    sb_overflow;
};

void   telnet_reset(struct telnet *t);
/* Decode in[0..n) into out (capacity >= n). Returns data byte count. */
size_t telnet_rx(struct telnet *t, const uint8_t *in, size_t n, uint8_t *out,
                 const struct telnet_cb *cb);
/* Double IACs. out capacity must be >= 2n. Returns bytes written. */
size_t telnet_escape(const uint8_t *in, size_t n, uint8_t *out);
/* Build IAC SB COM-PORT-OPTION cmd val... IAC SE. Returns length (<= 2*vlen+6). */
size_t telnet_comport_sb(uint8_t *out, uint8_t cmd, const uint8_t *val, size_t vlen);

#endif
