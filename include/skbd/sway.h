#ifndef SKBD_SWAY_H
#define SKBD_SWAY_H

#include "skbd/json.h"

#include <stddef.h>
#include <stdint.h>

/* i3-ipc header: 6 bytes of magic "i3-ipc" (the C string's NUL is not sent),
   then a little-endian uint32 payload length, then a little-endian uint32 type.
   RUN_COMMAND is 0, SUBSCRIBE is 2, GET_TREE is 4, GET_INPUTS is 100.
   Event replies set bit 31 of the type. Workspace event 0, window event 3,
   shutdown event 6, input event 21.
   Subscribe consumes its socket, so commands use a second connection.
   The layout command is: input type:keyboard xkb_switch_layout <index>
   Payloads over SKBD_IPC_MAX_PAYLOAD are rejected. Short reads are retried.
   JSON fields the daemon reads from a node: id, type, focused, nodes,
   floating_nodes, app_id, pid, window, window_properties.class,
   window_properties.transient_for.
   From an input: identifier, name, type, xkb_active_layout_index,
   xkb_layout_names. Change strings: focus, close, xkb_layout, xkb_keymap,
   removed. Node types the view check uses: con, floating_con. */

#define SKBD_IPC_MAGIC "i3-ipc"
#define SKBD_IPC_MAGIC_LEN 6u
#define SKBD_IPC_HEADER_BYTES 14u
#define SKBD_IPC_RUN_COMMAND 0u
#define SKBD_IPC_SUBSCRIBE 2u
#define SKBD_IPC_GET_TREE 4u
#define SKBD_IPC_GET_INPUTS 100u
#define SKBD_IPC_EVENT_BIT 0x80000000u
#define SKBD_IPC_EV_WORKSPACE 0u
#define SKBD_IPC_EV_WINDOW 3u
#define SKBD_IPC_EV_SHUTDOWN 6u
#define SKBD_IPC_EV_INPUT 21u
#define SKBD_IPC_MAX_PAYLOAD (8u * 1024u * 1024u)

#define SKBD_NODE_CON "con"
#define SKBD_NODE_FLOATING_CON "floating_con"
#define SKBD_INPUT_KEYBOARD "keyboard"
#define SKBD_CHANGE_FOCUS "focus"
#define SKBD_CHANGE_CLOSE "close"
#define SKBD_CHANGE_XKB_LAYOUT "xkb_layout"
#define SKBD_CHANGE_XKB_KEYMAP "xkb_keymap"
#define SKBD_CHANGE_REMOVED "removed"

/* One socket. Not a session. fd is -1 when closed. */
typedef struct {
    int fd;
} SkbdIpc;

/* app_id NULL means the field was absent. An empty string means it was present
   and empty: that still counts as a view, and it is not an app key.
   class_name follows the same rule. window is the raw JSON integer. */
typedef struct SkbdNode {
    int64_t id;
    char *type;
    int focused;
    char *app_id;
    char *class_name;
    int has_window;
    int64_t window;
    int has_pid;
    int32_t pid;
    int has_transient_for;
    int32_t transient_for;
    struct SkbdNode *nodes;
    size_t nodes_len;
    size_t nodes_cap;
    struct SkbdNode *floating_nodes;
    size_t floating_len;
    size_t floating_cap;
} SkbdNode;

typedef struct {
    char *identifier;
    char *name;
    char *type;
    int has_layout_index;
    int32_t layout_index;
    char **layout_names;
    size_t layout_name_count;
    size_t layout_name_cap;
} SkbdInput;

typedef enum {
    SKBD_INPUT_REMOVED = 0,
    SKBD_INPUT_XKB_LAYOUT,
    SKBD_INPUT_XKB_KEYMAP,
    SKBD_INPUT_OTHER
} SkbdInputChange;

typedef enum {
    SKBD_WINDOW_FOCUS = 0,
    SKBD_WINDOW_CLOSE,
    SKBD_WINDOW_OTHER
} SkbdWindowChange;

typedef enum {
    SKBD_WORKSPACE_FOCUS = 0,
    SKBD_WORKSPACE_OTHER
} SkbdWorkspaceChange;

typedef enum {
    SKBD_EV_WORKSPACE = 0,
    SKBD_EV_WINDOW,
    SKBD_EV_SHUTDOWN,
    SKBD_EV_INPUT,
    SKBD_EV_OTHER
} SkbdEventKind;

typedef struct {
    SkbdEventKind kind;
    uint32_t raw_type;
    SkbdWindowChange window_change;
    SkbdInputChange input_change;
    SkbdWorkspaceChange workspace_change;
    int has_container;
    SkbdNode container;
    int has_input;
    SkbdInput input;
    int has_workspace;
    SkbdNode workspace;
} SkbdEvent;

/* Rust: switch_layout's command text. index is rendered as an unsigned decimal.
   The buffer receives: input type:keyboard xkb_switch_layout <index> */
int skbd_layout_command(uint32_t index, char *buf, size_t cap);

/* Absolute path. The parent directory must be mode 0700 and owned by this
   user. The socket is opened with O_NOFOLLOW and must be owned by this user.
   The socket mode is not checked. */
int skbd_ipc_check_socket(const char *path, char *err, size_t err_cap);

/* Rust: connect. path is the SWAYSOCK filesystem path. Does not read the environment.
   Connects through the inode pinned by skbd_ipc_check_socket's rules. */
int skbd_ipc_connect(SkbdIpc *ipc, const char *path, char *err, size_t err_cap);

void skbd_ipc_close(SkbdIpc *ipc);

/* Rust: Connection::subscribe. Subscribes to window, input, workspace, and shutdown.
   After success this socket only receives events. */
int skbd_ipc_subscribe(SkbdIpc *ipc, char *err, size_t err_cap);

/* Rust: Connection::get_tree. *out is one owned root. */
int skbd_ipc_get_tree(SkbdIpc *ipc, SkbdNode **out, char *err, size_t err_cap);

/* Rust: Connection::get_inputs. *out_len is 0 and *out is NULL when the list is empty. */
int skbd_ipc_get_inputs(SkbdIpc *ipc, SkbdInput **out, size_t *out_len, char *err, size_t err_cap);

/* Rust: Connection::run_command for the layout switch.
   Return 0 means the exchange finished. *accepted is 1 only when every reply
   object succeeded. Return -1 means the socket failed; the caller does not
   know if the layout moved. */
int skbd_ipc_switch_layout(SkbdIpc *ipc, uint32_t index, int *accepted, char *err, size_t err_cap);

/* Rust: the subscribe iterator's next event. Blocks. Short reads are retried. */
int skbd_ipc_read_event(SkbdIpc *ipc, SkbdEvent **out, char *err, size_t err_cap);

/* Header codec. out must hold SKBD_IPC_HEADER_BYTES. No Rust twin. */
int skbd_ipc_encode_header(uint32_t length, uint32_t type, unsigned char *out, size_t out_cap);
int skbd_ipc_decode_header(const unsigned char *in, size_t in_len, uint32_t *length, uint32_t *type, char *err, size_t err_cap);

/* Parse helpers for fixture bytes. No socket. No Rust twin; they feed the same fields. */
int skbd_node_from_json(const SkbdJson *json, SkbdNode *out, char *err, size_t err_cap);
int skbd_input_from_json(const SkbdJson *json, SkbdInput *out, char *err, size_t err_cap);
int skbd_event_from_payload(uint32_t event_type, const char *payload, size_t len, SkbdEvent **out, char *err, size_t err_cap);

void skbd_node_free(SkbdNode *node);
void skbd_inputs_free(SkbdInput *inputs, size_t len);
void skbd_event_free(SkbdEvent *event);

#endif
