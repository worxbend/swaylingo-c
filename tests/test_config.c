#include "skbd/config.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int g_failures;

static const char *LAYOUTS[] = {
    "English (US)",
    "Ukrainian",
    "Russian",
    "German",
    "Polish",
    "French",
};

static void fail(const char *what) {
    fprintf(stderr, "fail: %s\n", what);
    g_failures = 1;
}

static void expect_u32(unsigned got, unsigned want, const char *what) {
    if (got != want) {
        fprintf(stderr, "fail: %s: got %u want %u\n", what, got, want);
        g_failures = 1;
    }
}

static void expect_has(const char *hay, const char *needle, const char *what) {
    if (hay == NULL || strstr(hay, needle) == NULL) {
        fprintf(stderr, "fail: %s\n text: %s\n", what, hay != NULL ? hay : "(null)");
        g_failures = 1;
    }
}

static const SkbdLayoutToken *find_app(const SkbdConfigFile *file, const char *app) {
    size_t i;

    for (i = 0; i < file->apps_len; i++) {
        if (file->apps[i].app != NULL && strcmp(file->apps[i].app, app) == 0) {
            return &file->apps[i].token;
        }
    }
    return NULL;
}

static int rule_index(const SkbdRules *rules, const char *app, uint32_t *index) {
    size_t i;

    for (i = 0; i < rules->apps_len; i++) {
        if (rules->apps[i].app != NULL && strcmp(rules->apps[i].app, app) == 0) {
            *index = rules->apps[i].index;
            return 1;
        }
    }
    return 0;
}

static int resolve_name_token(const char *name, const char *const *names, size_t name_count, uint32_t *index, char *err, size_t err_cap) {
    SkbdLayoutToken token;
    char owned[64];
    int wrote;

    memset(&token, 0, sizeof token);
    wrote = snprintf(owned, sizeof owned, "%s", name);
    if (wrote < 0 || (size_t)wrote >= sizeof owned) {
        fail("name does not fit");
        return -1;
    }
    token.kind = SKBD_TOKEN_NAME;
    token.name = owned;
    wrote = skbd_config_resolve_token(&token, names, name_count, index, err, err_cap);
    token.name = NULL;
    return wrote;
}

static void expect_name_token(const SkbdLayoutToken *token, const char *name, const char *what) {
    if (token == NULL || token->kind != SKBD_TOKEN_NAME || token->name == NULL || strcmp(token->name, name) != 0) {
        fail(what);
    }
}

/* port: bad_token_error_includes_the_known_layout_name */
static void bad_token_error_includes_the_known_layout_name(void) {
    SkbdConfigFile file;
    SkbdRules rules;
    SkbdRuleSource from;
    char err[2048];

    memset(&file, 0, sizeof file);
    memset(&rules, 0, sizeof rules);
    err[0] = '\0';
    from.fallback_label = "skbd.toml";
    from.apps_label = "skbd.toml";
    if (skbd_config_parse("fallback = \"zz\"\n", "skbd.toml", &file, err, sizeof err) != 0) {
        fail(err[0] != '\0' ? err : "bad token parse");
        skbd_config_free(&file);
        return;
    }
    if (skbd_config_resolve_rules(&file, LAYOUTS, 6, &from, &rules, err, sizeof err) == 0) {
        fail("bad token resolved");
    }
    expect_has(err, "skbd.toml", "bad token path");
    expect_has(err, "0 English (US)", "bad token english");
    expect_has(err, "1 Ukrainian", "bad token ukrainian");
    skbd_rules_free(&rules);
    skbd_config_free(&file);
}

/* port: parse_error_names_the_file */
static void parse_error_names_the_file(void) {
    SkbdConfigFile file;
    char err[512];

    memset(&file, 0, sizeof file);
    err[0] = '\0';
    if (skbd_config_parse("fallback = [\n", "/tmp/skbd.toml", &file, err, sizeof err) == 0) {
        fail("parse error succeeded");
    }
    expect_has(err, "/tmp/skbd.toml", "parse error path");
    skbd_config_free(&file);
}

/* port: us_and_en_prefer_a_name_containing_us */
static void us_and_en_prefer_a_name_containing_us(void) {
    const char *layouts[] = {"English", "English (UK)", "English (US)"};
    uint32_t index = 99;
    char err[256];

    err[0] = '\0';
    if (resolve_name_token("us", layouts, 3, &index, err, sizeof err) != 0) {
        fail(err[0] != '\0' ? err : "us prefer");
        return;
    }
    expect_u32(index, 2, "us prefer");
    index = 99;
    if (resolve_name_token("en", layouts, 3, &index, err, sizeof err) != 0) {
        fail(err[0] != '\0' ? err : "en prefer");
        return;
    }
    expect_u32(index, 2, "en prefer");
}

/* port: us_uses_english_when_no_name_contains_us */
static void us_uses_english_when_no_name_contains_us(void) {
    const char *layouts[] = {"English (UK)", "Ukrainian"};
    uint32_t index = 99;
    char err[256];

    err[0] = '\0';
    if (resolve_name_token("us", layouts, 2, &index, err, sizeof err) != 0) {
        fail(err[0] != '\0' ? err : "us english fallback");
        return;
    }
    expect_u32(index, 0, "us english fallback");
}

/* port: short_codes_map_to_language_names */
static void short_codes_map_to_language_names(void) {
    static const struct {
        const char *code;
        unsigned index;
    } codes[] = {
        {"ua", 1},
        {"uk", 1},
        {"ru", 2},
        {"de", 3},
        {"pl", 4},
        {"fr", 5},
    };
    size_t i;

    for (i = 0; i < sizeof codes / sizeof codes[0]; i++) {
        uint32_t index = 99;
        char err[256];

        err[0] = '\0';
        if (resolve_name_token(codes[i].code, LAYOUTS, 6, &index, err, sizeof err) != 0) {
            fail(err[0] != '\0' ? err : codes[i].code);
            continue;
        }
        expect_u32(index, codes[i].index, codes[i].code);
    }
}

/* port: exact_name_and_integer_index_match */
static void exact_name_and_integer_index_match(void) {
    SkbdLayoutToken token;
    uint32_t index = 99;
    char err[1024];

    err[0] = '\0';
    if (resolve_name_token("Ukrainian", LAYOUTS, 6, &index, err, sizeof err) != 0) {
        fail(err[0] != '\0' ? err : "exact name");
    } else {
        expect_u32(index, 1, "exact name");
    }
    memset(&token, 0, sizeof token);
    token.kind = SKBD_TOKEN_INDEX;
    token.index = 1;
    index = 99;
    if (skbd_config_resolve_token(&token, LAYOUTS, 6, &index, err, sizeof err) != 0) {
        fail("index token");
    } else {
        expect_u32(index, 1, "index token");
    }
    index = 99;
    if (resolve_name_token("1", LAYOUTS, 6, &index, err, sizeof err) != 0) {
        fail(err[0] != '\0' ? err : "decimal name");
    } else {
        expect_u32(index, 1, "decimal name");
    }
    token.index = 9;
    err[0] = '\0';
    if (skbd_config_resolve_token(&token, LAYOUTS, 6, &index, err, sizeof err) == 0) {
        fail("index 9 resolved");
    }
    expect_has(err, "3 German", "index 9 known");
}

/* port: exact_name_wins_over_the_short_code */
static void exact_name_wins_over_the_short_code(void) {
    const char *layouts[] = {"us", "English (US)"};
    uint32_t index = 99;
    char err[256];

    err[0] = '\0';
    if (resolve_name_token("us", layouts, 2, &index, err, sizeof err) != 0) {
        fail(err[0] != '\0' ? err : "exact short code");
        return;
    }
    expect_u32(index, 0, "exact short code");
}

/* port: example_file_parses */
static void example_file_parses(void) {
    SkbdConfigFile file;
    char text[4096];
    char err[512];
    FILE *handle;
    size_t nread;

    memset(&file, 0, sizeof file);
    err[0] = '\0';
    handle = fopen("config.example.toml", "r");
    if (handle == NULL) {
        fail("example open");
        return;
    }
    nread = fread(text, 1, sizeof text - 1, handle);
    if (ferror(handle) != 0 || nread == sizeof text - 1) {
        fclose(handle);
        fail("example read");
        return;
    }
    fclose(handle);
    text[nread] = '\0';
    if (strlen(text) != nread) {
        fail("example nul");
        return;
    }
    if (skbd_config_parse(text, "config.example.toml", &file, err, sizeof err) != 0) {
        fail(err[0] != '\0' ? err : "example parse");
        skbd_config_free(&file);
        return;
    }
    if (!file.has_fallback) {
        fail("example fallback missing");
    }
    expect_name_token(&file.fallback, "us", "example fallback");
    expect_name_token(find_app(&file, "Telegram"), "ua", "example Telegram");
    expect_name_token(find_app(&file, "discord"), "ua", "example discord");
    expect_name_token(find_app(&file, "jetbrains-idea"), "us", "example jetbrains-idea");
    expect_name_token(find_app(&file, "Code"), "us", "example Code");
    expect_name_token(find_app(&file, "code"), "us", "example code");
    skbd_config_free(&file);
}

/* port: apps_accept_a_string_or_an_integer */
static void apps_accept_a_string_or_an_integer(void) {
    const char *text = "fallback = 0\n\n[apps]\nTelegram = \"ua\"\nCode = 0\n";
    SkbdConfigFile file;
    SkbdRules rules;
    SkbdRuleSource from;
    uint32_t index = 99;
    char err[1024];

    memset(&file, 0, sizeof file);
    memset(&rules, 0, sizeof rules);
    err[0] = '\0';
    from.fallback_label = "c.toml";
    from.apps_label = "c.toml";
    if (skbd_config_parse(text, "c.toml", &file, err, sizeof err) != 0) {
        fail(err[0] != '\0' ? err : "apps parse");
        skbd_config_free(&file);
        return;
    }
    if (skbd_config_resolve_rules(&file, LAYOUTS, 6, &from, &rules, err, sizeof err) != 0) {
        fail(err[0] != '\0' ? err : "apps resolve");
        skbd_rules_free(&rules);
        skbd_config_free(&file);
        return;
    }
    expect_u32(rules.fallback, 0, "apps fallback");
    if (!rule_index(&rules, "Telegram", &index)) {
        fail("apps Telegram missing");
    } else {
        expect_u32(index, 1, "apps Telegram");
    }
    if (!rule_index(&rules, "Code", &index)) {
        fail("apps Code missing");
    } else {
        expect_u32(index, 0, "apps Code");
    }
    skbd_rules_free(&rules);
    skbd_config_free(&file);
}

/* port: missing_explicit_config_names_the_path */
static void missing_explicit_config_names_the_path(void) {
    SkbdConfigFile file;
    char path[128];
    char err[256];
    int wrote;

    memset(&file, 0, sizeof file);
    err[0] = '\0';
    wrote = snprintf(path, sizeof path, "/tmp/skbd-missing-%ld", (long)getpid());
    if (wrote < 0 || (size_t)wrote >= sizeof path) {
        fail("missing path does not fit");
        return;
    }
    (void)unlink(path);
    if (skbd_config_load_required(path, &file, err, sizeof err) == 0) {
        fail("missing required succeeded");
    }
    expect_has(err, path, "missing required path");
    expect_has(err, "file not found", "missing required text");
    skbd_config_free(&file);
}

/* port: missing_default_path_is_an_empty_config */
static void missing_default_path_is_an_empty_config(void) {
    SkbdConfigFile file;
    char path[128];
    char err[256];
    int wrote;

    memset(&file, 0, sizeof file);
    err[0] = '\0';
    wrote = snprintf(path, sizeof path, "/tmp/skbd-absent-%ld", (long)getpid());
    if (wrote < 0 || (size_t)wrote >= sizeof path) {
        fail("absent path does not fit");
        return;
    }
    (void)unlink(path);
    if (skbd_config_load_optional(path, &file, err, sizeof err) != 0) {
        fail(err[0] != '\0' ? err : "missing optional");
        skbd_config_free(&file);
        return;
    }
    if (file.has_fallback) {
        fail("optional fallback set");
    }
    if (file.apps_len != 0) {
        fail("optional apps not empty");
    }
    skbd_config_free(&file);
}

/* port: config_path_uses_xdg_then_home */
static void config_path_uses_xdg_then_home(void) {
    char buf[256];
    char err[128];

    err[0] = '\0';
    buf[0] = '\0';
    if (skbd_config_path("/xdg", "/home/me", buf, sizeof buf, err, sizeof err) != 0) {
        fail("xdg path rc");
    }
    if (strcmp(buf, "/xdg/swaylingo/config.toml") != 0) {
        fail("xdg path");
    }
    if (skbd_config_path(NULL, "/home/me", buf, sizeof buf, err, sizeof err) != 0) {
        fail("home path rc");
    }
    if (strcmp(buf, "/home/me/.config/swaylingo/config.toml") != 0) {
        fail("home path");
    }
    if (skbd_config_path("", "/home/me", buf, sizeof buf, err, sizeof err) != 0) {
        fail("empty xdg rc");
    }
    if (strcmp(buf, "/home/me/.config/swaylingo/config.toml") != 0) {
        fail("empty xdg path");
    }
    err[0] = '\0';
    if (skbd_config_path(NULL, NULL, buf, sizeof buf, err, sizeof err) == 0) {
        fail("missing HOME rc");
    }
    expect_has(err, "HOME", "missing HOME");
}

static void remove_path(const char *path) {
    if (path != NULL && path[0] != '\0') {
        (void)unlink(path);
    }
}

/* Rust read_to_string follows a symlink and does not reject mode 0664. */
static void group_writable_config_and_symlink_load(void) {
    char dir[128];
    char file[180];
    char link_path[180];
    char err[256];
    SkbdConfigFile loaded;
    const char text[] = "fallback = \"us\"\n";
    ssize_t nwrite;
    int fd;
    int wrote;

    wrote = snprintf(dir, sizeof dir, "/tmp/skbd-cfg-%ld", (long)getpid());
    if (wrote < 0 || (size_t)wrote >= sizeof dir) {
        fail("cfg dir");
        return;
    }
    wrote = snprintf(file, sizeof file, "%s/c.toml", dir);
    if (wrote < 0 || (size_t)wrote >= sizeof file) {
        fail("cfg file");
        return;
    }
    wrote = snprintf(link_path, sizeof link_path, "%s/link.toml", dir);
    if (wrote < 0 || (size_t)wrote >= sizeof link_path) {
        fail("cfg link");
        return;
    }
    remove_path(link_path);
    remove_path(file);
    if (mkdir(dir, 0700) != 0 && errno != EEXIST) {
        fail("cfg mkdir");
        return;
    }
    fd = open(file, O_CREAT | O_WRONLY | O_TRUNC | O_CLOEXEC, 0664);
    if (fd < 0) {
        fail("cfg open");
        rmdir(dir);
        return;
    }
    nwrite = write(fd, text, sizeof text - 1);
    if (nwrite != (ssize_t)(sizeof text - 1)) {
        close(fd);
        fail("cfg write");
        remove_path(file);
        rmdir(dir);
        return;
    }
    if (close(fd) != 0 || chmod(file, 0664) != 0) {
        fail("cfg mode");
        remove_path(file);
        rmdir(dir);
        return;
    }
    memset(&loaded, 0, sizeof loaded);
    err[0] = '\0';
    if (skbd_config_load_required(file, &loaded, err, sizeof err) != 0 || !loaded.has_fallback) {
        fail(err[0] != '\0' ? err : "cfg load");
        skbd_config_free(&loaded);
        remove_path(file);
        rmdir(dir);
        return;
    }
    skbd_config_free(&loaded);
    if (symlink(file, link_path) != 0) {
        fail("cfg symlink");
        remove_path(file);
        rmdir(dir);
        return;
    }
    memset(&loaded, 0, sizeof loaded);
    err[0] = '\0';
    if (skbd_config_load_required(link_path, &loaded, err, sizeof err) != 0 || !loaded.has_fallback) {
        fail(err[0] != '\0' ? err : "cfg symlink load");
    }
    skbd_config_free(&loaded);
    remove_path(link_path);
    remove_path(file);
    rmdir(dir);
}

int main(void) {
    bad_token_error_includes_the_known_layout_name();
    parse_error_names_the_file();
    us_and_en_prefer_a_name_containing_us();
    us_uses_english_when_no_name_contains_us();
    short_codes_map_to_language_names();
    exact_name_and_integer_index_match();
    exact_name_wins_over_the_short_code();
    example_file_parses();
    apps_accept_a_string_or_an_integer();
    missing_explicit_config_names_the_path();
    missing_default_path_is_an_empty_config();
    config_path_uses_xdg_then_home();
    group_writable_config_and_symlink_load();
    return g_failures == 0 ? 0 : 1;
}
