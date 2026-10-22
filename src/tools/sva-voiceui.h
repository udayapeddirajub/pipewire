#ifndef SVA_VOICEUI_H
#define SVA_VOICEUI_H

#include <stdbool.h>
#include <stdint.h>

#include <spa/utils/list.h>
#include <spa/utils/hook.h>

#include <pipewire/pipewire.h>

#define DEFAULT_TARGET_ROLE "VoiceUI"
#define DEFAULT_TARGET_NAME "pal_source_voice_ui"

/* Client-side view of the recognition lifecycle; the plugin owns the real
 * state. DETECTED is driven by a real SPA_PARAM_Props event on the node. */
enum sva_state {
    SVA_STATE_IDLE = 0,
    SVA_STATE_LOADED,
    SVA_STATE_ACTIVE,
    SVA_STATE_DETECTED,
};

struct global {
    struct spa_list link;
    uint32_t id;
    uint32_t permissions;
    uint32_t version;
    char *type;
    struct pw_properties *props;
};

struct data {
    struct pw_main_loop *loop;
    struct pw_context *context;
    struct pw_core *core;
    struct pw_registry *registry;
    struct pw_node *node;

    struct spa_hook core_listener;
    struct spa_hook registry_listener;
    struct spa_hook node_listener;

    struct spa_list globals;

    const char *target_name;
    const char *target_role;
    uint32_t target_id;
    bool have_target_id;
    bool list;
    bool info;
    bool verbose;

    const char *command;
    const char *param_id;
    const char *json;
    const char *sva_key;
    const char *sva_value;

    struct global *target;
    int pending;
    int exit_code;
    bool listen;
    bool listening;
    uint32_t sva_node_id;

    bool interactive;
    enum sva_state state;
    bool detected;
    struct spa_source *stdin_source;
};

const char *phase2_sva_key(const char *key);
bool parse_bool_value(const char *value, bool *result);
bool validate_sva_property(const char *key, const char *value);
bool build_sva_props_pod(struct spa_pod_builder *builder, const char *key,
        const char *value, struct spa_pod **pod);

void global_free(struct global *global);
const char *global_prop(struct global *global, const char *key);
bool global_is_voiceui_node(struct data *data, struct global *global);
void print_global(struct global *global);
bool sva_stream_bind_target(struct data *data);
bool sva_stream_set_key(struct data *data, const char *key, const char *value);
bool sva_stream_set_param(struct data *data, const char *param_id, const char *json);
bool sva_stream_send_command(struct data *data, const char *command_id, const char *json);
bool run_action(struct data *data);

extern const struct pw_core_events core_events;
extern const struct pw_registry_events registry_events;
void sva_listener_attach_node(struct data *data);
void sva_listener_start_interactive(struct data *data);
const char *sva_state_name(enum sva_state state);

bool sva_lab_find_latest_dump(char *out, size_t out_sz);
bool sva_lab_pcm_to_wav(const char *pcm_path, const char *wav_path);
void sva_lab_on_detected(struct data *data);

#endif /* SVA_VOICEUI_H */
