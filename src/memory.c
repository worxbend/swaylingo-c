#include "skbd/memory.h"

#include "skbd/util.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void free_app_rows(SkbdAppIndex *apps, size_t len) {
    size_t slot;

    if (apps == NULL) {
        return;
    }
    for (slot = 0; slot < len; slot++) {
        free(apps[slot].app);
    }
    free(apps);
}

static void free_stored_apps(SkbdMemory *mem) {
    size_t slot;

    if (mem->app_tokens == NULL) {
        mem->app_tokens_len = 0;
        mem->app_tokens_cap = 0;
        return;
    }
    for (slot = 0; slot < mem->app_tokens_len; slot++) {
        free(mem->app_tokens[slot].app);
        skbd_token_free(&mem->app_tokens[slot].token);
    }
    free(mem->app_tokens);
    mem->app_tokens = NULL;
    mem->app_tokens_len = 0;
    mem->app_tokens_cap = 0;
}

static int layout_set_contains(const uint32_t *items, size_t len, uint32_t layout) {
    size_t slot;

    for (slot = 0; slot < len; slot++) {
        if (items[slot] == layout) {
            return 1;
        }
    }
    return 0;
}

static int layout_set_remove(uint32_t *items, size_t *len, uint32_t layout) {
    size_t slot;

    for (slot = 0; slot < *len; slot++) {
        if (items[slot] != layout) {
            continue;
        }
        *len -= 1;
        if (slot < *len) {
            items[slot] = items[*len];
        }
        return 1;
    }
    return 0;
}

static int window_at(const SkbdMemory *mem, int64_t id, size_t *slot_out) {
    size_t slot;

    for (slot = 0; slot < mem->by_window_len; slot++) {
        if (mem->by_window[slot].id == id) {
            if (slot_out != NULL) {
                *slot_out = slot;
            }
            return 1;
        }
    }
    return 0;
}

static void window_remove(SkbdMemory *mem, int64_t id) {
    size_t slot;

    if (!window_at(mem, id, &slot)) {
        return;
    }
    mem->by_window_len -= 1;
    if (slot < mem->by_window_len) {
        mem->by_window[slot] = mem->by_window[mem->by_window_len];
    }
}

static int app_at(const SkbdAppIndex *apps, size_t len, const char *name, size_t *slot_out) {
    size_t slot;

    if (name == NULL) {
        return 0;
    }
    for (slot = 0; slot < len; slot++) {
        if (strcmp(apps[slot].app, name) == 0) {
            if (slot_out != NULL) {
                *slot_out = slot;
            }
            return 1;
        }
    }
    return 0;
}

static int x11_at(const SkbdMemory *mem, int32_t x11, size_t *slot_out) {
    size_t slot;

    for (slot = 0; slot < mem->container_by_x11_len; slot++) {
        if (mem->container_by_x11[slot].x11 == x11) {
            if (slot_out != NULL) {
                *slot_out = slot;
            }
            return 1;
        }
    }
    return 0;
}

static int container_has_x11(const SkbdMemory *mem, int64_t container) {
    size_t slot;

    for (slot = 0; slot < mem->container_by_x11_len; slot++) {
        if (mem->container_by_x11[slot].container == container) {
            return 1;
        }
    }
    return 0;
}

static void drop_container_x11(SkbdMemory *mem, int64_t container) {
    size_t slot;

    slot = 0;
    while (slot < mem->container_by_x11_len) {
        if (mem->container_by_x11[slot].container != container) {
            slot++;
            continue;
        }
        mem->container_by_x11_len -= 1;
        if (slot < mem->container_by_x11_len) {
            mem->container_by_x11[slot] = mem->container_by_x11[mem->container_by_x11_len];
        }
    }
}

static int grow_from(void *current, void **updated, size_t *cap, size_t need, size_t elem, char *err, size_t err_cap) {
    void *raw;

    raw = current;
    if (skbd_grow(&raw, cap, need, elem, err, err_cap) != 0) {
        return -1;
    }
    *updated = raw;
    return 0;
}

static int reserve_one(void *current, void **updated, size_t *cap, size_t len, size_t elem, char *err, size_t err_cap) {
    if (len == SIZE_MAX) {
        return skbd_err_set(err, err_cap, "grow overflow");
    }
    return grow_from(current, updated, cap, len + 1, elem, err, err_cap);
}

static int reserve_windows(SkbdMemory *mem, char *err, size_t err_cap) {
    void *updated;

    if (reserve_one(mem->by_window, &updated, &mem->by_window_cap, mem->by_window_len, sizeof *mem->by_window, err, err_cap) != 0) {
        return -1;
    }
    mem->by_window = updated;
    return 0;
}

static int reserve_pending(SkbdMemory *mem, char *err, size_t err_cap) {
    void *updated;

    if (reserve_one(mem->pending, &updated, &mem->pending_cap, mem->pending_len, sizeof *mem->pending, err, err_cap) != 0) {
        return -1;
    }
    mem->pending = updated;
    return 0;
}

static int reserve_x11(SkbdMemory *mem, char *err, size_t err_cap) {
    void *updated;

    if (reserve_one(mem->container_by_x11, &updated, &mem->container_by_x11_cap, mem->container_by_x11_len, sizeof *mem->container_by_x11, err, err_cap) != 0) {
        return -1;
    }
    mem->container_by_x11 = updated;
    return 0;
}

/* A new x11 id on a container that has none needs one free slot.
   Replacing that container's id, or overwriting an existing id, reuses a slot. */
static int ensure_x11(SkbdMemory *mem, int64_t container, int32_t x11, char *err, size_t err_cap) {
    if (x11_at(mem, x11, NULL) || container_has_x11(mem, container)) {
        return 0;
    }
    return reserve_x11(mem, err, err_cap);
}

static void bind_x11(SkbdMemory *mem, int64_t container, int32_t x11) {
    size_t slot;

    drop_container_x11(mem, container);
    for (slot = 0; slot < mem->container_by_x11_len; slot++) {
        if (mem->container_by_x11[slot].x11 != x11) {
            continue;
        }
        mem->container_by_x11[slot].container = container;
        return;
    }
    mem->container_by_x11[mem->container_by_x11_len].x11 = x11;
    mem->container_by_x11[mem->container_by_x11_len].container = container;
    mem->container_by_x11_len++;
}

static int transient_layout(const SkbdMemory *mem, int32_t x11, uint32_t *layout) {
    size_t link;
    size_t window;
    int64_t container;

    if (!x11_at(mem, x11, &link)) {
        return 0;
    }
    container = mem->container_by_x11[link].container;
    if (!window_at(mem, container, &window)) {
        return 0;
    }
    *layout = mem->by_window[window].layout;
    return 1;
}

static uint32_t layout_for_new(const SkbdMemory *mem, const SkbdHappening *happening) {
    uint32_t layout;
    size_t slot;

    if (happening->has_transient_for && transient_layout(mem, happening->transient_for, &layout)) {
        return layout;
    }
    if (app_at(mem->apps, mem->apps_len, happening->app, &slot)) {
        return mem->apps[slot].index;
    }
    return mem->fallback;
}

static uint32_t intended(const SkbdMemory *mem) {
    if (mem->has_requested) {
        return mem->requested;
    }
    return mem->active;
}

static void action_clear(SkbdAction *action) {
    action->kind = SKBD_ACTION_NONE;
    action->index = 0;
}

static void commit_request(SkbdMemory *mem, uint32_t layout, SkbdAction *action) {
    if (layout == intended(mem)) {
        action_clear(action);
        return;
    }
    layout_set_remove(mem->replaced, &mem->replaced_len, layout);
    if (!layout_set_contains(mem->pending, mem->pending_len, layout)) {
        mem->pending[mem->pending_len++] = layout;
    }
    mem->has_requested = 1;
    mem->requested = layout;
    action->kind = SKBD_ACTION_SWITCH;
    action->index = layout;
}

static int prepare_request(SkbdMemory *mem, uint32_t layout, char *err, size_t err_cap) {
    if (layout == intended(mem) || layout_set_contains(mem->pending, mem->pending_len, layout)) {
        return 0;
    }
    return reserve_pending(mem, err, err_cap);
}

static int put_app(SkbdAppIndex **apps, size_t *len, size_t *cap, const char *name, uint32_t layout, char *err, size_t err_cap) {
    void *updated;
    char *copy;
    size_t slot;

    if (name == NULL) {
        return skbd_err_set(err, err_cap, "app name is null");
    }
    if (app_at(*apps, *len, name, &slot)) {
        (*apps)[slot].index = layout;
        return 0;
    }
    if (*len == SIZE_MAX) {
        return skbd_err_set(err, err_cap, "grow overflow");
    }
    if (grow_from(*apps, &updated, cap, *len + 1, sizeof **apps, err, err_cap) != 0) {
        return -1;
    }
    *apps = updated;
    copy = skbd_strdup(name);
    if (copy == NULL) {
        return skbd_err_set(err, err_cap, "out of memory");
    }
    (*apps)[*len].app = copy;
    (*apps)[*len].index = layout;
    (*len)++;
    return 0;
}

static int copy_token(SkbdLayoutToken *dst, const SkbdLayoutToken *src, char *err, size_t err_cap) {
    memset(dst, 0, sizeof *dst);
    if (src->kind == SKBD_TOKEN_INDEX) {
        dst->kind = SKBD_TOKEN_INDEX;
        dst->index = src->index;
        return 0;
    }
    if (src->kind != SKBD_TOKEN_NAME) {
        return skbd_err_set(err, err_cap, "unknown layout token");
    }
    if (src->name == NULL) {
        return skbd_err_set(err, err_cap, "layout token name is null");
    }
    dst->name = skbd_strdup(src->name);
    if (dst->name == NULL) {
        return skbd_err_set(err, err_cap, "out of memory");
    }
    dst->kind = SKBD_TOKEN_NAME;
    return 0;
}

typedef struct {
    int has_fallback;
    SkbdLayoutToken fallback;
    SkbdStoredApp *apps;
    size_t len;
    size_t cap;
} KeptTokens;

typedef struct {
    uint32_t fallback;
    SkbdAppIndex *apps;
    size_t len;
    size_t cap;
} FreshRules;

static void free_kept(KeptTokens *kept) {
    size_t slot;

    skbd_token_free(&kept->fallback);
    if (kept->apps != NULL) {
        for (slot = 0; slot < kept->len; slot++) {
            free(kept->apps[slot].app);
            skbd_token_free(&kept->apps[slot].token);
        }
        free(kept->apps);
    }
    memset(kept, 0, sizeof *kept);
}

static int copy_stored(SkbdStoredApp *dst, const SkbdConfigApp *src, char *err, size_t err_cap) {
    SkbdStoredApp copied;

    memset(&copied, 0, sizeof copied);
    if (src->app == NULL) {
        return skbd_err_set(err, err_cap, "app name is null");
    }
    copied.app = skbd_strdup(src->app);
    if (copied.app == NULL) {
        return skbd_err_set(err, err_cap, "out of memory");
    }
    if (copy_token(&copied.token, &src->token, err, err_cap) != 0) {
        free(copied.app);
        return -1;
    }
    *dst = copied;
    return 0;
}

static int copy_kept(KeptTokens *kept, const SkbdLayoutToken *fallback, const SkbdConfigApp *apps, size_t app_count, char *err, size_t err_cap) {
    size_t slot;
    void *updated;

    memset(kept, 0, sizeof *kept);
    if (app_count > 0 && apps == NULL) {
        return skbd_err_set(err, err_cap, "app rules are null");
    }
    if (fallback != NULL && copy_token(&kept->fallback, fallback, err, err_cap) != 0) {
        return -1;
    }
    kept->has_fallback = fallback != NULL;
    for (slot = 0; slot < app_count; slot++) {
        if (kept->len == SIZE_MAX) {
            free_kept(kept);
            return skbd_err_set(err, err_cap, "grow overflow");
        }
        if (grow_from(kept->apps, &updated, &kept->cap, kept->len + 1, sizeof *kept->apps, err, err_cap) != 0) {
            free_kept(kept);
            return -1;
        }
        kept->apps = updated;
        if (copy_stored(&kept->apps[kept->len], &apps[slot], err, err_cap) != 0) {
            free_kept(kept);
            return -1;
        }
        kept->len++;
    }
    return 0;
}

static void install_kept(SkbdMemory *mem, KeptTokens *kept) {
    skbd_token_free(&mem->fallback_token);
    free_stored_apps(mem);
    mem->has_fallback_token = kept->has_fallback;
    mem->fallback_token = kept->fallback;
    mem->app_tokens = kept->apps;
    mem->app_tokens_len = kept->len;
    mem->app_tokens_cap = kept->cap;
    kept->fallback.name = NULL;
    kept->apps = NULL;
    kept->len = 0;
    kept->cap = 0;
    kept->has_fallback = 0;
}

static int on_close(SkbdMemory *mem, int64_t id, SkbdAction *action) {
    action_clear(action);
    window_remove(mem, id);
    drop_container_x11(mem, id);
    if (mem->has_focused && mem->focused == id) {
        mem->has_focused = 0;
    }
    return 0;
}

static int on_unfocus(SkbdMemory *mem, SkbdAction *action, char *err, size_t err_cap) {
    if (prepare_request(mem, mem->fallback, err, err_cap) != 0) {
        return -1;
    }
    mem->has_focused = 0;
    commit_request(mem, mem->fallback, action);
    return 0;
}

static int accept_newest(SkbdMemory *mem, uint32_t layout, char *err, size_t err_cap) {
    size_t slot;
    void *updated;
    size_t need;

    if (mem->pending_len > SIZE_MAX - mem->replaced_len) {
        return skbd_err_set(err, err_cap, "grow overflow");
    }
    need = mem->replaced_len + mem->pending_len;
    if (grow_from(mem->replaced, &updated, &mem->replaced_cap, need, sizeof *mem->replaced, err, err_cap) != 0) {
        return -1;
    }
    mem->replaced = updated;
    mem->active = layout;
    layout_set_remove(mem->pending, &mem->pending_len, layout);
    for (slot = 0; slot < mem->pending_len; slot++) {
        if (layout_set_contains(mem->replaced, mem->replaced_len, mem->pending[slot])) {
            continue;
        }
        mem->replaced[mem->replaced_len++] = mem->pending[slot];
    }
    mem->pending_len = 0;
    mem->has_requested = 0;
    return 0;
}

static int store_user_layout(SkbdMemory *mem, uint32_t layout, char *err, size_t err_cap) {
    size_t slot = 0;
    int known;

    known = mem->has_focused && window_at(mem, mem->focused, &slot);
    if (mem->has_focused && !known && reserve_windows(mem, err, err_cap) != 0) {
        return -1;
    }
    mem->active = layout;
    mem->pending_len = 0;
    mem->has_requested = 0;
    if (!mem->has_focused) {
        return 0;
    }
    if (known) {
        mem->by_window[slot].layout = layout;
        return 0;
    }
    mem->by_window[mem->by_window_len].id = mem->focused;
    mem->by_window[mem->by_window_len].layout = layout;
    mem->by_window_len++;
    return 0;
}

static int on_layout(SkbdMemory *mem, uint32_t layout, SkbdAction *action, char *err, size_t err_cap) {
    action_clear(action);
    if (mem->has_requested && mem->requested == layout) {
        /* Coming back from our newest request, not a key the person pressed. */
        return accept_newest(mem, layout, err, err_cap);
    }
    if (layout_set_remove(mem->pending, &mem->pending_len, layout)) {
        return 0;
    }
    if (layout_set_remove(mem->replaced, &mem->replaced_len, layout)) {
        return 0;
    }
    if (layout == mem->active) {
        return 0;
    }
    return store_user_layout(mem, layout, err, err_cap);
}

static int on_focus(SkbdMemory *mem, const SkbdHappening *happening, SkbdAction *action, char *err, size_t err_cap) {
    int known;
    size_t slot = 0;
    uint32_t layout;
    int64_t id;

    id = happening->id;
    if (happening->has_x11 && ensure_x11(mem, id, happening->x11, err, err_cap) != 0) {
        return -1;
    }
    known = window_at(mem, id, &slot);
    if (mem->has_focused && mem->focused == id) {
        if (happening->has_x11) {
            bind_x11(mem, id, happening->x11);
        }
        action_clear(action);
        return 0;
    }
    if (!known && reserve_windows(mem, err, err_cap) != 0) {
        return -1;
    }
    /* The x11 update can hide the parent, so the requested index is not known yet. */
    if (reserve_pending(mem, err, err_cap) != 0) {
        return -1;
    }
    if (happening->has_x11) {
        bind_x11(mem, id, happening->x11);
    }
    if (known) {
        layout = mem->by_window[slot].layout;
    } else {
        layout = layout_for_new(mem, happening);
    }
    mem->has_focused = 1;
    mem->focused = id;
    if (!known) {
        mem->by_window[mem->by_window_len].id = id;
        mem->by_window[mem->by_window_len].layout = layout;
        mem->by_window_len++;
    }
    commit_request(mem, layout, action);
    return 0;
}

static void discard_fresh(FreshRules *fresh) {
    free_app_rows(fresh->apps, fresh->len);
    fresh->apps = NULL;
    fresh->len = 0;
    fresh->cap = 0;
}

/* A miss prints and continues. Keeping the old index would address the old list. */
static int resolve_fresh(const SkbdMemory *mem, const char *const *names, size_t name_count, FreshRules *fresh, char *err, size_t err_cap) {
    char resolve_err[1024];
    size_t slot;
    uint32_t layout;

    fresh->fallback = mem->fallback;
    fresh->apps = NULL;
    fresh->len = 0;
    fresh->cap = 0;
    if (mem->has_fallback_token) {
        if (skbd_config_resolve_token(&mem->fallback_token, names, name_count, &layout, resolve_err, sizeof resolve_err) != 0) {
            fprintf(stderr, "fallback layout no longer matches: %s\n", resolve_err);
            fresh->fallback = SKBD_DEFAULT_LAYOUT;
        } else {
            fresh->fallback = layout;
        }
    }
    for (slot = 0; slot < mem->app_tokens_len; slot++) {
        if (skbd_config_resolve_token(&mem->app_tokens[slot].token, names, name_count, &layout, resolve_err, sizeof resolve_err) != 0) {
            fprintf(stderr, "skipping app %s: %s\n", mem->app_tokens[slot].app, resolve_err);
            continue;
        }
        if (put_app(&fresh->apps, &fresh->len, &fresh->cap, mem->app_tokens[slot].app, layout, err, err_cap) != 0) {
            discard_fresh(fresh);
            return -1;
        }
    }
    return 0;
}

static int on_keymap_reset(SkbdMemory *mem, const SkbdHappening *happening, SkbdAction *action, char *err, size_t err_cap) {
    int have_tokens;
    FreshRules fresh;
    void *updated;

    action_clear(action);
    memset(&fresh, 0, sizeof fresh);
    have_tokens = mem->has_fallback_token || mem->app_tokens_len > 0;
    if (have_tokens) {
        if (happening->name_count > 0 && happening->names == NULL) {
            return skbd_err_set(err, err_cap, "layout names are null");
        }
        if (resolve_fresh(mem, happening->names, happening->name_count, &fresh, err, err_cap) != 0) {
            return -1;
        }
    }
    if (mem->has_focused && mem->by_window_cap == 0) {
        if (grow_from(mem->by_window, &updated, &mem->by_window_cap, 1, sizeof *mem->by_window, err, err_cap) != 0) {
            discard_fresh(&fresh);
            return -1;
        }
        mem->by_window = updated;
    }
    if (have_tokens) {
        free_app_rows(mem->apps, mem->apps_len);
        mem->apps = fresh.apps;
        mem->apps_len = fresh.len;
        mem->apps_cap = fresh.cap;
        mem->fallback = fresh.fallback;
    }
    mem->by_window_len = 0;
    mem->pending_len = 0;
    mem->replaced_len = 0;
    mem->has_requested = 0;
    if (happening->has_index) {
        mem->active = happening->index;
    }
    if (mem->has_focused) {
        mem->by_window[0].id = mem->focused;
        mem->by_window[0].layout = mem->active;
        mem->by_window_len = 1;
    }
    return 0;
}

void skbd_memory_free(SkbdMemory *mem) {
    if (mem == NULL) {
        return;
    }
    free_app_rows(mem->apps, mem->apps_len);
    free_stored_apps(mem);
    skbd_token_free(&mem->fallback_token);
    free(mem->by_window);
    free(mem->container_by_x11);
    free(mem->pending);
    free(mem->replaced);
    free(mem->keyboard_id);
    memset(mem, 0, sizeof *mem);
}

int skbd_memory_with_layouts(SkbdMemory *mem, uint32_t active, uint32_t fallback, const SkbdAppBinding *apps, size_t app_count, char *err, size_t err_cap) {
    size_t slot;

    if (mem == NULL) {
        return skbd_err_set(err, err_cap, "memory is null");
    }
    memset(mem, 0, sizeof *mem);
    if (app_count > 0 && apps == NULL) {
        return skbd_err_set(err, err_cap, "app rules are null");
    }
    mem->active = active;
    mem->fallback = fallback;
    for (slot = 0; slot < app_count; slot++) {
        if (put_app(&mem->apps, &mem->apps_len, &mem->apps_cap, apps[slot].app, apps[slot].index, err, err_cap) != 0) {
            skbd_memory_free(mem);
            return -1;
        }
    }
    return 0;
}

int skbd_memory_keep_tokens(SkbdMemory *mem, const SkbdLayoutToken *fallback, const SkbdConfigApp *apps, size_t app_count, char *err, size_t err_cap) {
    KeptTokens kept;

    if (mem == NULL) {
        return skbd_err_set(err, err_cap, "memory is null");
    }
    if (copy_kept(&kept, fallback, apps, app_count, err, err_cap) != 0) {
        return -1;
    }
    install_kept(mem, &kept);
    return 0;
}

int skbd_memory_reload_rules(SkbdMemory *mem, const SkbdConfigFile *file, const char *const *names, size_t name_count, const SkbdRuleSource *from, char *err, size_t err_cap) {
    SkbdRules rules;
    const SkbdLayoutToken *fallback;

    if (mem == NULL) {
        return skbd_err_set(err, err_cap, "memory is null");
    }
    if (file == NULL) {
        return skbd_err_set(err, err_cap, "config file is null");
    }
    if (from == NULL) {
        return skbd_err_set(err, err_cap, "rule source is null");
    }
    if (name_count > 0 && names == NULL) {
        return skbd_err_set(err, err_cap, "layout names are null");
    }
    if (file->apps_len > 0 && file->apps == NULL) {
        return skbd_err_set(err, err_cap, "app rules are null");
    }
    memset(&rules, 0, sizeof rules);
    if (skbd_config_resolve_rules(file, names, name_count, from, &rules, err, err_cap) != 0) {
        skbd_rules_free(&rules);
        return -1;
    }
    fallback = file->has_fallback ? &file->fallback : NULL;
    if (skbd_memory_keep_tokens(mem, fallback, file->apps, file->apps_len, err, err_cap) != 0) {
        skbd_rules_free(&rules);
        return -1;
    }
    free_app_rows(mem->apps, mem->apps_len);
    mem->apps = rules.apps;
    mem->apps_len = rules.apps_len;
    mem->apps_cap = rules.apps_cap;
    mem->fallback = rules.fallback;
    rules.apps = NULL;
    rules.apps_len = 0;
    rules.apps_cap = 0;
    skbd_rules_free(&rules);
    return 0;
}

int skbd_memory_focused_id(const SkbdMemory *mem, int64_t *id) {
    if (mem == NULL || !mem->has_focused) {
        return 0;
    }
    if (id != NULL) {
        *id = mem->focused;
    }
    return 1;
}

int skbd_memory_watch_keyboard(SkbdMemory *mem, const char *identifier, char *err, size_t err_cap) {
    char *copy;

    if (mem == NULL) {
        return skbd_err_set(err, err_cap, "memory is null");
    }
    if (identifier == NULL) {
        return skbd_err_set(err, err_cap, "keyboard id is null");
    }
    copy = skbd_strdup(identifier);
    if (copy == NULL) {
        return skbd_err_set(err, err_cap, "out of memory");
    }
    free(mem->keyboard_id);
    mem->keyboard_id = copy;
    return 0;
}

const char *skbd_memory_keyboard_id(const SkbdMemory *mem) {
    if (mem == NULL || mem->keyboard_id == NULL) {
        return "";
    }
    return mem->keyboard_id;
}

int skbd_memory_note_existing(SkbdMemory *mem, int64_t id, const int32_t *x11, char *err, size_t err_cap) {
    int known;

    if (mem == NULL) {
        return skbd_err_set(err, err_cap, "memory is null");
    }
    known = window_at(mem, id, NULL);
    if (!known && reserve_windows(mem, err, err_cap) != 0) {
        return -1;
    }
    if (x11 != NULL && ensure_x11(mem, id, *x11, err, err_cap) != 0) {
        return -1;
    }
    if (!known) {
        mem->by_window[mem->by_window_len].id = id;
        mem->by_window[mem->by_window_len].layout = mem->active;
        mem->by_window_len++;
    }
    if (x11 != NULL) {
        bind_x11(mem, id, *x11);
    }
    return 0;
}

int skbd_memory_remember_current(SkbdMemory *mem, int64_t id, char *err, size_t err_cap) {
    if (skbd_memory_note_existing(mem, id, NULL, err, err_cap) != 0) {
        return -1;
    }
    mem->has_focused = 1;
    mem->focused = id;
    return 0;
}

int skbd_memory_apply(SkbdMemory *mem, const SkbdHappening *happening, SkbdAction *action, char *err, size_t err_cap) {
    if (mem == NULL) {
        return skbd_err_set(err, err_cap, "memory is null");
    }
    if (happening == NULL) {
        return skbd_err_set(err, err_cap, "happening is null");
    }
    if (action == NULL) {
        return skbd_err_set(err, err_cap, "action is null");
    }
    action_clear(action);
    switch (happening->kind) {
    case SKBD_HAPPEN_FOCUS:
        return on_focus(mem, happening, action, err, err_cap);
    case SKBD_HAPPEN_UNFOCUS:
        return on_unfocus(mem, action, err, err_cap);
    case SKBD_HAPPEN_CLOSE:
        return on_close(mem, happening->id, action);
    case SKBD_HAPPEN_LAYOUT:
        return on_layout(mem, happening->index, action, err, err_cap);
    case SKBD_HAPPEN_KEYMAP_RESET:
        return on_keymap_reset(mem, happening, action, err, err_cap);
    default:
        return skbd_err_set(err, err_cap, "unknown happening");
    }
}

void skbd_memory_abandon_switch(SkbdMemory *mem) {
    if (mem == NULL || !mem->has_requested) {
        return;
    }
    layout_set_remove(mem->pending, &mem->pending_len, mem->requested);
    mem->has_requested = 0;
}
