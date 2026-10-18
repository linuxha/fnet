/* config.h — fnetd.conf INI parser (REQ-002..REQ-005, §4.2.2) */
#ifndef FNETD_CONFIG_H
#define FNETD_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CFG_NAME_MAX   32
#define CFG_PATH_MAX   256
#define CFG_TAG_MAX    17       /* 16 chars + NUL (REQ-071) */
#define CFG_ALLOW_MAX  16
#define CFG_ALIAS_MAX  32

enum port_type  { PT_TTY, PT_RFC2217, PT_LISTEN };
enum load_eol   { EOL_CRLF, EOL_CR, EOL_LF, EOL_RAW };
enum load_miss  { MISS_S9, MISS_NONE };
enum load_valid { VALID_WARN, VALID_REJECT, VALID_OFF };
enum punch_mode { PUNCH_OVERWRITE, PUNCH_APPEND, PUNCH_VERSION };

struct cidr {
    int     family;             /* AF_INET / AF_INET6 */
    uint8_t addr[16];
    unsigned bits;
};

/*
 * All fields are fixed-size so two configs can be compared with memcmp
 * (REQ-004). Always zero the struct before filling it.
 */
struct port_cfg {
    char     name[CFG_NAME_MAX];
    int      type;
    bool     enabled;
    char     device[CFG_PATH_MAX];
    char     host[CFG_PATH_MAX];
    unsigned port;
    char     bind[64];
    struct cidr allow[CFG_ALLOW_MAX];
    unsigned nallow;
    bool     on_new_replace;
    bool     telnet;
    unsigned baud;
    char     flexnet_root[CFG_PATH_MAX];
    char     tape_root[CFG_PATH_MAX];
    char     default_cpu[CFG_TAG_MAX];
    unsigned xfer_timeout_ms;
    unsigned sync_loss_count;
    unsigned sync_loss_garbage;
    unsigned sync_interval;
    unsigned char_delay_us;
    unsigned line_delay_ms;
    int      load_eol;
    int      load_missing;
    int      load_validate;
    int      punch_mode;
    unsigned punch_timeout;
    bool     read_only;
    unsigned legacy_drive;
    int      log_level;         /* -1 = inherit */
};

struct cpu_alias {
    char tag[CFG_TAG_MAX];
    char dir[CFG_TAG_MAX];
};

struct config {
    char     user[CFG_NAME_MAX];
    int      log_sink;
    char     log_file[CFG_PATH_MAX];
    int      log_level;
    char     stats_file[CFG_PATH_MAX];
    unsigned stats_interval;
    char     ctl_socket[108];
    struct port_cfg  defaults;
    struct cpu_alias alias[CFG_ALIAS_MAX];
    unsigned nalias;
    struct port_cfg *ports;
    unsigned nports;
};

int  config_load(const char *path, struct config *out, char *err, size_t errsz);
void config_free(struct config *c);
void config_defaults(struct config *c);

/* Apply one key=value to a port config (used by the parser and fnetctl). */
int  config_port_kv(struct port_cfg *pc, const char *key, const char *val,
                    char *err, size_t errsz);
/* Check required fields for the port type. */
int  config_port_validate(const struct port_cfg *pc, char *err, size_t errsz);
/* true if the key changes the link (port must be restarted). */
bool config_key_is_link(const char *key);

bool port_cfg_equal(const struct port_cfg *a, const struct port_cfg *b);
bool cidr_match(const struct cidr *c, int family, const void *addr);
int  cidr_parse(const char *s, struct cidr *out);
const char *port_type_name(int t);

#endif
