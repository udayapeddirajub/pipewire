/* pw-voiceui.c - thin entry point: usage() + main() only
 *
 * Built on top of Kiruthika's pw-voiceui (PipeWire MR !2969). The original
 * 643-line monolith is split into four modules per the design-review action
 * item; this file keeps only argument parsing, PipeWire bring-up/teardown and
 * the dispatch into run_action(). The behaviour-bearing code lives in:
 *
 *   sva-contract.c  on-wire SVA property contract (key/validate/POD)
 *   sva-stream.c    node discovery, bind, param/command dispatch, run_action
 *   sva-listener.c  all event callbacks + the interactive REPL
 *   sva-lab.c       LAB (look-ahead buffer) PCM -> WAV wrapping
 *
 * The split is additive: no CLI surface changes. The only new behaviour is an
 * interactive driver that engages ONLY when `listen` is run on a TTY; piped or
 * redirected stdin stays non-interactive so the proven test procedure is
 * untouched.
 */
#include "config.h"

#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <spa/utils/string.h>

#include <pipewire/pipewire.h>

#include "sva-voiceui.h"

static void usage(const char *name)
{
    fprintf(stderr,
        "Usage:\n"
        "  %s [options] list\n"
        "  %s [options] info\n"
        "  %s [options] listen\n"
        "  %s [options] set <sva-key> <value>\n"
        "  %s [options] set-param <param-id> <param-json>\n"
        "  %s [options] send-command <command-id> <command-json>\n"
        "\n"
        "Options:\n"
        "  -n, --name <node-name>     Target node.name (default: %s)\n"
        "  -r, --role <media-role>    Target media.role (default: %s)\n"
        "  -i, --id <node-id>         Target explicit PipeWire node id\n"
        "  -v, --verbose              Print the POD being sent\n"
        "  -h, --help                 Show this help\n"
        "\n"
        "Note: `listen` on a TTY starts an interactive driver that walks the\n"
        "      recognition lifecycle using only the keys/commands above.\n"
        "Note: piped or redirected stdin keeps `listen` non-interactive, so the\n"
        "      existing scripted test procedure is unchanged.\n"
        "Note: on a detection event the captured LAB keyword audio is wrapped to\n"
        "      a playable WAV beside the plugin's /tmp PCM dump.\n",
        name, name, name, name, name, name, DEFAULT_TARGET_NAME, DEFAULT_TARGET_ROLE);
}

int main(int argc, char *argv[])
{
    struct data data = { 0 };
    struct global *global, *tmp;
    int opt;
    static const struct option long_options[] = {
        { "name", required_argument, NULL, 'n' },
        { "role", required_argument, NULL, 'r' },
        { "id", required_argument, NULL, 'i' },
        { "verbose", no_argument, NULL, 'v' },
        { "help", no_argument, NULL, 'h' },
        { NULL, 0, NULL, 0 }
    };

    data.target_name = DEFAULT_TARGET_NAME;
    data.target_role = DEFAULT_TARGET_ROLE;
    data.exit_code = EXIT_SUCCESS;
    spa_list_init(&data.globals);

    while ((opt = getopt_long(argc, argv, "n:r:i:vh", long_options, NULL)) != -1) {
        char *end;
        switch (opt) {
        case 'n':
            data.target_name = optarg;
            break;
        case 'r':
            data.target_role = optarg;
            break;
        case 'i':
            data.target_id = strtoul(optarg, &end, 10);
            if (*end != '\0') {
                fprintf(stderr, "invalid node id '%s'\n", optarg);
                return EXIT_FAILURE;
            }
            data.have_target_id = true;
            break;
        case 'v':
            data.verbose = true;
            break;
        case 'h':
            usage(argv[0]);
            return EXIT_SUCCESS;
        default:
            usage(argv[0]);
            return EXIT_FAILURE;
        }
    }

    if (optind >= argc) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    data.command = argv[optind++];
    if (spa_streq(data.command, "list")) {
        data.list = true;
    } else if (spa_streq(data.command, "info")) {
        data.info = true;
    } else if (spa_streq(data.command, "set")) {
        if (optind + 2 != argc) {
            usage(argv[0]);
            return EXIT_FAILURE;
        }
        data.sva_key = argv[optind++];
        data.sva_value = argv[optind++];
        if (!validate_sva_property(data.sva_key, data.sva_value))
            return EXIT_FAILURE;
    } else if (spa_streq(data.command, "set-param") ||
            spa_streq(data.command, "send-command")) {
        if (optind + 2 != argc) {
            usage(argv[0]);
            return EXIT_FAILURE;
        }
        data.param_id = argv[optind++];
        data.json = argv[optind++];
    } else if (spa_streq(data.command, "listen")) {
        data.listen = true;
        /* Interactive driver only when stdin is a real terminal; piped or
         * redirected stdin stays non-interactive to preserve the existing
         * scripted test procedure. */
        if (isatty(STDIN_FILENO))
            data.interactive = true;
    } else {
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    pw_init(&argc, &argv);

    data.loop = pw_main_loop_new(NULL);
    if (data.loop == NULL) {
        fprintf(stderr, "can't create main loop: %m\n");
        return EXIT_FAILURE;
    }

    data.context = pw_context_new(pw_main_loop_get_loop(data.loop), NULL, 0);
    if (data.context == NULL) {
        fprintf(stderr, "can't create context: %m\n");
        data.exit_code = EXIT_FAILURE;
        goto out;
    }

    data.core = pw_context_connect(data.context, NULL, 0);
    if (data.core == NULL) {
        fprintf(stderr, "can't connect: %m\n");
        data.exit_code = EXIT_FAILURE;
        goto out;
    }
    pw_core_add_listener(data.core, &data.core_listener, &core_events, &data);

    data.registry = pw_core_get_registry(data.core, PW_VERSION_REGISTRY, 0);
    if (data.registry == NULL) {
        fprintf(stderr, "can't get registry: %m\n");
        data.exit_code = EXIT_FAILURE;
        goto out;
    }
    pw_registry_add_listener(data.registry, &data.registry_listener, &registry_events, &data);

    data.pending = pw_core_sync(data.core, PW_ID_CORE, 0);
    pw_main_loop_run(data.loop);

    if (!run_action(&data))
        data.exit_code = EXIT_FAILURE;

out:
    if (data.stdin_source)
        pw_loop_destroy_source(pw_main_loop_get_loop(data.loop), data.stdin_source);
    if (data.node)
        pw_proxy_destroy((struct pw_proxy *)data.node);
    if (data.registry)
        pw_proxy_destroy((struct pw_proxy *)data.registry);
    if (data.core)
        pw_core_disconnect(data.core);
    if (data.context)
        pw_context_destroy(data.context);
    if (data.loop)
        pw_main_loop_destroy(data.loop);
    spa_list_for_each_safe(global, tmp, &data.globals, link)
        global_free(global);
    pw_deinit();
    return data.exit_code;
}
