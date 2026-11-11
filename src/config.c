#include "skbd/config.h"

#include "skbd/util.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Same value as SKBD_DEFAULT_LAYOUT. memory.h includes this header. */
enum { CONFIG_DEFAULT_LAYOUT = 0 };

enum { READ_CHUNK = 4096 };
enum { CONFIG_MAX_BYTES = 1024 * 1024 };

typedef struct {
    const char *text;
    size_t at;
    size_t line;
    const char *path;
    SkbdConfigFile *file;
    char *err;
    size_t err_cap;
    int in_apps;
} Parser;

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} ByteBuf;

typedef struct {
    const char *const *codes;
    size_t code_count;
    const char *language;
    const char *prefer;
} ShortCode;

/* Rust SHORT_CODES. us/en prefer a name containing "(us)", else any English name. */
static const char *const CODE_US[] = {"us", "en"};
static const char *const CODE_UA[] = {"ua", "uk"};
static const char *const CODE_RU[] = {"ru"};
static const char *const CODE_DE[] = {"de"};
static const char *const CODE_PL[] = {"pl"};
static const char *const CODE_FR[] = {"fr"};

static const ShortCode SHORT_CODES[] = {
    {CODE_US, 2, "english", "(us)"},
    {CODE_UA, 2, "ukrainian", NULL},
    {CODE_RU, 1, "russian", NULL},
    {CODE_DE, 1, "german", NULL},
    {CODE_PL, 1, "polish", NULL},
    {CODE_FR, 1, "french", NULL},
};

static int ascii_lower(unsigned char ch) {
    if (ch >= 'A' && ch <= 'Z') {
        return ch - 'A' + 'a';
    }
    return ch;
}

static int is_ascii_space(unsigned char ch) {
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\v' || ch == '\f';
}

static int is_bare(unsigned char ch) {
    return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '-';
}

static int is_digit(unsigned char ch) {
    return ch >= '0' && ch <= '9';
}

static int ascii_eq(const char *left, const char *right) {
    size_t i = 0;

    if (left == NULL || right == NULL) {
        return left == right;
    }
    while (left[i] != '\0' || right[i] != '\0') {
        if (ascii_lower((unsigned char)left[i]) != ascii_lower((unsigned char)right[i])) {
            return 0;
        }
        if (left[i] == '\0') {
            break;
        }
        i++;
    }
    return 1;
}

static int ascii_contains(const char *haystack, const char *needle) {
    size_t needle_len;
    size_t start;
    size_t offset;

    if (haystack == NULL || needle == NULL) {
        return 0;
    }
    needle_len = strlen(needle);
    if (needle_len == 0) {
        return 1;
    }
    for (start = 0; haystack[start] != '\0'; start++) {
        for (offset = 0; offset < needle_len; offset++) {
            unsigned char hay = (unsigned char)haystack[start + offset];
            if (hay == '\0') {
                return 0;
            }
            if (ascii_lower(hay) != ascii_lower((unsigned char)needle[offset])) {
                break;
            }
        }
        if (offset == needle_len) {
            return 1;
        }
    }
    return 0;
}

static void trim_inplace(char *text) {
    char *start = text;
    size_t len;

    while (*start != '\0' && is_ascii_space((unsigned char)*start)) {
        start++;
    }
    if (start != text) {
        memmove(text, start, strlen(start) + 1);
    }
    len = strlen(text);
    while (len > 0 && is_ascii_space((unsigned char)text[len - 1])) {
        len--;
    }
    text[len] = '\0';
}

static char *trim_copy(const char *text) {
    const char *start;
    const char *end;
    size_t len;
    char *copy;

    if (text == NULL) {
        return NULL;
    }
    start = text;
    while (*start != '\0' && is_ascii_space((unsigned char)*start)) {
        start++;
    }
    end = start + strlen(start);
    while (end > start && is_ascii_space((unsigned char)end[-1])) {
        end--;
    }
    len = (size_t)(end - start);
    copy = malloc(len + 1);
    if (copy == NULL) {
        return NULL;
    }
    memcpy(copy, start, len);
    copy[len] = '\0';
    return copy;
}

static int fail_at(const Parser *parser, const char *msg) {
    return skbd_err_fmt(parser->err, parser->err_cap, "%s: line %zu: %s", parser->path, parser->line, msg);
}

static int fail_type(const Parser *parser, const char *label) {
    return skbd_err_fmt(
        parser->err,
        parser->err_cap,
        "%s: %s must be a string or a non-negative integer",
        parser->path,
        label);
}

static int oom(const Parser *parser) {
    return skbd_err_set(parser->err, parser->err_cap, "out of memory");
}

static void skip_inline(Parser *parser) {
    while (parser->text[parser->at] == ' ' || parser->text[parser->at] == '\t' || parser->text[parser->at] == '\r') {
        parser->at++;
    }
}

static void skip_gap(Parser *parser) {
    for (;;) {
        skip_inline(parser);
        if (parser->text[parser->at] == '#') {
            while (parser->text[parser->at] != '\0' && parser->text[parser->at] != '\n') {
                parser->at++;
            }
        }
        if (parser->text[parser->at] != '\n') {
            return;
        }
        parser->at++;
        parser->line++;
    }
}

static int finish_line(Parser *parser) {
    skip_inline(parser);
    if (parser->text[parser->at] == '#') {
        while (parser->text[parser->at] != '\0' && parser->text[parser->at] != '\n') {
            parser->at++;
        }
    }
    if (parser->text[parser->at] == '\n') {
        parser->at++;
        parser->line++;
        return 0;
    }
    if (parser->text[parser->at] == '\0') {
        return 0;
    }
    return fail_at(parser, "trailing characters");
}

static int buf_add(ByteBuf *buf, char ch, Parser *parser) {
    if (buf->len + 2 > buf->cap) {
        size_t next = buf->cap == 0 ? 32 : buf->cap * 2;
        char *grown;

        if (next < buf->cap || next < buf->len + 2) {
            return oom(parser);
        }
        grown = realloc(buf->data, next);
        if (grown == NULL) {
            return oom(parser);
        }
        buf->data = grown;
        buf->cap = next;
    }
    buf->data[buf->len] = ch;
    buf->len++;
    buf->data[buf->len] = '\0';
    return 0;
}

static int decode_escape(Parser *parser, unsigned char *out) {
    unsigned char ch;

    if (parser->text[parser->at] == '\0' || parser->text[parser->at] == '\n') {
        return fail_at(parser, "unterminated string");
    }
    ch = (unsigned char)parser->text[parser->at];
    parser->at++;
    switch (ch) {
    case 'b':
        *out = '\b';
        return 0;
    case 't':
        *out = '\t';
        return 0;
    case 'n':
        *out = '\n';
        return 0;
    case 'f':
        *out = '\f';
        return 0;
    case 'r':
        *out = '\r';
        return 0;
    case '"':
        *out = '"';
        return 0;
    case '\\':
        *out = '\\';
        return 0;
    default:
        return fail_at(parser, "unsupported escape");
    }
}

static int parse_quoted(Parser *parser, char **out) {
    ByteBuf buf;

    memset(&buf, 0, sizeof buf);
    if (parser->text[parser->at] != '"') {
        return fail_at(parser, "expected string");
    }
    parser->at++;
    while (parser->text[parser->at] != '\0' && parser->text[parser->at] != '"') {
        unsigned char ch = (unsigned char)parser->text[parser->at];

        if (ch == '\n') {
            free(buf.data);
            return fail_at(parser, "unterminated string");
        }
        if (ch == '\\') {
            parser->at++;
            if (decode_escape(parser, &ch) != 0) {
                free(buf.data);
                return -1;
            }
        } else {
            if ((ch < 0x20 && ch != '\t') || ch == 0x7f) {
                free(buf.data);
                return fail_at(parser, "control character in string");
            }
            parser->at++;
        }
        if (buf_add(&buf, (char)ch, parser) != 0) {
            free(buf.data);
            return -1;
        }
    }
    if (parser->text[parser->at] != '"') {
        free(buf.data);
        return fail_at(parser, "unterminated string");
    }
    parser->at++;
    if (buf.data == NULL) {
        buf.data = malloc(1);
        if (buf.data == NULL) {
            return oom(parser);
        }
        buf.data[0] = '\0';
    }
    *out = buf.data;
    return 0;
}

static int parse_key(Parser *parser, char **key) {
    size_t start;
    size_t len;

    *key = NULL;
    skip_inline(parser);
    if (parser->text[parser->at] == '"') {
        return parse_quoted(parser, key);
    }
    if (!is_bare((unsigned char)parser->text[parser->at])) {
        return fail_at(parser, "expected key");
    }
    start = parser->at;
    while (is_bare((unsigned char)parser->text[parser->at])) {
        parser->at++;
    }
    len = parser->at - start;
    *key = malloc(len + 1);
    if (*key == NULL) {
        return oom(parser);
    }
    memcpy(*key, parser->text + start, len);
    (*key)[len] = '\0';
    return 0;
}

static int parse_layout_index(Parser *parser, const char *label, SkbdLayoutToken *token) {
    int negative = 0;
    int overflow = 0;
    uint64_t acc = 0;

    if (parser->text[parser->at] == '+' || parser->text[parser->at] == '-') {
        negative = parser->text[parser->at] == '-';
        parser->at++;
    }
    if (!is_digit((unsigned char)parser->text[parser->at])) {
        return fail_at(parser, "invalid integer");
    }
    /* "01" is a name, not index 1. A bare integer with a leading zero is invalid TOML. */
    if (parser->text[parser->at] == '0' && is_digit((unsigned char)parser->text[parser->at + 1])) {
        return fail_at(parser, "invalid integer");
    }
    while (is_digit((unsigned char)parser->text[parser->at])) {
        unsigned digit = (unsigned)(parser->text[parser->at] - '0');

        if (!overflow) {
            if (acc > (UINT64_MAX - digit) / 10) {
                overflow = 1;
            } else {
                acc = acc * 10u + digit;
            }
        }
        parser->at++;
    }
    if (overflow || acc > UINT32_MAX || (negative && acc != 0)) {
        return fail_type(parser, label);
    }
    token->kind = SKBD_TOKEN_INDEX;
    token->index = (uint32_t)acc;
    token->name = NULL;
    return 0;
}

static int parse_layout(Parser *parser, const char *label, SkbdLayoutToken *token) {
    memset(token, 0, sizeof *token);
    if (parser->text[parser->at] == '\0' || parser->text[parser->at] == '\n' || parser->text[parser->at] == '#') {
        return fail_at(parser, "expected value");
    }
    if (parser->text[parser->at] == '"') {
        char *text = NULL;

        if (parse_quoted(parser, &text) != 0) {
            return -1;
        }
        trim_inplace(text);
        if (text[0] == '\0') {
            free(text);
            return skbd_err_fmt(parser->err, parser->err_cap, "%s: %s is empty", parser->path, label);
        }
        token->kind = SKBD_TOKEN_NAME;
        token->name = text;
        return 0;
    }
    if (parser->text[parser->at] == '+' || parser->text[parser->at] == '-' || is_digit((unsigned char)parser->text[parser->at])) {
        return parse_layout_index(parser, label, token);
    }
    return fail_type(parser, label);
}

static int has_app(const SkbdConfigFile *file, const char *key) {
    size_t i;

    if (file->apps == NULL) {
        return 0;
    }
    for (i = 0; i < file->apps_len; i++) {
        if (file->apps[i].app != NULL && strcmp(file->apps[i].app, key) == 0) {
            return 1;
        }
    }
    return 0;
}

static int push_app(SkbdConfigFile *file, char *key, SkbdLayoutToken *token, char *err, size_t err_cap) {
    void *raw = file->apps;

    if (skbd_grow(&raw, &file->apps_cap, file->apps_len + 1, sizeof(SkbdConfigApp), err, err_cap) != 0) {
        return -1;
    }
    file->apps = raw;
    file->apps[file->apps_len].app = key;
    file->apps[file->apps_len].token = *token;
    token->name = NULL;
    file->apps_len++;
    return 0;
}

static int parse_app(Parser *parser, char *key) {
    char *label;
    size_t label_cap;
    SkbdLayoutToken token;

    if (key[0] == '\0') {
        free(key);
        return skbd_err_fmt(parser->err, parser->err_cap, "%s: apps key is empty", parser->path);
    }
    if (has_app(parser->file, key)) {
        int rc = skbd_err_fmt(parser->err, parser->err_cap, "%s: duplicate key `%s`", parser->path, key);
        free(key);
        return rc;
    }
    label_cap = strlen(key) + sizeof "apps.";
    label = malloc(label_cap);
    if (label == NULL) {
        free(key);
        return oom(parser);
    }
    snprintf(label, label_cap, "apps.%s", key);
    if (parse_layout(parser, label, &token) != 0) {
        free(label);
        free(key);
        return -1;
    }
    free(label);
    if (push_app(parser->file, key, &token, parser->err, parser->err_cap) != 0) {
        skbd_token_free(&token);
        free(key);
        return -1;
    }
    return finish_line(parser);
}

static int parse_table(Parser *parser) {
    char *name = NULL;

    parser->at++;
    if (parse_key(parser, &name) != 0) {
        return -1;
    }
    skip_inline(parser);
    if (parser->text[parser->at] != ']') {
        free(name);
        return fail_at(parser, "invalid table header");
    }
    parser->at++;
    if (strcmp(name, "apps") != 0) {
        int rc = skbd_err_fmt(parser->err, parser->err_cap, "%s: unknown key `%s`", parser->path, name);
        free(name);
        return rc;
    }
    free(name);
    if (parser->in_apps) {
        return skbd_err_fmt(parser->err, parser->err_cap, "%s: duplicate key `apps`", parser->path);
    }
    parser->in_apps = 1;
    return finish_line(parser);
}

/* A key under [apps] is an app id, including one named fallback. */
static int parse_assignment(Parser *parser) {
    char *key = NULL;

    if (parse_key(parser, &key) != 0) {
        return -1;
    }
    skip_inline(parser);
    if (parser->text[parser->at] != '=') {
        free(key);
        return fail_at(parser, "expected '='");
    }
    parser->at++;
    skip_inline(parser);
    if (parser->in_apps) {
        return parse_app(parser, key);
    }
    if (strcmp(key, "apps") == 0) {
        free(key);
        return skbd_err_fmt(parser->err, parser->err_cap, "%s: apps must be a table", parser->path);
    }
    if (strcmp(key, "fallback") != 0) {
        int rc = skbd_err_fmt(parser->err, parser->err_cap, "%s: unknown key `%s`", parser->path, key);
        free(key);
        return rc;
    }
    free(key);
    if (parser->file->has_fallback) {
        return skbd_err_fmt(parser->err, parser->err_cap, "%s: duplicate key `fallback`", parser->path);
    }
    if (parse_layout(parser, "fallback", &parser->file->fallback) != 0) {
        return -1;
    }
    parser->file->has_fallback = 1;
    return finish_line(parser);
}

static int parse_document(Parser *parser) {
    skip_gap(parser);
    while (parser->text[parser->at] != '\0') {
        int rc = parser->text[parser->at] == '[' ? parse_table(parser) : parse_assignment(parser);

        if (rc != 0) {
            return -1;
        }
        skip_gap(parser);
    }
    return 0;
}

static size_t base_len(const char *base) {
    size_t len = strlen(base);

    while (len > 1 && base[len - 1] == '/') {
        len--;
    }
    return len;
}

static int rooted_config_path(const char *path) {
    size_t at = 0;

    if (path == NULL || path[0] != '/') {
        return 0;
    }
    while (path[at] != '\0') {
        size_t start;

        if (path[at] != '/') {
            return 0;
        }
        at++;
        if (path[at] == '\0') {
            return 1;
        }
        if (path[at] == '/') {
            return 0;
        }
        start = at;
        while (path[at] != '\0' && path[at] != '/') {
            at++;
        }
        if (at - start == 2 && path[start] == '.' && path[start + 1] == '.') {
            return 0;
        }
    }
    return 1;
}

static int write_config_path(char *buf, size_t cap, const char *base, const char *rest) {
    size_t len;
    int slash;
    int wrote;

    if (base == NULL) {
        base = "";
    }
    len = base_len(base);
    if (len > (size_t)INT_MAX) {
        return -1;
    }
    slash = len > 0 && base[len - 1] != '/';
    if (slash) {
        wrote = snprintf(buf, cap, "%.*s/%s", (int)len, base, rest);
    } else {
        wrote = snprintf(buf, cap, "%.*s%s", (int)len, base, rest);
    }
    if (wrote < 0 || (size_t)wrote >= cap) {
        return -1;
    }
    return 0;
}

static int read_fd(int fd, char **out, const char *path, char *err, size_t err_cap) {
    size_t cap = READ_CHUNK;
    size_t len = 0;
    char *bytes = malloc(cap);

    if (bytes == NULL) {
        return skbd_err_set(err, err_cap, "out of memory");
    }
    for (;;) {
        ssize_t nread;
        size_t room;
        size_t limit;

        if (len > CONFIG_MAX_BYTES) {
            free(bytes);
            return skbd_err_fmt(err, err_cap, "%s: file is too large", path);
        }
        if (len + 1 >= cap) {
            size_t next = cap * 2;
            char *grown;

            if (next < cap) {
                free(bytes);
                return skbd_err_set(err, err_cap, "out of memory");
            }
            grown = realloc(bytes, next);
            if (grown == NULL) {
                free(bytes);
                return skbd_err_set(err, err_cap, "out of memory");
            }
            bytes = grown;
            cap = next;
        }
        limit = CONFIG_MAX_BYTES - len;
        room = cap - len - 1;
        if (room > limit) {
            room = limit;
        }
        if (room == 0) {
            char extra;

            nread = read(fd, &extra, 1);
            if (nread < 0 && errno == EINTR) {
                continue;
            }
            if (nread < 0) {
                free(bytes);
                return skbd_err_fmt(err, err_cap, "%s: %s", path, strerror(errno));
            }
            if (nread > 0) {
                free(bytes);
                return skbd_err_fmt(err, err_cap, "%s: file is too large", path);
            }
            break;
        }
        nread = read(fd, bytes + len, room);
        if (nread < 0) {
            if (errno == EINTR) {
                continue;
            }
            free(bytes);
            return skbd_err_fmt(err, err_cap, "%s: %s", path, strerror(errno));
        }
        if (nread == 0) {
            break;
        }
        len += (size_t)nread;
    }
    bytes[len] = '\0';
    if (strlen(bytes) != len) {
        free(bytes);
        return skbd_err_fmt(err, err_cap, "%s: file contains a NUL byte", path);
    }
    *out = bytes;
    return 0;
}

/* 0 and *text set, 1 if the file is missing, -1 on error. */
static int read_config_text(const char *path, char **text, char *err, size_t err_cap) {
    int fd;
    struct stat st;

    *text = NULL;
    if (path == NULL) {
        return skbd_err_set(err, err_cap, "config path is null");
    }
    do {
        fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0) {
        if (errno == ENOENT) {
            return 1;
        }
        return skbd_err_fmt(err, err_cap, "%s: %s", path, strerror(errno));
    }
    if (fstat(fd, &st) != 0) {
        int saved = errno;
        close(fd);
        return skbd_err_fmt(err, err_cap, "%s: %s", path, strerror(saved));
    }
    if (!S_ISREG(st.st_mode)) {
        int kind = S_ISDIR(st.st_mode) ? EISDIR : EINVAL;
        close(fd);
        return skbd_err_fmt(err, err_cap, "%s: %s", path, strerror(kind));
    }
    if (st.st_uid != geteuid() && st.st_uid != 0) {
        close(fd);
        return skbd_err_fmt(err, err_cap, "%s: file is not owned by this user", path);
    }
    if (st.st_size < 0 || (unsigned long long)st.st_size > (unsigned long long)CONFIG_MAX_BYTES) {
        close(fd);
        return skbd_err_fmt(err, err_cap, "%s: file is too large", path);
    }
    if (read_fd(fd, text, path, err, err_cap) != 0) {
        close(fd);
        return -1;
    }
    close(fd);
    return 0;
}

static int append_text(char *err, size_t err_cap, size_t *used, const char *text) {
    size_t len;
    size_t room;

    if (err == NULL || err_cap == 0 || used == NULL || text == NULL) {
        return -1;
    }
    if (*used >= err_cap) {
        err[err_cap - 1] = '\0';
        return 0;
    }
    len = strlen(text);
    room = err_cap - *used - 1;
    if (len > room) {
        len = room;
    }
    memcpy(err + *used, text, len);
    *used += len;
    err[*used] = '\0';
    return 0;
}

static int set_unknown(const char *token, const char *const *names, size_t name_count, char *err, size_t err_cap) {
    size_t used = 0;
    size_t i;

    if (err == NULL || err_cap == 0) {
        return -1;
    }
    err[0] = '\0';
    append_text(err, err_cap, &used, "unknown layout \"");
    append_text(err, err_cap, &used, token != NULL ? token : "");
    append_text(err, err_cap, &used, "\"");
    if (names == NULL || name_count == 0) {
        append_text(err, err_cap, &used, "\nknown layouts: none");
        return -1;
    }
    append_text(err, err_cap, &used, "\nknown layouts:");
    for (i = 0; i < name_count; i++) {
        char line[64];

        if (i > UINT32_MAX) {
            break;
        }
        snprintf(line, sizeof line, "\n%u ", (unsigned)i);
        append_text(err, err_cap, &used, line);
        append_text(err, err_cap, &used, names[i] != NULL ? names[i] : "");
    }
    return -1;
}

static int decimal_index(const char *token, uint32_t *index) {
    size_t i;
    uint64_t acc = 0;

    if (token == NULL || token[0] == '\0') {
        return 0;
    }
    for (i = 0; token[i] != '\0'; i++) {
        if (!is_digit((unsigned char)token[i])) {
            return 0;
        }
    }
    /* "01" is a name, not index 1. */
    if (i > 1 && token[0] == '0') {
        return 0;
    }
    for (i = 0; token[i] != '\0'; i++) {
        unsigned digit = (unsigned)(token[i] - '0');

        if (acc > (UINT64_MAX - digit) / 10 || acc > UINT32_MAX) {
            return 0;
        }
        acc = acc * 10u + digit;
    }
    if (acc > UINT32_MAX) {
        return 0;
    }
    *index = (uint32_t)acc;
    return 1;
}

static int index_in_range(uint32_t layout, const char *const *names, size_t name_count, uint32_t *found, char *err, size_t err_cap) {
    char text[16];

    if ((size_t)layout < name_count) {
        *found = layout;
        return 0;
    }
    snprintf(text, sizeof text, "%u", (unsigned)layout);
    return set_unknown(text, names, name_count, err, err_cap);
}

static int find_exact(const char *token, const char *const *names, size_t name_count, uint32_t *found) {
    size_t i;

    for (i = 0; i < name_count; i++) {
        if (names[i] != NULL && strcmp(names[i], token) == 0) {
            if (i > UINT32_MAX) {
                return 0;
            }
            *found = (uint32_t)i;
            return 1;
        }
    }
    for (i = 0; i < name_count; i++) {
        if (names[i] != NULL && ascii_eq(names[i], token)) {
            if (i > UINT32_MAX) {
                return 0;
            }
            *found = (uint32_t)i;
            return 1;
        }
    }
    return 0;
}

static int find_language(const char *const *names, size_t name_count, const char *language, const char *prefer, uint32_t *found) {
    size_t i;

    for (i = 0; i < name_count; i++) {
        if (names[i] == NULL || !ascii_contains(names[i], language)) {
            continue;
        }
        if (prefer != NULL && !ascii_contains(names[i], prefer)) {
            continue;
        }
        if (i > UINT32_MAX) {
            return 0;
        }
        *found = (uint32_t)i;
        return 1;
    }
    return 0;
}

static const ShortCode *match_short(const char *token) {
    size_t i;
    size_t code;

    for (i = 0; i < sizeof SHORT_CODES / sizeof SHORT_CODES[0]; i++) {
        for (code = 0; code < SHORT_CODES[i].code_count; code++) {
            if (ascii_eq(SHORT_CODES[i].codes[code], token)) {
                return &SHORT_CODES[i];
            }
        }
    }
    return NULL;
}

static int resolve_name(const char *token, const char *const *names, size_t name_count, uint32_t *found, char *err, size_t err_cap) {
    const ShortCode *code;

    if (decimal_index(token, found)) {
        return index_in_range(*found, names, name_count, found, err, err_cap);
    }
    if (find_exact(token, names, name_count, found)) {
        return 0;
    }
    code = match_short(token);
    if (code != NULL) {
        if (code->prefer != NULL && find_language(names, name_count, code->language, code->prefer, found)) {
            return 0;
        }
        if (find_language(names, name_count, code->language, NULL, found)) {
            return 0;
        }
    }
    return set_unknown(token, names, name_count, err, err_cap);
}

static int rules_put(SkbdRules *rules, char *app, uint32_t layout, char *err, size_t err_cap) {
    size_t i;
    void *raw;

    if (app == NULL) {
        return skbd_err_set(err, err_cap, "app name is null");
    }
    for (i = 0; i < rules->apps_len; i++) {
        if (rules->apps[i].app != NULL && strcmp(rules->apps[i].app, app) == 0) {
            rules->apps[i].index = layout;
            free(app);
            return 0;
        }
    }
    raw = rules->apps;
    if (skbd_grow(&raw, &rules->apps_cap, rules->apps_len + 1, sizeof(SkbdAppIndex), err, err_cap) != 0) {
        return -1;
    }
    rules->apps = raw;
    rules->apps[rules->apps_len].app = app;
    rules->apps[rules->apps_len].index = layout;
    rules->apps_len++;
    return 0;
}

void skbd_token_free(SkbdLayoutToken *token) {
    if (token == NULL) {
        return;
    }
    free(token->name);
    token->name = NULL;
    token->index = 0;
    token->kind = SKBD_TOKEN_INDEX;
}

void skbd_config_free(SkbdConfigFile *file) {
    size_t i;

    if (file == NULL) {
        return;
    }
    skbd_token_free(&file->fallback);
    if (file->apps != NULL) {
        for (i = 0; i < file->apps_len; i++) {
            free(file->apps[i].app);
            skbd_token_free(&file->apps[i].token);
        }
        free(file->apps);
    }
    memset(file, 0, sizeof *file);
}

void skbd_rules_free(SkbdRules *rules) {
    size_t i;

    if (rules == NULL) {
        return;
    }
    if (rules->apps != NULL) {
        for (i = 0; i < rules->apps_len; i++) {
            free(rules->apps[i].app);
        }
        free(rules->apps);
    }
    memset(rules, 0, sizeof *rules);
}

int skbd_config_path(const char *xdg, const char *home, char *buf, size_t cap, char *err, size_t err_cap) {
    const char *base;
    const char *rest;

    if (buf == NULL || cap == 0) {
        return skbd_err_set(err, err_cap, "config path buffer is null");
    }
    if (xdg != NULL && xdg[0] == '/') {
        base = xdg;
        rest = "swaylingo/config.toml";
    } else if (home != NULL && home[0] == '/') {
        base = home;
        rest = ".config/swaylingo/config.toml";
    } else if (home == NULL || home[0] == '\0') {
        return skbd_err_set(err, err_cap, "HOME is not set");
    } else {
        return skbd_err_set(err, err_cap, "HOME is not absolute");
    }
    if (write_config_path(buf, cap, base, rest) != 0 || !rooted_config_path(buf)) {
        buf[0] = '\0';
        return skbd_err_set(err, err_cap, "config path does not fit");
    }
    return 0;
}

int skbd_config_default_path(char *buf, size_t cap, char *err, size_t err_cap) {
    const char *xdg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");

    return skbd_config_path(xdg, home, buf, cap, err, err_cap);
}

int skbd_config_parse(const char *text, const char *path, SkbdConfigFile *out, char *err, size_t err_cap) {
    Parser parser;

    if (out == NULL) {
        return skbd_err_set(err, err_cap, "config is null");
    }
    memset(out, 0, sizeof *out);
    if (text == NULL) {
        return skbd_err_set(err, err_cap, "config text is null");
    }
    memset(&parser, 0, sizeof parser);
    parser.text = text;
    parser.line = 1;
    parser.path = path != NULL ? path : "";
    parser.file = out;
    parser.err = err;
    parser.err_cap = err_cap;
    if (parse_document(&parser) != 0) {
        skbd_config_free(out);
        return -1;
    }
    return 0;
}

int skbd_config_load_required(const char *path, SkbdConfigFile *out, char *err, size_t err_cap) {
    char *text = NULL;
    int rc;

    if (out == NULL) {
        return skbd_err_set(err, err_cap, "config is null");
    }
    memset(out, 0, sizeof *out);
    rc = read_config_text(path, &text, err, err_cap);
    if (rc < 0) {
        return -1;
    }
    if (rc > 0) {
        return skbd_err_fmt(err, err_cap, "%s: file not found", path != NULL ? path : "");
    }
    rc = skbd_config_parse(text, path, out, err, err_cap);
    free(text);
    return rc;
}

int skbd_config_load_optional(const char *path, SkbdConfigFile *out, char *err, size_t err_cap) {
    char *text = NULL;
    int rc;

    if (out == NULL) {
        return skbd_err_set(err, err_cap, "config is null");
    }
    memset(out, 0, sizeof *out);
    rc = read_config_text(path, &text, err, err_cap);
    if (rc < 0) {
        return -1;
    }
    if (rc > 0) {
        return 0;
    }
    rc = skbd_config_parse(text, path, out, err, err_cap);
    free(text);
    return rc;
}

int skbd_config_resolve_token(const SkbdLayoutToken *token, const char *const *names, size_t name_count, uint32_t *index, char *err, size_t err_cap) {
    uint32_t found = 0;
    char *trimmed;
    int rc;

    if (index == NULL) {
        return skbd_err_set(err, err_cap, "layout index is null");
    }
    if (token == NULL) {
        return skbd_err_set(err, err_cap, "layout token is null");
    }
    if (names == NULL) {
        name_count = 0;
    }
    if (token->kind == SKBD_TOKEN_INDEX) {
        rc = index_in_range(token->index, names, name_count, &found, err, err_cap);
        if (rc != 0) {
            return rc;
        }
        *index = found;
        return 0;
    }
    if (token->kind != SKBD_TOKEN_NAME) {
        return skbd_err_set(err, err_cap, "layout token kind is invalid");
    }
    if (token->name == NULL) {
        return set_unknown("", names, name_count, err, err_cap);
    }
    trimmed = trim_copy(token->name);
    if (trimmed == NULL) {
        return skbd_err_set(err, err_cap, "out of memory");
    }
    rc = resolve_name(trimmed, names, name_count, &found, err, err_cap);
    free(trimmed);
    if (rc != 0) {
        return rc;
    }
    *index = found;
    return 0;
}

int skbd_config_resolve_rules(const SkbdConfigFile *file, const char *const *names, size_t name_count, const SkbdRuleSource *from, SkbdRules *out, char *err, size_t err_cap) {
    size_t detail_cap = err_cap > 256 ? err_cap : 256;
    char *detail;
    uint32_t fallback = CONFIG_DEFAULT_LAYOUT;
    size_t i;

    if (out == NULL) {
        return skbd_err_set(err, err_cap, "rules are null");
    }
    memset(out, 0, sizeof *out);
    if (file == NULL) {
        return skbd_err_set(err, err_cap, "config is null");
    }
    if (from == NULL || from->fallback_label == NULL || from->apps_label == NULL) {
        return skbd_err_set(err, err_cap, "rule source is null");
    }
    if (file->apps_len > 0 && file->apps == NULL) {
        return skbd_err_set(err, err_cap, "config apps are null");
    }
    if (detail_cap > SIZE_MAX / 2) {
        detail_cap = SIZE_MAX / 2;
    }
    detail = malloc(detail_cap);
    if (detail == NULL) {
        return skbd_err_set(err, err_cap, "out of memory");
    }
    detail[0] = '\0';
    if (file->has_fallback) {
        if (skbd_config_resolve_token(&file->fallback, names, name_count, &fallback, detail, detail_cap) != 0) {
            int rc = skbd_err_fmt(err, err_cap, "%s: %s", from->fallback_label, detail);
            free(detail);
            return rc;
        }
    }
    out->fallback = fallback;
    for (i = 0; i < file->apps_len; i++) {
        uint32_t layout = 0;
        char *copied;
        const char *app = file->apps[i].app;

        if (app == NULL) {
            free(detail);
            skbd_rules_free(out);
            return skbd_err_set(err, err_cap, "app name is null");
        }
        if (skbd_config_resolve_token(&file->apps[i].token, names, name_count, &layout, detail, detail_cap) != 0) {
            int rc = skbd_err_fmt(err, err_cap, "%s: apps.%s: %s", from->apps_label, app, detail);
            free(detail);
            skbd_rules_free(out);
            return rc;
        }
        copied = skbd_strdup(app);
        if (copied == NULL) {
            free(detail);
            skbd_rules_free(out);
            return skbd_err_set(err, err_cap, "out of memory");
        }
        if (rules_put(out, copied, layout, err, err_cap) != 0) {
            free(copied);
            free(detail);
            skbd_rules_free(out);
            return -1;
        }
    }
    free(detail);
    return 0;
}
