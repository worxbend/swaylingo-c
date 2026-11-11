#define _GNU_SOURCE

#include "skbd/runtime.h"

#include "skbd/util.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

const char skbd_help[] =
    "Remember the Sway keyboard layout for each window.\n"
    "\n"
    "Usage: skbd [options]\n"
    "\n"
    "Options:\n"
    "  -h, --help           Print this help and exit\n"
    "  --version            Print the version and exit\n"
    "  --verbose            Print focus, layout, and switch lines\n"
    "  --config PATH        Config file to read\n"
    "  --default TOKEN      Layout for a new window and an empty desktop\n"
    "\n"
    "The config file is $XDG_CONFIG_HOME/swaylingo/config.toml, or\n"
    "~/.config/swaylingo/config.toml when XDG_CONFIG_HOME is unset. A missing\n"
    "file at that path is an empty config. --config requires the file to exist.\n"
    "--default overrides the fallback key.\n"
    "\n"
    "A token is a layout index, the layout's exact name, or a short code:\n"
    "us, en, ua, uk, ru, de, pl, fr. App keys are a Wayland app_id or an X11\n"
    "WM_CLASS.\n";

const char skbd_version[] = "0.4.0";

volatile sig_atomic_t skbd_reload_requested = 0;

static void on_sighup(int signo) {
    (void)signo;
    skbd_reload_requested = 1;
}

static int copy_text(char *dst, size_t cap, const char *src) {
    size_t len;

    if (dst == NULL || cap == 0 || src == NULL) {
        return -1;
    }
    len = strlen(src);
    if (len >= cap) {
        return -1;
    }
    memcpy(dst, src, len + 1);
    return 0;
}

static void free_names(char **names, size_t count) {
    size_t i;

    if (names == NULL) {
        return;
    }
    for (i = 0; i < count; i++) {
        free(names[i]);
    }
    free(names);
}

static int copy_name_list(char **src, size_t count, char ***out, size_t *out_count, char *err, size_t err_cap) {
    char **copy;
    size_t i;

    *out = NULL;
    *out_count = 0;
    if (count == 0) {
        return 0;
    }
    if (src == NULL || count > SIZE_MAX / sizeof(*copy)) {
        return skbd_err_set(err, err_cap, "layout names are missing");
    }
    copy = calloc(count, sizeof(*copy));
    if (copy == NULL) {
        return skbd_err_set(err, err_cap, "out of memory");
    }
    for (i = 0; i < count; i++) {
        if (src[i] == NULL) {
            free_names(copy, i);
            return skbd_err_set(err, err_cap, "layout name is missing");
        }
        copy[i] = skbd_strdup(src[i]);
        if (copy[i] == NULL) {
            free_names(copy, i);
            return skbd_err_set(err, err_cap, "out of memory");
        }
    }
    *out = copy;
    *out_count = count;
    return 0;
}

static int utf8_ok(const unsigned char *text) {
    while (*text != 0) {
        if (*text <= 0x7F) {
            text++;
            continue;
        }
        if (*text >= 0xC2 && *text <= 0xDF) {
            if ((text[1] & 0xC0) != 0x80) {
                return 0;
            }
            text += 2;
            continue;
        }
        if (*text == 0xE0) {
            if (text[1] < 0xA0 || text[1] > 0xBF || (text[2] & 0xC0) != 0x80) {
                return 0;
            }
            text += 3;
            continue;
        }
        if ((*text >= 0xE1 && *text <= 0xEC) || (*text >= 0xEE && *text <= 0xEF)) {
            if ((text[1] & 0xC0) != 0x80 || (text[2] & 0xC0) != 0x80) {
                return 0;
            }
            text += 3;
            continue;
        }
        if (*text == 0xED) {
            if (text[1] < 0x80 || text[1] > 0x9F || (text[2] & 0xC0) != 0x80) {
                return 0;
            }
            text += 3;
            continue;
        }
        if (*text == 0xF0) {
            if (text[1] < 0x90 || text[1] > 0xBF || (text[2] & 0xC0) != 0x80 || (text[3] & 0xC0) != 0x80) {
                return 0;
            }
            text += 4;
            continue;
        }
        if (*text >= 0xF1 && *text <= 0xF3) {
            if ((text[1] & 0xC0) != 0x80 || (text[2] & 0xC0) != 0x80 || (text[3] & 0xC0) != 0x80) {
                return 0;
            }
            text += 4;
            continue;
        }
        if (*text == 0xF4) {
            if (text[1] < 0x80 || text[1] > 0x8F || (text[2] & 0xC0) != 0x80 || (text[3] & 0xC0) != 0x80) {
                return 0;
            }
            text += 4;
            continue;
        }
        return 0;
    }
    return 1;
}

static int under_systemd(void) {
    const char *invocation;

    invocation = getenv("INVOCATION_ID");
    return invocation != NULL && invocation[0] != '\0';
}

static int input_is_keyboard(const SkbdInput *input) {
    return input != NULL && input->type != NULL && strcmp(input->type, SKBD_INPUT_KEYBOARD) == 0;
}

static int input_index(const SkbdInput *input, uint32_t *index) {
    if (input == NULL || !input->has_layout_index) {
        return 0;
    }
    return skbd_layout_index(&input->layout_index, index);
}

static void note_focus(int verbose, const SkbdHappening *happening) {
    if (!verbose || happening == NULL) {
        return;
    }
    fprintf(stderr, "Focus { id: %" PRId64 ", app: ", happening->id);
    if (happening->app == NULL) {
        fputs("None", stderr);
    } else {
        fprintf(stderr, "Some(\"%s\")", happening->app);
    }
    fputs(", x11: ", stderr);
    if (!happening->has_x11) {
        fputs("None", stderr);
    } else {
        fprintf(stderr, "Some(%" PRId32 ")", happening->x11);
    }
    fputs(", transient_for: ", stderr);
    if (!happening->has_transient_for) {
        fputs("None", stderr);
    } else {
        fprintf(stderr, "Some(%" PRId32 ")", happening->transient_for);
    }
    fputs(" }\n", stderr);
}

static int lock_exclusive(int fd) {
    for (;;) {
        if (flock(fd, LOCK_EX | LOCK_NB) == 0) {
            return 0;
        }
        if (errno != EINTR) {
            return -1;
        }
    }
}

static int lock_name(const char *dir, char *buf, size_t cap) {
    size_t len;
    int wrote;

    if (dir == NULL) {
        dir = "";
    }
    len = strlen(dir);
    while (len > 1 && dir[len - 1] == '/') {
        len--;
    }
    if (len > (size_t)INT_MAX) {
        return -1;
    }
    if (len == 1 && dir[0] == '/') {
        wrote = snprintf(buf, cap, "/skbd.lock");
    } else {
        wrote = snprintf(buf, cap, "%.*s/skbd.lock", (int)len, dir);
    }
    if (wrote < 0 || (size_t)wrote >= cap) {
        return -1;
    }
    return 0;
}

static const SkbdInput *keyboard_with_both(const SkbdInput *inputs, size_t count, uint32_t *active) {
    size_t i;

    for (i = 0; i < count; i++) {
        uint32_t index;

        if (!input_is_keyboard(&inputs[i]) || !input_index(&inputs[i], &index)) {
            continue;
        }
        if (inputs[i].layout_name_count == 0) {
            continue;
        }
        *active = index;
        return &inputs[i];
    }
    return NULL;
}

static const SkbdInput *first_indexed_keyboard(const SkbdInput *inputs, size_t count, uint32_t *active) {
    size_t i;

    for (i = 0; i < count; i++) {
        uint32_t index;

        if (!input_is_keyboard(&inputs[i]) || !input_index(&inputs[i], &index)) {
            continue;
        }
        *active = index;
        return &inputs[i];
    }
    return NULL;
}

static void longest_names(const SkbdInput *inputs, size_t count, char ***names, size_t *name_count) {
    size_t i;
    size_t best;

    best = 0;
    *names = NULL;
    *name_count = 0;
    for (i = 0; i < count; i++) {
        if (!input_is_keyboard(&inputs[i])) {
            continue;
        }
        if (inputs[i].layout_name_count > best) {
            best = inputs[i].layout_name_count;
            *names = inputs[i].layout_names;
            *name_count = best;
        }
    }
}

static void warn_index_disagreement(const SkbdInput *inputs, size_t count, uint32_t chosen) {
    size_t i;

    for (i = 0; i < count; i++) {
        uint32_t index;

        if (!input_is_keyboard(&inputs[i]) || !input_index(&inputs[i], &index)) {
            continue;
        }
        if (index != chosen) {
            fprintf(stderr,
                "keyboards disagree on layout index (%" PRIu32 " vs %" PRIu32 "); using %" PRIu32 "\n",
                chosen, index, chosen);
        }
    }
}

static int fill_keyboard(const SkbdInput *input, uint32_t active, char **names, size_t name_count, SkbdKeyboard *out, char *err, size_t err_cap) {
    memset(out, 0, sizeof *out);
    if (input->identifier == NULL) {
        return skbd_err_set(err, err_cap, "keyboard has no identifier");
    }
    out->identifier = skbd_strdup(input->identifier);
    if (out->identifier == NULL) {
        return skbd_err_set(err, err_cap, "out of memory");
    }
    out->active = active;
    if (copy_name_list(names, name_count, &out->names, &out->name_count, err, err_cap) != 0) {
        skbd_keyboard_free(out);
        return -1;
    }
    return 0;
}

static int walk_views(SkbdMemory *memory, const SkbdNode *node, char *err, size_t err_cap) {
    size_t i;
    int32_t x11;
    const int32_t *x11_ptr;

    if (node == NULL) {
        return 0;
    }
    if (skbd_node_is_view(node)) {
        x11_ptr = NULL;
        if (skbd_x11_id(node, &x11)) {
            x11_ptr = &x11;
        }
        if (skbd_memory_note_existing(memory, node->id, x11_ptr, err, err_cap) != 0) {
            return -1;
        }
    }
    if (node->nodes != NULL) {
        for (i = 0; i < node->nodes_len; i++) {
            if (walk_views(memory, &node->nodes[i], err, err_cap) != 0) {
                return -1;
            }
        }
    }
    if (node->floating_nodes != NULL) {
        for (i = 0; i < node->floating_len; i++) {
            if (walk_views(memory, &node->floating_nodes[i], err, err_cap) != 0) {
                return -1;
            }
        }
    }
    return 0;
}

static int apply_watched_input(SkbdMemory *memory, SkbdInputChange change, const SkbdInput *input, int verbose, SkbdAction *action, char *err, size_t err_cap) {
    SkbdHappening happening;
    uint32_t index;

    action->kind = SKBD_ACTION_NONE;
    action->index = 0;
    if (memory == NULL || input == NULL || input->type == NULL) {
        return 0;
    }
    if (!input_is_keyboard(input) || input->identifier == NULL) {
        return 0;
    }
    if (strcmp(input->identifier, skbd_memory_keyboard_id(memory)) != 0) {
        return 0;
    }
    memset(&happening, 0, sizeof happening);
    if (change == SKBD_INPUT_XKB_KEYMAP) {
        happening.kind = SKBD_HAPPEN_KEYMAP_RESET;
        if (input_index(input, &index)) {
            happening.has_index = 1;
            happening.index = index;
        }
        happening.names = (const char *const *)input->layout_names;
        happening.name_count = input->layout_name_count;
    } else if (change == SKBD_INPUT_XKB_LAYOUT) {
        if (!input_index(input, &index)) {
            return 0;
        }
        if (verbose) {
            fprintf(stderr, "layout %" PRIu32 "\n", index);
        }
        happening.kind = SKBD_HAPPEN_LAYOUT;
        happening.has_index = 1;
        happening.index = index;
    } else {
        return 0;
    }
    return skbd_memory_apply(memory, &happening, action, err, err_cap);
}

int skbd_install_sighup(char *err, size_t err_cap) {
    struct sigaction action;
    sigset_t blocked;

    memset(&action, 0, sizeof action);
    if (sigemptyset(&action.sa_mask) != 0) {
        return skbd_err_fmt(err, err_cap, "sigemptyset failed: %s", strerror(errno));
    }
    action.sa_handler = on_sighup;
    action.sa_flags = SA_RESTART;
    if (sigaction(SIGHUP, &action, NULL) != 0) {
        return skbd_err_fmt(err, err_cap, "SIGHUP handler failed: %s", strerror(errno));
    }
    /* Blocked here so SA_RESTART cannot swallow SIGHUP inside read or write.
       skbd_wait_readable unblocks it only for ppoll, which is not restarted. */
    if (sigemptyset(&blocked) != 0 || sigaddset(&blocked, SIGHUP) != 0 || sigprocmask(SIG_BLOCK, &blocked, NULL) != 0) {
        return skbd_err_fmt(err, err_cap, "SIGHUP block failed: %s", strerror(errno));
    }
    return 0;
}

int skbd_reload_pending(void) {
    return skbd_reload_requested != 0;
}

int skbd_wait_readable(int fd, int *ready, char *err, size_t err_cap) {
    struct pollfd polled;
    sigset_t mask;
    int rc;

    if (ready == NULL) {
        return skbd_err_set(err, err_cap, "wait output is missing");
    }
    *ready = 0;
    if (fd < 0) {
        return skbd_err_set(err, err_cap, "sway event socket is closed");
    }
    if (sigprocmask(SIG_SETMASK, NULL, &mask) != 0 || sigdelset(&mask, SIGHUP) != 0) {
        return skbd_err_fmt(err, err_cap, "sway event wait failed: %s", strerror(errno));
    }
    polled.fd = fd;
    polled.events = POLLIN;
    polled.revents = 0;
    do {
        rc = ppoll(&polled, 1, NULL, &mask);
    } while (rc < 0 && errno == EINTR && skbd_reload_requested == 0);
    if (rc < 0) {
        if (errno == EINTR) {
            return 0;
        }
        return skbd_err_fmt(err, err_cap, "sway event wait failed: %s", strerror(errno));
    }
    if ((polled.revents & (POLLERR | POLLNVAL)) != 0) {
        return skbd_err_set(err, err_cap, "sway event socket is closed");
    }
    if ((polled.revents & POLLIN) != 0) {
        *ready = 1;
        return 0;
    }
    if ((polled.revents & POLLHUP) != 0) {
        return skbd_err_set(err, err_cap, "sway event socket is closed");
    }
    return 0;
}

void skbd_parsed_free(SkbdParsed *parsed) {
    if (parsed == NULL) {
        return;
    }
    free(parsed->options.config_path);
    free(parsed->options.default_layout);
    memset(parsed, 0, sizeof *parsed);
}

void skbd_keyboard_free(SkbdKeyboard *keyboard) {
    size_t i;

    if (keyboard == NULL) {
        return;
    }
    free(keyboard->identifier);
    if (keyboard->names != NULL) {
        for (i = 0; i < keyboard->name_count; i++) {
            free(keyboard->names[i]);
        }
        free(keyboard->names);
    }
    memset(keyboard, 0, sizeof *keyboard);
}

void skbd_lock_close(SkbdLock *lock) {
    if (lock == NULL) {
        return;
    }
    if (lock->fd >= 0) {
        close(lock->fd);
        lock->fd = -1;
    }
}

int skbd_parse_args(int argc, char **argv, SkbdParsed *out, char *err, size_t err_cap) {
    int i;
    int failed;

    if (out == NULL) {
        return skbd_err_set(err, err_cap, "arguments are missing");
    }
    memset(out, 0, sizeof *out);
    if (argv == NULL) {
        out->kind = SKBD_PARSED_RUN;
        return 0;
    }
    for (i = 1; i < argc; i++) {
        const char *arg;
        const char *value;
        char *copy;

        arg = argv[i];
        if (arg == NULL) {
            failed = skbd_err_set(err, err_cap, "unknown argument: ");
            skbd_parsed_free(out);
            return failed;
        }
        if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            skbd_parsed_free(out);
            out->kind = SKBD_PARSED_HELP;
            return 0;
        }
        if (strcmp(arg, "--version") == 0) {
            skbd_parsed_free(out);
            out->kind = SKBD_PARSED_VERSION;
            return 0;
        }
        if (strcmp(arg, "--verbose") == 0) {
            out->options.verbose = 1;
            continue;
        }
        if (strcmp(arg, "--config") != 0 && strcmp(arg, "--default") != 0) {
            failed = skbd_err_fmt(err, err_cap, "unknown argument: %s", arg);
            skbd_parsed_free(out);
            return failed;
        }
        if (i + 1 >= argc || argv[i + 1] == NULL || argv[i + 1][0] == '\0') {
            failed = skbd_err_fmt(err, err_cap, "%s needs a value", arg);
            skbd_parsed_free(out);
            return failed;
        }
        value = argv[++i];
        copy = skbd_strdup(value);
        if (copy == NULL) {
            failed = skbd_err_set(err, err_cap, "out of memory");
            skbd_parsed_free(out);
            return failed;
        }
        if (strcmp(arg, "--config") == 0) {
            free(out->options.config_path);
            out->options.config_path = copy;
        } else {
            free(out->options.default_layout);
            out->options.default_layout = copy;
        }
    }
    out->kind = SKBD_PARSED_RUN;
    return 0;
}

int skbd_app_key(const char *app_id, const char *class_name, char *out, size_t out_cap) {
    const char *key;

    key = NULL;
    if (app_id != NULL && app_id[0] != '\0') {
        key = app_id;
    } else if (class_name != NULL && class_name[0] != '\0') {
        key = class_name;
    }
    if (key == NULL) {
        return 0;
    }
    if (copy_text(out, out_cap, key) != 0) {
        return -1;
    }
    return 1;
}

int skbd_node_is_view(const SkbdNode *node) {
    int container;

    if (node == NULL || node->type == NULL) {
        return 0;
    }
    container = strcmp(node->type, SKBD_NODE_CON) == 0 || strcmp(node->type, SKBD_NODE_FLOATING_CON) == 0;
    if (!container) {
        return 0;
    }
    return node->app_id != NULL || node->has_window || node->has_pid;
}

int skbd_layout_index(const int32_t *index, uint32_t *out) {
    if (index == NULL || *index < 0 || out == NULL) {
        return 0;
    }
    *out = (uint32_t)*index;
    return 1;
}

int skbd_x11_id(const SkbdNode *node, int32_t *out) {
    if (node == NULL || out == NULL || !node->has_window) {
        return 0;
    }
    if (node->window < INT32_MIN || node->window > INT32_MAX) {
        return 0;
    }
    *out = (int32_t)node->window;
    return 1;
}

int skbd_transient_parent(const SkbdNode *node, int32_t *out) {
    if (node == NULL || out == NULL || !node->has_transient_for) {
        return 0;
    }
    *out = node->transient_for;
    return 1;
}

int skbd_focus_happening(const SkbdNode *node, SkbdHappening *out) {
    int32_t x11;
    int32_t parent;

    if (node == NULL || out == NULL) {
        return -1;
    }
    memset(out, 0, sizeof *out);
    out->kind = SKBD_HAPPEN_FOCUS;
    out->id = node->id;
    if (node->app_id != NULL && node->app_id[0] != '\0') {
        out->app = node->app_id;
    } else if (node->class_name != NULL && node->class_name[0] != '\0') {
        out->app = node->class_name;
    }
    if (skbd_x11_id(node, &x11)) {
        out->has_x11 = 1;
        out->x11 = x11;
    }
    if (skbd_transient_parent(node, &parent)) {
        out->has_transient_for = 1;
        out->transient_for = parent;
    }
    return 0;
}

const SkbdNode *skbd_focused_view(const SkbdNode *node) {
    size_t i;
    const SkbdNode *found;

    if (node == NULL) {
        return NULL;
    }
    if (node->focused && skbd_node_is_view(node)) {
        return node;
    }
    if (node->nodes != NULL) {
        for (i = 0; i < node->nodes_len; i++) {
            found = skbd_focused_view(&node->nodes[i]);
            if (found != NULL) {
                return found;
            }
        }
    }
    if (node->floating_nodes != NULL) {
        for (i = 0; i < node->floating_len; i++) {
            found = skbd_focused_view(&node->floating_nodes[i]);
            if (found != NULL) {
                return found;
            }
        }
    }
    return NULL;
}

const SkbdNode *skbd_focused_in_workspace(const SkbdNode *current) {
    if (current == NULL) {
        return NULL;
    }
    if (current->nodes_len == 0 && current->floating_len == 0) {
        return NULL;
    }
    return skbd_focused_view(current);
}

int skbd_first_keyboard(const SkbdInput *inputs, size_t count, SkbdKeyboard *out, char *err, size_t err_cap) {
    const SkbdInput *chosen;
    char **names;
    size_t name_count;
    uint32_t active;

    if (out == NULL) {
        return skbd_err_set(err, err_cap, "keyboard output is missing");
    }
    memset(out, 0, sizeof *out);
    if (inputs == NULL) {
        count = 0;
    }
    names = NULL;
    name_count = 0;
    chosen = keyboard_with_both(inputs, count, &active);
    if (chosen == NULL) {
        chosen = first_indexed_keyboard(inputs, count, &active);
        if (chosen != NULL) {
            longest_names(inputs, count, &names, &name_count);
        }
    } else {
        names = chosen->layout_names;
        name_count = chosen->layout_name_count;
    }
    if (chosen == NULL) {
        return skbd_err_set(err, err_cap, "no keyboard layout from sway");
    }
    if (fill_keyboard(chosen, active, names, name_count, out, err, err_cap) != 0) {
        return -1;
    }
    warn_index_disagreement(inputs, count, out->active);
    return 0;
}

SkbdAction skbd_apply_input(SkbdMemory *memory, SkbdInputChange change, const SkbdInput *input, int verbose) {
    SkbdAction action;
    char err[256];

    action.kind = SKBD_ACTION_NONE;
    action.index = 0;
    if (apply_watched_input(memory, change, input, verbose, &action, err, sizeof err) != 0) {
        action.kind = SKBD_ACTION_NONE;
        action.index = 0;
    }
    return action;
}

int skbd_classify_swaysock(SkbdSockKind kind, const char *path, int service, char *path_out, size_t path_cap, SkbdStopKind *stop, char *err, size_t err_cap) {
    const char *message;
    SkbdStopKind how;

    how = SKBD_STOP_FAIL;
    switch (kind) {
    case SKBD_SOCK_MISSING:
        message = "SWAYSOCK is not set";
        how = service ? SKBD_STOP_IDLE : SKBD_STOP_FAIL;
        break;
    case SKBD_SOCK_EMPTY:
        message = "SWAYSOCK is empty";
        how = service ? SKBD_STOP_IDLE : SKBD_STOP_FAIL;
        break;
    case SKBD_SOCK_NOT_UTF8:
        message = "SWAYSOCK is not valid UTF-8";
        break;
    case SKBD_SOCK_PATH:
        if (path == NULL || copy_text(path_out, path_cap, path) != 0) {
            if (stop != NULL) {
                *stop = SKBD_STOP_FAIL;
            }
            return skbd_err_set(err, err_cap, "SWAYSOCK path does not fit");
        }
        if (stop != NULL) {
            *stop = SKBD_STOP_OK;
        }
        return 0;
    default:
        message = "SWAYSOCK is not set";
        break;
    }
    if (stop != NULL) {
        *stop = how;
    }
    return skbd_err_set(err, err_cap, message);
}

int skbd_ensure_socket(const char *path, char *err, size_t err_cap) {
    return skbd_ipc_check_socket(path, err, err_cap);
}

int skbd_refuse_root(uid_t euid, char *err, size_t err_cap) {
    if (euid == 0) {
        return skbd_err_set(err, err_cap, "refusing to run as root. The unit is a systemd user service");
    }
    return 0;
}

int skbd_require_runtime_dir(const char *raw, char *err, size_t err_cap) {
    if (raw == NULL) {
        return skbd_err_set(err, err_cap, "XDG_RUNTIME_DIR is unset");
    }
    if (raw[0] == '\0') {
        return skbd_err_set(err, err_cap, "XDG_RUNTIME_DIR is empty");
    }
    if (raw[0] != '/') {
        return skbd_err_set(err, err_cap, "XDG_RUNTIME_DIR is not absolute");
    }
    return 0;
}

int skbd_check_runtime_dir(const char *dir, uid_t euid, char *err, size_t err_cap) {
    int fd = -1;

    if (skbd_open_private_dir(dir, euid, "XDG_RUNTIME_DIR", &fd, err, err_cap) != 0) {
        return -1;
    }
    close(fd);
    return 0;
}

int skbd_lock_runtime_dir(const char *dir, SkbdLock *out, char *err, size_t err_cap) {
    char shown[PATH_MAX];
    int dir_fd = -1;
    int fd;
    int saved;
    struct stat st;
    uid_t euid;

    if (out == NULL) {
        return skbd_err_set(err, err_cap, "lock is missing");
    }
    out->fd = -1;
    euid = geteuid();
    if (skbd_open_private_dir(dir, euid, "XDG_RUNTIME_DIR", &dir_fd, err, err_cap) != 0) {
        return -1;
    }
    if (lock_name(dir, shown, sizeof shown) != 0) {
        close(dir_fd);
        return skbd_err_set(err, err_cap, "lock path does not fit");
    }
    fd = openat(dir_fd, "skbd.lock", O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK, 0600);
    saved = errno;
    close(dir_fd);
    if (fd < 0) {
        return skbd_err_fmt(err, err_cap, "cannot open %s: %s", shown, strerror(saved));
    }
    if (fstat(fd, &st) != 0) {
        saved = errno;
        close(fd);
        return skbd_err_fmt(err, err_cap, "cannot stat %s: %s", shown, strerror(saved));
    }
    if (!S_ISREG(st.st_mode)) {
        close(fd);
        return skbd_err_fmt(err, err_cap, "lock file is not a regular file: %s", shown);
    }
    if (st.st_uid != euid) {
        close(fd);
        return skbd_err_fmt(err, err_cap, "lock file is not owned by this user: %s", shown);
    }
    if (fchmod(fd, 0600) != 0) {
        saved = errno;
        close(fd);
        return skbd_err_fmt(err, err_cap, "cannot open %s: %s", shown, strerror(saved));
    }
    if (lock_exclusive(fd) != 0) {
        saved = errno;
        close(fd);
        if (saved == EAGAIN || saved == EWOULDBLOCK) {
            return skbd_err_set(err, err_cap, "skbd: already running");
        }
        return skbd_err_fmt(err, err_cap, "cannot lock %s: %s", shown, strerror(saved));
    }
    out->fd = fd;
    return 0;
}

int skbd_close_window(SkbdMemory *memory, int64_t id) {
    int64_t focused;
    int was_focused;
    SkbdHappening happening;
    SkbdAction action;
    char err[128];

    if (memory == NULL) {
        return 0;
    }
    was_focused = skbd_memory_focused_id(memory, &focused) && focused == id;
    memset(&happening, 0, sizeof happening);
    happening.kind = SKBD_HAPPEN_CLOSE;
    happening.id = id;
    action.kind = SKBD_ACTION_NONE;
    action.index = 0;
    (void)skbd_memory_apply(memory, &happening, &action, err, sizeof err);
    return was_focused;
}

int skbd_remember_open_views(SkbdMemory *memory, const SkbdNode *root, char *err, size_t err_cap) {
    const SkbdNode *focused;

    if (memory == NULL) {
        return skbd_err_set(err, err_cap, "memory is null");
    }
    if (root == NULL) {
        return 0;
    }
    if (walk_views(memory, root, err, err_cap) != 0) {
        return -1;
    }
    focused = skbd_focused_view(root);
    if (focused == NULL) {
        return 0;
    }
    return skbd_memory_remember_current(memory, focused->id, err, err_cap);
}

int skbd_adopt_next_keyboard(SkbdMemory *memory, const SkbdInput *inputs, size_t count, char *id_out, size_t id_cap, char *err, size_t err_cap) {
    const char *watched;
    size_t i;
    size_t len;

    if (id_out != NULL && id_cap > 0) {
        id_out[0] = '\0';
    }
    if (memory == NULL || id_out == NULL || id_cap == 0) {
        return skbd_err_set(err, err_cap, "keyboard adopt is missing");
    }
    if (inputs == NULL) {
        count = 0;
    }
    watched = skbd_memory_keyboard_id(memory);
    for (i = 0; i < count; i++) {
        uint32_t index;
        const char *identifier;

        if (!input_is_keyboard(&inputs[i])) {
            continue;
        }
        identifier = inputs[i].identifier;
        if (identifier == NULL || strcmp(identifier, watched) == 0) {
            continue;
        }
        if (!input_index(&inputs[i], &index) || inputs[i].layout_name_count == 0) {
            continue;
        }
        len = strlen(identifier);
        if (len >= id_cap) {
            return skbd_err_set(err, err_cap, "keyboard id does not fit");
        }
        if (skbd_memory_watch_keyboard(memory, identifier, err, err_cap) != 0) {
            return -1;
        }
        memcpy(id_out, identifier, len + 1);
        return 1;
    }
    return 0;
}

int skbd_overridden_fallback(SkbdConfigFile *file, const char *default_layout, const char *source_label, char *label, size_t label_cap, char *err, size_t err_cap) {
    const char *text;
    char *name;

    if (file == NULL || label == NULL) {
        return skbd_err_set(err, err_cap, "fallback override is missing");
    }
    text = default_layout != NULL ? "--default" : (source_label != NULL ? source_label : "");
    if (copy_text(label, label_cap, text) != 0) {
        return skbd_err_set(err, err_cap, "fallback label does not fit");
    }
    if (default_layout == NULL) {
        return 0;
    }
    name = skbd_strdup(default_layout);
    if (name == NULL) {
        label[0] = '\0';
        return skbd_err_set(err, err_cap, "out of memory");
    }
    skbd_token_free(&file->fallback);
    file->has_fallback = 1;
    file->fallback.kind = SKBD_TOKEN_NAME;
    file->fallback.name = name;
    file->fallback.index = 0;
    return 0;
}

static int sway_socket_path(char **path_out, SkbdStopKind *stop, char *err, size_t err_cap) {
    const char *raw;
    SkbdSockKind kind;
    size_t len;
    char *path;

    *path_out = NULL;
    raw = getenv("SWAYSOCK");
    if (raw == NULL) {
        kind = SKBD_SOCK_MISSING;
    } else if (raw[0] == '\0') {
        kind = SKBD_SOCK_EMPTY;
    } else if (!utf8_ok((const unsigned char *)raw)) {
        kind = SKBD_SOCK_NOT_UTF8;
    } else {
        kind = SKBD_SOCK_PATH;
    }
    if (kind != SKBD_SOCK_PATH) {
        return skbd_classify_swaysock(kind, NULL, under_systemd(), NULL, 0, stop, err, err_cap);
    }
    len = strlen(raw);
    path = malloc(len + 1);
    if (path == NULL) {
        if (stop != NULL) {
            *stop = SKBD_STOP_FAIL;
        }
        return skbd_err_set(err, err_cap, "out of memory");
    }
    if (skbd_classify_swaysock(kind, raw, under_systemd(), path, len + 1, stop, err, err_cap) != 0) {
        free(path);
        return -1;
    }
    *path_out = path;
    return 0;
}

static int open_sway(SkbdIpc *ipc, SkbdStopKind *stop, char *err, size_t err_cap) {
    char *path;

    ipc->fd = -1;
    path = NULL;
    if (sway_socket_path(&path, stop, err, err_cap) != 0) {
        return -1;
    }
    if (skbd_ensure_socket(path, err, err_cap) != 0) {
        free(path);
        *stop = SKBD_STOP_FAIL;
        return -1;
    }
    if (unsetenv("I3SOCK") != 0) {
        free(path);
        *stop = SKBD_STOP_FAIL;
        return skbd_err_fmt(err, err_cap, "cannot clear I3SOCK: %s", strerror(errno));
    }
    if (skbd_ipc_connect(ipc, path, err, err_cap) != 0) {
        free(path);
        *stop = SKBD_STOP_FAIL;
        return -1;
    }
    free(path);
    return 0;
}

static int read_config_source(const SkbdOptions *options, char **source_out, SkbdConfigFile *file, SkbdStopKind *stop, char *err, size_t err_cap) {
    char fallback_path[PATH_MAX];
    const char *path;

    *source_out = NULL;
    memset(file, 0, sizeof *file);
    if (options->config_path != NULL) {
        path = options->config_path;
        if (skbd_config_load_required(path, file, err, err_cap) != 0) {
            *stop = SKBD_STOP_USAGE;
            return -1;
        }
    } else {
        if (skbd_config_default_path(fallback_path, sizeof fallback_path, err, err_cap) != 0) {
            *stop = SKBD_STOP_FAIL;
            return -1;
        }
        path = fallback_path;
        if (skbd_config_load_optional(path, file, err, err_cap) != 0) {
            *stop = SKBD_STOP_USAGE;
            return -1;
        }
    }
    *source_out = skbd_strdup(path);
    if (*source_out == NULL) {
        skbd_config_free(file);
        *stop = SKBD_STOP_FAIL;
        return skbd_err_set(err, err_cap, "out of memory");
    }
    return 0;
}

static int take_fallback_label(SkbdConfigFile *file, const char *default_layout, const char *source, char **label_out, char *err, size_t err_cap) {
    const char *text;
    size_t cap;
    char *label;

    *label_out = NULL;
    text = default_layout != NULL ? "--default" : (source != NULL ? source : "");
    cap = strlen(text) + 1;
    label = malloc(cap);
    if (label == NULL) {
        return skbd_err_set(err, err_cap, "out of memory");
    }
    if (skbd_overridden_fallback(file, default_layout, source, label, cap, err, err_cap) != 0) {
        free(label);
        return -1;
    }
    *label_out = label;
    return 0;
}

static int names_for_reload(const SkbdInput *inputs, size_t count, const char *watched, char ***out_names, size_t *out_count, char *err, size_t err_cap) {
    size_t i;
    SkbdKeyboard keyboard;

    *out_names = NULL;
    *out_count = 0;
    if (inputs == NULL) {
        count = 0;
    }
    for (i = 0; i < count; i++) {
        if (inputs[i].identifier == NULL || watched == NULL) {
            continue;
        }
        if (strcmp(inputs[i].identifier, watched) != 0 || inputs[i].layout_name_count == 0) {
            continue;
        }
        return copy_name_list(inputs[i].layout_names, inputs[i].layout_name_count, out_names, out_count, err, err_cap);
    }
    memset(&keyboard, 0, sizeof keyboard);
    if (skbd_first_keyboard(inputs, count, &keyboard, err, err_cap) != 0) {
        return -1;
    }
    *out_names = keyboard.names;
    *out_count = keyboard.name_count;
    keyboard.names = NULL;
    keyboard.name_count = 0;
    skbd_keyboard_free(&keyboard);
    return 0;
}

static int switch_if_needed(SkbdIpc *commands, SkbdMemory *memory, SkbdAction action, int verbose, SkbdStopKind *stop, char *err, size_t err_cap) {
    int accepted;

    if (action.kind != SKBD_ACTION_SWITCH) {
        return 0;
    }
    if (verbose) {
        fprintf(stderr, "switch %" PRIu32 "\n", action.index);
    }
    accepted = 0;
    if (skbd_ipc_switch_layout(commands, action.index, &accepted, err, err_cap) != 0) {
        *stop = SKBD_STOP_FAIL;
        return -1;
    }
    if (!accepted) {
        skbd_memory_abandon_switch(memory);
    }
    return 0;
}

static int sync_focus(SkbdIpc *commands, SkbdMemory *memory, int verbose, SkbdAction *action, SkbdStopKind *stop, char *err, size_t err_cap) {
    SkbdNode *tree;
    const SkbdNode *view;
    SkbdHappening happening;
    int rc;

    tree = NULL;
    if (skbd_ipc_get_tree(commands, &tree, err, err_cap) != 0) {
        skbd_node_free(tree);
        *stop = SKBD_STOP_FAIL;
        return -1;
    }
    memset(&happening, 0, sizeof happening);
    view = skbd_focused_view(tree);
    if (view != NULL) {
        skbd_focus_happening(view, &happening);
        note_focus(verbose, &happening);
    } else {
        happening.kind = SKBD_HAPPEN_UNFOCUS;
        if (verbose) {
            fputs("Unfocus\n", stderr);
        }
    }
    rc = skbd_memory_apply(memory, &happening, action, err, err_cap);
    skbd_node_free(tree);
    if (rc != 0) {
        *stop = SKBD_STOP_FAIL;
    }
    return rc;
}

static int watched_keyboard_removed(const SkbdMemory *memory, const SkbdEvent *event) {
    const char *watched;

    if (event->kind != SKBD_EV_INPUT || event->input_change != SKBD_INPUT_REMOVED) {
        return 0;
    }
    if (!event->has_input || event->input.identifier == NULL) {
        return 0;
    }
    watched = skbd_memory_keyboard_id(memory);
    return strcmp(event->input.identifier, watched) == 0;
}

static void replace_watched_keyboard(SkbdIpc *commands, SkbdMemory *memory, int verbose) {
    char err[1024];
    char id[1024];
    SkbdInput *inputs;
    size_t count;
    int adopted;

    inputs = NULL;
    count = 0;
    if (skbd_ipc_get_inputs(commands, &inputs, &count, err, sizeof err) != 0) {
        fprintf(stderr, "could not read keyboards: %s\n", err);
        skbd_inputs_free(inputs, count);
        return;
    }
    adopted = skbd_adopt_next_keyboard(memory, inputs, count, id, sizeof id, err, sizeof err);
    if (adopted < 0) {
        fprintf(stderr, "could not read keyboards: %s\n", err);
    } else if (adopted == 1 && verbose) {
        fprintf(stderr, "keyboard %s\n", id);
    }
    skbd_inputs_free(inputs, count);
}

static int react_window_focus(SkbdMemory *memory, const SkbdEvent *event, int verbose, SkbdAction *action, SkbdStopKind *stop, char *err, size_t err_cap) {
    SkbdHappening happening;
    int rc;

    if (!event->has_container || event->window_change != SKBD_WINDOW_FOCUS || !skbd_node_is_view(&event->container)) {
        return 0;
    }
    if (skbd_focus_happening(&event->container, &happening) != 0) {
        *stop = SKBD_STOP_FAIL;
        return skbd_err_set(err, err_cap, "focus event is missing");
    }
    note_focus(verbose, &happening);
    rc = skbd_memory_apply(memory, &happening, action, err, err_cap);
    if (rc != 0) {
        *stop = SKBD_STOP_FAIL;
    }
    return rc;
}

static int react(SkbdIpc *commands, SkbdMemory *memory, const SkbdEvent *event, int verbose, SkbdAction *action, SkbdStopKind *stop, char *err, size_t err_cap) {
    const SkbdNode *current;
    const SkbdNode *view;
    SkbdHappening happening;
    int rc;

    action->kind = SKBD_ACTION_NONE;
    action->index = 0;
    if (watched_keyboard_removed(memory, event)) {
        replace_watched_keyboard(commands, memory, verbose);
        return 0;
    }
    if (event->kind == SKBD_EV_WORKSPACE && event->workspace_change == SKBD_WORKSPACE_FOCUS) {
        current = event->has_workspace ? &event->workspace : NULL;
        view = skbd_focused_in_workspace(current);
        if (view == NULL) {
            return sync_focus(commands, memory, verbose, action, stop, err, err_cap);
        }
        if (skbd_focus_happening(view, &happening) != 0) {
            *stop = SKBD_STOP_FAIL;
            return skbd_err_set(err, err_cap, "focus event is missing");
        }
        note_focus(verbose, &happening);
        rc = skbd_memory_apply(memory, &happening, action, err, err_cap);
        if (rc != 0) {
            *stop = SKBD_STOP_FAIL;
        }
        return rc;
    }
    if (event->kind == SKBD_EV_WINDOW && event->window_change == SKBD_WINDOW_CLOSE && event->has_container &&
        skbd_node_is_view(&event->container)) {
        if (verbose) {
            fprintf(stderr, "Close { id: %" PRId64 " }\n", event->container.id);
        }
        if (skbd_close_window(memory, event->container.id)) {
            return sync_focus(commands, memory, verbose, action, stop, err, err_cap);
        }
        return 0;
    }
    if (event->kind == SKBD_EV_WINDOW) {
        return react_window_focus(memory, event, verbose, action, stop, err, err_cap);
    }
    if (event->kind == SKBD_EV_INPUT && event->has_input) {
        rc = apply_watched_input(memory, event->input_change, &event->input, verbose, action, err, err_cap);
        if (rc != 0) {
            *stop = SKBD_STOP_FAIL;
        }
        return rc;
    }
    return 0;
}

static int try_reload(SkbdIpc *commands, SkbdMemory *memory, const SkbdOptions *options, char *err, size_t err_cap) {
    SkbdConfigFile file;
    char *source;
    char *label;
    SkbdInput *inputs;
    size_t count;
    char **names;
    size_t name_count;
    SkbdRuleSource from;
    SkbdStopKind stop;
    int rc;

    memset(&file, 0, sizeof file);
    source = NULL;
    label = NULL;
    inputs = NULL;
    names = NULL;
    count = 0;
    name_count = 0;
    stop = SKBD_STOP_FAIL;
    rc = -1;
    if (read_config_source(options, &source, &file, &stop, err, err_cap) != 0) {
        goto done;
    }
    if (take_fallback_label(&file, options->default_layout, source, &label, err, err_cap) != 0) {
        goto done;
    }
    if (skbd_ipc_get_inputs(commands, &inputs, &count, err, err_cap) != 0) {
        goto done;
    }
    if (names_for_reload(inputs, count, skbd_memory_keyboard_id(memory), &names, &name_count, err, err_cap) != 0) {
        goto done;
    }
    from.fallback_label = label;
    from.apps_label = source;
    rc = skbd_memory_reload_rules(memory, &file, (const char *const *)names, name_count, &from, err, err_cap);
done:
    free_names(names, name_count);
    skbd_inputs_free(inputs, count);
    free(label);
    free(source);
    skbd_config_free(&file);
    return rc;
}

static void maybe_reload(SkbdIpc *commands, SkbdMemory *memory, const SkbdOptions *options) {
    char err[4096];

    if (!skbd_reload_requested) {
        return;
    }
    skbd_reload_requested = 0;
    err[0] = '\0';
    if (try_reload(commands, memory, options, err, sizeof err) != 0) {
        fprintf(stderr, "config reload failed: %s\n", err);
        return;
    }
    if (options->verbose) {
        fputs("config reloaded\n", stderr);
    }
}

static int seed(SkbdIpc *commands, int verbose, const SkbdConfigFile *file, const char *fallback_label, const char *source_label, SkbdMemory *memory, SkbdStopKind *stop, char *err, size_t err_cap) {
    SkbdInput *inputs;
    size_t input_count;
    SkbdKeyboard keyboard;
    SkbdRules rules;
    SkbdAppBinding *bindings;
    SkbdNode *tree;
    SkbdRuleSource from;
    SkbdAction action;
    SkbdHappening happening;
    int64_t focused_id;
    size_t i;
    int rc;

    inputs = NULL;
    tree = NULL;
    bindings = NULL;
    input_count = 0;
    rc = -1;
    memset(&keyboard, 0, sizeof keyboard);
    memset(&rules, 0, sizeof rules);
    memset(&action, 0, sizeof action);
    memset(memory, 0, sizeof *memory);
    *stop = SKBD_STOP_FAIL;
    if (skbd_ipc_get_inputs(commands, &inputs, &input_count, err, err_cap) != 0) {
        goto done;
    }
    if (skbd_first_keyboard(inputs, input_count, &keyboard, err, err_cap) != 0) {
        goto done;
    }
    from.fallback_label = fallback_label;
    from.apps_label = source_label;
    if (skbd_config_resolve_rules(file, (const char *const *)keyboard.names, keyboard.name_count, &from, &rules, err, err_cap) != 0) {
        *stop = SKBD_STOP_USAGE;
        goto done;
    }
    if (verbose) {
        fprintf(stderr, "config %s\n", source_label);
        fprintf(stderr, "active layout %" PRIu32 "\n", keyboard.active);
        fprintf(stderr, "fallback %" PRIu32 "\n", rules.fallback);
    }
    if (rules.apps_len > 0) {
        bindings = calloc(rules.apps_len, sizeof *bindings);
        if (bindings == NULL) {
            skbd_err_set(err, err_cap, "out of memory");
            goto done;
        }
        for (i = 0; i < rules.apps_len; i++) {
            bindings[i].app = rules.apps[i].app;
            bindings[i].index = rules.apps[i].index;
        }
    }
    if (skbd_memory_with_layouts(memory, keyboard.active, rules.fallback, bindings, rules.apps_len, err, err_cap) != 0) {
        goto done;
    }
    if (skbd_memory_keep_tokens(memory, file->has_fallback ? &file->fallback : NULL, file->apps, file->apps_len, err, err_cap) != 0) {
        goto done;
    }
    if (skbd_memory_watch_keyboard(memory, keyboard.identifier, err, err_cap) != 0) {
        goto done;
    }
    if (skbd_ipc_get_tree(commands, &tree, err, err_cap) != 0) {
        goto done;
    }
    if (skbd_remember_open_views(memory, tree, err, err_cap) != 0) {
        goto done;
    }
    if (skbd_memory_focused_id(memory, &focused_id)) {
        if (verbose) {
            fprintf(stderr, "focused window %" PRId64 "\n", focused_id);
        }
    } else {
        if (verbose) {
            fputs("no focused window\n", stderr);
        }
        memset(&happening, 0, sizeof happening);
        happening.kind = SKBD_HAPPEN_UNFOCUS;
        if (skbd_memory_apply(memory, &happening, &action, err, err_cap) != 0) {
            goto done;
        }
    }
    if (switch_if_needed(commands, memory, action, verbose, stop, err, err_cap) != 0) {
        goto done;
    }
    *stop = SKBD_STOP_OK;
    rc = 0;
done:
    free(bindings);
    skbd_node_free(tree);
    skbd_rules_free(&rules);
    skbd_keyboard_free(&keyboard);
    skbd_inputs_free(inputs, input_count);
    return rc;
}

static int event_loop(SkbdIpc *commands, SkbdIpc *events, SkbdMemory *memory, const SkbdOptions *options, SkbdStopKind *stop, char *err, size_t err_cap) {
    for (;;) {
        SkbdEvent *event;
        SkbdAction action;
        int ready;
        int read_rc;

        maybe_reload(commands, memory, options);
        ready = 0;
        if (skbd_wait_readable(events->fd, &ready, err, err_cap) != 0) {
            *stop = SKBD_STOP_FAIL;
            return -1;
        }
        maybe_reload(commands, memory, options);
        if (!ready) {
            continue;
        }
        event = NULL;
        memset(&action, 0, sizeof action);
        read_rc = skbd_ipc_read_event(events, &event, err, err_cap);
        if (read_rc != 0) {
            skbd_event_free(event);
            *stop = SKBD_STOP_FAIL;
            return -1;
        }
        if (event == NULL) {
            *stop = SKBD_STOP_FAIL;
            return skbd_err_set(err, err_cap, "missing sway event");
        }
        if (event->kind == SKBD_EV_SHUTDOWN) {
            skbd_event_free(event);
            *stop = SKBD_STOP_OK;
            return 0;
        }
        if (react(commands, memory, event, options->verbose, &action, stop, err, err_cap) != 0) {
            skbd_event_free(event);
            return -1;
        }
        if (switch_if_needed(commands, memory, action, options->verbose, stop, err, err_cap) != 0) {
            skbd_event_free(event);
            return -1;
        }
        skbd_event_free(event);
    }
}

static int lock_from_env(SkbdLock *lock, char *err, size_t err_cap) {
    const char *raw;

    raw = getenv("XDG_RUNTIME_DIR");
    if (skbd_require_runtime_dir(raw, err, err_cap) != 0) {
        return -1;
    }
    return skbd_lock_runtime_dir(raw, lock, err, err_cap);
}

int skbd_run_options(const SkbdOptions *options, SkbdStopKind *stop, char *err, size_t err_cap) {
    SkbdLock lock;
    SkbdConfigFile file;
    SkbdIpc commands;
    SkbdIpc events;
    SkbdMemory memory;
    char *source;
    char *label;
    int rc;

    if (options == NULL || stop == NULL) {
        return skbd_err_set(err, err_cap, "runtime options are missing");
    }
    *stop = SKBD_STOP_FAIL;
    lock.fd = -1;
    commands.fd = -1;
    events.fd = -1;
    source = NULL;
    label = NULL;
    memset(&file, 0, sizeof file);
    memset(&memory, 0, sizeof memory);
    rc = -1;
    if (skbd_refuse_root(geteuid(), err, err_cap) != 0) {
        goto done;
    }
    if (lock_from_env(&lock, err, err_cap) != 0) {
        goto done;
    }
    if (skbd_install_sighup(err, err_cap) != 0) {
        goto done;
    }
    if (read_config_source(options, &source, &file, stop, err, err_cap) != 0) {
        goto done;
    }
    if (take_fallback_label(&file, options->default_layout, source, &label, err, err_cap) != 0) {
        goto done;
    }
    if (open_sway(&commands, stop, err, err_cap) != 0) {
        goto done;
    }
    if (open_sway(&events, stop, err, err_cap) != 0) {
        goto done;
    }
    if (skbd_ipc_subscribe(&events, err, err_cap) != 0) {
        *stop = SKBD_STOP_FAIL;
        goto done;
    }
    if (seed(&commands, options->verbose, &file, label, source, &memory, stop, err, err_cap) != 0) {
        goto done;
    }
    rc = event_loop(&commands, &events, &memory, options, stop, err, err_cap);
done:
    skbd_memory_free(&memory);
    skbd_ipc_close(&events);
    skbd_ipc_close(&commands);
    skbd_config_free(&file);
    free(label);
    free(source);
    skbd_lock_close(&lock);
    return rc;
}

#ifndef SKBD_TEST
int main(int argc, char **argv) {
    SkbdParsed parsed;
    SkbdStopKind stop;
    char err[4096];

    err[0] = '\0';
    stop = SKBD_STOP_FAIL;
    if (skbd_parse_args(argc, argv, &parsed, err, sizeof err) != 0) {
        fprintf(stderr, "%s\n", err);
        return SKBD_EXIT_USAGE;
    }
    if (parsed.kind == SKBD_PARSED_HELP) {
        fputs(skbd_help, stdout);
        skbd_parsed_free(&parsed);
        return SKBD_EXIT_OK;
    }
    if (parsed.kind == SKBD_PARSED_VERSION) {
        printf("%s\n", skbd_version);
        skbd_parsed_free(&parsed);
        return SKBD_EXIT_OK;
    }
    if (skbd_run_options(&parsed.options, &stop, err, sizeof err) != 0) {
        if (err[0] != '\0') {
            fprintf(stderr, "%s\n", err);
        }
        skbd_parsed_free(&parsed);
        if (stop == SKBD_STOP_USAGE) {
            return SKBD_EXIT_USAGE;
        }
        if (stop == SKBD_STOP_IDLE) {
            return SKBD_EXIT_OK;
        }
        return SKBD_EXIT_FAIL;
    }
    skbd_parsed_free(&parsed);
    return SKBD_EXIT_OK;
}
#endif
