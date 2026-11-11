#include "skbd/runtime.h"

#include <stdio.h>
#include <string.h>

static int fails;

static char *name_en_ua[] = {"English (US)", "Ukrainian"};
static char *name_en[] = {"English (US)"};
static char *name_ua[] = {"Ukrainian"};
static char *name_ru_de[] = {"Russian", "German"};

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "FAIL %s\n", what);
        fails++;
    }
}

static void set_node(SkbdNode *node, const char *type, int app, int window, int pid) {
    memset(node, 0, sizeof *node);
    node->type = (char *)type;
    if (app) {
        node->app_id = (char *)"app";
    }
    if (window) {
        node->has_window = 1;
        node->window = 10;
    }
    if (pid) {
        node->has_pid = 1;
        node->pid = 20;
    }
}

static void set_input(SkbdInput *input, const char *kind, const char *identifier, int has_index, int32_t index, char **names, size_t name_count) {
    memset(input, 0, sizeof *input);
    input->type = (char *)kind;
    input->identifier = (char *)identifier;
    input->has_layout_index = has_index;
    input->layout_index = index;
    input->layout_names = names;
    input->layout_name_count = name_count;
}

static int same_names(const SkbdKeyboard *keyboard, char **names, size_t count) {
    size_t i;

    if (keyboard->name_count != count) {
        return 0;
    }
    for (i = 0; i < count; i++) {
        if (keyboard->names[i] == NULL || strcmp(keyboard->names[i], names[i]) != 0) {
            return 0;
        }
    }
    return 1;
}

static void focus_happening(SkbdHappening *happening, int64_t id) {
    memset(happening, 0, sizeof *happening);
    happening->kind = SKBD_HAPPEN_FOCUS;
    happening->id = id;
}

static int setup_memory(SkbdMemory *memory, uint32_t active, uint32_t fallback, const SkbdAppBinding *apps, size_t app_count, char *err, size_t err_cap) {
    memset(memory, 0, sizeof *memory);
    if (skbd_memory_with_layouts(memory, active, fallback, apps, app_count, err, err_cap) != 0) {
        fprintf(stderr, "FAIL memory setup %s\n", err);
        fails++;
        return -1;
    }
    return 0;
}

/* port: app_id_wins_and_empty_falls_through_to_class */
static void test_app_id_wins_and_empty_falls_through_to_class(void) {
    char key[32];

    check(skbd_app_key("Telegram", "telegram", key, sizeof key) == 1, "app id");
    check(strcmp(key, "Telegram") == 0, key);
    check(skbd_app_key("", "Code", key, sizeof key) == 1, "empty app");
    check(strcmp(key, "Code") == 0, key);
    check(skbd_app_key(NULL, "Code", key, sizeof key) == 1, "class");
    check(strcmp(key, "Code") == 0, key);
    check(skbd_app_key(NULL, NULL, key, sizeof key) == 0, "neither");
    check(skbd_app_key("", "", key, sizeof key) == 0, "both empty");
}

/* port: is_view_needs_a_container_and_an_app_window_or_pid */
static void test_is_view_needs_a_container_and_an_app_window_or_pid(void) {
    SkbdNode node;

    set_node(&node, "con", 1, 0, 0);
    check(skbd_node_is_view(&node), "app");
    set_node(&node, "con", 0, 1, 0);
    check(skbd_node_is_view(&node), "window");
    set_node(&node, "con", 0, 0, 1);
    check(skbd_node_is_view(&node), "pid");
    set_node(&node, "floating_con", 0, 0, 1);
    check(skbd_node_is_view(&node), "floating");
    set_node(&node, "con", 0, 0, 0);
    check(!skbd_node_is_view(&node), "empty con");
    set_node(&node, "workspace", 1, 1, 1);
    check(!skbd_node_is_view(&node), "workspace");
    set_node(&node, "output", 0, 0, 1);
    check(!skbd_node_is_view(&node), "output");
}

/* port: two_keyboards_that_disagree_use_the_first */
static void test_two_keyboards_that_disagree_use_the_first(void) {
    SkbdInput inputs[3];
    SkbdKeyboard keyboard;
    char err[128];

    set_input(&inputs[0], "pointer", "mouse", 0, 0, NULL, 0);
    set_input(&inputs[1], "keyboard", "kb-a", 1, 0, name_en_ua, 2);
    set_input(&inputs[2], "keyboard", "kb-b", 1, 1, name_ua, 1);
    err[0] = '\0';
    memset(&keyboard, 0, sizeof keyboard);
    check(skbd_first_keyboard(inputs, 3, &keyboard, err, sizeof err) == 0, err);
    check(keyboard.identifier != NULL && strcmp(keyboard.identifier, "kb-a") == 0, "id");
    check(keyboard.active == 0, "active");
    check(same_names(&keyboard, name_en_ua, 2), "names");
    skbd_keyboard_free(&keyboard);
}

/* port: keyboard_with_both_an_index_and_names_wins */
static void test_keyboard_with_both_an_index_and_names_wins(void) {
    SkbdInput inputs[3];
    SkbdKeyboard keyboard;
    char err[128];

    set_input(&inputs[0], "keyboard", "names-only", 0, 0, name_en_ua, 2);
    set_input(&inputs[1], "keyboard", "index-only", 1, 1, NULL, 0);
    set_input(&inputs[2], "keyboard", "both", 1, 0, name_en, 1);
    err[0] = '\0';
    memset(&keyboard, 0, sizeof keyboard);
    check(skbd_first_keyboard(inputs, 3, &keyboard, err, sizeof err) == 0, err);
    check(keyboard.identifier != NULL && strcmp(keyboard.identifier, "both") == 0, "id");
    check(keyboard.active == 0, "active");
    check(same_names(&keyboard, name_en, 1), "names");
    skbd_keyboard_free(&keyboard);
}

/* port: first_index_pairs_with_the_longest_name_list */
static void test_first_index_pairs_with_the_longest_name_list(void) {
    SkbdInput inputs[4];
    SkbdKeyboard keyboard;
    char err[128];

    set_input(&inputs[0], "keyboard", "index-only", 1, 1, NULL, 0);
    set_input(&inputs[1], "keyboard", "short", 0, 0, name_en, 1);
    set_input(&inputs[2], "keyboard", "long", 0, 0, name_en_ua, 2);
    set_input(&inputs[3], "keyboard", "also-long", 0, 0, name_ru_de, 2);
    err[0] = '\0';
    memset(&keyboard, 0, sizeof keyboard);
    check(skbd_first_keyboard(inputs, 4, &keyboard, err, sizeof err) == 0, err);
    check(keyboard.identifier != NULL && strcmp(keyboard.identifier, "index-only") == 0, "id");
    check(keyboard.active == 1, "active");
    check(same_names(&keyboard, name_en_ua, 2), "names");
    skbd_keyboard_free(&keyboard);
}

/* port: layout_event_from_another_keyboard_is_ignored */
static void test_layout_event_from_another_keyboard_is_ignored(void) {
    SkbdMemory memory;
    SkbdInput other;
    SkbdInput own;
    SkbdHappening happening;
    SkbdAction action;
    char err[256];

    err[0] = '\0';
    if (setup_memory(&memory, 0, 0, NULL, 0, err, sizeof err) != 0) {
        return;
    }
    if (skbd_memory_watch_keyboard(&memory, "kb-a", err, sizeof err) != 0) {
        fprintf(stderr, "FAIL watch %s\n", err);
        fails++;
        skbd_memory_free(&memory);
        return;
    }
    if (skbd_memory_remember_current(&memory, 1, err, sizeof err) != 0) {
        fprintf(stderr, "FAIL remember %s\n", err);
        fails++;
        skbd_memory_free(&memory);
        return;
    }
    set_input(&other, "keyboard", "kb-b", 1, 1, NULL, 0);
    action = skbd_apply_input(&memory, SKBD_INPUT_XKB_LAYOUT, &other, 0);
    check(action.kind == SKBD_ACTION_NONE, "other layout");
    focus_happening(&happening, 2);
    check(skbd_memory_apply(&memory, &happening, &action, err, sizeof err) == 0, err);
    check(action.kind == SKBD_ACTION_NONE, "focus 2");
    set_input(&own, "keyboard", "kb-a", 1, 1, NULL, 0);
    action = skbd_apply_input(&memory, SKBD_INPUT_XKB_LAYOUT, &own, 0);
    focus_happening(&happening, 1);
    check(skbd_memory_apply(&memory, &happening, &action, err, sizeof err) == 0, err);
    check(action.kind == SKBD_ACTION_SWITCH && action.index == 0, "switch back");
    skbd_memory_free(&memory);
}

/* port: keymap_event_stores_the_index_it_carries */
static void test_keymap_event_stores_the_index_it_carries(void) {
    SkbdMemory memory;
    SkbdInput input;
    SkbdHappening happening;
    SkbdAction action;
    char err[256];

    err[0] = '\0';
    if (setup_memory(&memory, 1, 0, NULL, 0, err, sizeof err) != 0) {
        return;
    }
    if (skbd_memory_watch_keyboard(&memory, "kb-a", err, sizeof err) != 0 ||
        skbd_memory_remember_current(&memory, 4, err, sizeof err) != 0) {
        fprintf(stderr, "FAIL keymap setup %s\n", err);
        fails++;
        skbd_memory_free(&memory);
        return;
    }
    set_input(&input, "keyboard", "kb-a", 1, 0, NULL, 0);
    action = skbd_apply_input(&memory, SKBD_INPUT_XKB_KEYMAP, &input, 0);
    check(action.kind == SKBD_ACTION_NONE, "keymap action");
    focus_happening(&happening, 5);
    check(skbd_memory_apply(&memory, &happening, &action, err, sizeof err) == 0, err);
    check(action.kind == SKBD_ACTION_NONE, "new window stays");
    skbd_memory_free(&memory);
}

/* port: workspace_focus_uses_the_view_in_current */
static void test_workspace_focus_uses_the_view_in_current(void) {
    SkbdNode child;
    SkbdNode workspace;
    const SkbdNode *view;

    set_node(&child, "con", 1, 0, 0);
    child.id = 7;
    child.focused = 1;
    memset(&workspace, 0, sizeof workspace);
    workspace.type = (char *)"workspace";
    workspace.nodes = &child;
    workspace.nodes_len = 1;
    view = skbd_focused_in_workspace(&workspace);
    check(view != NULL && view->id == 7, "focused view");
}

/* port: workspace_without_a_view_asks_for_the_tree */
static void test_workspace_without_a_view_asks_for_the_tree(void) {
    SkbdNode workspace;
    SkbdNode child;
    SkbdNode parent;

    memset(&workspace, 0, sizeof workspace);
    workspace.type = (char *)"workspace";
    check(skbd_focused_in_workspace(&workspace) == NULL, "empty");
    check(skbd_focused_in_workspace(NULL) == NULL, "absent");
    set_node(&child, "con", 0, 0, 0);
    child.focused = 1;
    memset(&parent, 0, sizeof parent);
    parent.type = (char *)"workspace";
    parent.nodes = &child;
    parent.nodes_len = 1;
    check(skbd_focused_in_workspace(&parent) == NULL, "focused non-view");
}

/* port: close_syncs_only_the_focused_window */
static void test_close_syncs_only_the_focused_window(void) {
    SkbdMemory memory;
    SkbdHappening happening;
    SkbdAction action;
    int64_t focused;
    char err[256];

    err[0] = '\0';
    if (setup_memory(&memory, 0, 0, NULL, 0, err, sizeof err) != 0) {
        return;
    }
    focus_happening(&happening, 4);
    if (skbd_memory_apply(&memory, &happening, &action, err, sizeof err) != 0) {
        fprintf(stderr, "FAIL focus 4 %s\n", err);
        fails++;
        skbd_memory_free(&memory);
        return;
    }
    focus_happening(&happening, 5);
    if (skbd_memory_apply(&memory, &happening, &action, err, sizeof err) != 0) {
        fprintf(stderr, "FAIL focus 5 %s\n", err);
        fails++;
        skbd_memory_free(&memory);
        return;
    }
    check(!skbd_close_window(&memory, 4), "close other");
    check(skbd_memory_focused_id(&memory, &focused) == 1 && focused == 5, "still 5");
    check(skbd_close_window(&memory, 5), "close focused");
    check(skbd_memory_focused_id(&memory, &focused) == 0, "none focused");
    skbd_memory_free(&memory);
}

/* port: startup_keeps_the_active_layout_for_every_open_view */
static void test_startup_keeps_the_active_layout_for_every_open_view(void) {
    SkbdNode focused_node;
    SkbdNode other;
    SkbdNode root;
    SkbdMemory memory;
    SkbdAppBinding apps[1];
    SkbdHappening happening;
    SkbdAction action;
    int64_t focused;
    char err[256];

    memset(&focused_node, 0, sizeof focused_node);
    focused_node.id = 1;
    focused_node.type = (char *)"con";
    focused_node.focused = 1;
    focused_node.app_id = (char *)"Telegram";
    focused_node.has_window = 1;
    focused_node.window = 50;
    memset(&other, 0, sizeof other);
    other.id = 2;
    other.type = (char *)"floating_con";
    other.app_id = (char *)"Telegram";
    memset(&root, 0, sizeof root);
    root.type = (char *)"root";
    root.nodes = &focused_node;
    root.nodes_len = 1;
    root.floating_nodes = &other;
    root.floating_len = 1;
    apps[0].app = "Telegram";
    apps[0].index = 1;
    err[0] = '\0';
    if (setup_memory(&memory, 0, 0, apps, 1, err, sizeof err) != 0) {
        return;
    }
    if (skbd_remember_open_views(&memory, &root, err, sizeof err) != 0) {
        fprintf(stderr, "FAIL remember views %s\n", err);
        fails++;
        skbd_memory_free(&memory);
        return;
    }
    check(skbd_memory_focused_id(&memory, &focused) == 1 && focused == 1, "startup focus");
    focus_happening(&happening, 2);
    happening.app = "Telegram";
    check(skbd_memory_apply(&memory, &happening, &action, err, sizeof err) == 0, err);
    check(action.kind == SKBD_ACTION_NONE, "known window");
    focus_happening(&happening, 3);
    happening.app = "Telegram";
    happening.has_x11 = 1;
    happening.x11 = 51;
    happening.has_transient_for = 1;
    happening.transient_for = 50;
    check(skbd_memory_apply(&memory, &happening, &action, err, sizeof err) == 0, err);
    check(action.kind == SKBD_ACTION_NONE, "dialog copies parent");
    skbd_memory_free(&memory);
}

/* port: removed_watched_keyboard_is_replaced_and_the_other_is_ignored_until_then */
static void test_removed_watched_keyboard_is_replaced(void) {
    SkbdMemory memory;
    SkbdInput kb_b;
    SkbdInput kb_c;
    SkbdInput inputs[4];
    SkbdHappening happening;
    SkbdAction action;
    char id[64];
    char err[256];
    int adopted;

    err[0] = '\0';
    if (setup_memory(&memory, 0, 0, NULL, 0, err, sizeof err) != 0) {
        return;
    }
    if (skbd_memory_watch_keyboard(&memory, "kb-a", err, sizeof err) != 0 ||
        skbd_memory_remember_current(&memory, 1, err, sizeof err) != 0) {
        fprintf(stderr, "FAIL adopt setup %s\n", err);
        fails++;
        skbd_memory_free(&memory);
        return;
    }
    set_input(&kb_b, "keyboard", "kb-b", 1, 1, name_en_ua, 2);
    action = skbd_apply_input(&memory, SKBD_INPUT_XKB_LAYOUT, &kb_b, 0);
    check(action.kind == SKBD_ACTION_NONE, "ignored before adopt");
    focus_happening(&happening, 2);
    check(skbd_memory_apply(&memory, &happening, &action, err, sizeof err) == 0, err);
    check(action.kind == SKBD_ACTION_NONE, "focus 2");
    set_input(&inputs[0], "keyboard", "kb-a", 1, 0, name_en_ua, 2);
    set_input(&inputs[1], "keyboard", "index-only", 1, 1, NULL, 0);
    set_input(&inputs[2], "keyboard", "kb-b", 1, 1, name_en_ua, 2);
    set_input(&inputs[3], "keyboard", "kb-c", 1, 0, name_en, 1);
    adopted = skbd_adopt_next_keyboard(&memory, inputs, 4, id, sizeof id, err, sizeof err);
    check(adopted == 1, err);
    check(strcmp(id, "kb-b") == 0, id);
    check(strcmp(skbd_memory_keyboard_id(&memory), "kb-b") == 0, "watched");
    action = skbd_apply_input(&memory, SKBD_INPUT_XKB_LAYOUT, &kb_b, 0);
    focus_happening(&happening, 1);
    check(skbd_memory_apply(&memory, &happening, &action, err, sizeof err) == 0, err);
    check(action.kind == SKBD_ACTION_SWITCH && action.index == 0, "switch to stored");
    set_input(&kb_c, "keyboard", "kb-c", 1, 2, name_en, 1);
    action = skbd_apply_input(&memory, SKBD_INPUT_XKB_LAYOUT, &kb_c, 0);
    check(action.kind == SKBD_ACTION_NONE, "other still ignored");
    focus_happening(&happening, 3);
    check(skbd_memory_apply(&memory, &happening, &action, err, sizeof err) == 0, err);
    check(action.kind == SKBD_ACTION_NONE, "new window");
    skbd_memory_free(&memory);
}

int main(void) {
    test_app_id_wins_and_empty_falls_through_to_class();
    test_is_view_needs_a_container_and_an_app_window_or_pid();
    test_two_keyboards_that_disagree_use_the_first();
    test_keyboard_with_both_an_index_and_names_wins();
    test_first_index_pairs_with_the_longest_name_list();
    test_layout_event_from_another_keyboard_is_ignored();
    test_keymap_event_stores_the_index_it_carries();
    test_workspace_focus_uses_the_view_in_current();
    test_workspace_without_a_view_asks_for_the_tree();
    test_close_syncs_only_the_focused_window();
    test_startup_keeps_the_active_layout_for_every_open_view();
    test_removed_watched_keyboard_is_replaced();
    return fails == 0 ? 0 : 1;
}
