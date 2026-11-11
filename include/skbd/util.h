#ifndef SKBD_UTIL_H
#define SKBD_UTIL_H

#include <stddef.h>
#include <sys/types.h>

/* Error policy: a fallible function returns 0 on success and -1 on failure.
   The caller passes char *err and size_t err_cap. On failure the callee
   writes a NUL-terminated message into err when err is not NULL and err_cap
   is greater than 0. The message is cut to fit err_cap, including the NUL.
   On success, err is left unchanged. err may be NULL when the caller does
   not want the text. */

/* Writes msg and returns -1. msg may be NULL. */
int skbd_err_set(char *err, size_t err_cap, const char *msg);

/* Writes a formatted message and returns -1. fmt is a printf format. */
int skbd_err_fmt(char *err, size_t err_cap, const char *fmt, ...);

/* Owned copy of text, including the NUL. NULL if text is NULL or malloc fails. */
char *skbd_strdup(const char *text);

/* Grow *ptr so it can hold at least need elements of elem_size.
   *cap is the element capacity, not the byte size. *ptr is NULL when *cap is 0.
   Growth doubles from 4. New slots are not zeroed. On failure *ptr and *cap
   stay as they were. Rust has no twin; tables in this tree share this check. */
int skbd_grow(void **ptr, size_t *cap, size_t need, size_t elem_size, char *err, size_t err_cap);

/* Opens an absolute directory without following a symlink. The final directory
   must be mode 0700 and owned by euid. Each ancestor must be owned by root or
   euid and must not be writable by group or other, except a root-owned sticky
   directory. On success *out_fd is owned by the caller. */
int skbd_open_private_dir(const char *dir, uid_t euid, const char *label, int *out_fd, char *err, size_t err_cap);

#endif
