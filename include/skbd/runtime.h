#ifndef SKBD_RUNTIME_H
#define SKBD_RUNTIME_H

#include "skbd/memory.h"
#include "skbd/sway.h"

#include <signal.h>
#include <stddef.h>
#include <sys/types.h>

/* Daemon runtime. Pure helpers are declared here so tests do not open a socket.
   Exit 0 is help, version, Sway shutdown, or an idle systemd start.
   Exit 1 is a Sway or environment failure. Exit 2 is usage or a bad config.
   Missing or empty SWAYSOCK is exit 1 at the CLI and exit 0 under systemd.
   Root is refused. One process-lifetime flag: skbd_reload_requested. */

#define SKBD_EXIT_OK 0
#define SKBD_EXIT_FAIL 1
#define SKBD_EXIT_USAGE 2

/* Rust: HELP. The text lists the flags. */
extern const char skbd_help[];

/* Rust: CARGO_PKG_VERSION. The string is 0.4.0. */
extern const char skbd_version[];

/* Rust: RELOAD_REQUESTED. The SIGHUP handler may only store this flag. */
extern volatile sig_atomic_t skbd_reload_requested;

typedef struct {
    int verbose;
    char *config_path;
    char *default_layout;
} SkbdOptions;

typedef enum {
    SKBD_PARSED_RUN = 0,
    SKBD_PARSED_HELP,
    SKBD_PARSED_VERSION
} SkbdParsedKind;

typedef struct {
    SkbdParsedKind kind;
    SkbdOptions options;
} SkbdParsed;

typedef enum {
    SKBD_STOP_OK = 0,
    SKBD_STOP_USAGE,
    SKBD_STOP_FAIL,
    SKBD_STOP_IDLE
} SkbdStopKind;

typedef enum {
    SKBD_SOCK_MISSING = 0,
    SKBD_SOCK_EMPTY,
    SKBD_SOCK_NOT_UTF8,
    SKBD_SOCK_PATH
} SkbdSockKind;

typedef struct {
    int fd;
} SkbdLock;

typedef struct {
    char *identifier;
    uint32_t active;
    char **names;
    size_t name_count;
} SkbdKeyboard;

void skbd_parsed_free(SkbdParsed *parsed);
void skbd_keyboard_free(SkbdKeyboard *keyboard);
void skbd_lock_close(SkbdLock *lock);

/* Rust: parse_arg_list. argv[0] is the program name and is skipped.
   On failure the message is a usage error. *out is zeroed on failure. */
int skbd_parse_args(int argc, char **argv, SkbdParsed *out, char *err, size_t err_cap);

/* Rust: app_key. Returns 1 and writes the key when app_id or class is usable.
   An empty app_id falls through to class. Returns 0 when neither is usable.
   Returns -1 when the key does not fit in out. */
int skbd_app_key(const char *app_id, const char *class_name, char *out, size_t out_cap);

/* Rust: is_view. A con or floating_con with an app_id, a window, or a pid. */
int skbd_node_is_view(const SkbdNode *node);

/* Rust: layout_index. Returns 1 when index is non-NULL and >= 0. */
int skbd_layout_index(const int32_t *index, uint32_t *out);

/* Rust: x11_id. Returns 1 when window is present and fits in int32. */
int skbd_x11_id(const SkbdNode *node, int32_t *out);

/* Rust: transient_parent. Returns 1 when window_properties.transient_for is set. */
int skbd_transient_parent(const SkbdNode *node, int32_t *out);

/* Rust: focus_of. out->app points at the node's app_id or class_name. */
int skbd_focus_happening(const SkbdNode *node, SkbdHappening *out);

/* Rust: focused_view. NULL when this subtree has no focused view. */
const SkbdNode *skbd_focused_view(const SkbdNode *node);

/* Rust: focused_in_workspace. NULL means the caller must read the tree.
   current NULL, a node with no children, or children and no focused view. */
const SkbdNode *skbd_focused_in_workspace(const SkbdNode *current);

/* Rust: first_keyboard. The first keyboard with both an index and a non-empty
   name list. Otherwise the first index paired with the longest name list.
   On failure *out is zeroed and freeable. */
int skbd_first_keyboard(const SkbdInput *inputs, size_t count, SkbdKeyboard *out, char *err, size_t err_cap);

/* Rust: apply_input. Events for any other identifier return ACTION_NONE. */
SkbdAction skbd_apply_input(SkbdMemory *memory, SkbdInputChange change, const SkbdInput *input, int verbose);

/* Rust: classify_swaysock. service is nonzero when INVOCATION_ID is set and
   not empty. Missing and empty are IDLE under systemd and FAIL otherwise.
   Not UTF-8 is always FAIL. On success the path is copied to path_out. */
int skbd_classify_swaysock(SkbdSockKind kind, const char *path, int service, char *path_out, size_t path_cap, SkbdStopKind *stop, char *err, size_t err_cap);

/* Rust: ensure_socket. The path must be an absolute socket owned by this user.
   A final symlink is not followed. Parent mode is not checked. */
int skbd_ensure_socket(const char *path, char *err, size_t err_cap);

/* Rust: refuse_root. euid 0 fails. The message says this is a user service. */
int skbd_refuse_root(uid_t euid, char *err, size_t err_cap);

/* Rust: require_runtime_dir. NULL is unset. Empty and relative paths fail.
   Does not stat. */
int skbd_require_runtime_dir(const char *raw, char *err, size_t err_cap);

/* Rust: check_runtime_dir. The directory must be absolute, mode 0700, and owned
   by euid. Each component is opened with O_NOFOLLOW. */
int skbd_check_runtime_dir(const char *dir, uid_t euid, char *err, size_t err_cap);

/* Rust: lock_runtime_dir. Opens dir/skbd.lock on the pinned directory fd with
   O_NOFOLLOW and locks it. A second lock fails with "skbd: already running".
   On failure out->fd is -1. */
int skbd_lock_runtime_dir(const char *dir, SkbdLock *out, char *err, size_t err_cap);

/* Rust: close_window. Returns 1 when id was the focused window. */
int skbd_close_window(SkbdMemory *memory, int64_t id);

/* Rust: remember_open_views. Every open view keeps the active layout.
   The focused view is marked focused. App rules are not applied. */
int skbd_remember_open_views(SkbdMemory *memory, const SkbdNode *root, char *err, size_t err_cap);

/* Rust: adopt_next_keyboard. Returns 1 and writes the id when another keyboard
   has both an index and a name list. Returns 0 when none does. Skips the id
   memory is already watching. */
int skbd_adopt_next_keyboard(SkbdMemory *memory, const SkbdInput *inputs, size_t count, char *id_out, size_t id_cap, char *err, size_t err_cap);

/* Rust: overridden_fallback. When default_layout is non-NULL it replaces the
   file fallback with that name token and label becomes "--default".
   Otherwise label copies source_label. */
int skbd_overridden_fallback(SkbdConfigFile *file, const char *default_layout, const char *source_label, char *label, size_t label_cap, char *err, size_t err_cap);

/* Rust: install_sighup. The handler only stores skbd_reload_requested. */
int skbd_install_sighup(char *err, size_t err_cap);

/* Returns 1 when SIGHUP has been stored and not yet cleared by the caller. */
int skbd_reload_pending(void);

#endif
