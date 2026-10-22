/* sva-stream.c - node discovery, bind, and param/command dispatch
 *
 * Owns everything about finding the VoiceUI node on the registry, binding a
 * proxy to it, and turning a user action into an on-wire operation. The POD
 * wire format itself lives in sva-contract.c; this module only drives it.
 *
 * Extracted from Kiruthika's pw-voiceui.c (PipeWire MR !2969). The dispatch
 * was factored into sva_stream_set_key / sva_stream_set_param /
 * sva_stream_send_command so the same code path serves both the one-shot CLI
 * and the interactive REPL - the bytes put on the wire are unchanged.
 */
#include "config.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <spa/debug/file.h>
#include <spa/debug/pod.h>
#include <spa/debug/types.h>
#include <spa/param/props.h>
#include <spa/pod/dynamic.h>
#include <spa/utils/json-pod.h>
#include <spa/utils/keys.h>
#include <spa/utils/result.h>
#include <spa/utils/string.h>

#include <pipewire/pipewire.h>

#include "sva-voiceui.h"

void global_free(struct global *global)
{
    if (global == NULL)
        return;
    spa_list_remove(&global->link);
    pw_properties_free(global->props);
    free(global->type);
    free(global);
}

const char *global_prop(struct global *global, const char *key)
{
    return global->props ? pw_properties_get(global->props, key) : NULL;
}

bool global_is_voiceui_node(struct data *data, struct global *global)
{
    const char *name, *role, *media_class;

    if (!spa_streq(global->type, PW_TYPE_INTERFACE_Node))
        return false;

    if (data->have_target_id)
        return global->id == data->target_id;

    name = global_prop(global, PW_KEY_NODE_NAME);
    role = global_prop(global, PW_KEY_MEDIA_ROLE);
    media_class = global_prop(global, PW_KEY_MEDIA_CLASS);

    if (data->target_name && name && spa_streq(name, data->target_name))
        return true;

    return role && spa_streq(role, data->target_role) &&
        media_class && strstr(media_class, "Audio/Source") != NULL;
}

void print_global(struct global *global)
{
    const char *name = global_prop(global, PW_KEY_NODE_NAME);
    const char *role = global_prop(global, PW_KEY_MEDIA_ROLE);
    const char *media_class = global_prop(global, PW_KEY_MEDIA_CLASS);

    printf("id %u", global->id);
    if (name)
        printf(", node.name=%s", name);
    if (media_class)
        printf(", media.class=%s", media_class);
    if (role)
        printf(", media.role=%s", role);
    printf("\n");
}

static bool parse_pod(const char *type_name, const char *json, const struct spa_type_info *types,
        struct spa_pod_dynamic_builder *builder, struct spa_pod **pod)
{
    const struct spa_type_info *type_info;
    struct spa_error_location loc;
    int res;

    type_info = spa_debug_type_find_short(types, type_name);
    if (type_info == NULL) {
        fprintf(stderr, "unknown type '%s'\n", type_name);
        return false;
    }

    res = spa_json_to_pod_checked(&builder->b, 0, type_info, json, strlen(json), &loc);
    if (res < 0) {
        if (loc.line != 0)
            spa_debug_file_error_location(stderr, &loc,
                    "syntax error in json '%s': %s", json, loc.reason);
        fprintf(stderr, "can't make POD for '%s': %s\n", type_name,
                loc.reason ? loc.reason : spa_strerror(res));
        return false;
    }

    *pod = spa_pod_builder_deref(&builder->b, 0);
    if (*pod == NULL) {
        fprintf(stderr, "can't dereference built POD\n");
        return false;
    }
    return true;
}

bool sva_stream_bind_target(struct data *data)
{
    if (data->target == NULL) {
        fprintf(stderr, "VoiceUI node not found\n");
        return false;
    }

    data->node = pw_registry_bind(data->registry, data->target->id,
            PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, 0);
    if (data->node == NULL) {
        fprintf(stderr, "can't bind VoiceUI node id:%u: %m\n", data->target->id);
        return false;
    }
    return true;
}

bool sva_stream_set_key(struct data *data, const char *key, const char *value)
{
    uint8_t buffer[1024];
    spa_auto(spa_pod_dynamic_builder) builder = { 0 };
    struct spa_pod *pod;

    spa_pod_dynamic_builder_init(&builder, buffer, sizeof(buffer), 4096);
    if (!build_sva_props_pod(&builder.b, key, value, &pod)) {
        fprintf(stderr, "can't build SVA Props POD\n");
        return false;
    }
    if (data->verbose)
        spa_debug_pod(0, NULL, pod);
    pw_node_set_param(data->node, SPA_PARAM_Props, 0, pod);
    return true;
}

bool sva_stream_set_param(struct data *data, const char *param_id, const char *json)
{
    uint8_t buffer[1024];
    spa_auto(spa_pod_dynamic_builder) builder = { 0 };
    struct spa_pod *pod;
    const struct spa_type_info *type_info;

    spa_pod_dynamic_builder_init(&builder, buffer, sizeof(buffer), 4096);
    if (!parse_pod(param_id, json, spa_type_param, &builder, &pod))
        return false;
    type_info = spa_debug_type_find_short(spa_type_param, param_id);
    if (data->verbose)
        spa_debug_pod(0, NULL, pod);
    pw_node_set_param(data->node, type_info->type, 0, pod);
    return true;
}

bool sva_stream_send_command(struct data *data, const char *command_id, const char *json)
{
    uint8_t buffer[1024];
    spa_auto(spa_pod_dynamic_builder) builder = { 0 };
    struct spa_pod *pod;

    spa_pod_dynamic_builder_init(&builder, buffer, sizeof(buffer), 4096);
    if (!parse_pod(command_id, json, spa_type_node_command_id, &builder, &pod))
        return false;
    if (data->verbose)
        spa_debug_pod(0, NULL, pod);
    pw_node_send_command(data->node, (struct spa_command *)pod);
    return true;
}

bool run_action(struct data *data)
{
    if (data->list)
        return true;

    if (data->target == NULL) {
        fprintf(stderr, "VoiceUI node not found\n");
        return false;
    }

    if (data->info) {
        print_global(data->target);
        return true;
    }

    if (!sva_stream_bind_target(data))
        return false;

    /* Interactive TTY driver: a stdin REPL that walks the recognition
     * lifecycle using only the existing keys/commands, and observes real
     * detection events pushed on SPA_PARAM_Props. */
    if (data->interactive) {
        data->sva_node_id = data->target->id;
        sva_listener_attach_node(data);
        sva_listener_start_interactive(data);
        pw_main_loop_run(data->loop);
        return data->exit_code == EXIT_SUCCESS;
    }

    if (data->listen) {
        data->sva_node_id = data->target->id;
        sva_listener_attach_node(data);
        printf("Listening for SVA events on node %u (Ctrl+C to quit)...\n", data->sva_node_id);
        data->listening = true;
        pw_main_loop_run(data->loop);
        return data->exit_code == EXIT_SUCCESS;
    }

    if (spa_streq(data->command, "set")) {
        if (!sva_stream_set_key(data, data->sva_key, data->sva_value))
            return false;
    } else if (spa_streq(data->command, "set-param")) {
        if (!sva_stream_set_param(data, data->param_id, data->json))
            return false;
    } else if (spa_streq(data->command, "send-command")) {
        if (!sva_stream_send_command(data, data->param_id, data->json))
            return false;
    } else {
        fprintf(stderr, "unknown command '%s'\n", data->command);
        return false;
    }

    data->pending = pw_core_sync(data->core, PW_ID_CORE, 0);
    pw_main_loop_run(data->loop);
    return data->exit_code == EXIT_SUCCESS;
}
