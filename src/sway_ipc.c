#include "skbd/sway.h"

#include "skbd/util.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

/* serde_json order for Window, Input, Workspace, Shutdown. No spaces. */
static const char SUBSCRIBE_EVENTS[] = "[\"window\",\"input\",\"workspace\",\"shutdown\"]";

enum {
    IPC_MAX_TEXT = 4096,
    IPC_MAX_NODES = 65536,
    IPC_MAX_LAYOUT_NAMES = 64,
    IPC_MAX_INPUTS = 256
};

static void input_clear(SkbdInput *input) {
    size_t i;

    if (input == NULL) {
        return;
    }
    free(input->identifier);
    free(input->name);
    free(input->type);
    if (input->layout_names != NULL) {
        for (i = 0; i < input->layout_name_count; i++) {
            free(input->layout_names[i]);
        }
        free(input->layout_names);
    }
    memset(input, 0, sizeof *input);
}

static void node_clear(SkbdNode *node) {
    size_t i;

    if (node == NULL) {
        return;
    }
    free(node->type);
    free(node->app_id);
    free(node->class_name);
    if (node->nodes != NULL) {
        for (i = 0; i < node->nodes_len; i++) {
            node_clear(&node->nodes[i]);
        }
        free(node->nodes);
    }
    if (node->floating_nodes != NULL) {
        for (i = 0; i < node->floating_len; i++) {
            node_clear(&node->floating_nodes[i]);
        }
        free(node->floating_nodes);
    }
    memset(node, 0, sizeof *node);
}

static int oom(char *err, size_t err_cap) {
    return skbd_err_set(err, err_cap, "out of memory");
}

static int dup_text(const char *text, char **out, char *err, size_t err_cap) {
    size_t len;

    if (text == NULL) {
        return oom(err, err_cap);
    }
    len = strlen(text);
    if (len > IPC_MAX_TEXT) {
        return skbd_err_set(err, err_cap, "ipc string is too long");
    }
    *out = skbd_strdup(text);
    if (*out == NULL) {
        return oom(err, err_cap);
    }
    return 0;
}

static int take_optional_string(const SkbdJson *object, const char *key, char **out, char *err, size_t err_cap) {
    const SkbdJson *field = skbd_json_get(object, key);

    *out = NULL;
    if (field == NULL || field->kind == SKBD_JSON_NULL) {
        return 0;
    }
    if (field->kind != SKBD_JSON_STRING || field->text == NULL) {
        return skbd_err_fmt(err, err_cap, "%s is not a string", key);
    }
    return dup_text(field->text, out, err, err_cap);
}

static int take_required_string(const SkbdJson *object, const char *key, char **out, char *err, size_t err_cap) {
    const SkbdJson *field = skbd_json_get(object, key);

    *out = NULL;
    if (field == NULL || field->kind == SKBD_JSON_NULL) {
        return skbd_err_fmt(err, err_cap, "%s is missing", key);
    }
    if (field->kind != SKBD_JSON_STRING || field->text == NULL) {
        return skbd_err_fmt(err, err_cap, "%s is not a string", key);
    }
    return dup_text(field->text, out, err, err_cap);
}

static int take_optional_i64(const SkbdJson *object, const char *key, int *present, int64_t *out, char *err, size_t err_cap) {
    const SkbdJson *field = skbd_json_get(object, key);

    *present = 0;
    if (field == NULL || field->kind == SKBD_JSON_NULL) {
        return 0;
    }
    if (field->kind != SKBD_JSON_NUMBER || field->number_ok == 0) {
        return skbd_err_fmt(err, err_cap, "%s is not an integer", key);
    }
    *out = (int64_t)field->number;
    *present = 1;
    return 0;
}

static int take_required_i64(const SkbdJson *object, const char *key, int64_t *out, char *err, size_t err_cap) {
    int present = 0;

    if (take_optional_i64(object, key, &present, out, err, err_cap) != 0) {
        return -1;
    }
    if (!present) {
        return skbd_err_fmt(err, err_cap, "%s is missing", key);
    }
    return 0;
}

static int take_optional_i32(const SkbdJson *object, const char *key, int *present, int32_t *out, char *err, size_t err_cap) {
    int64_t value = 0;
    int seen = 0;

    if (take_optional_i64(object, key, &seen, &value, err, err_cap) != 0) {
        return -1;
    }
    if (!seen) {
        *present = 0;
        return 0;
    }
    if (value > INT32_MAX || value < INT32_MIN) {
        return skbd_err_fmt(err, err_cap, "%s is out of range", key);
    }
    *out = (int32_t)value;
    *present = 1;
    return 0;
}

static int take_required_bool(const SkbdJson *object, const char *key, int *out, char *err, size_t err_cap) {
    const SkbdJson *field = skbd_json_get(object, key);

    if (field == NULL || field->kind != SKBD_JSON_BOOL) {
        return skbd_err_fmt(err, err_cap, "%s is not a bool", key);
    }
    *out = field->boolean ? 1 : 0;
    return 0;
}

static int append_nodes(const SkbdJson *field, SkbdNode **list, size_t *len, size_t *cap, char *err, size_t err_cap) {
    size_t i;

    if (field->kind != SKBD_JSON_ARRAY) {
        return skbd_err_set(err, err_cap, "node children are not an array");
    }
    if (field->len != 0 && field->children == NULL) {
        return skbd_err_set(err, err_cap, "node children are not an array");
    }
    if (*len > IPC_MAX_NODES || field->len > IPC_MAX_NODES - *len) {
        return skbd_err_set(err, err_cap, "node children are too many");
    }
    for (i = 0; i < field->len; i++) {
        void *grown = *list;

        if (skbd_grow(&grown, cap, *len + 1, sizeof **list, err, err_cap) != 0) {
            return -1;
        }
        *list = grown;
        memset(&(*list)[*len], 0, sizeof **list);
        if (skbd_node_from_json(&field->children[i], &(*list)[*len], err, err_cap) != 0) {
            return -1;
        }
        *len += 1;
    }
    return 0;
}

static int read_optional_nodes(const SkbdJson *json, SkbdNode *out, char *err, size_t err_cap) {
    const SkbdJson *field = skbd_json_get(json, "nodes");

    if (field == NULL) {
        return 0;
    }
    return append_nodes(field, &out->nodes, &out->nodes_len, &out->nodes_cap, err, err_cap);
}

static int read_floating_nodes(const SkbdJson *json, SkbdNode *out, char *err, size_t err_cap) {
    const SkbdJson *field = skbd_json_get(json, "floating_nodes");

    if (field == NULL) {
        return skbd_err_set(err, err_cap, "floating_nodes is missing");
    }
    return append_nodes(field, &out->floating_nodes, &out->floating_len, &out->floating_cap, err, err_cap);
}

static int take_window_properties(const SkbdJson *json, SkbdNode *out, char *err, size_t err_cap) {
    const SkbdJson *props = skbd_json_get(json, "window_properties");

    if (props == NULL || props->kind == SKBD_JSON_NULL) {
        return 0;
    }
    if (props->kind != SKBD_JSON_OBJECT) {
        return skbd_err_set(err, err_cap, "window_properties is not an object");
    }
    if (take_optional_string(props, "class", &out->class_name, err, err_cap) != 0) {
        return -1;
    }
    return take_optional_i32(props, "transient_for", &out->has_transient_for, &out->transient_for, err, err_cap);
}

static int fill_node(const SkbdJson *json, SkbdNode *out, char *err, size_t err_cap) {
    int64_t window = 0;
    int seen = 0;

    if (json == NULL || json->kind != SKBD_JSON_OBJECT) {
        return skbd_err_set(err, err_cap, "node is not an object");
    }
    if (take_required_i64(json, "id", &out->id, err, err_cap) != 0) {
        return -1;
    }
    if (take_required_string(json, "type", &out->type, err, err_cap) != 0) {
        return -1;
    }
    if (take_required_bool(json, "focused", &out->focused, err, err_cap) != 0) {
        return -1;
    }
    if (take_optional_string(json, "app_id", &out->app_id, err, err_cap) != 0) {
        return -1;
    }
    if (take_optional_i64(json, "window", &seen, &window, err, err_cap) != 0) {
        return -1;
    }
    if (seen) {
        out->has_window = 1;
        out->window = window;
    }
    if (take_optional_i32(json, "pid", &out->has_pid, &out->pid, err, err_cap) != 0) {
        return -1;
    }
    if (take_window_properties(json, out, err, err_cap) != 0) {
        return -1;
    }
    if (read_optional_nodes(json, out, err, err_cap) != 0) {
        return -1;
    }
    return read_floating_nodes(json, out, err, err_cap);
}

static int take_layout_names(const SkbdJson *json, SkbdInput *out, char *err, size_t err_cap) {
    const SkbdJson *field = skbd_json_get(json, "xkb_layout_names");
    size_t i;

    if (field == NULL) {
        return 0;
    }
    if (field->kind != SKBD_JSON_ARRAY) {
        return skbd_err_set(err, err_cap, "xkb_layout_names is not an array");
    }
    if (field->len != 0 && field->children == NULL) {
        return skbd_err_set(err, err_cap, "xkb_layout_names is not an array");
    }
    for (i = 0; i < field->len; i++) {
        const SkbdJson *element = &field->children[i];
        void *grown;
        char *name = NULL;

        if (element->kind == SKBD_JSON_NULL) {
            continue;
        }
        if (element->kind != SKBD_JSON_STRING || element->text == NULL) {
            return skbd_err_set(err, err_cap, "xkb_layout_names is not an array of strings");
        }
        if (out->layout_name_count >= IPC_MAX_LAYOUT_NAMES) {
            return skbd_err_set(err, err_cap, "xkb_layout_names is too long");
        }
        if (dup_text(element->text, &name, err, err_cap) != 0) {
            return -1;
        }
        grown = out->layout_names;
        if (skbd_grow(&grown, &out->layout_name_cap, out->layout_name_count + 1, sizeof *out->layout_names, err, err_cap) != 0) {
            free(name);
            return -1;
        }
        out->layout_names = grown;
        out->layout_names[out->layout_name_count] = name;
        out->layout_name_count++;
    }
    return 0;
}

static int fill_input(const SkbdJson *json, SkbdInput *out, char *err, size_t err_cap) {
    if (json == NULL || json->kind != SKBD_JSON_OBJECT) {
        return skbd_err_set(err, err_cap, "input is not an object");
    }
    if (take_required_string(json, "identifier", &out->identifier, err, err_cap) != 0) {
        return -1;
    }
    if (take_required_string(json, "name", &out->name, err, err_cap) != 0) {
        return -1;
    }
    if (take_required_string(json, "type", &out->type, err, err_cap) != 0) {
        return -1;
    }
    if (take_optional_i32(json, "xkb_active_layout_index", &out->has_layout_index, &out->layout_index, err, err_cap) != 0) {
        return -1;
    }
    return take_layout_names(json, out, err, err_cap);
}

static int read_change(const SkbdJson *json, const char **text, char *err, size_t err_cap) {
    const SkbdJson *field = skbd_json_get(json, "change");

    if (field == NULL || field->kind != SKBD_JSON_STRING || field->text == NULL) {
        return skbd_err_set(err, err_cap, "ipc event change is missing");
    }
    *text = field->text;
    return 0;
}

static int fill_window_event(SkbdEvent *event, const SkbdJson *json, char *err, size_t err_cap) {
    const char *change = NULL;
    const SkbdJson *container;

    event->kind = SKBD_EV_WINDOW;
    if (read_change(json, &change, err, err_cap) != 0) {
        return -1;
    }
    if (strcmp(change, SKBD_CHANGE_FOCUS) == 0) {
        event->window_change = SKBD_WINDOW_FOCUS;
    } else if (strcmp(change, SKBD_CHANGE_CLOSE) == 0) {
        event->window_change = SKBD_WINDOW_CLOSE;
    } else {
        event->window_change = SKBD_WINDOW_OTHER;
    }
    container = skbd_json_get(json, "container");
    if (container == NULL) {
        return skbd_err_set(err, err_cap, "container is missing");
    }
    if (skbd_node_from_json(container, &event->container, err, err_cap) != 0) {
        return -1;
    }
    event->has_container = 1;
    return 0;
}

static int fill_input_event(SkbdEvent *event, const SkbdJson *json, char *err, size_t err_cap) {
    const char *change = NULL;
    const SkbdJson *input;

    event->kind = SKBD_EV_INPUT;
    if (read_change(json, &change, err, err_cap) != 0) {
        return -1;
    }
    if (strcmp(change, SKBD_CHANGE_REMOVED) == 0) {
        event->input_change = SKBD_INPUT_REMOVED;
    } else if (strcmp(change, SKBD_CHANGE_XKB_LAYOUT) == 0) {
        event->input_change = SKBD_INPUT_XKB_LAYOUT;
    } else if (strcmp(change, SKBD_CHANGE_XKB_KEYMAP) == 0) {
        event->input_change = SKBD_INPUT_XKB_KEYMAP;
    } else {
        event->input_change = SKBD_INPUT_OTHER;
    }
    input = skbd_json_get(json, "input");
    if (input == NULL) {
        return skbd_err_set(err, err_cap, "input is missing");
    }
    if (skbd_input_from_json(input, &event->input, err, err_cap) != 0) {
        return -1;
    }
    event->has_input = 1;
    return 0;
}

static int fill_workspace_event(SkbdEvent *event, const SkbdJson *json, char *err, size_t err_cap) {
    const char *change = NULL;
    const SkbdJson *current;

    event->kind = SKBD_EV_WORKSPACE;
    if (read_change(json, &change, err, err_cap) != 0) {
        return -1;
    }
    if (strcmp(change, SKBD_CHANGE_FOCUS) == 0) {
        event->workspace_change = SKBD_WORKSPACE_FOCUS;
    } else {
        event->workspace_change = SKBD_WORKSPACE_OTHER;
    }
    current = skbd_json_get(json, "current");
    if (current == NULL || current->kind == SKBD_JSON_NULL) {
        event->has_workspace = 0;
        return 0;
    }
    if (skbd_node_from_json(current, &event->workspace, err, err_cap) != 0) {
        return -1;
    }
    event->has_workspace = 1;
    return 0;
}

static int fill_shutdown_event(SkbdEvent *event, const SkbdJson *json, char *err, size_t err_cap) {
    const char *change = NULL;

    event->kind = SKBD_EV_SHUTDOWN;
    if (read_change(json, &change, err, err_cap) != 0) {
        return -1;
    }
    if (change[0] == '\0') {
        return skbd_err_set(err, err_cap, "ipc event change is missing");
    }
    return 0;
}

static int load_event_object(const char *payload, size_t len, SkbdJson **out, char *err, size_t err_cap) {
    if (skbd_json_parse(payload, len, out, err, err_cap) != 0) {
        return -1;
    }
    if (*out == NULL || (*out)->kind != SKBD_JSON_OBJECT) {
        skbd_json_free(*out);
        *out = NULL;
        return skbd_err_set(err, err_cap, "ipc event is not an object");
    }
    return 0;
}

static int known_event(uint32_t bare) {
    return bare == SKBD_IPC_EV_WORKSPACE || bare == SKBD_IPC_EV_WINDOW || bare == SKBD_IPC_EV_SHUTDOWN || bare == SKBD_IPC_EV_INPUT;
}

static int fill_known_event(SkbdEvent *event, uint32_t bare, const SkbdJson *json, char *err, size_t err_cap) {
    if (bare == SKBD_IPC_EV_WORKSPACE) {
        return fill_workspace_event(event, json, err, err_cap);
    }
    if (bare == SKBD_IPC_EV_WINDOW) {
        return fill_window_event(event, json, err, err_cap);
    }
    if (bare == SKBD_IPC_EV_SHUTDOWN) {
        return fill_shutdown_event(event, json, err, err_cap);
    }
    return fill_input_event(event, json, err, err_cap);
}

static void put_u32(unsigned char *out, uint32_t value) {
    out[0] = (unsigned char)(value & 0xffu);
    out[1] = (unsigned char)((value >> 8) & 0xffu);
    out[2] = (unsigned char)((value >> 16) & 0xffu);
    out[3] = (unsigned char)((value >> 24) & 0xffu);
}

static uint32_t read_u32(const unsigned char *in) {
    return (uint32_t)in[0] | ((uint32_t)in[1] << 8) | ((uint32_t)in[2] << 16) | ((uint32_t)in[3] << 24);
}

static int show_magic(const unsigned char *in, char *shown, size_t shown_cap) {
    size_t i;

    if (shown_cap < 7) {
        return -1;
    }
    for (i = 0; i < SKBD_IPC_MAGIC_LEN; i++) {
        unsigned char c = in[i];
        shown[i] = (c >= 32 && c < 127) ? (char)c : '?';
    }
    shown[SKBD_IPC_MAGIC_LEN] = '\0';
    return 0;
}

static int require_ipc(const SkbdIpc *ipc, char *err, size_t err_cap) {
    if (ipc == NULL || ipc->fd < 0) {
        return skbd_err_set(err, err_cap, "ipc is not connected");
    }
    return 0;
}

/* MSG_NOSIGNAL: a dead socket must not raise SIGPIPE. SIGHUP is the only handled signal. */
static int write_full(int fd, const void *buf, size_t len, char *err, size_t err_cap) {
    const unsigned char *bytes = buf;
    size_t sent = 0;

    while (sent < len) {
        ssize_t n = send(fd, bytes + sent, len - sent, MSG_NOSIGNAL);

        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return skbd_err_fmt(err, err_cap, "ipc write failed: %s", strerror(errno));
        }
        if (n == 0) {
            return skbd_err_set(err, err_cap, "ipc write failed: short write");
        }
        sent += (size_t)n;
    }
    return 0;
}

static int read_full(int fd, void *buf, size_t len, char *err, size_t err_cap) {
    unsigned char *bytes = buf;
    size_t got = 0;

    while (got < len) {
        ssize_t n = read(fd, bytes + got, len - got);

        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return skbd_err_fmt(err, err_cap, "ipc read failed: %s", strerror(errno));
        }
        if (n == 0) {
            return skbd_err_set(err, err_cap, "ipc connection closed");
        }
        got += (size_t)n;
    }
    return 0;
}

static int read_message(int fd, uint32_t *type, unsigned char **payload, size_t *payload_len, char *err, size_t err_cap) {
    unsigned char header[SKBD_IPC_HEADER_BYTES];
    uint32_t length = 0;
    unsigned char *body = NULL;

    *payload = NULL;
    *payload_len = 0;
    if (read_full(fd, header, sizeof header, err, err_cap) != 0) {
        return -1;
    }
    if (skbd_ipc_decode_header(header, sizeof header, &length, type, err, err_cap) != 0) {
        return -1;
    }
    if (length > SKBD_IPC_MAX_PAYLOAD) {
        shutdown(fd, SHUT_RDWR);
        return skbd_err_set(err, err_cap, "ipc payload is too large");
    }
    if (length == 0) {
        return 0;
    }
    body = malloc(length);
    if (body == NULL) {
        return oom(err, err_cap);
    }
    if (read_full(fd, body, length, err, err_cap) != 0) {
        free(body);
        return -1;
    }
    *payload = body;
    *payload_len = length;
    return 0;
}

static int send_command(int fd, uint32_t type, const void *payload, size_t payload_len, char *err, size_t err_cap) {
    unsigned char header[SKBD_IPC_HEADER_BYTES];

    if (payload_len > SKBD_IPC_MAX_PAYLOAD || payload_len > UINT32_MAX) {
        return skbd_err_set(err, err_cap, "ipc payload is too large");
    }
    if (payload_len > 0 && payload == NULL) {
        return skbd_err_set(err, err_cap, "ipc payload is missing");
    }
    if (skbd_ipc_encode_header((uint32_t)payload_len, type, header, sizeof header) != 0) {
        return skbd_err_set(err, err_cap, "ipc header is too small");
    }
    if (write_full(fd, header, sizeof header, err, err_cap) != 0) {
        return -1;
    }
    if (payload_len == 0) {
        return 0;
    }
    return write_full(fd, payload, payload_len, err, err_cap);
}

static int exchange(SkbdIpc *ipc, uint32_t type, const void *payload, size_t payload_len, unsigned char **reply, size_t *reply_len, char *err, size_t err_cap) {
    uint32_t got_type = 0;

    *reply = NULL;
    *reply_len = 0;
    if (require_ipc(ipc, err, err_cap) != 0) {
        return -1;
    }
    if (send_command(ipc->fd, type, payload, payload_len, err, err_cap) != 0) {
        return -1;
    }
    if (read_message(ipc->fd, &got_type, reply, reply_len, err, err_cap) != 0) {
        return -1;
    }
    if (got_type != type) {
        free(*reply);
        *reply = NULL;
        *reply_len = 0;
        return skbd_err_fmt(err, err_cap, "did receive a reply with type '%" PRIu32 "' but send command with type '%" PRIu32 "'", got_type, type);
    }
    return 0;
}

static int set_cloexec(int fd, char *err, size_t err_cap) {
    int flags = fcntl(fd, F_GETFD);

    if (flags < 0 || fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0) {
        return skbd_err_fmt(err, err_cap, "ipc connect failed: %s", strerror(errno));
    }
    return 0;
}

static int connect_socket(int fd, const struct sockaddr_un *addr, char *err, size_t err_cap) {
    int rc;

    do {
        rc = connect(fd, (const struct sockaddr *)addr, sizeof *addr);
    } while (rc < 0 && errno == EINTR);
    if (rc < 0) {
        return skbd_err_fmt(err, err_cap, "ipc connect failed: %s", strerror(errno));
    }
    return 0;
}

int skbd_ipc_check_socket(const char *path, char *err, size_t err_cap) {
    struct stat meta;
    int saved;

    if (path == NULL || path[0] == '\0') {
        return skbd_err_fmt(err, err_cap, "SWAYSOCK path does not exist: %s", path == NULL ? "" : path);
    }
    if (path[0] != '/') {
        return skbd_err_fmt(err, err_cap, "SWAYSOCK path cannot be read: %s (not absolute)", path);
    }
    if (lstat(path, &meta) != 0) {
        saved = errno;
        if (saved == ENOENT) {
            return skbd_err_fmt(err, err_cap, "SWAYSOCK path does not exist: %s", path);
        }
        return skbd_err_fmt(err, err_cap, "SWAYSOCK path cannot be read: %s (%s)", path, strerror(saved));
    }
    if (!S_ISSOCK(meta.st_mode)) {
        return skbd_err_fmt(err, err_cap, "SWAYSOCK is not a socket: %s", path);
    }
    if (meta.st_uid != geteuid()) {
        return skbd_err_fmt(err, err_cap, "SWAYSOCK is not owned by this user: %s", path);
    }
    return 0;
}

static int check_socket_path(const char *path, char *err, size_t err_cap) {
    return skbd_ipc_check_socket(path, err, err_cap);
}

static int reply_accepted(const SkbdJson *root, int *accepted, char *err, size_t err_cap) {
    size_t i;

    if (root == NULL || root->kind != SKBD_JSON_ARRAY) {
        return skbd_err_set(err, err_cap, "ipc command reply is invalid");
    }
    *accepted = 1;
    for (i = 0; i < root->len; i++) {
        const SkbdJson *item = &root->children[i];
        const SkbdJson *success;
        const SkbdJson *message;
        const SkbdJson *parse_error;
        const char *text;
        int parsed;

        if (item->kind != SKBD_JSON_OBJECT) {
            return skbd_err_set(err, err_cap, "ipc command reply is invalid");
        }
        success = skbd_json_get(item, "success");
        if (success == NULL || success->kind != SKBD_JSON_BOOL) {
            return skbd_err_set(err, err_cap, "ipc command reply is invalid");
        }
        if (success->boolean) {
            continue;
        }
        *accepted = 0;
        message = skbd_json_get(item, "error");
        parse_error = skbd_json_get(item, "parse_error");
        text = "";
        if (message != NULL && message->kind == SKBD_JSON_STRING && message->text != NULL) {
            text = message->text;
        }
        parsed = parse_error != NULL && parse_error->kind == SKBD_JSON_BOOL && parse_error->boolean;
        if (parsed) {
            fprintf(stderr, "layout switch failed: command could not be parsed '%.200s'\n", text);
        } else {
            fprintf(stderr, "layout switch failed: command failed with '%.200s'\n", text);
        }
    }
    return 0;
}

static int inputs_from_json(const SkbdJson *root, SkbdInput **out, size_t *out_len, char *err, size_t err_cap) {
    SkbdInput *inputs;
    size_t i;

    if (root == NULL || root->kind != SKBD_JSON_ARRAY) {
        return skbd_err_set(err, err_cap, "inputs are not an array");
    }
    if (root->len == 0) {
        *out = NULL;
        *out_len = 0;
        return 0;
    }
    if (root->len != 0 && root->children == NULL) {
        return skbd_err_set(err, err_cap, "inputs are not an array");
    }
    if (root->len > IPC_MAX_INPUTS) {
        return skbd_err_set(err, err_cap, "inputs are too many");
    }
    inputs = calloc(root->len, sizeof *inputs);
    if (inputs == NULL) {
        return oom(err, err_cap);
    }
    for (i = 0; i < root->len; i++) {
        if (skbd_input_from_json(&root->children[i], &inputs[i], err, err_cap) != 0) {
            skbd_inputs_free(inputs, i);
            return -1;
        }
    }
    *out = inputs;
    *out_len = root->len;
    return 0;
}

int skbd_layout_command(uint32_t index, char *buf, size_t cap) {
    int wrote;

    if (buf == NULL || cap == 0) {
        return -1;
    }
    wrote = snprintf(buf, cap, "input type:keyboard xkb_switch_layout %" PRIu32, index);
    if (wrote < 0 || (size_t)wrote >= cap) {
        buf[0] = '\0';
        return -1;
    }
    return 0;
}

int skbd_ipc_connect(SkbdIpc *ipc, const char *path, char *err, size_t err_cap) {
    struct sockaddr_un addr;
    size_t path_len;
    int fd;

    if (ipc == NULL) {
        return skbd_err_set(err, err_cap, "ipc is not connected");
    }
    if (ipc->fd >= 0) {
        close(ipc->fd);
    }
    ipc->fd = -1;
    if (check_socket_path(path, err, err_cap) != 0) {
        return -1;
    }
    path_len = strlen(path);
    if (path_len >= sizeof addr.sun_path) {
        return skbd_err_fmt(err, err_cap, "SWAYSOCK path cannot be read: %s (File name too long)", path);
    }
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return skbd_err_fmt(err, err_cap, "ipc connect failed: %s", strerror(errno));
    }
    if (set_cloexec(fd, err, err_cap) != 0) {
        close(fd);
        return -1;
    }
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, path, path_len + 1);
    if (connect_socket(fd, &addr, err, err_cap) != 0) {
        close(fd);
        return -1;
    }
    ipc->fd = fd;
    return 0;
}

void skbd_ipc_close(SkbdIpc *ipc) {
    if (ipc == NULL) {
        return;
    }
    if (ipc->fd >= 0) {
        close(ipc->fd);
        ipc->fd = -1;
    }
}

int skbd_ipc_subscribe(SkbdIpc *ipc, char *err, size_t err_cap) {
    unsigned char *reply = NULL;
    size_t reply_len = 0;
    SkbdJson *json = NULL;
    const SkbdJson *success;
    int rc;

    if (exchange(ipc, SKBD_IPC_SUBSCRIBE, SUBSCRIBE_EVENTS, sizeof SUBSCRIBE_EVENTS - 1, &reply, &reply_len, err, err_cap) != 0) {
        return -1;
    }
    rc = skbd_json_parse((const char *)reply, reply_len, &json, err, err_cap);
    free(reply);
    if (rc != 0) {
        return -1;
    }
    success = skbd_json_get(json, "success");
    if (json->kind != SKBD_JSON_OBJECT || success == NULL || success->kind != SKBD_JSON_BOOL) {
        skbd_json_free(json);
        return skbd_err_set(err, err_cap, "ipc subscribe reply is invalid");
    }
    if (!success->boolean) {
        skbd_json_free(json);
        return skbd_err_fmt(err, err_cap, "failed to subscribe to events %s", SUBSCRIBE_EVENTS);
    }
    skbd_json_free(json);
    return 0;
}

int skbd_ipc_get_tree(SkbdIpc *ipc, SkbdNode **out, char *err, size_t err_cap) {
    unsigned char *reply = NULL;
    size_t reply_len = 0;
    SkbdJson *root = NULL;
    SkbdNode *node;
    int rc;

    if (out == NULL) {
        return skbd_err_set(err, err_cap, "tree output is missing");
    }
    *out = NULL;
    if (exchange(ipc, SKBD_IPC_GET_TREE, NULL, 0, &reply, &reply_len, err, err_cap) != 0) {
        return -1;
    }
    rc = skbd_json_parse((const char *)reply, reply_len, &root, err, err_cap);
    free(reply);
    if (rc != 0) {
        return -1;
    }
    node = calloc(1, sizeof *node);
    if (node == NULL) {
        skbd_json_free(root);
        return oom(err, err_cap);
    }
    if (skbd_node_from_json(root, node, err, err_cap) != 0) {
        skbd_json_free(root);
        skbd_node_free(node);
        return -1;
    }
    skbd_json_free(root);
    *out = node;
    return 0;
}

int skbd_ipc_get_inputs(SkbdIpc *ipc, SkbdInput **out, size_t *out_len, char *err, size_t err_cap) {
    unsigned char *reply = NULL;
    size_t reply_len = 0;
    SkbdJson *root = NULL;
    int rc;

    if (out == NULL || out_len == NULL) {
        return skbd_err_set(err, err_cap, "inputs output is missing");
    }
    *out = NULL;
    *out_len = 0;
    if (exchange(ipc, SKBD_IPC_GET_INPUTS, NULL, 0, &reply, &reply_len, err, err_cap) != 0) {
        return -1;
    }
    rc = skbd_json_parse((const char *)reply, reply_len, &root, err, err_cap);
    free(reply);
    if (rc != 0) {
        return -1;
    }
    rc = inputs_from_json(root, out, out_len, err, err_cap);
    skbd_json_free(root);
    return rc;
}

int skbd_ipc_switch_layout(SkbdIpc *ipc, uint32_t index, int *accepted, char *err, size_t err_cap) {
    char command[64];
    unsigned char *reply = NULL;
    size_t reply_len = 0;
    SkbdJson *root = NULL;
    int rc;

    if (accepted == NULL) {
        return skbd_err_set(err, err_cap, "ipc command reply is invalid");
    }
    *accepted = 0;
    if (skbd_layout_command(index, command, sizeof command) != 0) {
        return skbd_err_set(err, err_cap, "layout command is too long");
    }
    if (exchange(ipc, SKBD_IPC_RUN_COMMAND, command, strlen(command), &reply, &reply_len, err, err_cap) != 0) {
        return -1;
    }
    rc = skbd_json_parse((const char *)reply, reply_len, &root, err, err_cap);
    free(reply);
    if (rc != 0) {
        return -1;
    }
    rc = reply_accepted(root, accepted, err, err_cap);
    skbd_json_free(root);
    if (rc != 0) {
        *accepted = 0;
    }
    return rc;
}

int skbd_ipc_read_event(SkbdIpc *ipc, SkbdEvent **out, char *err, size_t err_cap) {
    uint32_t type = 0;
    unsigned char *payload = NULL;
    size_t payload_len = 0;
    int rc;

    if (out == NULL) {
        return skbd_err_set(err, err_cap, "event output is missing");
    }
    *out = NULL;
    if (require_ipc(ipc, err, err_cap) != 0) {
        return -1;
    }
    if (read_message(ipc->fd, &type, &payload, &payload_len, err, err_cap) != 0) {
        return -1;
    }
    rc = skbd_event_from_payload(type, (const char *)payload, payload_len, out, err, err_cap);
    free(payload);
    return rc;
}

int skbd_ipc_encode_header(uint32_t length, uint32_t type, unsigned char *out, size_t out_cap) {
    if (out == NULL || out_cap < SKBD_IPC_HEADER_BYTES) {
        return -1;
    }
    memcpy(out, SKBD_IPC_MAGIC, SKBD_IPC_MAGIC_LEN);
    put_u32(out + SKBD_IPC_MAGIC_LEN, length);
    put_u32(out + SKBD_IPC_MAGIC_LEN + 4, type);
    return 0;
}

int skbd_ipc_decode_header(const unsigned char *in, size_t in_len, uint32_t *length, uint32_t *type, char *err, size_t err_cap) {
    char shown[7];

    if (length == NULL || type == NULL) {
        return skbd_err_set(err, err_cap, "ipc header is invalid");
    }
    if (in == NULL || in_len < SKBD_IPC_HEADER_BYTES) {
        return skbd_err_set(err, err_cap, "ipc header is truncated");
    }
    if (memcmp(in, SKBD_IPC_MAGIC, SKBD_IPC_MAGIC_LEN) != 0) {
        if (show_magic(in, shown, sizeof shown) != 0) {
            return skbd_err_set(err, err_cap, "unexpected magic string, expected 'i3-ipc'");
        }
        return skbd_err_fmt(err, err_cap, "unexpected magic string, expected 'i3-ipc' but got '%s'", shown);
    }
    *length = read_u32(in + SKBD_IPC_MAGIC_LEN);
    *type = read_u32(in + SKBD_IPC_MAGIC_LEN + 4);
    return 0;
}

int skbd_node_from_json(const SkbdJson *json, SkbdNode *out, char *err, size_t err_cap) {
    int rc;

    if (out == NULL) {
        return skbd_err_set(err, err_cap, "node output is missing");
    }
    memset(out, 0, sizeof *out);
    rc = fill_node(json, out, err, err_cap);
    if (rc != 0) {
        node_clear(out);
    }
    return rc;
}

int skbd_input_from_json(const SkbdJson *json, SkbdInput *out, char *err, size_t err_cap) {
    int rc;

    if (out == NULL) {
        return skbd_err_set(err, err_cap, "input output is missing");
    }
    memset(out, 0, sizeof *out);
    rc = fill_input(json, out, err, err_cap);
    if (rc != 0) {
        input_clear(out);
    }
    return rc;
}

int skbd_event_from_payload(uint32_t event_type, const char *payload, size_t len, SkbdEvent **out, char *err, size_t err_cap) {
    uint32_t bare;
    SkbdEvent *event;
    SkbdJson *json = NULL;
    int rc;

    if (out == NULL) {
        return skbd_err_set(err, err_cap, "event output is missing");
    }
    *out = NULL;
    event = calloc(1, sizeof *event);
    if (event == NULL) {
        return oom(err, err_cap);
    }
    event->raw_type = event_type;
    event->window_change = SKBD_WINDOW_OTHER;
    event->input_change = SKBD_INPUT_OTHER;
    event->workspace_change = SKBD_WORKSPACE_OTHER;
    bare = event_type & ~SKBD_IPC_EVENT_BIT;
    if (!known_event(bare)) {
        event->kind = SKBD_EV_OTHER;
        *out = event;
        return 0;
    }
    if (load_event_object(payload, len, &json, err, err_cap) != 0) {
        skbd_event_free(event);
        return -1;
    }
    rc = fill_known_event(event, bare, json, err, err_cap);
    skbd_json_free(json);
    if (rc != 0) {
        skbd_event_free(event);
        return -1;
    }
    *out = event;
    return 0;
}

void skbd_node_free(SkbdNode *node) {
    if (node == NULL) {
        return;
    }
    node_clear(node);
    free(node);
}

void skbd_inputs_free(SkbdInput *inputs, size_t len) {
    size_t i;

    if (inputs == NULL) {
        return;
    }
    for (i = 0; i < len; i++) {
        input_clear(&inputs[i]);
    }
    free(inputs);
}

void skbd_event_free(SkbdEvent *event) {
    if (event == NULL) {
        return;
    }
    node_clear(&event->container);
    node_clear(&event->workspace);
    input_clear(&event->input);
    free(event);
}
