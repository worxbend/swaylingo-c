#include "skbd/memory.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

static SkbdHappening focus(int64_t id) {
    SkbdHappening happening;

    memset(&happening, 0, sizeof happening);
    happening.kind = SKBD_HAPPEN_FOCUS;
    happening.id = id;
    return happening;
}

static SkbdHappening layout_event(uint32_t index) {
    SkbdHappening happening;

    memset(&happening, 0, sizeof happening);
    happening.kind = SKBD_HAPPEN_LAYOUT;
    happening.has_index = 1;
    happening.index = index;
    return happening;
}

static SkbdHappening unfocus(void) {
    SkbdHappening happening;

    memset(&happening, 0, sizeof happening);
    happening.kind = SKBD_HAPPEN_UNFOCUS;
    return happening;
}

static SkbdHappening close_id(int64_t id) {
    SkbdHappening happening;

    memset(&happening, 0, sizeof happening);
    happening.kind = SKBD_HAPPEN_CLOSE;
    happening.id = id;
    return happening;
}

static SkbdHappening keymap_with_index(uint32_t index, const char *const *names, size_t name_count) {
    SkbdHappening happening;

    memset(&happening, 0, sizeof happening);
    happening.kind = SKBD_HAPPEN_KEYMAP_RESET;
    happening.has_index = 1;
    happening.index = index;
    happening.names = names;
    happening.name_count = name_count;
    return happening;
}

static SkbdHappening keymap_without_index(void) {
    SkbdHappening happening;

    memset(&happening, 0, sizeof happening);
    happening.kind = SKBD_HAPPEN_KEYMAP_RESET;
    return happening;
}

static SkbdHappening seen(int64_t id, const char *app, const int32_t *x11, const int32_t *transient_for) {
    SkbdHappening happening;

    memset(&happening, 0, sizeof happening);
    happening.kind = SKBD_HAPPEN_FOCUS;
    happening.id = id;
    happening.app = app;
    if (x11 != NULL) {
        happening.has_x11 = 1;
        happening.x11 = *x11;
    }
    if (transient_for != NULL) {
        happening.has_transient_for = 1;
        happening.transient_for = *transient_for;
    }
    return happening;
}

static SkbdAction apply_ok(SkbdMemory *mem, SkbdHappening happening) {
    SkbdAction action;
    char err[256];

    assert(skbd_memory_apply(mem, &happening, &action, err, sizeof err) == 0);
    return action;
}

static void memory_at(SkbdMemory *mem, uint32_t active) {
    char err[64];

    assert(skbd_memory_with_layouts(mem, active, SKBD_DEFAULT_LAYOUT, NULL, 0, err, sizeof err) == 0);
}

static void remember_english_and_ukrainian(SkbdMemory *mem) {
    /* port: remember_english_and_ukrainian */
    memory_at(mem, 0);
    apply_ok(mem, focus(1));
    apply_ok(mem, focus(2));
    apply_ok(mem, layout_event(1));
}

static void telegram_rules(SkbdMemory *mem, uint32_t fallback) {
    SkbdAppBinding binding;
    char err[64];

    binding.app = "Telegram";
    binding.index = 1;
    assert(skbd_memory_with_layouts(mem, 0, fallback, &binding, 1, err, sizeof err) == 0);
}

static int window_layout(const SkbdMemory *mem, int64_t id, uint32_t *layout) {
    size_t slot;

    for (slot = 0; slot < mem->by_window_len; slot++) {
        if (mem->by_window[slot].id == id) {
            *layout = mem->by_window[slot].layout;
            return 1;
        }
    }
    return 0;
}

static void expect_layout(const SkbdMemory *mem, int64_t id, uint32_t want) {
    uint32_t got;

    assert(window_layout(mem, id, &got));
    assert(got == want);
}

static void expect_absent(const SkbdMemory *mem, int64_t id) {
    uint32_t layout;

    assert(!window_layout(mem, id, &layout));
}

static int set_has(const uint32_t *items, size_t len, uint32_t layout) {
    size_t slot;

    for (slot = 0; slot < len; slot++) {
        if (items[slot] == layout) {
            return 1;
        }
    }
    return 0;
}

static void expect_switch(const SkbdAction *action, uint32_t index) {
    assert(action->kind == SKBD_ACTION_SWITCH);
    assert(action->index == index);
}

static void expect_none(const SkbdAction *action) {
    assert(action->kind == SKBD_ACTION_NONE);
}

static void expect_focused(const SkbdMemory *mem, int64_t id) {
    int64_t got;

    assert(skbd_memory_focused_id(mem, &got) == 1);
    assert(got == id);
}

static void expect_unfocused(const SkbdMemory *mem) {
    assert(skbd_memory_focused_id(mem, NULL) == 0);
}

static int app_layout(const SkbdMemory *mem, const char *name, uint32_t *layout) {
    size_t slot;

    for (slot = 0; slot < mem->apps_len; slot++) {
        if (strcmp(mem->apps[slot].app, name) == 0) {
            *layout = mem->apps[slot].index;
            return 1;
        }
    }
    return 0;
}

static SkbdLayoutToken name_token(char *name) {
    SkbdLayoutToken token;

    memset(&token, 0, sizeof token);
    token.kind = SKBD_TOKEN_NAME;
    token.name = name;
    return token;
}

static SkbdLayoutToken index_token(uint32_t index) {
    SkbdLayoutToken token;

    memset(&token, 0, sizeof token);
    token.kind = SKBD_TOKEN_INDEX;
    token.index = index;
    return token;
}

static void same_windows(const SkbdMemory *mem, const SkbdWindowLayout *saved, size_t saved_len) {
    size_t slot;
    uint32_t layout;

    assert(mem->by_window_len == saved_len);
    for (slot = 0; slot < saved_len; slot++) {
        assert(window_layout(mem, saved[slot].id, &layout));
        assert(layout == saved[slot].layout);
    }
}

static void focus_restores_the_previous_windows_layout(void) {
    /* port: focus_restores_the_previous_windows_layout */
    SkbdMemory mem;
    SkbdAction action;

    remember_english_and_ukrainian(&mem);
    action = apply_ok(&mem, focus(1));
    expect_switch(&action, 0);
    assert(mem.has_requested);
    assert(mem.requested == 0);
    assert(set_has(mem.pending, mem.pending_len, 0));
    expect_layout(&mem, 1, 0);
    expect_layout(&mem, 2, 1);
    skbd_memory_free(&mem);
}

static void restore_echo_does_not_overwrite_the_window(void) {
    /* port: restore_echo_does_not_overwrite_the_window */
    SkbdMemory mem;
    SkbdAction action;

    remember_english_and_ukrainian(&mem);
    apply_ok(&mem, focus(1));
    action = apply_ok(&mem, layout_event(0));
    expect_none(&action);
    assert(mem.pending_len == 0);
    assert(!mem.has_requested);
    expect_layout(&mem, 1, 0);
    skbd_memory_free(&mem);
}

static void duplicate_layout_event_keeps_the_map(void) {
    /* port: duplicate_layout_event_keeps_the_map */
    SkbdMemory mem;

    remember_english_and_ukrainian(&mem);
    apply_ok(&mem, focus(1));
    apply_ok(&mem, layout_event(0));
    apply_ok(&mem, layout_event(0));
    expect_layout(&mem, 1, 0);
    expect_layout(&mem, 2, 1);
    skbd_memory_free(&mem);
}

static void new_window_uses_the_default_layout(void) {
    /* port: new_window_uses_the_default_layout */
    SkbdMemory mem;
    SkbdAction action;

    memory_at(&mem, 0);
    apply_ok(&mem, focus(1));
    apply_ok(&mem, layout_event(1));
    action = apply_ok(&mem, focus(9));
    expect_switch(&action, SKBD_DEFAULT_LAYOUT);
    expect_layout(&mem, 9, SKBD_DEFAULT_LAYOUT);
    expect_layout(&mem, 1, 1);
    skbd_memory_free(&mem);
}

static void empty_desktop_switches_to_the_default_layout(void) {
    /* port: empty_desktop_switches_to_the_default_layout */
    SkbdMemory mem;
    SkbdAction action;

    memory_at(&mem, 0);
    apply_ok(&mem, focus(1));
    apply_ok(&mem, layout_event(1));
    action = apply_ok(&mem, unfocus());
    expect_switch(&action, SKBD_DEFAULT_LAYOUT);
    expect_unfocused(&mem);
    expect_layout(&mem, 1, 1);
    apply_ok(&mem, layout_event(0));
    expect_layout(&mem, 1, 1);
    assert(mem.active == SKBD_DEFAULT_LAYOUT);
    skbd_memory_free(&mem);
}

static void window_open_at_startup_keeps_the_active_layout(void) {
    /* port: window_open_at_startup_keeps_the_active_layout */
    SkbdMemory mem;
    SkbdAction action;
    char err[64];

    memory_at(&mem, 1);
    assert(skbd_memory_remember_current(&mem, 1, err, sizeof err) == 0);
    action = apply_ok(&mem, focus(2));
    expect_layout(&mem, 1, 1);
    expect_switch(&action, SKBD_DEFAULT_LAYOUT);
    expect_layout(&mem, 2, SKBD_DEFAULT_LAYOUT);
    skbd_memory_free(&mem);
}

static void focus_during_a_pending_switch_does_not_overwrite_either_window(void) {
    /* port: focus_during_a_pending_switch_does_not_overwrite_either_window */
    SkbdMemory mem;
    SkbdAction action;

    remember_english_and_ukrainian(&mem);
    apply_ok(&mem, focus(1));
    action = apply_ok(&mem, focus(2));
    expect_switch(&action, 1);
    assert(set_has(mem.pending, mem.pending_len, 0));
    assert(set_has(mem.pending, mem.pending_len, 1));
    expect_layout(&mem, 1, 0);
    expect_layout(&mem, 2, 1);
    apply_ok(&mem, layout_event(0));
    apply_ok(&mem, layout_event(1));
    assert(mem.pending_len == 0);
    expect_layout(&mem, 1, 0);
    expect_layout(&mem, 2, 1);
    skbd_memory_free(&mem);
}

static void user_layout_clears_outstanding_requests_and_stores_on_the_focused_window(void) {
    /* port: user_layout_clears_outstanding_requests_and_stores_on_the_focused_window */
    SkbdMemory mem;

    remember_english_and_ukrainian(&mem);
    apply_ok(&mem, focus(1));
    apply_ok(&mem, focus(2));
    apply_ok(&mem, layout_event(2));
    assert(mem.pending_len == 0);
    assert(!mem.has_requested);
    assert(mem.active == 2);
    expect_layout(&mem, 2, 2);
    expect_layout(&mem, 1, 0);
    skbd_memory_free(&mem);
}

static void close_drops_the_window_and_ignores_a_later_layout(void) {
    /* port: close_drops_the_window_and_ignores_a_later_layout */
    SkbdMemory mem;

    memory_at(&mem, 0);
    apply_ok(&mem, focus(4));
    apply_ok(&mem, close_id(4));
    apply_ok(&mem, layout_event(1));
    expect_absent(&mem, 4);
    skbd_memory_free(&mem);
}

static void close_of_the_focused_window_clears_focus(void) {
    /* port: close_of_the_focused_window_clears_focus */
    SkbdMemory mem;
    SkbdAction action;

    memory_at(&mem, 0);
    apply_ok(&mem, focus(4));
    expect_focused(&mem, 4);
    action = apply_ok(&mem, close_id(4));
    expect_none(&action);
    expect_unfocused(&mem);
    expect_absent(&mem, 4);
    apply_ok(&mem, layout_event(1));
    expect_unfocused(&mem);
    expect_absent(&mem, 4);
    skbd_memory_free(&mem);
}

static void keymap_reset_forgets_other_windows_and_keeps_the_focused_one(void) {
    /* port: keymap_reset_forgets_other_windows_and_keeps_the_focused_one */
    SkbdMemory mem;

    remember_english_and_ukrainian(&mem);
    apply_ok(&mem, keymap_with_index(0, NULL, 0));
    expect_absent(&mem, 1);
    expect_layout(&mem, 2, 0);
    assert(mem.active == 0);
    assert(mem.pending_len == 0);
    assert(!mem.has_requested);
    skbd_memory_free(&mem);
}

static void replaced_switch_echo_after_the_newest_echo_does_not_change_the_focused_window(void) {
    /* port: replaced_switch_echo_after_the_newest_echo_does_not_change_the_focused_window */
    SkbdMemory mem;

    remember_english_and_ukrainian(&mem);
    apply_ok(&mem, focus(1));
    apply_ok(&mem, focus(2));
    assert(mem.has_requested);
    assert(mem.requested == 1);
    assert(set_has(mem.pending, mem.pending_len, 0));
    apply_ok(&mem, layout_event(1));
    assert(mem.active == 1);
    assert(mem.pending_len == 0);
    assert(!mem.has_requested);
    expect_layout(&mem, 2, 1);
    apply_ok(&mem, layout_event(0));
    expect_focused(&mem, 2);
    assert(mem.active == 1);
    expect_layout(&mem, 2, 1);
    expect_layout(&mem, 1, 0);
    skbd_memory_free(&mem);
}

static void keymap_reset_keeps_tokens_and_skips_an_unknown_app(void) {
    /* port: keymap_reset_keeps_tokens_and_skips_an_unknown_app */
    SkbdMemory mem;
    SkbdLayoutToken fallback;
    SkbdConfigApp apps[2];
    char err[128];
    char us_name[] = "us";
    char ua_name[] = "ua";
    char zz_name[] = "zz";
    char telegram[] = "Telegram";
    char ghost[] = "Ghost";
    const char *uk_then_us[] = {"Ukrainian", "English (US)"};
    const char *us_then_uk[] = {"English (US)", "Ukrainian"};
    uint32_t layout;

    memory_at(&mem, 0);
    fallback = name_token(us_name);
    apps[0].app = telegram;
    apps[0].token = name_token(ua_name);
    apps[1].app = ghost;
    apps[1].token = name_token(zz_name);
    assert(skbd_memory_keep_tokens(&mem, &fallback, apps, 2, err, sizeof err) == 0);
    assert(skbd_memory_remember_current(&mem, 4, err, sizeof err) == 0);
    apply_ok(&mem, focus(8));
    apply_ok(&mem, keymap_with_index(0, uk_then_us, 2));
    assert(mem.fallback == 1);
    assert(app_layout(&mem, "Telegram", &layout));
    assert(layout == 0);
    assert(!app_layout(&mem, "Ghost", &layout));
    expect_absent(&mem, 4);
    expect_layout(&mem, 8, 0);
    assert(mem.has_fallback_token);
    assert(mem.fallback_token.kind == SKBD_TOKEN_NAME);
    assert(strcmp(mem.fallback_token.name, "us") == 0);
    apply_ok(&mem, keymap_with_index(0, us_then_uk, 2));
    assert(mem.fallback == 0);
    assert(app_layout(&mem, "Telegram", &layout));
    assert(layout == 1);
    assert(!app_layout(&mem, "Ghost", &layout));
    skbd_memory_free(&mem);
}

static void keymap_reset_uses_the_default_when_the_fallback_token_is_gone(void) {
    /* port: keymap_reset_uses_the_default_when_the_fallback_token_is_gone */
    SkbdMemory mem;
    SkbdLayoutToken fallback;
    char err[128];
    char zz_name[] = "zz";
    const char *names[] = {"English (US)"};

    assert(skbd_memory_with_layouts(&mem, 1, 1, NULL, 0, err, sizeof err) == 0);
    fallback = name_token(zz_name);
    assert(skbd_memory_keep_tokens(&mem, &fallback, NULL, 0, err, sizeof err) == 0);
    apply_ok(&mem, keymap_with_index(1, names, 1));
    assert(mem.fallback == SKBD_DEFAULT_LAYOUT);
    assert(mem.active == 1);
    skbd_memory_free(&mem);
}

static void keymap_reset_without_an_index_keeps_the_active_layout(void) {
    /* port: keymap_reset_without_an_index_keeps_the_active_layout */
    SkbdMemory mem;

    remember_english_and_ukrainian(&mem);
    apply_ok(&mem, keymap_without_index());
    expect_layout(&mem, 2, 1);
    assert(mem.active == 1);
    skbd_memory_free(&mem);
}

static void abandoned_switch_still_records_the_next_user_layout(void) {
    /* port: abandoned_switch_still_records_the_next_user_layout */
    SkbdMemory mem;
    char err[64];

    memory_at(&mem, 1);
    assert(skbd_memory_remember_current(&mem, 1, err, sizeof err) == 0);
    apply_ok(&mem, layout_event(2));
    apply_ok(&mem, focus(3));
    skbd_memory_abandon_switch(&mem);
    apply_ok(&mem, layout_event(1));
    assert(mem.pending_len == 0);
    assert(!mem.has_requested);
    expect_layout(&mem, 3, 1);
    assert(mem.active == 1);
    skbd_memory_free(&mem);
}

static void new_telegram_uses_the_ua_index(void) {
    /* port: new_telegram_uses_the_ua_index */
    SkbdMemory mem;
    SkbdAction action;

    telegram_rules(&mem, 0);
    action = apply_ok(&mem, seen(3, "Telegram", NULL, NULL));
    expect_switch(&action, 1);
    expect_layout(&mem, 3, 1);
    skbd_memory_free(&mem);
}

static void new_unknown_app_uses_the_fallback(void) {
    /* port: new_unknown_app_uses_the_fallback */
    SkbdMemory mem;
    SkbdAction action;

    telegram_rules(&mem, 2);
    action = apply_ok(&mem, seen(4, "Firefox", NULL, NULL));
    expect_switch(&action, 2);
    expect_layout(&mem, 4, 2);
    skbd_memory_free(&mem);
}

static void empty_desktop_requests_the_fallback_index(void) {
    /* port: empty_desktop_requests_the_fallback_index */
    SkbdMemory mem;
    SkbdAction action;
    char err[64];

    assert(skbd_memory_with_layouts(&mem, 0, 2, NULL, 0, err, sizeof err) == 0);
    assert(skbd_memory_remember_current(&mem, 1, err, sizeof err) == 0);
    action = apply_ok(&mem, unfocus());
    expect_switch(&action, 2);
    expect_unfocused(&mem);
    expect_layout(&mem, 1, 0);
    skbd_memory_free(&mem);
}

static void remembered_window_ignores_the_app_rule(void) {
    /* port: remembered_window_ignores_the_app_rule */
    SkbdMemory mem;
    SkbdAction action;

    telegram_rules(&mem, 0);
    apply_ok(&mem, seen(7, "Telegram", NULL, NULL));
    apply_ok(&mem, layout_event(1));
    apply_ok(&mem, layout_event(0));
    apply_ok(&mem, focus(8));
    apply_ok(&mem, layout_event(1));
    action = apply_ok(&mem, seen(7, "Telegram", NULL, NULL));
    expect_switch(&action, 0);
    expect_layout(&mem, 7, 0);
    skbd_memory_free(&mem);
}

static void x11_dialog_copies_the_parent_layout_and_skips_the_app_rule(void) {
    /* port: x11_dialog_copies_the_parent_layout_and_skips_the_app_rule */
    SkbdMemory mem;
    SkbdAction action;
    int32_t parent_x11 = 100;
    int32_t dialog_x11 = 101;
    int32_t nested_x11 = 102;

    telegram_rules(&mem, 0);
    apply_ok(&mem, seen(10, "Telegram", &parent_x11, NULL));
    apply_ok(&mem, layout_event(1));
    apply_ok(&mem, layout_event(0));
    action = apply_ok(&mem, seen(11, "Telegram", &dialog_x11, &parent_x11));
    expect_none(&action);
    expect_layout(&mem, 11, 0);
    expect_layout(&mem, 10, 0);
    action = apply_ok(&mem, seen(14, "Telegram", &nested_x11, &dialog_x11));
    expect_none(&action);
    expect_layout(&mem, 14, 0);
    action = apply_ok(&mem, seen(12, "Telegram", NULL, NULL));
    expect_switch(&action, 1);
    expect_layout(&mem, 12, 1);
    skbd_memory_free(&mem);
}

static void reload_replaces_apps_and_does_not_clear_window_indexes(void) {
    /* port: reload_replaces_apps_and_does_not_clear_window_indexes */
    SkbdMemory mem;
    SkbdConfigFile file;
    SkbdConfigApp app;
    SkbdRuleSource from;
    SkbdWindowLayout saved[4];
    SkbdAction action;
    size_t saved_len;
    int64_t focused = 0;
    int was_focused;
    char err[512];
    char firefox[] = "Firefox";
    char nope[] = "Nope";
    char zz_name[] = "zz";
    const char *names[] = {"English (US)", "Ukrainian"};
    uint32_t layout;
    int32_t x11 = 40;

    telegram_rules(&mem, 0);
    assert(skbd_memory_watch_keyboard(&mem, "kb-a", err, sizeof err) == 0);
    apply_ok(&mem, seen(4, "Telegram", &x11, NULL));
    apply_ok(&mem, layout_event(1));
    assert(mem.by_window_len <= 4);
    saved_len = mem.by_window_len;
    memcpy(saved, mem.by_window, saved_len * sizeof saved[0]);
    was_focused = skbd_memory_focused_id(&mem, &focused);
    from.fallback_label = "c.toml";
    from.apps_label = "c.toml";
    memset(&file, 0, sizeof file);
    file.has_fallback = 1;
    file.fallback = index_token(0);
    app.app = firefox;
    app.token = index_token(1);
    file.apps = &app;
    file.apps_len = 1;
    file.apps_cap = 1;
    assert(skbd_memory_reload_rules(&mem, &file, names, 2, &from, err, sizeof err) == 0);
    same_windows(&mem, saved, saved_len);
    if (was_focused) {
        expect_focused(&mem, focused);
    } else {
        expect_unfocused(&mem);
    }
    assert(strcmp(skbd_memory_keyboard_id(&mem), "kb-a") == 0);
    assert(mem.fallback == 0);
    assert(app_layout(&mem, "Firefox", &layout));
    assert(layout == 1);
    assert(!app_layout(&mem, "Telegram", &layout));
    file.fallback = name_token(zz_name);
    app.app = nope;
    app.token = index_token(0);
    assert(skbd_memory_reload_rules(&mem, &file, names, 2, &from, err, sizeof err) == -1);
    assert(strstr(err, "zz") != NULL);
    same_windows(&mem, saved, saved_len);
    if (was_focused) {
        expect_focused(&mem, focused);
    } else {
        expect_unfocused(&mem);
    }
    assert(app_layout(&mem, "Firefox", &layout));
    assert(layout == 1);
    assert(!app_layout(&mem, "Nope", &layout));
    action = apply_ok(&mem, seen(9, "Firefox", NULL, NULL));
    expect_none(&action);
    expect_layout(&mem, 9, 1);
    expect_layout(&mem, 4, saved[0].layout);
    skbd_memory_free(&mem);
}

int main(void) {
    focus_restores_the_previous_windows_layout();
    restore_echo_does_not_overwrite_the_window();
    duplicate_layout_event_keeps_the_map();
    new_window_uses_the_default_layout();
    empty_desktop_switches_to_the_default_layout();
    window_open_at_startup_keeps_the_active_layout();
    focus_during_a_pending_switch_does_not_overwrite_either_window();
    user_layout_clears_outstanding_requests_and_stores_on_the_focused_window();
    close_drops_the_window_and_ignores_a_later_layout();
    close_of_the_focused_window_clears_focus();
    keymap_reset_forgets_other_windows_and_keeps_the_focused_one();
    replaced_switch_echo_after_the_newest_echo_does_not_change_the_focused_window();
    keymap_reset_keeps_tokens_and_skips_an_unknown_app();
    keymap_reset_uses_the_default_when_the_fallback_token_is_gone();
    keymap_reset_without_an_index_keeps_the_active_layout();
    abandoned_switch_still_records_the_next_user_layout();
    new_telegram_uses_the_ua_index();
    new_unknown_app_uses_the_fallback();
    empty_desktop_requests_the_fallback_index();
    remembered_window_ignores_the_app_rule();
    x11_dialog_copies_the_parent_layout_and_skips_the_app_rule();
    reload_replaces_apps_and_does_not_clear_window_indexes();
    return 0;
}
