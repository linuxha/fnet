/* fnetimg — offline FLEX disk image utility (REQ-120..REQ-124) */
#include "flexdsk.h"
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void usage(FILE *f)
{
    fprintf(f,
        "usage: fnetimg create <file> --tracks T --sectors S [--track0 S0] [--label L] [--volume N]\n"
        "       fnetimg info   <file>\n"
        "       fnetimg ls     [-a] <file>\n"
        "       fnetimg check  <file>\n"
        "       fnetimg --version\n");
}

/* Host paths are not confined: open the parent directory and pass the base name. */
static int open_dir_of(const char *path, char *base, size_t basesz)
{
    char a[4096], b[4096];
    int fd;

    str_copy(a, path, sizeof a);
    str_copy(b, path, sizeof b);
    str_copy(base, basename(a), basesz);
    fd = open(dirname(b), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0)
        fprintf(stderr, "fnetimg: %s: %s\n", path, strerror(errno));
    return fd;
}

static flexdsk_t *open_image(const char *path, bool rw)
{
    char base[256];
    const char *why;
    flexdsk_t *d;
    int dfd = open_dir_of(path, base, sizeof base);

    if (dfd < 0)
        return NULL;
    if (flexdsk_open(dfd, base, rw, &d, NULL, &why) < 0) {
        fprintf(stderr, "fnetimg: %s: %s\n", path, why ? why : strerror(errno));
        close(dfd);
        return NULL;
    }
    close(dfd);
    return d;
}

static int need_ulong(const char *opt, const char *v, unsigned long max, unsigned *out)
{
    unsigned long n;
    if (parse_ulong(v, max, &n) < 0) {
        fprintf(stderr, "fnetimg: invalid %s '%s'\n", opt, v);
        return -1;
    }
    *out = (unsigned)n;
    return 0;
}

static int cmd_create(int argc, char **argv)
{
    static const struct option opts[] = {
        { "tracks",  required_argument, NULL, 't' },
        { "sectors", required_argument, NULL, 's' },
        { "track0",  required_argument, NULL, '0' },
        { "label",   required_argument, NULL, 'l' },
        { "volume",  required_argument, NULL, 'v' },
        { NULL, 0, NULL, 0 },
    };
    flex_create_t p = { .volume = 1 };
    const char *why;
    char base[256];
    int c, dfd, rc;

    optind = 1;
    while ((c = getopt_long(argc, argv, "t:s:0:l:v:", opts, NULL)) != -1) {
        switch (c) {
        case 't': if (need_ulong("tracks", optarg, 1000, &p.tracks) < 0) return 2; break;
        case 's': if (need_ulong("sectors", optarg, 1000, &p.spt) < 0) return 2; break;
        case '0': if (need_ulong("track0", optarg, 1000, &p.t0_spt) < 0) return 2; break;
        case 'v': if (need_ulong("volume", optarg, 70000, &p.volume) < 0) return 2; break;
        case 'l': str_copy(p.label, optarg, sizeof p.label); break;
        default:  usage(stderr); return 2;
        }
    }
    if (optind != argc - 1 || !p.tracks || !p.spt) {
        usage(stderr);
        return 2;
    }
    if (!p.label[0]) {
        char tmp[256], *dot;
        str_copy(tmp, argv[optind], sizeof tmp);
        str_copy(p.label, basename(tmp), sizeof p.label);
        if ((dot = strchr(p.label, '.')))
            *dot = '\0';
    }
    if (flexdsk_create_check(&p, &why) < 0) {
        fprintf(stderr, "fnetimg: %s\n", why);
        return 2;
    }
    dfd = open_dir_of(argv[optind], base, sizeof base);
    if (dfd < 0)
        return 1;
    rc = flexdsk_create(dfd, base, &p, &why);
    close(dfd);
    if (rc < 0) {
        fprintf(stderr, "fnetimg: %s: %s\n", argv[optind], why);
        return 1;
    }
    return 0;
}

static int cmd_info(const char *path)
{
    flexdsk_t *d = open_image(path, false);
    flex_sir_t s;

    if (!d)
        return 1;
    if (flexdsk_read_sir(d, &s) < 0) {
        fprintf(stderr, "fnetimg: %s: cannot read SIR\n", path);
        flexdsk_close(d);
        return 1;
    }
    printf("file:          %s\n", path);
    printf("size:          %u sectors (%u bytes)\n", d->g.blocks, d->g.blocks * FLEX_SECTOR_SIZE);
    printf("geometry:      %u tracks, %u sectors/track, track 0 has %u sectors\n",
           d->g.max_trk + 1u, d->g.spt, d->g.t0_spt);
    printf("label:         %s\n", s.label);
    printf("volume:        %u\n", s.volume);
    printf("created:       %02u-%02u-%02u\n", s.month, s.day, s.year);
    printf("free sectors:  %u\n", s.free_sectors);
    printf("first free:    %u/%u\n", s.ff_trk, s.ff_sec);
    printf("last free:     %u/%u\n", s.lf_trk, s.lf_sec);
    flexdsk_close(d);
    return 0;
}

static int print_dirent(const flex_dirent_t *e, void *arg)
{
    (void)arg;
    printf("%-12s %3u/%-3u %3u/%-3u %5u  %s  %02u-%02u-%02u%s\n",
           e->name, e->st, e->ss, e->et, e->es, e->sectors,
           e->random ? "R" : "-", e->month, e->day, e->year, e->deleted ? "  (deleted)" : "");
    return 0;
}

static int cmd_ls(int argc, char **argv)
{
    bool all = false;
    flexdsk_t *d;
    int c, rc;

    optind = 1;
    while ((c = getopt(argc, argv, "a")) != -1) {
        if (c != 'a') {
            usage(stderr);
            return 2;
        }
        all = true;
    }
    if (optind != argc - 1) {
        usage(stderr);
        return 2;
    }
    if (!(d = open_image(argv[optind], false)))
        return 1;
    printf("%-12s %7s %7s %5s  %s  %s\n", "NAME", "START", "END", "SECS", "R", "DATE");
    rc = flexdsk_dir(d, all, print_dirent, NULL);
    flexdsk_close(d);
    return rc < 0 ? 1 : 0;
}

static void print_report(const char *msg, void *arg)
{
    (void)arg;
    printf("%s\n", msg);
}

static int cmd_check(const char *path)
{
    flexdsk_t *d = open_image(path, false);
    int n;

    if (!d)
        return 1;
    n = flexdsk_check(d, print_report, NULL);
    flexdsk_close(d);
    return n == 0 ? 0 : 1;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        usage(stderr);
        return 2;
    }
    if (!strcmp(argv[1], "--version") || !strcmp(argv[1], "-V")) {
        printf("fnetimg %s\n", FNET_VERSION);
        return 0;
    }
    if (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) {
        usage(stdout);
        return 0;
    }
    if (!strcmp(argv[1], "create"))
        return cmd_create(argc - 1, argv + 1);
    if (!strcmp(argv[1], "ls"))
        return cmd_ls(argc - 1, argv + 1);
    if (argc == 3 && !strcmp(argv[1], "info"))
        return cmd_info(argv[2]);
    if (argc == 3 && !strcmp(argv[1], "check"))
        return cmd_check(argv[2]);
    usage(stderr);
    return 2;
}
