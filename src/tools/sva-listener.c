/* sva-listener.c - all event callbacks + the interactive REPL
 *
 * Owns every PipeWire callback the tool registers (core done/error, registry
 * add/remove, and - crucially - the node param listener Kiruthika declared but
 * never wired) plus the stdin-driven interactive driver.
 *
 * Two gaps in the base pw-voiceui.c are closed here, without adding any new
 * CLI surface:
 *
 *   1. The bound node's SPA_PARAM_Props is where the plugin actually pushes
 *      detection (status/confidence/kw_start/kw_end as strings). The base
 *      only listened on an "sva.state" metadata key that the plugin never
 *      publishes - a dead path, now removed. sva_listener_attach_node() wires
 *      the unused node_listener hook and subscribes to SPA_PARAM_Props so real
 *      events arrive.
 *
 *   2. sva_listener_start_interactive() runs a line REPL on stdin that walks
 *      IDLE -> LOADED -> ACTIVE -> DETECTED using only the existing
 *      set/set-param/send-command vocabulary. DETECTED is driven by the real
 *      node event from (1), not by the client guessing.
 */
#include "config.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <spa/debug/pod.h>
#include <spa/param/props.h>
#include <spa/pod/iter.h>
#include <spa/pod/parser.h>
#include <spa/utils/result.h>
#include <spa/utils/string.h>

#include <pipewire/pipewire.h>

#include "sva-voiceui.h"

const char *sva_state_name(enum sva_state state)
{
    switch (state) {
    case SVA_STATE_IDLE:     return "IDLE";
    case SVA_STATE_LOADED:   return "LOADED";
    case SVA_STATE_ACTIVE:   return "ACTIVE";
    case SVA_STATE_DETECTED: return "DETECTED";
    }
    return "UNKNOWN";
}

/* ---- core callbacks ---------------------------------------------------- */

static void done(void *user_data, uint32_t id, int seq)
{
    struct data *data = user_data;

    if (data->listening)
        return;

    if (data->pending == seq)
        pw_main_loop_quit(data->loop);
}

static void core_error(void *user_data, uint32_t id, int seq, int res, const char *message)
{
    struct data *data = user_data;

    fprintf(stderr, "PipeWire error id:%u seq:%d res:%d (%s): %s\n",
        id, seq, res, spa_strerror(res), message);

    if (data->listening)
        return;

    data->exit_code = EXIT_FAILURE;
    if (seq == data->pending || data->pending == 0)
        pw_main_loop_quit(data->loop);
}

const struct pw_core_events core_events = {
    PW_VERSION_CORE_EVENTS,
    .done = done,
    .error = core_error,
};

/* ---- node param listener : the real detection path --------------------- */

/* The plugin pushes detection as an Object Props carrying a single
 * SPA_PROP_params struct of alternating key/value strings. Walk that struct,
 * print each pair, and when status==detected advance the lifecycle and hand
 * off to the LAB module. */
static void node_param(void *user_data, int seq, uint32_t id,
        uint32_t index, uint32_t next, const struct spa_pod *param)
{
    struct data *data = user_data;
    const struct spa_pod_prop *prop;
    struct spa_pod_object *obj;
    bool detected = false;

    if (param == NULL || id != SPA_PARAM_Props)
        return;
    if (!spa_pod_is_object_type(param, SPA_TYPE_OBJECT_Props))
        return;

    if (data->verbose)
        spa_debug_pod(0, NULL, param);

    obj = (struct spa_pod_object *)param;
    SPA_POD_OBJECT_FOREACH(obj, prop) {
        const struct spa_pod *val;
        struct spa_pod_parser p;
        struct spa_pod_frame f;
        const char *pair[2] = { NULL, NULL };
        int n = 0;

        if (prop->key != SPA_PROP_params)
            continue;

        val = &prop->value;
        if (!spa_pod_is_struct(val))
            continue;

        printf("\n-- SVA detection event --\n");
        spa_pod_parser_pod(&p, val);
        if (spa_pod_parser_push_struct(&p, &f) < 0)
            continue;
        for (;;) {
            const char *s;
            if (spa_pod_parser_get_string(&p, &s) < 0)
                break;
            pair[n & 1] = s;
            if ((n & 1) == 1) {
                printf("  %s = %s\n", pair[0], pair[1]);
                if (spa_streq(pair[0], "status") &&
                        pair[1] && spa_streq(pair[1], "detected"))
                    detected = true;
            }
            n++;
        }
        spa_pod_parser_pop(&p, &f);
        printf("-------------------------\n");
    }

    if (detected && !data->detected) {
        data->detected = true;
        data->state = SVA_STATE_DETECTED;
        printf("[state] -> %s\n", sva_state_name(data->state));
        sva_lab_on_detected(data);
        if (data->interactive)
            printf("sva> ");
        fflush(stdout);
    }
}

static const struct pw_node_events node_events = {
    PW_VERSION_NODE_EVENTS,
    .param = node_param,
};

void sva_listener_attach_node(struct data *data)
{
    uint32_t ids[1] = { SPA_PARAM_Props };

    if (data->node == NULL)
        return;

    pw_node_add_listener(data->node, &data->node_listener, &node_events, data);
    /* Ask the server to stream SPA_PARAM_Props updates as the plugin pushes
     * them - this is what makes real detection events arrive. */
    pw_node_subscribe_params(data->node, ids, SPA_N_ELEMENTS(ids));
}

/* ---- registry callbacks ------------------------------------------------ */

static void registry_global(void *user_data, uint32_t id, uint32_t permissions,
        const char *type, uint32_t version, const struct spa_dict *props)
{
    struct data *data = user_data;
    struct global *global;

    global = calloc(1, sizeof(*global));
    if (global == NULL) {
        data->exit_code = EXIT_FAILURE;
        pw_main_loop_quit(data->loop);
        return;
    }

    global->id = id;
    global->permissions = permissions;
    global->version = version;
    global->type = strdup(type);
    global->props = props ? pw_properties_new_dict(props) : NULL;
    spa_list_append(&data->globals, &global->link);

    if (global_is_voiceui_node(data, global)) {
        if (data->list)
            print_global(global);
        if (data->target == NULL)
            data->target = global;
    }
}

static void registry_global_remove(void *user_data, uint32_t id)
{
    struct data *data = user_data;
    struct global *global, *tmp;

    spa_list_for_each_safe(global, tmp, &data->globals, link) {
        if (global->id == id) {
            if (data->target == global)
                data->target = NULL;
            global_free(global);
            return;
        }
    }
}

const struct pw_registry_events registry_events = {
    PW_VERSION_REGISTRY_EVENTS,
    .global = registry_global,
    .global_remove = registry_global_remove,
};

/* ---- interactive REPL -------------------------------------------------- */

static void interactive_help(void)
{
    printf(
        "Interactive SVA driver. Commands (existing vocabulary only):\n"
        "  set <sva-key> <value>              e.g. set sva.model.path /data/model.bin\n"
        "  set-param <param-id> <json>\n"
        "  send-command <command-id> <json>\n"
        "  state                              show the current lifecycle state\n"
        "  help                               show this help\n"
        "  quit                               exit\n"
        "\n"
        "Typical lifecycle:\n"
        "  set sva.model.path <path>   (IDLE  -> LOADED)\n"
        "  set sva.mode LPI            (LOADED -> ACTIVE)\n"
        "  <wait for detection event>  (ACTIVE -> DETECTED)\n");
}

/* Advance the client-side lifecycle view based on the key just set. The
 * plugin owns the real state; this only mirrors the operations issued. */
static void interactive_advance(struct data *data, const char *key)
{
    const char *wire = phase2_sva_key(key);

    if (spa_streq(wire, "sva.sound.model.path")) {
        if (data->state < SVA_STATE_LOADED)
            data->state = SVA_STATE_LOADED;
    } else if (spa_streq(wire, "sva.mode")) {
        if (data->state < SVA_STATE_ACTIVE)
            data->state = SVA_STATE_ACTIVE;
    } else if (spa_streq(wire, "sva.reset")) {
        data->state = SVA_STATE_IDLE;
        data->detected = false;
    }
    printf("[state] -> %s\n", sva_state_name(data->state));
}

static void interactive_dispatch(struct data *data, char *line)
{
    char *argv[4] = { NULL, NULL, NULL, NULL };
    int argc = 0;
    char *save = NULL;
    char *tok;

    for (tok = strtok_r(line, " \t", &save); tok && argc < 4;
            tok = strtok_r(NULL, " \t", &save))
        argv[argc++] = tok;

    if (argc == 0)
        return;

    if (spa_streq(argv[0], "quit") || spa_streq(argv[0], "q") ||
            spa_streq(argv[0], "exit")) {
        pw_main_loop_quit(data->loop);
        return;
    }
    if (spa_streq(argv[0], "help") || spa_streq(argv[0], "?")) {
        interactive_help();
        return;
    }
    if (spa_streq(argv[0], "state")) {
        printf("[state] %s\n", sva_state_name(data->state));
        return;
    }
    if (spa_streq(argv[0], "set")) {
        if (argc != 3) {
            fprintf(stderr, "usage: set <sva-key> <value>\n");
            return;
        }
        if (!validate_sva_property(argv[1], argv[2]))
            return;
        if (sva_stream_set_key(data, argv[1], argv[2]))
            interactive_advance(data, argv[1]);
        return;
    }
    if (spa_streq(argv[0], "set-param")) {
        if (argc != 3) {
            fprintf(stderr, "usage: set-param <param-id> <json>\n");
            return;
        }
        sva_stream_set_param(data, argv[1], argv[2]);
        return;
    }
    if (spa_streq(argv[0], "send-command")) {
        if (argc != 3) {
            fprintf(stderr, "usage: send-command <command-id> <json>\n");
            return;
        }
        sva_stream_send_command(data, argv[1], argv[2]);
        return;
    }
    fprintf(stderr, "unknown command '%s' (try 'help')\n", argv[0]);
}

static void on_stdin(void *user_data, int fd, uint32_t mask)
{
    struct data *data = user_data;
    char buf[1024];
    ssize_t n;

    if (mask & (SPA_IO_ERR | SPA_IO_HUP)) {
        pw_main_loop_quit(data->loop);
        return;
    }
    if (!(mask & SPA_IO_IN))
        return;

    n = read(fd, buf, sizeof(buf) - 1);
    if (n <= 0) {
        pw_main_loop_quit(data->loop);
        return;
    }
    buf[n] = '\0';

    /* The input may carry several newline-separated lines in one read. */
    char *save = NULL;
    char *line;
    for (line = strtok_r(buf, "\n", &save); line;
            line = strtok_r(NULL, "\n", &save))
        interactive_dispatch(data, line);

    printf("sva> ");
    fflush(stdout);
}

void sva_listener_start_interactive(struct data *data)
{
    struct pw_loop *loop = pw_main_loop_get_loop(data->loop);

    data->state = SVA_STATE_IDLE;
    data->detected = false;

    interactive_help();
    printf("[state] %s\n", sva_state_name(data->state));
    printf("sva> ");
    fflush(stdout);

    data->stdin_source = pw_loop_add_io(loop, STDIN_FILENO,
            SPA_IO_IN | SPA_IO_ERR | SPA_IO_HUP, false, on_stdin, data);
    /* Keep the core alive and responsive while we wait on stdin. */
    data->listening = true;
}
