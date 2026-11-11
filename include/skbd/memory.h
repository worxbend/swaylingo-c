#ifndef SKBD_MEMORY_H
#define SKBD_MEMORY_H

#include "skbd/config.h"

#include <stddef.h>
#include <stdint.h>

/* Per-window XKB layout index.
   A user layout change is stored on the focused window. pending remembers
   every index this process requested, including one a later request replaced.
   When the newest request is echoed, pending is cleared. A replaced index is
   still ignored once, so that late echo is not stored. Any other index is
   stored on the focused window, and the outstanding requests are cleared.
   The window we left is not rewritten on focus.

   A window seen for the first time uses the layout for its app, or the
   fallback. The fallback is SKBD_DEFAULT_LAYOUT when the config does not set
   one (the first xkb_layout, us when the list is us,ua). A desktop with no
   focused window uses the fallback. A window already in the map keeps the
   layout stored on it.

   Windows already open at startup keep the layout that was active then,
   focused or not. An app rule does not apply to them. An X11 dialog whose
   transient_for is the X11 id of a window already in the map copies that
   window's layout and does not use an app rule. A window with no
   transient_for still uses the app rule.

   Keys in by_window are Sway container ids. They die with the window, so the
   map is not written to disk. active is the index Sway has confirmed. pending
   is every index this process requested and has not seen echoed. requested is
   the newest of those. When that echo arrives, pending is cleared and the
   replaced indexes move to replaced, so one late echo is still ignored.
   fallback_token and app_tokens are resolved again on a keymap reset. The
   indexes are not kept across that reset.

   Tables grow by doubling. Lengths stay 0 while their pointers are NULL. */

/* Rust: DEFAULT_LAYOUT. First layout in the Sway xkb_layout list. */
#define SKBD_DEFAULT_LAYOUT 0u

/* Rust: Happening. Borrowed pointers are valid only for the apply call. */
typedef enum {
    SKBD_HAPPEN_FOCUS = 1,
    SKBD_HAPPEN_UNFOCUS,
    SKBD_HAPPEN_CLOSE,
    SKBD_HAPPEN_LAYOUT,
    SKBD_HAPPEN_KEYMAP_RESET
} SkbdHappenKind;

typedef struct {
    SkbdHappenKind kind;
    int64_t id;
    const char *app;
    int has_x11;
    int32_t x11;
    int has_transient_for;
    int32_t transient_for;
    int has_index;
    uint32_t index;
    const char *const *names;
    size_t name_count;
} SkbdHappening;

/* Rust: Action. SWITCH carries index. NONE leaves the keyboard where it is. */
typedef enum {
    SKBD_ACTION_NONE = 0,
    SKBD_ACTION_SWITCH
} SkbdActionKind;

typedef struct {
    SkbdActionKind kind;
    uint32_t index;
} SkbdAction;

typedef struct {
    int64_t id;
    uint32_t layout;
} SkbdWindowLayout;

typedef struct {
    int32_t x11;
    int64_t container;
} SkbdX11Link;

typedef struct {
    char *app;
    SkbdLayoutToken token;
} SkbdStoredApp;

/* Rust: Memory. */
typedef struct {
    SkbdWindowLayout *by_window;
    size_t by_window_len;
    size_t by_window_cap;
    SkbdX11Link *container_by_x11;
    size_t container_by_x11_len;
    size_t container_by_x11_cap;
    SkbdAppIndex *apps;
    size_t apps_len;
    size_t apps_cap;
    uint32_t fallback;
    int has_fallback_token;
    SkbdLayoutToken fallback_token;
    SkbdStoredApp *app_tokens;
    size_t app_tokens_len;
    size_t app_tokens_cap;
    int has_focused;
    int64_t focused;
    uint32_t active;
    uint32_t *pending;
    size_t pending_len;
    size_t pending_cap;
    uint32_t *replaced;
    size_t replaced_len;
    size_t replaced_cap;
    int has_requested;
    uint32_t requested;
    char *keyboard_id;
} SkbdMemory;

/* Drops every owned string and table. Safe on a zeroed struct and on NULL. */
void skbd_memory_free(SkbdMemory *mem);

/* Rust: with_layouts. Copies app names. On failure *mem is zeroed and freeable.
   Pass app_count 0 and apps NULL when the config sets no app rules. */
int skbd_memory_with_layouts(SkbdMemory *mem, uint32_t active, uint32_t fallback, const SkbdAppBinding *apps, size_t app_count, char *err, size_t err_cap);

/* Rust: keep_tokens. Copies tokens. fallback NULL clears the stored fallback token.
   A keymap reset resolves these copies. The caller's config stays the owner of its own. */
int skbd_memory_keep_tokens(SkbdMemory *mem, const SkbdLayoutToken *fallback, const SkbdConfigApp *apps, size_t app_count, char *err, size_t err_cap);

/* Rust: reload_rules. Resolves first. A bad token leaves the old rules in place.
   by_window and the focused id are not touched. */
int skbd_memory_reload_rules(SkbdMemory *mem, const SkbdConfigFile *file, const char *const *names, size_t name_count, const SkbdRuleSource *from, char *err, size_t err_cap);

/* Rust: focused_id. Returns 1 and writes *id when a window is focused, else 0. */
int skbd_memory_focused_id(const SkbdMemory *mem, int64_t *id);

/* Rust: watch_keyboard. Copies identifier. Layout events from any other id are ignored. */
int skbd_memory_watch_keyboard(SkbdMemory *mem, const char *identifier, char *err, size_t err_cap);

/* Rust: keyboard_id. Never NULL. Empty until watch_keyboard succeeds. */
const char *skbd_memory_keyboard_id(const SkbdMemory *mem);

/* Rust: note_existing. Stores the active layout. Does not apply an app rule.
   x11 NULL means the view has no X11 window id. */
int skbd_memory_note_existing(SkbdMemory *mem, int64_t id, const int32_t *x11, char *err, size_t err_cap);

/* Rust: remember_current. The view focused at startup. Keeps the active layout
   and marks it focused. Does not store an X11 id. */
int skbd_memory_remember_current(SkbdMemory *mem, int64_t id, char *err, size_t err_cap);

/* Rust: apply. On success writes *action and returns 0. On failure the map is
   unchanged, *action is none, and the return is -1. */
int skbd_memory_apply(SkbdMemory *mem, const SkbdHappening *happening, SkbdAction *action, char *err, size_t err_cap);

/* Rust: abandon_switch. The command socket rejected the newest switch, so that
   one will not echo. An older request can still echo and must stay ignored. */
void skbd_memory_abandon_switch(SkbdMemory *mem);

#endif
