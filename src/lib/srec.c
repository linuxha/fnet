/* srec.c — Motorola S-Record line checker */
#include "srec.h"

#include <string.h>

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static int hexbyte(const char *p)
{
    int h = hexval(p[0]), l = hexval(p[1]);
    return (h < 0 || l < 0) ? -1 : (h << 4 | l);
}

enum srec_err srec_check_line(const char *line, size_t len, srec_info_t *out)
{
    srec_info_t info;
    unsigned addr_len, sum;
    int v;

    while (len && (line[len - 1] == '\r' || line[len - 1] == '\n' ||
                   line[len - 1] == ' ' || line[len - 1] == '\t'))
        len--;
    if (len == 0)
        return SREC_EMPTY;
    if (len < 4 || (line[0] != 'S' && line[0] != 's') || line[1] < '0' || line[1] > '9')
        return SREC_ESYNTAX;
    if (len % 2)
        return SREC_ESYNTAX;

    memset(&info, 0, sizeof info);
    info.type = line[1];
    switch (info.type) {
    case '0': case '1': case '5': case '9': addr_len = 2; break;
    case '2': case '6': case '8':           addr_len = 3; break;
    case '3': case '7':                     addr_len = 4; break;
    default:                                return SREC_ETYPE;
    }
    info.terminator = info.type >= '7';

    if ((v = hexbyte(line + 2)) < 0)
        return SREC_ESYNTAX;
    info.count = (unsigned)v;
    if ((len - 4) / 2 != info.count)
        return SREC_ECOUNT;
    if (info.count < addr_len + 1)
        return SREC_ECOUNT;

    sum = info.count;
    for (unsigned i = 0; i < info.count; i++) {
        if ((v = hexbyte(line + 4 + 2 * i)) < 0)
            return SREC_ESYNTAX;
        if (i < addr_len)
            info.addr = info.addr << 8 | (uint32_t)v;
        if (i + 1 < info.count)
            sum += (unsigned)v;
        else if ((uint8_t)~sum != (uint8_t)v)
            return SREC_ECKSUM;
    }
    info.data_len = info.count - addr_len - 1;
    if (out)
        *out = info;
    return SREC_OK;
}

const char *srec_strerror(enum srec_err e)
{
    switch (e) {
    case SREC_OK:      return "ok";
    case SREC_EMPTY:   return "empty line";
    case SREC_ESYNTAX: return "syntax error";
    case SREC_ETYPE:   return "unknown record type";
    case SREC_ECOUNT:  return "byte count mismatch";
    case SREC_ECKSUM:  return "checksum error";
    }
    return "?";
}

void srec_check_buf(const char *buf, size_t len, srec_summary_t *sum,
                    void (*report)(unsigned line, enum srec_err e, void *arg), void *arg)
{
    size_t start = 0;

    memset(sum, 0, sizeof *sum);
    while (start < len) {
        size_t end = start;
        srec_info_t info;
        enum srec_err e;

        while (end < len && buf[end] != '\n' && buf[end] != '\r')
            end++;
        sum->lines++;
        e = srec_check_line(buf + start, end - start, &info);
        if (e == SREC_OK) {
            sum->records++;
            if (info.terminator)
                sum->terminated = true;
        } else if (e != SREC_EMPTY) {
            if (!sum->errors) {
                sum->first_err_line = sum->lines;
                sum->first_err = e;
            }
            sum->errors++;
            if (report)
                report(sum->lines, e, arg);
        }
        /* consume one line ending (CR, LF or CRLF) */
        if (end < len && buf[end] == '\r')
            end++;
        if (end < len && buf[end] == '\n')
            end++;
        start = end;
    }
}
