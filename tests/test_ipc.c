#include "skbd/sway.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

static int failures = 0;

static void check(int cond, const char *file, int line, const char *text) {
    if (!cond) {
        fprintf(stderr, "fail %s:%d: %s\n", file, line, text);
        failures++;
    }
}

#define CHECK(cond) check((cond), __FILE__, __LINE__, #cond)

static const char NODE_JSON[] =
    "{\"id\":7,\"type\":\"con\",\"focused\":true,\"app_id\":\"\","
    "\"pid\":null,\"window\":50,\"nodes\":[],\"floating_nodes\":[],"
    "\"window_properties\":{\"class\":\"Code\",\"transient_for\":9}}";

static const char TREE_JSON[] =
    "{\"id\":1,\"type\":\"root\",\"focused\":false,\"nodes\":["
    "{\"id\":2,\"type\":\"con\",\"focused\":true,\"app_id\":\"Telegram\","
    "\"window\":50,\"nodes\":[],\"floating_nodes\":[]}"
    "],\"floating_nodes\":["
    "{\"id\":3,\"type\":\"floating_con\",\"focused\":false,\"app_id\":\"Telegram\","
    "\"nodes\":[],\"floating_nodes\":[]}"
    "]}";

static const char SUBSCRIBE_JSON[] = "[\"window\",\"input\",\"workspace\",\"shutdown\"]";

static void release_node(SkbdNode *node) {
    SkbdNode *held = calloc(1, sizeof *held);

    if (held == NULL) {
        CHECK(0);
        return;
    }
    *held = *node;
    memset(node, 0, sizeof *node);
    skbd_node_free(held);
}

static void release_input(SkbdInput *input) {
    SkbdInput *held = calloc(1, sizeof *held);

    if (held == NULL) {
        CHECK(0);
        return;
    }
    *held = *input;
    memset(input, 0, sizeof *input);
    skbd_inputs_free(held, 1);
}

static int write_full(int fd, const void *buf, size_t len) {
    const unsigned char *bytes = buf;
    size_t sent = 0;

    while (sent < len) {
        ssize_t n = write(fd, bytes + sent, len - sent);

        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
            return -1;
        }
        sent += (size_t)n;
    }
    return 0;
}

static int read_full(int fd, void *buf, size_t len) {
    unsigned char *bytes = buf;
    size_t got = 0;

    while (got < len) {
        ssize_t n = read(fd, bytes + got, len - got);

        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
            return -1;
        }
        got += (size_t)n;
    }
    return 0;
}

struct Script {
    int fd;
    int push_only;
    int chunk;
    int fail;
    uint32_t expect_type;
    const char *expect_payload;
    const unsigned char *reply;
    size_t reply_len;
};

static void *serve(void *arg) {
    struct Script *script = arg;
    unsigned char header[SKBD_IPC_HEADER_BYTES];
    uint32_t length = 0;
    uint32_t type = 0;
    unsigned char *payload = NULL;
    size_t i;

    if (!script->push_only) {
        if (read_full(script->fd, header, sizeof header) != 0 ||
            skbd_ipc_decode_header(header, sizeof header, &length, &type, NULL, 0) != 0) {
            script->fail = 1;
            return NULL;
        }
        if (type != script->expect_type) {
            script->fail = 2;
            return NULL;
        }
        if (length > 0) {
            payload = malloc(length);
            if (payload == NULL || read_full(script->fd, payload, length) != 0) {
                free(payload);
                script->fail = 3;
                return NULL;
            }
        }
        if (script->expect_payload != NULL) {
            size_t expect_len = strlen(script->expect_payload);

            if (expect_len != length || (expect_len > 0 && memcmp(payload, script->expect_payload, expect_len) != 0)) {
                free(payload);
                script->fail = 4;
                return NULL;
            }
        }
        free(payload);
    }
    if (script->chunk) {
        for (i = 0; i < script->reply_len; i++) {
            if (write_full(script->fd, script->reply + i, 1) != 0) {
                script->fail = 5;
                return NULL;
            }
        }
        return NULL;
    }
    if (script->reply_len > 0 && write_full(script->fd, script->reply, script->reply_len) != 0) {
        script->fail = 5;
    }
    return NULL;
}

static unsigned char *frame(uint32_t type, const char *payload, size_t *out_len) {
    size_t n = payload == NULL ? 0 : strlen(payload);
    unsigned char *buf = malloc(SKBD_IPC_HEADER_BYTES + n);

    if (buf == NULL) {
        return NULL;
    }
    if (skbd_ipc_encode_header((uint32_t)n, type, buf, SKBD_IPC_HEADER_BYTES) != 0) {
        free(buf);
        return NULL;
    }
    if (n > 0) {
        memcpy(buf + SKBD_IPC_HEADER_BYTES, payload, n);
    }
    *out_len = SKBD_IPC_HEADER_BYTES + n;
    return buf;
}

static int with_server(
    struct Script script,
    int (*client)(SkbdIpc *, void *, char *, size_t),
    void *ctx,
    int *client_rc,
    char *err,
    size_t err_cap) {
    int fds[2];
    pthread_t thread;
    SkbdIpc ipc;

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
        fprintf(stderr, "socketpair failed\n");
        return -1;
    }
    script.fd = fds[1];
    script.fail = 0;
    if (pthread_create(&thread, NULL, serve, &script) != 0) {
        close(fds[0]);
        close(fds[1]);
        return -1;
    }
    ipc.fd = fds[0];
    *client_rc = client(&ipc, ctx, err, err_cap);
    skbd_ipc_close(&ipc);
    pthread_join(thread, NULL);
    close(fds[1]);
    if (script.fail != 0) {
        fprintf(stderr, "server fail %d\n", script.fail);
        return -1;
    }
    return 0;
}

struct TreeCtx {
    SkbdNode *node;
};

static int client_tree(SkbdIpc *ipc, void *ctx, char *err, size_t err_cap) {
    struct TreeCtx *tree = ctx;
    return skbd_ipc_get_tree(ipc, &tree->node, err, err_cap);
}

struct InputCtx {
    SkbdInput *inputs;
    size_t count;
};

static int client_inputs(SkbdIpc *ipc, void *ctx, char *err, size_t err_cap) {
    struct InputCtx *inputs = ctx;
    return skbd_ipc_get_inputs(ipc, &inputs->inputs, &inputs->count, err, err_cap);
}

struct SwitchCtx {
    uint32_t index;
    int accepted;
};

static int client_switch(SkbdIpc *ipc, void *ctx, char *err, size_t err_cap) {
    struct SwitchCtx *layout = ctx;
    return skbd_ipc_switch_layout(ipc, layout->index, &layout->accepted, err, err_cap);
}

static int client_subscribe(SkbdIpc *ipc, void *ctx, char *err, size_t err_cap) {
    (void)ctx;
    return skbd_ipc_subscribe(ipc, err, err_cap);
}

struct EventCtx {
    SkbdEvent *event;
};

static int client_event(SkbdIpc *ipc, void *ctx, char *err, size_t err_cap) {
    struct EventCtx *event = ctx;
    return skbd_ipc_read_event(ipc, &event->event, err, err_cap);
}

static struct Script command_script(uint32_t type, const char *expect_payload, const unsigned char *reply, size_t reply_len) {
    struct Script script;

    memset(&script, 0, sizeof script);
    script.expect_type = type;
    script.expect_payload = expect_payload;
    script.reply = reply;
    script.reply_len = reply_len;
    script.chunk = 1;
    return script;
}

static void test_header(void) {
    unsigned char buf[SKBD_IPC_HEADER_BYTES];
    unsigned char short_buf[4];
    uint32_t length = 0;
    uint32_t type = 0;
    char err[128];

    CHECK(skbd_ipc_encode_header(0x01020304u, 100u, buf, sizeof buf) == 0);
    CHECK(memcmp(buf, "i3-ipc", 6) == 0);
    CHECK(buf[6] == 0x04 && buf[7] == 0x03 && buf[8] == 0x02 && buf[9] == 0x01);
    CHECK(buf[10] == 100 && buf[11] == 0 && buf[12] == 0 && buf[13] == 0);
    err[0] = '\0';
    CHECK(skbd_ipc_decode_header(buf, sizeof buf, &length, &type, err, sizeof err) == 0);
    CHECK(length == 0x01020304u);
    CHECK(type == 100u);
    CHECK(skbd_ipc_encode_header(1, 1, NULL, 14) != 0);
    CHECK(skbd_ipc_encode_header(1, 1, buf, 13) != 0);
    err[0] = '\0';
    CHECK(skbd_ipc_decode_header(short_buf, sizeof short_buf, &length, &type, err, sizeof err) != 0);
    CHECK(strstr(err, "truncated") != NULL);
    memset(buf, 'x', sizeof buf);
    err[0] = '\0';
    CHECK(skbd_ipc_decode_header(buf, sizeof buf, &length, &type, err, sizeof err) != 0);
    CHECK(strstr(err, "unexpected magic") != NULL);
    CHECK(skbd_ipc_decode_header(buf, sizeof buf, NULL, &type, err, sizeof err) != 0);
}

static void test_layout_command(void) {
    char buf[64];

    CHECK(skbd_layout_command(0, buf, sizeof buf) == 0);
    CHECK(strcmp(buf, "input type:keyboard xkb_switch_layout 0") == 0);
    CHECK(skbd_layout_command(4294967295u, buf, sizeof buf) == 0);
    CHECK(strcmp(buf, "input type:keyboard xkb_switch_layout 4294967295") == 0);
    CHECK(skbd_layout_command(1, buf, 8) != 0);
    CHECK(buf[0] == '\0');
}

static SkbdJson *must_parse(const char *text) {
    SkbdJson *json = NULL;
    char err[160];

    err[0] = '\0';
    CHECK(skbd_json_parse(text, strlen(text), &json, err, sizeof err) == 0);
    return json;
}

static void test_node_and_input(void) {
    SkbdJson *json;
    SkbdNode node;
    SkbdInput input;
    char err[160];

    memset(&node, 0, sizeof node);
    json = must_parse(NODE_JSON);
    err[0] = '\0';
    CHECK(skbd_node_from_json(json, &node, err, sizeof err) == 0);
    CHECK(node.id == 7);
    CHECK(strcmp(node.type, "con") == 0);
    CHECK(node.focused == 1);
    CHECK(node.app_id != NULL && node.app_id[0] == '\0');
    CHECK(node.has_pid == 0);
    CHECK(node.has_window == 1 && node.window == 50);
    CHECK(strcmp(node.class_name, "Code") == 0);
    CHECK(node.has_transient_for == 1 && node.transient_for == 9);
    CHECK(node.nodes_len == 0 && node.floating_len == 0);
    skbd_json_free(json);
    release_node(&node);

    json = must_parse("{\"id\":1,\"type\":\"workspace\",\"focused\":false,\"nodes\":[]}");
    err[0] = '\0';
    CHECK(skbd_node_from_json(json, &node, err, sizeof err) != 0);
    CHECK(strstr(err, "floating_nodes") != NULL);
    skbd_json_free(json);

    json = must_parse(TREE_JSON);
    CHECK(skbd_node_from_json(json, &node, err, sizeof err) == 0);
    CHECK(node.nodes_len == 1 && node.nodes[0].id == 2);
    CHECK(strcmp(node.nodes[0].app_id, "Telegram") == 0);
    CHECK(node.nodes[0].has_window == 1 && node.nodes[0].window == 50);
    CHECK(node.floating_len == 1);
    CHECK(strcmp(node.floating_nodes[0].type, "floating_con") == 0);
    skbd_json_free(json);
    release_node(&node);

    json = must_parse("{\"id\":4,\"type\":\"con\",\"focused\":false,\"app_id\":null,\"nodes\":[],\"floating_nodes\":[]}");
    CHECK(skbd_node_from_json(json, &node, err, sizeof err) == 0);
    CHECK(node.app_id == NULL);
    CHECK(node.has_window == 0 && node.has_pid == 0);
    skbd_json_free(json);
    release_node(&node);

    json = must_parse("{\"id\":5,\"type\":\"con\",\"focused\":false,\"pid\":2147483648,\"nodes\":[],\"floating_nodes\":[]}");
    err[0] = '\0';
    CHECK(skbd_node_from_json(json, &node, err, sizeof err) != 0);
    CHECK(strstr(err, "out of range") != NULL);
    skbd_json_free(json);

    json = must_parse(
        "{\"identifier\":\"kb-a\",\"name\":\"kb-a\",\"type\":\"keyboard\","
        "\"xkb_active_layout_index\":-1,\"xkb_layout_names\":[\"English (US)\",null,\"Ukrainian\"]}");
    CHECK(skbd_input_from_json(json, &input, err, sizeof err) == 0);
    CHECK(strcmp(input.identifier, "kb-a") == 0);
    CHECK(strcmp(input.type, "keyboard") == 0);
    CHECK(input.has_layout_index == 1 && input.layout_index == -1);
    CHECK(input.layout_name_count == 2);
    CHECK(strcmp(input.layout_names[0], "English (US)") == 0);
    CHECK(strcmp(input.layout_names[1], "Ukrainian") == 0);
    skbd_json_free(json);
    release_input(&input);

    json = must_parse("{\"identifier\":\"mouse\",\"name\":\"mouse\",\"type\":\"pointer\",\"xkb_active_layout_index\":null}");
    CHECK(skbd_input_from_json(json, &input, err, sizeof err) == 0);
    CHECK(input.has_layout_index == 0);
    CHECK(input.layout_name_count == 0);
    CHECK(input.layout_names == NULL);
    skbd_json_free(json);
    release_input(&input);
}

static void test_events(void) {
    char text[1600];
    char err[160];
    SkbdEvent *event = NULL;
    int wrote;

    wrote = snprintf(text, sizeof text, "{\"change\":\"focus\",\"container\":%s}", NODE_JSON);
    CHECK(wrote > 0 && (size_t)wrote < sizeof text);
    err[0] = '\0';
    CHECK(skbd_event_from_payload(SKBD_IPC_EV_WINDOW | SKBD_IPC_EVENT_BIT, text, strlen(text), &event, err, sizeof err) == 0);
    CHECK(event->kind == SKBD_EV_WINDOW);
    CHECK(event->raw_type == (SKBD_IPC_EV_WINDOW | SKBD_IPC_EVENT_BIT));
    CHECK(event->window_change == SKBD_WINDOW_FOCUS);
    CHECK(event->has_container == 1 && event->container.id == 7);
    skbd_event_free(event);
    event = NULL;

    wrote = snprintf(text, sizeof text, "{\"change\":\"title\",\"container\":%s}", NODE_JSON);
    CHECK(wrote > 0 && (size_t)wrote < sizeof text);
    CHECK(skbd_event_from_payload(SKBD_IPC_EV_WINDOW, text, strlen(text), &event, err, sizeof err) == 0);
    CHECK(event->window_change == SKBD_WINDOW_OTHER);
    skbd_event_free(event);
    event = NULL;

    wrote = snprintf(
        text,
        sizeof text,
        "{\"change\":\"xkb_layout\",\"input\":{\"identifier\":\"kb-a\",\"name\":\"kb-a\",\"type\":\"keyboard\",\"xkb_active_layout_index\":1}}");
    CHECK(wrote > 0 && (size_t)wrote < sizeof text);
    CHECK(skbd_event_from_payload(SKBD_IPC_EV_INPUT | SKBD_IPC_EVENT_BIT, text, strlen(text), &event, err, sizeof err) == 0);
    CHECK(event->kind == SKBD_EV_INPUT);
    CHECK(event->input_change == SKBD_INPUT_XKB_LAYOUT);
    CHECK(event->has_input == 1);
    CHECK(strcmp(event->input.identifier, "kb-a") == 0);
    CHECK(event->input.layout_index == 1);
    skbd_event_free(event);
    event = NULL;

    CHECK(skbd_event_from_payload(
              SKBD_IPC_EV_INPUT,
              "{\"change\":\"removed\",\"input\":{\"identifier\":\"kb-a\",\"name\":\"kb-a\",\"type\":\"keyboard\"}}",
              strlen("{\"change\":\"removed\",\"input\":{\"identifier\":\"kb-a\",\"name\":\"kb-a\",\"type\":\"keyboard\"}}"),
              &event,
              err,
              sizeof err) == 0);
    CHECK(event->input_change == SKBD_INPUT_REMOVED);
    skbd_event_free(event);
    event = NULL;

    CHECK(skbd_event_from_payload(
              SKBD_IPC_EV_INPUT,
              "{\"change\":\"xkb_keymap\",\"input\":{\"identifier\":\"kb-a\",\"name\":\"kb-a\",\"type\":\"keyboard\",\"xkb_active_layout_index\":0}}",
              strlen("{\"change\":\"xkb_keymap\",\"input\":{\"identifier\":\"kb-a\",\"name\":\"kb-a\",\"type\":\"keyboard\",\"xkb_active_layout_index\":0}}"),
              &event,
              err,
              sizeof err) == 0);
    CHECK(event->input_change == SKBD_INPUT_XKB_KEYMAP);
    skbd_event_free(event);
    event = NULL;

    CHECK(skbd_event_from_payload(
              SKBD_IPC_EV_INPUT,
              "{\"change\":\"added\",\"input\":{\"identifier\":\"kb-a\",\"name\":\"kb-a\",\"type\":\"keyboard\"}}",
              strlen("{\"change\":\"added\",\"input\":{\"identifier\":\"kb-a\",\"name\":\"kb-a\",\"type\":\"keyboard\"}}"),
              &event,
              err,
              sizeof err) == 0);
    CHECK(event->input_change == SKBD_INPUT_OTHER);
    skbd_event_free(event);
    event = NULL;

    CHECK(skbd_event_from_payload(SKBD_IPC_EV_WORKSPACE, "{\"change\":\"focus\",\"current\":null}", strlen("{\"change\":\"focus\",\"current\":null}"), &event, err, sizeof err) == 0);
    CHECK(event->workspace_change == SKBD_WORKSPACE_FOCUS);
    CHECK(event->has_workspace == 0);
    skbd_event_free(event);
    event = NULL;

    wrote = snprintf(
        text,
        sizeof text,
        "{\"change\":\"focus\",\"current\":{\"id\":8,\"type\":\"workspace\",\"focused\":true,\"nodes\":[],\"floating_nodes\":[]}}");
    CHECK(wrote > 0 && (size_t)wrote < sizeof text);
    CHECK(skbd_event_from_payload(SKBD_IPC_EV_WORKSPACE, text, strlen(text), &event, err, sizeof err) == 0);
    CHECK(event->has_workspace == 1 && event->workspace.id == 8);
    CHECK(event->workspace.nodes_len == 0 && event->workspace.floating_len == 0);
    skbd_event_free(event);
    event = NULL;

    CHECK(skbd_event_from_payload(SKBD_IPC_EV_WORKSPACE, "{\"change\":\"init\",\"current\":null}", strlen("{\"change\":\"init\",\"current\":null}"), &event, err, sizeof err) == 0);
    CHECK(event->workspace_change == SKBD_WORKSPACE_OTHER);
    skbd_event_free(event);
    event = NULL;

    CHECK(skbd_event_from_payload(SKBD_IPC_EV_SHUTDOWN, "{\"change\":\"exit\"}", strlen("{\"change\":\"exit\"}"), &event, err, sizeof err) == 0);
    CHECK(event->kind == SKBD_EV_SHUTDOWN);
    skbd_event_free(event);
    event = NULL;

    CHECK(skbd_event_from_payload(7, "nope", 4, &event, err, sizeof err) == 0);
    CHECK(event->kind == SKBD_EV_OTHER);
    CHECK(event->raw_type == 7);
    skbd_event_free(event);

    err[0] = '\0';
    event = (SkbdEvent *)1;
    CHECK(skbd_event_from_payload(SKBD_IPC_EV_WINDOW, "{", 1, &event, err, sizeof err) != 0);
    CHECK(event == NULL);
    CHECK(strstr(err, "truncated") != NULL);
}

/* port: socket_path_must_exist_and_be_a_socket */
static void test_socket_path(void) {
    char dir[] = "/tmp/skbd-ipc-XXXXXX";
    char missing[160];
    char file_path[160];
    char sock_path[160];
    char err[256];
    SkbdIpc ipc;
    struct sockaddr_un addr;
    int listener = -1;
    int file_fd = -1;
    int wrote;

    ipc.fd = -1;
    if (mkdtemp(dir) == NULL) {
        CHECK(0);
        return;
    }
    wrote = snprintf(missing, sizeof missing, "%s/missing", dir);
    CHECK(wrote > 0 && (size_t)wrote < sizeof missing);
    wrote = snprintf(file_path, sizeof file_path, "%s/file", dir);
    CHECK(wrote > 0 && (size_t)wrote < sizeof file_path);
    wrote = snprintf(sock_path, sizeof sock_path, "%s/sock", dir);
    CHECK(wrote > 0 && (size_t)wrote < sizeof sock_path);

    err[0] = '\0';
    CHECK(skbd_ipc_connect(&ipc, missing, err, sizeof err) != 0);
    CHECK(strstr(err, "does not exist") != NULL);
    CHECK(ipc.fd < 0);

    file_fd = open(file_path, O_CREAT | O_WRONLY | O_EXCL, 0600);
    CHECK(file_fd >= 0);
    if (file_fd >= 0) {
        CHECK(write(file_fd, "nope", 4) == 4);
        close(file_fd);
    }
    err[0] = '\0';
    CHECK(skbd_ipc_connect(&ipc, file_path, err, sizeof err) != 0);
    CHECK(strstr(err, "not a socket") != NULL);

    listener = socket(AF_UNIX, SOCK_STREAM, 0);
    CHECK(listener >= 0);
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    wrote = snprintf(addr.sun_path, sizeof addr.sun_path, "%s", sock_path);
    CHECK(wrote > 0 && (size_t)wrote < sizeof addr.sun_path);
    if (listener >= 0 && bind(listener, (struct sockaddr *)&addr, sizeof addr) == 0 && listen(listener, 1) == 0) {
        memcpy(err, "keep", 5);
        CHECK(skbd_ipc_connect(&ipc, sock_path, err, sizeof err) == 0);
        CHECK(strcmp(err, "keep") == 0);
        CHECK(ipc.fd >= 0);
    } else {
        CHECK(0);
    }
    skbd_ipc_close(&ipc);
    if (listener >= 0) {
        close(listener);
    }
    unlink(sock_path);
    unlink(file_path);
    rmdir(dir);
}

/* A mode 0644 socket in a private directory connects. A public parent does not. */
static void test_socket_parent_must_be_private(void) {
    char dir[] = "/tmp/skbd-ipc-parent-XXXXXX";
    char sock_path[160];
    char link_path[160];
    char err[256];
    SkbdIpc ipc;
    struct sockaddr_un addr;
    int listener = -1;
    int wrote;

    ipc.fd = -1;
    if (mkdtemp(dir) == NULL) {
        CHECK(0);
        return;
    }
    wrote = snprintf(sock_path, sizeof sock_path, "%s/sock", dir);
    CHECK(wrote > 0 && (size_t)wrote < sizeof sock_path);
    wrote = snprintf(link_path, sizeof link_path, "%s/link", dir);
    CHECK(wrote > 0 && (size_t)wrote < sizeof link_path);
    listener = socket(AF_UNIX, SOCK_STREAM, 0);
    CHECK(listener >= 0);
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    wrote = snprintf(addr.sun_path, sizeof addr.sun_path, "%s", sock_path);
    CHECK(wrote > 0 && (size_t)wrote < sizeof addr.sun_path);
    if (listener < 0 || bind(listener, (struct sockaddr *)&addr, sizeof addr) != 0 || listen(listener, 1) != 0) {
        CHECK(0);
    } else {
        CHECK(chmod(sock_path, 0644) == 0);
        memcpy(err, "keep", 5);
        CHECK(skbd_ipc_connect(&ipc, sock_path, err, sizeof err) == 0);
        CHECK(strcmp(err, "keep") == 0);
        skbd_ipc_close(&ipc);
        CHECK(symlink(sock_path, link_path) == 0);
        err[0] = '\0';
        CHECK(skbd_ipc_connect(&ipc, link_path, err, sizeof err) != 0);
        CHECK(strstr(err, "not a socket") != NULL);
        CHECK(ipc.fd < 0);
        CHECK(chmod(dir, 0755) == 0);
        err[0] = '\0';
        CHECK(skbd_ipc_connect(&ipc, sock_path, err, sizeof err) != 0);
        CHECK(strstr(err, "0700") != NULL);
        CHECK(ipc.fd < 0);
    }
    skbd_ipc_close(&ipc);
    if (listener >= 0) {
        close(listener);
    }
    unlink(link_path);
    unlink(sock_path);
    rmdir(dir);
}

static void test_tree_and_inputs(void) {
    unsigned char *reply = NULL;
    size_t reply_len = 0;
    struct TreeCtx tree;
    struct InputCtx inputs;
    char err[256];
    int client_rc = 1;
    const char *one_input =
        "[{\"identifier\":\"kb-a\",\"name\":\"At Keyboard\",\"type\":\"keyboard\","
        "\"xkb_active_layout_index\":0,\"xkb_layout_names\":[\"English (US)\",\"Ukrainian\"]}]";

    reply = frame(SKBD_IPC_GET_TREE, TREE_JSON, &reply_len);
    tree.node = NULL;
    CHECK(reply != NULL);
    CHECK(with_server(command_script(SKBD_IPC_GET_TREE, "", reply, reply_len), client_tree, &tree, &client_rc, err, sizeof err) == 0);
    CHECK(client_rc == 0);
    CHECK(tree.node != NULL && tree.node->id == 1 && tree.node->nodes_len == 1);
    skbd_node_free(tree.node);
    free(reply);

    reply = frame(SKBD_IPC_GET_INPUTS, "[]", &reply_len);
    inputs.inputs = (SkbdInput *)1;
    inputs.count = 9;
    client_rc = 1;
    CHECK(with_server(command_script(SKBD_IPC_GET_INPUTS, "", reply, reply_len), client_inputs, &inputs, &client_rc, err, sizeof err) == 0);
    CHECK(client_rc == 0);
    CHECK(inputs.inputs == NULL && inputs.count == 0);
    free(reply);

    reply = frame(SKBD_IPC_GET_INPUTS, one_input, &reply_len);
    inputs.inputs = NULL;
    inputs.count = 0;
    client_rc = 1;
    CHECK(with_server(command_script(SKBD_IPC_GET_INPUTS, "", reply, reply_len), client_inputs, &inputs, &client_rc, err, sizeof err) == 0);
    CHECK(client_rc == 0);
    CHECK(inputs.count == 1);
    CHECK(strcmp(inputs.inputs[0].name, "At Keyboard") == 0);
    CHECK(inputs.inputs[0].layout_name_count == 2);
    skbd_inputs_free(inputs.inputs, inputs.count);
    free(reply);
}

static void test_commands(void) {
    unsigned char *reply = NULL;
    size_t reply_len = 0;
    struct SwitchCtx layout;
    struct Script script;
    char err[256];
    int client_rc = 1;
    unsigned char huge[SKBD_IPC_HEADER_BYTES];
    unsigned char bad[SKBD_IPC_HEADER_BYTES];

    reply = frame(SKBD_IPC_RUN_COMMAND, "[]", &reply_len);
    layout.index = 2;
    layout.accepted = 0;
    CHECK(with_server(
              command_script(SKBD_IPC_RUN_COMMAND, "input type:keyboard xkb_switch_layout 2", reply, reply_len),
              client_switch,
              &layout,
              &client_rc,
              err,
              sizeof err) == 0);
    CHECK(client_rc == 0 && layout.accepted == 1);
    free(reply);

    reply = frame(SKBD_IPC_RUN_COMMAND, "[{\"success\":true}]", &reply_len);
    layout.accepted = 0;
    client_rc = 1;
    CHECK(with_server(command_script(SKBD_IPC_RUN_COMMAND, "input type:keyboard xkb_switch_layout 2", reply, reply_len), client_switch, &layout, &client_rc, err, sizeof err) == 0);
    CHECK(client_rc == 0 && layout.accepted == 1);
    free(reply);

    reply = frame(SKBD_IPC_RUN_COMMAND, "[{\"success\":true},{\"success\":false,\"parse_error\":false,\"error\":\"no\"}]", &reply_len);
    layout.accepted = 1;
    client_rc = 1;
    CHECK(with_server(command_script(SKBD_IPC_RUN_COMMAND, "input type:keyboard xkb_switch_layout 2", reply, reply_len), client_switch, &layout, &client_rc, err, sizeof err) == 0);
    CHECK(client_rc == 0 && layout.accepted == 0);
    free(reply);

    reply = frame(SKBD_IPC_RUN_COMMAND, "[{\"success\":false,\"parse_error\":true,\"error\":\"Unknown command\"}]", &reply_len);
    layout.accepted = 1;
    client_rc = 1;
    CHECK(with_server(command_script(SKBD_IPC_RUN_COMMAND, "input type:keyboard xkb_switch_layout 2", reply, reply_len), client_switch, &layout, &client_rc, err, sizeof err) == 0);
    CHECK(client_rc == 0 && layout.accepted == 0);
    free(reply);

    reply = frame(SKBD_IPC_RUN_COMMAND, "{\"success\":true}", &reply_len);
    layout.accepted = 1;
    client_rc = 0;
    err[0] = '\0';
    CHECK(with_server(command_script(SKBD_IPC_RUN_COMMAND, "input type:keyboard xkb_switch_layout 2", reply, reply_len), client_switch, &layout, &client_rc, err, sizeof err) == 0);
    CHECK(client_rc != 0 && layout.accepted == 0);
    CHECK(strstr(err, "invalid") != NULL);
    free(reply);

    reply = frame(SKBD_IPC_SUBSCRIBE, "{\"success\":true}", &reply_len);
    client_rc = 1;
    memcpy(err, "keep", 5);
    CHECK(with_server(command_script(SKBD_IPC_SUBSCRIBE, SUBSCRIBE_JSON, reply, reply_len), client_subscribe, NULL, &client_rc, err, sizeof err) == 0);
    CHECK(client_rc == 0);
    CHECK(strcmp(err, "keep") == 0);
    free(reply);

    reply = frame(SKBD_IPC_SUBSCRIBE, "{\"success\":false}", &reply_len);
    client_rc = 0;
    err[0] = '\0';
    CHECK(with_server(command_script(SKBD_IPC_SUBSCRIBE, SUBSCRIBE_JSON, reply, reply_len), client_subscribe, NULL, &client_rc, err, sizeof err) == 0);
    CHECK(client_rc != 0);
    CHECK(strstr(err, "failed to subscribe") != NULL);
    free(reply);

    reply = frame(1, "{}", &reply_len);
    {
        struct TreeCtx tree;
        tree.node = NULL;
        client_rc = 0;
        err[0] = '\0';
        CHECK(with_server(command_script(SKBD_IPC_GET_TREE, "", reply, reply_len), client_tree, &tree, &client_rc, err, sizeof err) == 0);
        CHECK(client_rc != 0);
        CHECK(tree.node == NULL);
        CHECK(strstr(err, "did receive a reply") != NULL);
    }
    free(reply);

    CHECK(skbd_ipc_encode_header(SKBD_IPC_MAX_PAYLOAD + 1, SKBD_IPC_GET_TREE, huge, sizeof huge) == 0);
    memset(&script, 0, sizeof script);
    script = command_script(SKBD_IPC_GET_TREE, "", huge, sizeof huge);
    script.chunk = 0;
    {
        struct TreeCtx tree;
        tree.node = NULL;
        client_rc = 0;
        err[0] = '\0';
        CHECK(with_server(script, client_tree, &tree, &client_rc, err, sizeof err) == 0);
        CHECK(client_rc != 0);
        CHECK(strstr(err, "too large") != NULL);
    }

    memset(bad, 'x', sizeof bad);
    script = command_script(SKBD_IPC_GET_TREE, "", bad, sizeof bad);
    script.chunk = 0;
    {
        struct TreeCtx tree;
        tree.node = NULL;
        client_rc = 0;
        err[0] = '\0';
        CHECK(with_server(script, client_tree, &tree, &client_rc, err, sizeof err) == 0);
        CHECK(client_rc != 0);
        CHECK(strstr(err, "unexpected magic") != NULL);
    }
}

static void test_event_short_read(void) {
    char text[1600];
    unsigned char *reply = NULL;
    size_t reply_len = 0;
    struct Script script;
    struct EventCtx ctx;
    char err[256];
    int client_rc = 1;
    int wrote;
    SkbdIpc closed;

    wrote = snprintf(text, sizeof text, "{\"change\":\"close\",\"container\":%s}", NODE_JSON);
    CHECK(wrote > 0 && (size_t)wrote < sizeof text);
    reply = frame(SKBD_IPC_EV_WINDOW | SKBD_IPC_EVENT_BIT, text, &reply_len);
    memset(&script, 0, sizeof script);
    script.push_only = 1;
    script.chunk = 1;
    script.reply = reply;
    script.reply_len = reply_len;
    ctx.event = NULL;
    CHECK(reply != NULL);
    CHECK(with_server(script, client_event, &ctx, &client_rc, err, sizeof err) == 0);
    CHECK(client_rc == 0);
    CHECK(ctx.event != NULL);
    CHECK(ctx.event->kind == SKBD_EV_WINDOW);
    CHECK(ctx.event->window_change == SKBD_WINDOW_CLOSE);
    CHECK(ctx.event->container.id == 7);
    skbd_event_free(ctx.event);
    free(reply);

    closed.fd = -1;
    err[0] = '\0';
    ctx.event = (SkbdEvent *)1;
    CHECK(skbd_ipc_read_event(&closed, &ctx.event, err, sizeof err) != 0);
    CHECK(ctx.event == NULL);
    CHECK(strstr(err, "not connected") != NULL);
}

int main(void) {
    alarm(15);
    test_header();
    test_layout_command();
    test_node_and_input();
    test_events();
    test_socket_path();
    test_socket_parent_must_be_private();
    test_tree_and_inputs();
    test_commands();
    test_event_short_read();
    alarm(0);
    return failures == 0 ? 0 : 1;
}
