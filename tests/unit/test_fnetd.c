/* test_fnetd.c — unit tests for fnetd modules (telnet codec, config, CIDR) */
#include "t.h"

#include "config.h"
#include "json.h"
#include "log.h"
#include "telnet.h"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

static uint8_t cmds[16][2];
static int ncmds;
static uint8_t sb[64];
static size_t sblen;

static void on_cmd(void *arg, uint8_t cmd, uint8_t opt)
{
    (void)arg;
    if (ncmds < 16) {
        cmds[ncmds][0] = cmd;
        cmds[ncmds][1] = opt;
        ncmds++;
    }
}

static void on_sb(void *arg, const uint8_t *b, size_t n)
{
    (void)arg;
    memcpy(sb, b, n);
    sblen = n;
}

static void test_telnet_rx(void)
{
    const struct telnet_cb cb = { on_cmd, on_sb, NULL };
    struct telnet t;
    uint8_t out[64];
    const uint8_t in[] = {
        'A', 0xff, 0xff, 'B', 0xff, 0xfb, 0x00, 'C', 0xff, 0xf1, 'D',
        0xff, 0xfa, 0x2c, 0x65, 0xff, 0xff, 0x01, 0xff, 0xf0, 'E',
    };
    size_t n;

    telnet_reset(&t);
    ncmds = 0;
    sblen = 0;
    n = telnet_rx(&t, in, sizeof in, out, &cb);
    CHECK_EQ(n, 6);
    CHECK(memcmp(out, "A\xff" "BCDE", 6) == 0);
    CHECK_EQ(ncmds, 1);
    CHECK_EQ(cmds[0][0], TN_WILL);
    CHECK_EQ(cmds[0][1], 0);
    CHECK_EQ(sblen, 4);
    CHECK(memcmp(sb, "\x2c\x65\xff\x01", 4) == 0);

    /* split across calls */
    telnet_reset(&t);
    n = telnet_rx(&t, (const uint8_t *)"\xff", 1, out, &cb);
    CHECK_EQ(n, 0);
    n = telnet_rx(&t, (const uint8_t *)"\xffZ", 2, out, &cb);
    CHECK_EQ(n, 2);
    CHECK_EQ(out[0], 0xff);
    CHECK_EQ(out[1], 'Z');
}

static void test_telnet_tx(void)
{
    uint8_t out[32];
    const uint8_t v[4] = { 0, 0, 0x96, 0x00 };
    size_t n = telnet_escape((const uint8_t *)"\x01\xff\x02", 3, out);

    CHECK_EQ(n, 4);
    CHECK(memcmp(out, "\x01\xff\xff\x02", 4) == 0);
    n = telnet_comport_sb(out, CPO_SET_BAUDRATE, v, 4);
    CHECK_EQ(n, 10);
    CHECK(memcmp(out, "\xff\xfa\x2c\x01\x00\x00\x96\x00\xff\xf0", 10) == 0);
}

static void test_cidr(void)
{
    struct cidr c;
    uint8_t a[16];

    CHECK_EQ(cidr_parse("192.168.1.0/24", &c), 0);
    inet_pton(AF_INET, "192.168.1.77", a);
    CHECK(cidr_match(&c, AF_INET, a));
    inet_pton(AF_INET, "192.168.2.1", a);
    CHECK(!cidr_match(&c, AF_INET, a));
    inet_pton(AF_INET6, "::ffff:192.168.1.5", a);
    CHECK(cidr_match(&c, AF_INET6, a));
    CHECK_EQ(cidr_parse("10.0.0.1", &c), 0);
    CHECK_EQ(c.bits, 32);
    CHECK_EQ(cidr_parse("fe80::/10", &c), 0);
    inet_pton(AF_INET6, "fe80::1", a);
    CHECK(cidr_match(&c, AF_INET6, a));
    CHECK_EQ(cidr_parse("10.0.0.1/33", &c), -1);
    CHECK_EQ(cidr_parse("bogus", &c), -1);
}

static int write_conf(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    if (!f)
        return -1;
    fputs(text, f);
    fclose(f);
    return 0;
}

static void test_config(void)
{
    char path[] = "/tmp/fnet-conf-XXXXXX", err[512];
    struct config c;
    int fd = mkstemp(path);

    close(fd);
    write_conf(path,
        "[daemon]\nlog_level = debug ; comment\n"
        "[defaults]\nflexnet_root = /srv/f\ntape_root = /srv/t\nlegacy_drive = 2\n"
        "[tape.cpu]\n6811 = 68HC11\n"
        "[port.a]\ntype = tty\ndevice = /dev/ttyS0\nbaud = 9600\n"
        "[port.b]\ntype = listen\nbind = 127.0.0.1\nport = 7000\nallow = 127.0.0.1, 10.0.0.0/8\n"
        "on_new = refuse\ntelnet = no\nlegacy_drive = 1\n");
    CHECK_EQ(config_load(path, &c, err, sizeof err), 0);
    CHECK_EQ(c.nports, 2);
    CHECK_EQ(c.log_level, FL_DEBUG);
    CHECK_EQ(c.nalias, 1);
    CHECK_STR(c.alias[0].dir, "68HC11");
    CHECK_STR(c.ports[0].flexnet_root, "/srv/f");
    CHECK_EQ(c.ports[0].legacy_drive, 2);
    CHECK_EQ(c.ports[1].legacy_drive, 1);
    CHECK_EQ(c.ports[1].nallow, 2);
    CHECK(!c.ports[1].on_new_replace);
    CHECK(!c.ports[1].telnet);
    config_free(&c);

    write_conf(path, "[daemon]\n\n\nbogus\n");
    CHECK_EQ(config_load(path, &c, err, sizeof err), -1);
    CHECK(strstr(err, ":4:") != NULL);

    write_conf(path, "[port.x]\ntype = tty\nbaud = 76800\ndevice = /dev/x\n");
    CHECK_EQ(config_load(path, &c, err, sizeof err), -1);
    CHECK(strstr(err, "unsupported tty baud") != NULL);

    write_conf(path, "[port.x]\ntype = listen\nport = 1\n");
    CHECK_EQ(config_load(path, &c, err, sizeof err), -1);
    CHECK(strstr(err, "bind") != NULL);

    write_conf(path, "[port.x]\ntype = tty\ndevice=/d\n[defaults]\nbaud=9600\n");
    CHECK_EQ(config_load(path, &c, err, sizeof err), -1);

    write_conf(path, "[port.x]\ntype = tty\ndevice=/d\n[port.x]\n");
    CHECK_EQ(config_load(path, &c, err, sizeof err), -1);
    CHECK(strstr(err, "duplicate") != NULL);

    write_conf(path, "[port.x]\ntype = tty\ndevice=/d\ndefault_cpu = 68$K\n");
    CHECK_EQ(config_load(path, &c, err, sizeof err), -1);
    unlink(path);
}

static void test_json(void)
{
    struct jpair kv[8];
    struct jb j;
    char s[64];
    int n;

    jb_init(&j);
    jb_obj(&j);
    jb_kstr(&j, "a", "x\"y\n");
    jb_key(&j, "arr");
    jb_arr(&j);
    jb_u64(&j, 1);
    jb_obj(&j);
    jb_kbool(&j, "t", true);
    jb_obj_end(&j);
    jb_arr_end(&j);
    jb_ki64(&j, "n", -5);
    jb_obj_end(&j);
    CHECK_STR(j.s, "{\"a\":\"x\\\"y\\n\",\"arr\":[1,{\"t\":true}],\"n\":-5}");
    CHECK_EQ(json_find_str(j.s, "a", s, sizeof s), 0);
    CHECK_STR(s, "x\"y\n");
    jb_free(&j);

    n = json_parse_flat("{\"cmd\":\"mount\", \"drv\": 2, \"ro\": true, \"x\": null}", kv, 8);
    CHECK_EQ(n, 4);
    CHECK_STR(jp_get(kv, n, "cmd"), "mount");
    CHECK_EQ(jp_long(kv, n, "drv", -1), 2);
    CHECK(jp_bool(kv, n, "ro"));
    CHECK_EQ(json_parse_flat("{\"a\":{}}", kv, 8), -1);
    CHECK_EQ(json_parse_until("{\"ok\":true,\"data\":{\"x\":[1]}}", kv, 8, "data"), 1);
    CHECK_EQ(json_find_str("{\"data\":{\"text\":\"no\"},\"text\":\"yes\"}", "text", s, sizeof s), 0);
    CHECK_STR(s, "yes");
}

int main(void)
{
    RUN(test_telnet_rx);
    RUN(test_telnet_tx);
    RUN(test_cidr);
    RUN(test_config);
    RUN(test_json);
    T_DONE();
}
