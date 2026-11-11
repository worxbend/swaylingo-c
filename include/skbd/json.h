#ifndef SKBD_JSON_H
#define SKBD_JSON_H

#include <stddef.h>

/* JSON subset: objects, arrays, strings, numbers, true, false, and null.
   Nesting deeper than SKBD_JSON_MAX_DEPTH is rejected. Input longer than
   SKBD_JSON_MAX_BYTES is rejected. Parser heap above 64 MiB is rejected.
   A string escape below U+0020 is rejected. Truncated input is rejected.
   A number that is not an integer, or that does not fit in long long, keeps
   number_ok at 0 and stores the raw literal in text. Object lookup is by exact key. */

#define SKBD_JSON_MAX_DEPTH 32
#define SKBD_JSON_MAX_BYTES (8u * 1024u * 1024u)

typedef enum {
    SKBD_JSON_NULL = 0,
    SKBD_JSON_BOOL,
    SKBD_JSON_NUMBER,
    SKBD_JSON_STRING,
    SKBD_JSON_ARRAY,
    SKBD_JSON_OBJECT
} SkbdJsonKind;

typedef struct SkbdJson {
    SkbdJsonKind kind;
    int boolean;
    long long number;
    int number_ok;
    char *text;
    char **keys;
    struct SkbdJson *children;
    size_t len;
    size_t cap;
} SkbdJson;

/* Parses one JSON value. On success *out is owned by the caller.
   On failure *out is NULL. There is no Rust twin; this is the serde_json stand-in. */
int skbd_json_parse(const char *bytes, size_t len, SkbdJson **out, char *err, size_t err_cap);

void skbd_json_free(SkbdJson *value);

/* Object field. NULL when object is NULL, not an object, or the key is absent. */
const SkbdJson *skbd_json_get(const SkbdJson *object, const char *key);

#endif
