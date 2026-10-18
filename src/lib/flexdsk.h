/* flexdsk.h — FLEX .DSK image library (REQ-060..REQ-065, REQ-120..REQ-124)
 *
 * Images are raw 256-byte sectors in track-major order. Track 0 may have
 * fewer sectors than the data tracks (DD disk with an SD track 0). The
 * structures follow docs/flexdsk.md; multi-byte fields are big-endian.
 */
#ifndef FNET_FLEXDSK_H
#define FNET_FLEXDSK_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

#define FLEX_SECTOR_SIZE   256
#define FLEX_SIR_BLOCK     2           /* T0S3 */
#define FLEX_SIR_OFFSET    16
#define FLEX_DIR_SECTOR    5
#define FLEX_DIR_ENTRY     24
#define FLEX_DIR_PER_SEC   10
#define FLEX_MAX_TRACKS    256
#define FLEX_MAX_SECTORS   255
#define FLEX_MIN_SECTORS   5
#define FLEX_LABEL_LEN     11
#define FLEX_MAX_IMAGE     ((uint64_t)FLEX_MAX_TRACKS * FLEX_MAX_SECTORS * FLEX_SECTOR_SIZE)

typedef struct {
    uint8_t  max_trk;      /* SIR endTrack: highest track number */
    uint8_t  spt;          /* SIR endSector: sectors per data track */
    uint8_t  t0_spt;       /* sectors on track 0 (derived from size) */
    uint32_t blocks;       /* total 256-byte blocks */
} flex_geom_t;

typedef struct {
    char     label[FLEX_LABEL_LEN + 1];
    unsigned volume;
    uint8_t  ff_trk, ff_sec;    /* first free */
    uint8_t  lf_trk, lf_sec;    /* last free */
    unsigned free_sectors;
    uint8_t  month, day, year;
    uint8_t  end_trk, end_sec;
} flex_sir_t;

typedef struct {
    char     name[13];          /* "NAME.EXT" */
    uint8_t  st, ss, et, es;
    unsigned sectors;
    uint8_t  random;
    uint8_t  month, day, year;
    bool     deleted;
} flex_dirent_t;

typedef struct flexdsk {
    int         fd;
    flex_geom_t g;
    bool        rw;
    bool        dirty;
    dev_t       dev;
    ino_t       ino;
} flexdsk_t;

typedef struct {
    unsigned tracks;            /* number of tracks including track 0 (2..256) */
    unsigned spt;               /* sectors per data track (5..255) */
    unsigned t0_spt;            /* 0 = same as spt */
    char     label[FLEX_LABEL_LEN + 1];
    unsigned volume;
    unsigned month, day, year;  /* 0 = today */
} flex_create_t;

/* flexdsk_open() result flags */
#define FLEXDSK_RO_FILE    0x1  /* file not writable */
#define FLEXDSK_RO_LOCKED  0x2  /* another writer holds the lock (REQ-045) */

uint16_t flex_cksum(const uint8_t buf[FLEX_SECTOR_SIZE]);

int  flexdsk_geom(const uint8_t sir[FLEX_SECTOR_SIZE], uint64_t size,
                  flex_geom_t *g, const char **why);
int  flexdsk_ts2blk(const flex_geom_t *g, unsigned trk, unsigned sec, uint32_t *blk);

/* dirfd/rel are resolved with open_beneath(). Returns 0 or -1 (errno, *why). */
int  flexdsk_open(int dirfd, const char *rel, bool want_rw, flexdsk_t **out,
                  unsigned *flags, const char **why);
int  flexdsk_read(flexdsk_t *d, unsigned trk, unsigned sec, uint8_t buf[FLEX_SECTOR_SIZE]);
int  flexdsk_write(flexdsk_t *d, unsigned trk, unsigned sec, const uint8_t buf[FLEX_SECTOR_SIZE]);
int  flexdsk_sync(flexdsk_t *d);
void flexdsk_close(flexdsk_t *d);

int  flexdsk_create_check(const flex_create_t *p, const char **why);
int  flexdsk_create(int dirfd, const char *rel, const flex_create_t *p, const char **why);

int  flexdsk_read_sir(flexdsk_t *d, flex_sir_t *sir);
typedef int (*flex_dir_cb)(const flex_dirent_t *e, void *arg);
int  flexdsk_dir(flexdsk_t *d, bool include_deleted, flex_dir_cb cb, void *arg);

typedef void (*flex_report_cb)(const char *msg, void *arg);
/* Returns number of errors found (0 = clean), -1 on I/O failure. */
int  flexdsk_check(flexdsk_t *d, flex_report_cb report, void *arg);

#endif
