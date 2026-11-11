#include "skbd/util.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int skbd_err_set(char *err, size_t err_cap, const char *msg) {
    int wrote;

    if (err == NULL || err_cap == 0) {
        return -1;
    }
    if (msg == NULL) {
        err[0] = '\0';
        return -1;
    }
    wrote = snprintf(err, err_cap, "%s", msg);
    if (wrote < 0) {
        err[0] = '\0';
    }
    return -1;
}

int skbd_err_fmt(char *err, size_t err_cap, const char *fmt, ...) {
    va_list args;
    int wrote;

    if (err == NULL || err_cap == 0) {
        return -1;
    }
    if (fmt == NULL) {
        err[0] = '\0';
        return -1;
    }
    va_start(args, fmt);
    wrote = vsnprintf(err, err_cap, fmt, args);
    va_end(args);
    if (wrote < 0) {
        err[0] = '\0';
    }
    return -1;
}

char *skbd_strdup(const char *text) {
    size_t len;
    char *copy;

    if (text == NULL) {
        return NULL;
    }
    len = strlen(text);
    if (len == SIZE_MAX) {
        return NULL;
    }
    copy = malloc(len + 1);
    if (copy == NULL) {
        return NULL;
    }
    memcpy(copy, text, len + 1);
    return copy;
}

int skbd_grow(void **ptr, size_t *cap, size_t need, size_t elem_size, char *err, size_t err_cap) {
    size_t next;
    void *grown;

    if (ptr == NULL || cap == NULL || elem_size == 0 || (*ptr == NULL && *cap != 0)) {
        return skbd_err_set(err, err_cap, "grow needs a buffer");
    }
    if (need <= *cap) {
        return 0;
    }
    next = *cap == 0 ? 4 : *cap;
    while (next < need) {
        if (next > SIZE_MAX / 2) {
            return skbd_err_set(err, err_cap, "grow overflow");
        }
        next *= 2;
    }
    if (next > SIZE_MAX / elem_size) {
        return skbd_err_set(err, err_cap, "grow overflow");
    }
    grown = realloc(*ptr, next * elem_size);
    if (grown == NULL) {
        return skbd_err_set(err, err_cap, "out of memory");
    }
    *ptr = grown;
    *cap = next;
    return 0;
}

static int read_failed(char *err, size_t err_cap, const char *label, const char *dir, int saved) {
    return skbd_err_fmt(err, err_cap, "%s cannot be read: %s (%s)", label, dir, strerror(saved));
}

static int copy_dir_path(const char *dir, char *buf, size_t cap) {
    size_t len;

    if (dir == NULL || dir[0] != '/') {
        return -1;
    }
    len = strlen(dir);
    while (len > 1 && dir[len - 1] == '/') {
        len--;
    }
    if (len >= cap) {
        return -1;
    }
    memcpy(buf, dir, len);
    buf[len] = '\0';
    return 0;
}

/* 1 copied, 0 at end, -1 empty, ".", "..", or longer than the buffer. */
static int take_component(const char *path, size_t *at, char *name, size_t name_cap) {
    size_t begin;
    size_t len;

    if (path[*at] == '\0') {
        return 0;
    }
    if (path[*at] == '/') {
        return -1;
    }
    begin = *at;
    while (path[*at] != '\0' && path[*at] != '/') {
        (*at)++;
    }
    len = *at - begin;
    if (len == 0 || len >= name_cap) {
        return -1;
    }
    if ((len == 1 && path[begin] == '.') || (len == 2 && path[begin] == '.' && path[begin + 1] == '.')) {
        return -1;
    }
    memcpy(name, path + begin, len);
    name[len] = '\0';
    if (path[*at] == '/') {
        (*at)++;
        if (path[*at] == '/') {
            return -1;
        }
    }
    return 1;
}

static int open_root(void) {
    int fd;

    do {
        fd = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    } while (fd < 0 && errno == EINTR);
    return fd;
}

static int open_component(int dir_fd, const char *name) {
    int fd;

    do {
        fd = openat(dir_fd, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    } while (fd < 0 && errno == EINTR);
    return fd;
}

static int final_dir_ok(const struct stat *st, uid_t euid, const char *dir, const char *label, char *err, size_t err_cap) {
    mode_t mode = st->st_mode & 0777;

    if (!S_ISDIR(st->st_mode)) {
        return skbd_err_fmt(err, err_cap, "%s is not a directory: %s", label, dir);
    }
    if (mode != 0700) {
        return skbd_err_fmt(err, err_cap, "%s must be mode 0700, not %04o: %s", label, (unsigned)mode, dir);
    }
    if (st->st_uid != euid) {
        return skbd_err_fmt(err, err_cap, "%s is not owned by this user: %s", label, dir);
    }
    return 0;
}

/* S_ISVTX is XSI and hidden when only _POSIX_C_SOURCE is set. Linux uses 01000.
   shortcut: root-owned sticky dirs such as /tmp stay walkable, upgrade if a runtime dir must not live there */
enum { DIR_STICKY_BIT = 01000 };

static int ancestor_dir_ok(const struct stat *st, uid_t euid, const char *dir, const char *label, char *err, size_t err_cap) {
    int group_or_other_write = (st->st_mode & 022) != 0;
    int sticky_root = (st->st_mode & DIR_STICKY_BIT) != 0 && st->st_uid == 0;

    if (!S_ISDIR(st->st_mode)) {
        return skbd_err_fmt(err, err_cap, "%s is not a directory: %s", label, dir);
    }
    if (st->st_uid != 0 && st->st_uid != euid) {
        return skbd_err_fmt(err, err_cap, "%s is not owned by this user: %s", label, dir);
    }
    if (group_or_other_write && !sticky_root) {
        return skbd_err_fmt(err, err_cap, "%s cannot be read: %s (unsafe directory)", label, dir);
    }
    return 0;
}

int skbd_open_private_dir(const char *dir, uid_t euid, const char *label, int *out_fd, char *err, size_t err_cap) {
    char stored[PATH_MAX];
    char name[NAME_MAX + 1];
    struct stat st;
    size_t at;
    int fd;
    int saved;

    if (out_fd == NULL) {
        return skbd_err_set(err, err_cap, "directory fd is missing");
    }
    *out_fd = -1;
    if (label == NULL || label[0] == '\0') {
        label = "path";
    }
    if (dir == NULL || dir[0] != '/') {
        return skbd_err_fmt(err, err_cap, "%s is not absolute", label);
    }
    if (copy_dir_path(dir, stored, sizeof stored) != 0) {
        return skbd_err_fmt(err, err_cap, "%s cannot be read: %s (File name too long)", label, dir);
    }
    fd = open_root();
    if (fd < 0) {
        return read_failed(err, err_cap, label, dir, errno);
    }
    if (fstat(fd, &st) != 0) {
        saved = errno;
        close(fd);
        return read_failed(err, err_cap, label, dir, saved);
    }
    if (stored[1] == '\0') {
        if (final_dir_ok(&st, euid, dir, label, err, err_cap) != 0) {
            close(fd);
            return -1;
        }
        *out_fd = fd;
        return 0;
    }
    if (ancestor_dir_ok(&st, euid, dir, label, err, err_cap) != 0) {
        close(fd);
        return -1;
    }
    at = 1;
    for (;;) {
        int next;
        int kind = take_component(stored, &at, name, sizeof name);

        if (kind != 1) {
            close(fd);
            return skbd_err_fmt(err, err_cap, "%s cannot be read: %s (unsafe directory)", label, dir);
        }
        next = open_component(fd, name);
        if (next < 0) {
            saved = errno;
            close(fd);
            if (saved == ELOOP || saved == ENOTDIR) {
                return skbd_err_fmt(err, err_cap, "%s is not a directory: %s", label, dir);
            }
            return read_failed(err, err_cap, label, dir, saved);
        }
        close(fd);
        fd = next;
        if (fstat(fd, &st) != 0) {
            saved = errno;
            close(fd);
            return read_failed(err, err_cap, label, dir, saved);
        }
        if (stored[at] == '\0') {
            if (final_dir_ok(&st, euid, dir, label, err, err_cap) != 0) {
                close(fd);
                return -1;
            }
            *out_fd = fd;
            return 0;
        }
        if (ancestor_dir_ok(&st, euid, dir, label, err, err_cap) != 0) {
            close(fd);
            return -1;
        }
    }
}
