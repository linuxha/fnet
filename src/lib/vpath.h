/* vpath.h — virtual paths confined to a service root (REQ-090) */
#ifndef FNET_VPATH_H
#define FNET_VPATH_H

#include <stddef.h>
#include <sys/types.h>

#define VPATH_MAX 256

/*
 * Join a virtual cwd ("/", "/a/b") with a client argument. An argument
 * starting with '/' is relative to the virtual root. "." and ".." are
 * resolved lexically; ".." above the root is an error.
 * Result always starts with '/'. Returns 0 or -1.
 */
int vpath_join(const char *cwd, const char *arg, char *out, size_t outsz);

/* "/" -> ".", "/a/b" -> "a/b" (pointer into vpath). */
const char *vpath_rel(const char *vpath);

/* Last component of a vpath ("" for "/"). */
const char *vpath_base(const char *vpath);

/* openat2(rootfd, rel, RESOLVE_BENEATH | RESOLVE_NO_MAGICLINKS). */
int open_beneath(int rootfd, const char *rel, int flags, mode_t mode);

/*
 * Open the parent directory of vpath beneath rootfd and copy the final
 * component into base. Returns the directory fd or -1.
 */
int open_parent_beneath(int rootfd, const char *vpath, char *base, size_t basesz);

#endif
