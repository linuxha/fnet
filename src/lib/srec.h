/* srec.h — Motorola S-Record line checker (REQ-082, REQ-088) */
#ifndef FNET_SREC_H
#define FNET_SREC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum srec_err {
    SREC_OK = 0,
    SREC_EMPTY,        /* blank line (not an error for callers that skip it) */
    SREC_ESYNTAX,      /* not S<digit>, odd length, non-hex */
    SREC_ETYPE,        /* S4 or unknown type */
    SREC_ECOUNT,       /* byte count does not match line length */
    SREC_ECKSUM,       /* checksum mismatch */
};

typedef struct {
    char     type;          /* '0'..'9' */
    unsigned count;         /* byte-count field */
    uint32_t addr;
    unsigned data_len;
    bool     terminator;    /* S7/S8/S9 */
} srec_info_t;

/* line need not be NUL-terminated; trailing CR/LF are ignored. */
enum srec_err srec_check_line(const char *line, size_t len, srec_info_t *out);
const char *srec_strerror(enum srec_err e);

typedef struct {
    unsigned lines, records, errors;
    bool     terminated;
    unsigned first_err_line;
    enum srec_err first_err;
} srec_summary_t;

/* Check a whole buffer. report() is called for each bad line (may be NULL). */
void srec_check_buf(const char *buf, size_t len, srec_summary_t *sum,
                    void (*report)(unsigned line, enum srec_err e, void *arg), void *arg);

#endif
