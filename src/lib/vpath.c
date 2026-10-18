/* vpath.c — virtual paths confined to a service root (REQ-090) */
#include "vpath.h"
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/openat2.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

int vpath_join(const char *cwd, const char *arg, char *out, size_t outsz)
{
    char tmp[2 * VPATH_MAX + 2];
    char *comp[VPATH_MAX];
    size_t ncomp = 0, len;
    char *save = NULL, *tok;

    if (strlen(cwd) >= VPATH_MAX || strlen(arg) >= VPATH_MAX)
        return -1;
    if (arg[0] == '/')
        snprintf(tmp, sizeof tmp, "%s", arg);
    else
        snprintf(tmp, sizeof tmp, "%s/%s", cwd, arg);

    for (tok = strtok_r(tmp, "/", &save); tok; tok = strtok_r(NULL, "/", &save)) {
        if (!strcmp(tok, "."))
            continue;
        if (!strcmp(tok, "..")) {
            if (ncomp == 0)
                return -1;          /* escape above root */
            ncomp--;
            continue;
        }
        if (ncomp == ARRAY_LEN(comp))
            return -1;
        comp[ncomp++] = tok;
    }

    if (outsz < 2)
        return -1;
    if (ncomp == 0) {
        out[0] = '/';
        out[1] = '\0';
        return 0;
    }
    len = 0;
    for (size_t i = 0; i < ncomp; i++) {
        size_t cl = strlen(comp[i]);
        if (len + 1 + cl + 1 > outsz || len + 1 + cl >= VPATH_MAX)
            return -1;
        out[len++] = '/';
        memcpy(out + len, comp[i], cl);
        len += cl;
    }
    out[len] = '\0';
    return 0;
}

const char *vpath_rel(const char *vpath)
{
    while (*vpath == '/')
        vpath++;
    return *vpath ? vpath : ".";
}

const char *vpath_base(const char *vpath)
{
    const char *s = strrchr(vpath, '/');
    return s ? s + 1 : vpath;
}

int open_beneath(int rootfd, const char *rel, int flags, mode_t mode)
{
    struct open_how how;

    memset(&how, 0, sizeof how);
    how.flags = (uint64_t)(unsigned)(flags | O_CLOEXEC);
    if (flags & (O_CREAT | O_TMPFILE))
        how.mode = mode;
    how.resolve = RESOLVE_BENEATH | RESOLVE_NO_MAGICLINKS;
    return (int)syscall(SYS_openat2, rootfd, rel, &how, sizeof how);
}

int open_parent_beneath(int rootfd, const char *vpath, char *base, size_t basesz)
{
    char parent[VPATH_MAX];
    const char *b = vpath_base(vpath);
    size_t plen = (size_t)(b - vpath);

    if (!*b || plen >= sizeof parent) {
        errno = EINVAL;
        return -1;
    }
    memcpy(parent, vpath, plen);
    parent[plen] = '\0';
    if (str_copy(base, b, basesz) >= basesz) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return open_beneath(rootfd, vpath_rel(parent), O_RDONLY | O_DIRECTORY, 0);
}
