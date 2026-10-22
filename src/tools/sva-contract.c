/* On-the-wire SVA property contract: key normalisation, value validation,
 * and the SPA_PARAM_Props POD builder. */
#include "config.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <spa/param/props.h>
#include <spa/pod/builder.h>
#include <spa/utils/string.h>

#include "sva-voiceui.h"

bool parse_bool_value(const char *value, bool *result)
{
    if (spa_streq(value, "true") || spa_streq(value, "1") || spa_streq(value, "yes")) {
        *result = true;
        return true;
    }
    if (spa_streq(value, "false") || spa_streq(value, "0") || spa_streq(value, "no")) {
        *result = false;
        return true;
    }
    return false;
}

const char *phase2_sva_key(const char *key)
{
    if (spa_streq(key, "sva.model.path"))
        return "sva.sound.model.path";
    return key;
}

bool validate_sva_property(const char *key, const char *value)
{
    char *end;
    long level;
    bool bool_value;
    const char *wire_key = phase2_sva_key(key);

    if (spa_streq(wire_key, "sva.confidence.level")) {
        errno = 0;
        level = strtol(value, &end, 10);
        if (errno != 0 || *end != '\0' || level < 0 || level > 100) {
            fprintf(stderr, "sva.confidence.level must be an integer in range 0..100\n");
            return false;
        }
        return true;
    }
    if (spa_streq(wire_key, "sva.sound.model.path")) {
        if (value[0] == '\0') {
            fprintf(stderr, "sva.model.path must not be empty\n");
            return false;
        }
        return true;
    }
    if (spa_streq(wire_key, "sva.capture.requested")) {
        if (!parse_bool_value(value, &bool_value)) {
            fprintf(stderr, "sva.capture.requested must be true/false, yes/no, or 1/0\n");
            return false;
        }
        return true;
    }
    if (spa_streq(wire_key, "sva.mode")) {
        if (!spa_streq(value, "LPI") && !spa_streq(value, "NLPI")) {
            fprintf(stderr, "sva.mode must be LPI or NLPI for phase2 plugin compatibility\n");
            return false;
        }
        return true;
    }
    if (spa_streq(wire_key, "sva.lab.enabled")) {
        if (!parse_bool_value(value, &bool_value)) {
            fprintf(stderr, "sva.lab.enabled must be true/false, yes/no, or 1/0\n");
            return false;
        }
        return true;
    }
    if (spa_streq(wire_key, "sva.lab.duration") ||
            spa_streq(wire_key, "sva.sample.rate") ||
            spa_streq(wire_key, "sva.channels") ||
            spa_streq(wire_key, "sva.bit.width")) {
        errno = 0;
        level = strtol(value, &end, 10);
        if (errno != 0 || *end != '\0' || level < 0) {
            fprintf(stderr, "%s must be a non-negative integer\n", key);
            return false;
        }
        return true;
    }
    if (spa_streq(wire_key, "sva.reset")) {
        if (!parse_bool_value(value, &bool_value)) {
            fprintf(stderr, "sva.reset must be true/false\n");
            return false;
        }
        return true;
    }
    if (spa_streq(wire_key, "sva.restart")) {
        if (!parse_bool_value(value, &bool_value)) {
            fprintf(stderr, "sva.restart must be true/false\n");
            return false;
        }
        return true;
    }
    fprintf(stderr, "unsupported SVA key '%s'\n", key);
    fprintf(stderr, "supported keys: sva.confidence.level, sva.model.path, "
            "sva.sound.model.path, sva.capture.requested, sva.mode, "
            "sva.lab.enabled, sva.lab.duration, sva.sample.rate, "
            "sva.channels, sva.bit.width, sva.reset, sva.restart\n");
    return false;
}

bool build_sva_props_pod(struct spa_pod_builder *builder, const char *key,
        const char *value, struct spa_pod **pod)
{
    struct spa_pod_frame frame[2];

    spa_pod_builder_push_object(builder, &frame[0], SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
    spa_pod_builder_prop(builder, SPA_PROP_params, 0);
    spa_pod_builder_push_struct(builder, &frame[1]);
    key = phase2_sva_key(key);
    spa_pod_builder_string(builder, key);

    if (spa_streq(key, "sva.confidence.level") ||
            spa_streq(key, "sva.lab.duration") ||
            spa_streq(key, "sva.sample.rate") ||
            spa_streq(key, "sva.channels") ||
            spa_streq(key, "sva.bit.width"))
        spa_pod_builder_int(builder, atoi(value));
    else if (spa_streq(key, "sva.capture.requested") ||
            spa_streq(key, "sva.lab.enabled") ||
            spa_streq(key, "sva.reset") ||
            spa_streq(key, "sva.restart")) {
        bool bool_value;
        parse_bool_value(value, &bool_value);
        spa_pod_builder_bool(builder, bool_value);
    } else {
        spa_pod_builder_string(builder, value);
    }

    spa_pod_builder_pop(builder, &frame[1]);
    *pod = spa_pod_builder_pop(builder, &frame[0]);
    return *pod != NULL;
}
