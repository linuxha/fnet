/* test_lib.c — libfnet unit tests (TEST-033, TEST-050, TEST-052, TEST-064, TEST-080, TEST-081) */
#include "t.h"

#include "flexdsk.h"
#include "srec.h"
#include "vpath.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

static char tmpdir[256];
static int tmpfd;

static void setup(void)
{
    snprintf(tmpdir, sizeof tmpdir, "/tmp/fnet-unit-XXXXXX");
    if (!mkdtemp(tmpdir)) {
        perror("mkdtemp");
        exit(2);
    }
    tmpfd = open(tmpdir, O_RDONLY | O_DIRECTORY);
}

static void teardown(void)
{
    char cmd[300];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", tmpdir);
    if (system(cmd) != 0)
        fprintf(stderr, "cleanup failed\n");
}

static void mk(const char *name, unsigned tracks, unsigned spt, unsigned t0)
{
    flex_create_t p = { .tracks = tracks, .spt = spt, .t0_spt = t0, .volume = 7 };
    const char *why = NULL;
    snprintf(p.label, sizeof p.label, "UNIT");
    if (flexdsk_create(tmpfd, name, &p, &why) < 0)
        fprintf(stderr, "create %s: %s\n", name, why);
}

static void test_cksum(void)
{
    uint8_t b[256];
    memset(b, 0, sizeof b);
    CHECK_EQ(flex_cksum(b), 0);
    memset(b, 0xff, sizeof b);
    CHECK_EQ(flex_cksum(b), 0xff00);       /* 256 * 255 = 65280 */
    b[0] = 0x01;
    CHECK_EQ(flex_cksum(b), 0xfe02);
}

/* TEST-033: DD image with SD track 0 */
static void test_ts2blk(void)
{
    flex_geom_t g = { .max_trk = 79, .spt = 18, .t0_spt = 10 };
    uint32_t b = 999;

    CHECK_EQ(flexdsk_ts2blk(&g, 0, 10, &b), 0);  CHECK_EQ(b, 9);
    CHECK_EQ(flexdsk_ts2blk(&g, 0, 11, &b), -1);
    CHECK_EQ(flexdsk_ts2blk(&g, 1, 1, &b), 0);   CHECK_EQ(b, 10);
    CHECK_EQ(flexdsk_ts2blk(&g, 0, 0, &b), 0);   CHECK_EQ(b, 0);
    CHECK_EQ(flexdsk_ts2blk(&g, 1, 0, &b), -1);
    CHECK_EQ(flexdsk_ts2blk(&g, 1, 19, &b), -1);
    CHECK_EQ(flexdsk_ts2blk(&g, 80, 1, &b), -1);
    CHECK_EQ(flexdsk_ts2blk(&g, 79, 18, &b), 0); CHECK_EQ(b, 10 + 78 * 18 + 17);
}

/* TEST-050 + TEST-080 */
static void test_geometries(void)
{
    static const unsigned geo[][3] = {
        { 35, 10, 0 }, { 77, 26, 0 }, { 80, 36, 20 }, { 256, 255, 0 },
    };
    for (size_t i = 0; i < sizeof geo / sizeof geo[0]; i++) {
        char name[32];
        flexdsk_t *d = NULL;
        flex_sir_t s;
        unsigned t0 = geo[i][2] ? geo[i][2] : geo[i][1];

        snprintf(name, sizeof name, "g%zu.dsk", i);
        mk(name, geo[i][0], geo[i][1], geo[i][2]);
        CHECK_EQ(flexdsk_open(tmpfd, name, true, &d, NULL, NULL), 0);
        if (!d)
            continue;
        CHECK_EQ(d->g.max_trk, geo[i][0] - 1);
        CHECK_EQ(d->g.spt, geo[i][1]);
        CHECK_EQ(d->g.t0_spt, t0);
        CHECK_EQ(d->g.blocks, t0 + (geo[i][0] - 1) * geo[i][1]);
        CHECK_EQ(flexdsk_read_sir(d, &s), 0);
        CHECK_EQ(s.free_sectors, (geo[i][0] - 1) * geo[i][1]);
        CHECK_STR(s.label, "UNIT");
        CHECK_EQ(s.volume, 7);
        CHECK_EQ(flexdsk_check(d, NULL, NULL), 0);
        flexdsk_close(d);
    }
    {
        struct stat st;
        CHECK_EQ(fstatat(tmpfd, "g3.dsk", &st, 0), 0);
        CHECK_EQ(st.st_size, 16711680);
    }
}

static void test_bad_images(void)
{
    flexdsk_t *d = NULL;
    const char *why = NULL;
    uint8_t z[256];
    char path[512];
    int fd;

    /* truncated: track 0 would have only 4 sectors */
    mk("trunc.dsk", 40, 18, 0);
    snprintf(path, sizeof path, "%s/trunc.dsk", tmpdir);
    CHECK_EQ(truncate(path, (39 * 18 + 4) * 256), 0);
    CHECK_EQ(flexdsk_open(tmpfd, "trunc.dsk", false, &d, NULL, &why), -1);
    CHECK(why != NULL);

    /* bad SIR: all zeros */
    memset(z, 0, sizeof z);
    fd = openat(tmpfd, "zero.dsk", O_CREAT | O_WRONLY, 0644);
    for (int i = 0; i < 100; i++)
        CHECK_EQ(write(fd, z, sizeof z), 256);
    close(fd);
    CHECK_EQ(flexdsk_open(tmpfd, "zero.dsk", false, &d, NULL, &why), -1);

    /* not multiple of 256 */
    fd = openat(tmpfd, "odd.dsk", O_CREAT | O_WRONLY, 0644);
    CHECK_EQ(write(fd, z, 100), 100);
    close(fd);
    CHECK_EQ(flexdsk_open(tmpfd, "odd.dsk", false, &d, NULL, &why), -1);
}

static void test_create_limits(void)
{
    flex_create_t p = { .tracks = 300, .spt = 18 };
    const char *why;
    CHECK_EQ(flexdsk_create_check(&p, &why), -1);
    p.tracks = 1;
    CHECK_EQ(flexdsk_create_check(&p, &why), -1);
    p.tracks = 2; p.spt = 4;
    CHECK_EQ(flexdsk_create_check(&p, &why), -1);
    p.spt = 256;
    CHECK_EQ(flexdsk_create_check(&p, &why), -1);
    p.spt = 18; p.volume = 65536;
    CHECK_EQ(flexdsk_create_check(&p, &why), -1);
    p.volume = 65535;
    CHECK_EQ(flexdsk_create_check(&p, &why), 0);
    /* existing file */
    mk("exists.dsk", 10, 10, 0);
    p.tracks = 10; p.spt = 10;
    CHECK_EQ(flexdsk_create(tmpfd, "exists.dsk", &p, &why), -1);
}

static void test_rw_and_lock(void)
{
    uint8_t w[256], r[256];
    flexdsk_t *a = NULL, *b = NULL;
    unsigned flags = 0;

    mk("rw.dsk", 40, 18, 0);
    CHECK_EQ(flexdsk_open(tmpfd, "rw.dsk", true, &a, &flags, NULL), 0);
    CHECK(a && a->rw);
    for (int i = 0; i < 256; i++)
        w[i] = (uint8_t)(i ^ 0x5a);
    CHECK_EQ(flexdsk_write(a, 5, 3, w), 0);
    CHECK_EQ(flexdsk_read(a, 5, 3, r), 0);
    CHECK(memcmp(w, r, 256) == 0);
    CHECK_EQ(flexdsk_write(a, 50, 1, w), -1);
    CHECK_EQ(errno, EINVAL);

    /* REQ-045: second writer downgraded to read-only */
    CHECK_EQ(flexdsk_open(tmpfd, "rw.dsk", true, &b, &flags, NULL), 0);
    CHECK(b && !b->rw);
    CHECK(flags & FLEXDSK_RO_LOCKED);
    CHECK_EQ(flexdsk_write(b, 5, 3, w), -1);
    CHECK_EQ(errno, EROFS);
    flexdsk_close(b);
    flexdsk_close(a);

    /* after the writer closes, a new writer gets the lock */
    CHECK_EQ(flexdsk_open(tmpfd, "rw.dsk", true, &a, &flags, NULL), 0);
    CHECK(a && a->rw);
    flexdsk_close(a);

    /* read-only file */
    CHECK_EQ(fchmodat(tmpfd, "rw.dsk", 0444, 0), 0);
    if (geteuid() != 0) {
        CHECK_EQ(flexdsk_open(tmpfd, "rw.dsk", true, &a, &flags, NULL), 0);
        CHECK(a && !a->rw);
        CHECK(flags & FLEXDSK_RO_FILE);
        flexdsk_close(a);
    }
}

static void put_entry(uint8_t *sec, int slot, const char *name, const char *ext,
                      uint8_t st, uint8_t ss, uint8_t et, uint8_t es, unsigned n)
{
    uint8_t *e = sec + 16 + slot * 24;
    memset(e, 0, 24);
    memcpy(e, name, strlen(name));
    memcpy(e + 8, ext, strlen(ext));
    e[13] = st; e[14] = ss; e[15] = et; e[16] = es;
    e[17] = (uint8_t)(n >> 8); e[18] = (uint8_t)n;
    e[21] = 1; e[22] = 2; e[23] = 83;
}

/* TEST-081: listing and corruption detection */
static void test_dir_and_check(void)
{
    uint8_t sec[256], sir[256];
    flexdsk_t *d = NULL;

    mk("files.dsk", 10, 10, 0);
    CHECK_EQ(flexdsk_open(tmpfd, "files.dsk", true, &d, NULL, NULL), 0);
    if (!d)
        return;
    /* allocate 1/1..1/3 to FOO.TXT: move free chain start to 1/4 */
    flexdsk_read(d, 0, 3, sir);
    sir[16 + 13] = 1; sir[16 + 14] = 4;
    sir[16 + 17] = 0; sir[16 + 18] = 90 - 3;
    flexdsk_write(d, 0, 3, sir);
    flexdsk_read(d, 1, 3, sec);
    sec[0] = 0; sec[1] = 0;
    flexdsk_write(d, 1, 3, sec);
    flexdsk_read(d, 0, 5, sec);
    put_entry(sec, 0, "FOO", "TXT", 1, 1, 1, 3, 3);
    put_entry(sec, 1, "DEL", "CMD", 0, 0, 0, 0, 0);
    sec[16 + 24] |= 0x80;          /* deleted */
    flexdsk_write(d, 0, 5, sec);
    CHECK_EQ(flexdsk_check(d, NULL, NULL), 0);

    /* wrong free count */
    sir[16 + 18] = 50;
    flexdsk_write(d, 0, 3, sir);
    CHECK(flexdsk_check(d, NULL, NULL) > 0);
    sir[16 + 18] = 87;
    flexdsk_write(d, 0, 3, sir);

    /* cross-link: FOO's last sector points into the free chain */
    flexdsk_read(d, 1, 3, sec);
    sec[0] = 1; sec[1] = 5;
    flexdsk_write(d, 1, 3, sec);
    CHECK(flexdsk_check(d, NULL, NULL) > 0);
    sec[0] = 0; sec[1] = 0;
    flexdsk_write(d, 1, 3, sec);
    CHECK_EQ(flexdsk_check(d, NULL, NULL), 0);

    /* cycle: 1/2 -> 1/1 */
    flexdsk_read(d, 1, 2, sec);
    sec[0] = 1; sec[1] = 1;
    flexdsk_write(d, 1, 2, sec);
    CHECK(flexdsk_check(d, NULL, NULL) > 0);
    flexdsk_close(d);
}

struct names { int n; char buf[4][13]; };

static int collect(const flex_dirent_t *e, void *arg)
{
    struct names *nm = arg;
    if (nm->n < 4)
        snprintf(nm->buf[nm->n], 13, "%s", e->name);
    nm->n++;
    return 0;
}

static void test_dir_listing(void)
{
    flexdsk_t *d = NULL;
    struct names nm = { 0 };

    CHECK_EQ(flexdsk_open(tmpfd, "files.dsk", false, &d, NULL, NULL), 0);
    if (!d)
        return;
    flexdsk_dir(d, false, collect, &nm);
    CHECK_EQ(nm.n, 1);
    CHECK_STR(nm.buf[0], "FOO.TXT");
    memset(&nm, 0, sizeof nm);
    flexdsk_dir(d, true, collect, &nm);
    CHECK_EQ(nm.n, 2);
    flexdsk_close(d);
}

static void test_vpath(void)
{
    char o[VPATH_MAX];

    CHECK_EQ(vpath_join("/", "games", o, sizeof o), 0);       CHECK_STR(o, "/games");
    CHECK_EQ(vpath_join("/games", "..", o, sizeof o), 0);     CHECK_STR(o, "/");
    CHECK_EQ(vpath_join("/", "..", o, sizeof o), -1);
    CHECK_EQ(vpath_join("/a/b", "../../..", o, sizeof o), -1);
    CHECK_EQ(vpath_join("/a/b", "/etc", o, sizeof o), 0);     CHECK_STR(o, "/etc");
    CHECK_EQ(vpath_join("/a", "./b//c/", o, sizeof o), 0);    CHECK_STR(o, "/a/b/c");
    CHECK_STR(vpath_rel("/"), ".");
    CHECK_STR(vpath_rel("/a/b"), "a/b");
    CHECK_STR(vpath_base("/a/b.dsk"), "b.dsk");
}

static void test_beneath(void)
{
    int fd;
    char path[512];

    CHECK_EQ(mkdirat(tmpfd, "root", 0755), 0);
    CHECK_EQ(mkdirat(tmpfd, "root/sub", 0755), 0);
    CHECK_EQ(symlinkat("/etc", tmpfd, "root/abs"), 0);
    CHECK_EQ(symlinkat("../..", tmpfd, "root/up"), 0);
    CHECK_EQ(symlinkat("sub", tmpfd, "root/inside"), 0);
    snprintf(path, sizeof path, "%s/root", tmpdir);
    int root = open(path, O_RDONLY | O_DIRECTORY);
    CHECK((fd = open_beneath(root, "sub", O_RDONLY | O_DIRECTORY, 0)) >= 0); close(fd);
    CHECK((fd = open_beneath(root, "inside", O_RDONLY | O_DIRECTORY, 0)) >= 0); close(fd);
    CHECK_EQ(open_beneath(root, "abs", O_RDONLY | O_DIRECTORY, 0), -1);
    CHECK_EQ(open_beneath(root, "up", O_RDONLY | O_DIRECTORY, 0), -1);
    CHECK_EQ(open_beneath(root, "../", O_RDONLY | O_DIRECTORY, 0), -1);
    CHECK_EQ(open_beneath(root, "/etc", O_RDONLY | O_DIRECTORY, 0), -1);
    close(root);
}

/* TEST-064 basis */
static void test_srec(void)
{
    srec_info_t i;
    srec_summary_t s;
    const char good[] = "S00600004844521B\r\nS1130000285F245F2212226A000424290008237C2A\nS9030000FC\n";
    const char bad[]  = "S1130000285F245F2212226A000424290008237C2A\n\nS1130000285F245F2212226A000424290008237C2B\nS9030000FC\n";

    CHECK_EQ(srec_check_line("S9030000FC", 10, &i), SREC_OK);
    CHECK(i.terminator);
    CHECK_EQ(srec_check_line("S9030000FD", 10, &i), SREC_ECKSUM);
    CHECK_EQ(srec_check_line("S9040000FC", 10, &i), SREC_ECOUNT);
    CHECK_EQ(srec_check_line("S9030000FG", 10, &i), SREC_ESYNTAX);
    CHECK_EQ(srec_check_line("S4030000FC", 10, &i), SREC_ETYPE);
    CHECK_EQ(srec_check_line("\r\n", 2, &i), SREC_EMPTY);
    CHECK_EQ(srec_check_line("S1130000285F245F2212226A000424290008237C2A\r", 43, &i), SREC_OK);
    CHECK_EQ(i.addr, 0);
    CHECK_EQ(i.data_len, 16);
    CHECK_EQ(srec_check_line("S20801000000010203F0", 20, &i), SREC_OK);
    CHECK_EQ(i.addr, 0x010000);

    srec_check_buf(good, sizeof good - 1, &s, NULL, NULL);
    CHECK_EQ(s.records, 3);
    CHECK_EQ(s.errors, 0);
    CHECK(s.terminated);

    srec_check_buf(bad, sizeof bad - 1, &s, NULL, NULL);
    CHECK_EQ(s.errors, 1);
    CHECK_EQ(s.first_err_line, 3);
    CHECK_EQ(s.first_err, SREC_ECKSUM);
}

int main(void)
{
    setup();
    RUN(test_cksum);
    RUN(test_ts2blk);
    RUN(test_geometries);
    RUN(test_bad_images);
    RUN(test_create_limits);
    RUN(test_rw_and_lock);
    RUN(test_dir_and_check);
    RUN(test_dir_listing);
    RUN(test_vpath);
    RUN(test_beneath);
    RUN(test_srec);
    teardown();
    T_DONE();
}
