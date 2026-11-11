#ifndef SKBD_CONFIG_H
#define SKBD_CONFIG_H

#include <stddef.h>
#include <stdint.h>

/* TOML subset: comments, bare and quoted keys, string and integer values,
   and one [apps] table. fallback and each app value are a layout index or a
   name. Names resolve against xkb_layout_names. App keys are a Wayland app_id
   or an X11 WM_CLASS. Any other top-level key is an error that names the file.
   An empty file is an empty config. A missing file is an error for a required
   path and an empty config for the default path. */

/* Rust: LayoutToken. INDEX is a TOML integer. NAME is a string, even when
   every character is a digit. "01" stays a name. */
typedef enum {
    SKBD_TOKEN_INDEX = 0,
    SKBD_TOKEN_NAME
} SkbdTokenKind;

typedef struct {
    SkbdTokenKind kind;
    uint32_t index;
    char *name;
} SkbdLayoutToken;

typedef struct {
    char *app;
    SkbdLayoutToken token;
} SkbdConfigApp;

/* Rust: ConfigFile. has_fallback is 0 when the file did not set one. */
typedef struct {
    int has_fallback;
    SkbdLayoutToken fallback;
    SkbdConfigApp *apps;
    size_t apps_len;
    size_t apps_cap;
} SkbdConfigFile;

/* Resolved app rule. app is owned. */
typedef struct {
    char *app;
    uint32_t index;
} SkbdAppIndex;

/* Rust: Rules. */
typedef struct {
    uint32_t fallback;
    SkbdAppIndex *apps;
    size_t apps_len;
    size_t apps_cap;
} SkbdRules;

/* Rust: RuleSource. Labels are borrowed. The fallback label can be "--default". */
typedef struct {
    const char *fallback_label;
    const char *apps_label;
} SkbdRuleSource;

/* Binding passed into memory before the names are copied. */
typedef struct {
    const char *app;
    uint32_t index;
} SkbdAppBinding;

void skbd_token_free(SkbdLayoutToken *token);
void skbd_config_free(SkbdConfigFile *file);
void skbd_rules_free(SkbdRules *rules);

/* Rust: default_path. $XDG_CONFIG_HOME/swaylingo/config.toml, or
   ~/.config/swaylingo/config.toml. Empty or relative XDG_CONFIG_HOME is ignored. */
int skbd_config_default_path(char *buf, size_t cap, char *err, size_t err_cap);

/* Rust: config_path. xdg and home may be NULL. An empty or relative xdg is ignored.
   home must be absolute. A base with an empty component or ".." is rejected. */
int skbd_config_path(const char *xdg, const char *home, char *buf, size_t cap, char *err, size_t err_cap);

/* Rust: parse_config. path is only the name used in errors. On failure *out
   is zeroed and freeable. */
int skbd_config_parse(const char *text, const char *path, SkbdConfigFile *out, char *err, size_t err_cap);

/* Rust: load_required. A missing file is an error. */
int skbd_config_load_required(const char *path, SkbdConfigFile *out, char *err, size_t err_cap);

/* Rust: load_optional. A missing file is an empty config. */
int skbd_config_load_optional(const char *path, SkbdConfigFile *out, char *err, size_t err_cap);

/* Rust: resolve_rules. On failure *out is zeroed and freeable. */
int skbd_config_resolve_rules(const SkbdConfigFile *file, const char *const *names, size_t name_count, const SkbdRuleSource *from, SkbdRules *out, char *err, size_t err_cap);

/* Rust: resolve_token. *index is written only on success.
   Order: decimal index (no leading zero), exact name, ASCII case-insensitive
   name, then the short codes us/en, ua/uk, ru, de, pl, fr. us and en prefer
   a name that contains "(us)" and otherwise any English name. */
int skbd_config_resolve_token(const SkbdLayoutToken *token, const char *const *names, size_t name_count, uint32_t *index, char *err, size_t err_cap);

#endif
