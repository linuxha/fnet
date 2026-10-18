/* telnet.c — Telnet / RFC-2217 codec */
#include "telnet.h"

#include <string.h>

enum { S_DATA, S_IAC, S_OPT, S_SB, S_SB_IAC };

void telnet_reset(struct telnet *t)
{
    memset(t, 0, sizeof *t);
}

size_t telnet_rx(struct telnet *t, const uint8_t *in, size_t n, uint8_t *out,
                 const struct telnet_cb *cb)
{
    size_t o = 0;

    for (size_t i = 0; i < n; i++) {
        uint8_t b = in[i];

        switch (t->state) {
        case S_DATA:
            if (b == TN_IAC)
                t->state = S_IAC;
            else
                out[o++] = b;
            break;
        case S_IAC:
            if (b == TN_IAC) {
                out[o++] = TN_IAC;
                t->state = S_DATA;
            } else if (b >= TN_WILL && b <= TN_DONT) {
                t->cmd = b;
                t->state = S_OPT;
            } else if (b == TN_SB) {
                t->sblen = 0;
                t->sb_overflow = false;
                t->state = S_SB;
            } else {
                t->state = S_DATA;      /* NOP, GA, etc. */
            }
            break;
        case S_OPT:
            if (cb && cb->cmd)
                cb->cmd(cb->arg, t->cmd, b);
            t->state = S_DATA;
            break;
        case S_SB:
            if (b == TN_IAC) {
                t->state = S_SB_IAC;
            } else if (t->sblen < sizeof t->sb) {
                t->sb[t->sblen++] = b;
            } else {
                t->sb_overflow = true;
            }
            break;
        case S_SB_IAC:
            if (b == TN_IAC) {
                if (t->sblen < sizeof t->sb)
                    t->sb[t->sblen++] = TN_IAC;
                t->state = S_SB;
            } else {
                if (b == TN_SE && !t->sb_overflow && t->sblen && cb && cb->sb)
                    cb->sb(cb->arg, t->sb, t->sblen);
                t->state = S_DATA;
            }
            break;
        }
    }
    return o;
}

size_t telnet_escape(const uint8_t *in, size_t n, uint8_t *out)
{
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        out[o++] = in[i];
        if (in[i] == TN_IAC)
            out[o++] = TN_IAC;
    }
    return o;
}

size_t telnet_comport_sb(uint8_t *out, uint8_t cmd, const uint8_t *val, size_t vlen)
{
    size_t o = 0;
    out[o++] = TN_IAC;
    out[o++] = TN_SB;
    out[o++] = TNO_COMPORT;
    out[o++] = cmd;
    o += telnet_escape(val, vlen, out + o);
    out[o++] = TN_IAC;
    out[o++] = TN_SE;
    return o;
}
