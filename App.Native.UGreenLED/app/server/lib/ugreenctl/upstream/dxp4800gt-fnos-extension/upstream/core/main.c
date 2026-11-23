#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "loader.h"
#include "hardware_cli.h"
#include "led/led.h"
#include "fan/thermal_curve.h"
#include "core/wake_cli.h"
#include "core/identity.h"
#include "ugreenctl.h"

#define UGREENCTL_VERSION "0.3.0"

struct cli_options {
    const char *model_directory;
    const char *model_id;
    bool force;
    bool apply;
};

static void usage(FILE *stream)
{
    (void)fprintf(stream,
                  "ugreenctl %s - UGREEN NAS Hardware Management Utility\n\n"
                  "Usage:\n"
                  "  ugreenctl [options] models\n"
                  "  ugreenctl [options] model\n"
                  "  ugreenctl [options] info\n"
                  "  ugreenctl [options] thermal status\n"
                  "  ugreenctl [options] fan status\n"
                  "  ugreenctl [options] fan set <fan-id> <0-255>\n"
                  "  ugreenctl [options] fan mode <fan-id> auto\n"
                  "  ugreenctl [options] power startup get\n"
                  "  ugreenctl [options] power startup set <on|off|restore>\n"
                  "  ugreenctl [options] power rtc-wake models|get\n"
                  "  ugreenctl [options] power rtc-wake set <unix-epoch>\n"
                  "  ugreenctl [options] power rtc-wake clear\n"
                  "  ugreenctl [options] network wol models|get [--interface NAME]\n"
                  "  ugreenctl [options] network wol set <on|off> [--interface NAME]\n"
                  "  ugreenctl led list\n"
                  "  ugreenctl led status [all|power|netdev|diskN...] [--json]\n"
                  "  ugreenctl led set <targets...> --color <blue|#RRGGBB|R,G,B>\n"
                  "              [--brightness <0-255|N%%>] [--mode on|off|blink|breath]\n"
                  "              [--on-ms N --off-ms N] [--force --apply]\n"
                  "  ugreenctl led on|off <targets...> [--force --apply]\n"
                  "  ugreenctl led identify <target> [--seconds N] [--force --apply]\n"
                  "  ugreenctl disk list|map [--json|--tsv]\n"
                  "  ugreenctl disk locate <device|serial|slotN> [--seconds N] [--force --apply]\n\n"
                  "Options (LED/disk options may also follow the command):\n"
                  "  --model <id>        select an exact model (wake commands use independent profiles)\n"
                  "  --plugin-dir <dir>  directory containing model .so files\n"
                  "  --apply             perform a requested write (writes are dry runs otherwise)\n"
                  "  --force             acknowledge an experimental model write\n"
                  "  --json              structured output for LED/disk commands\n"
                  "  --version           print the version\n"
                  "  --help              print this help\n\n"
                  "Fan control prefers the Linux hwmon node named it8613. If it is absent,\n"
                  "a guarded direct Super I/O fallback is available only with no active owner.\n"
                  "Writes require an exact DMI match and always select manual PWM mode.\n",
                  UGREENCTL_VERSION);
}

static const char *startup_policy_name(enum ugreenctl_startup_policy policy)
{
    switch (policy) {
    case UGREENCTL_STARTUP_ON:
        return "on";
    case UGREENCTL_STARTUP_OFF:
        return "off";
    case UGREENCTL_STARTUP_RESTORE:
        return "restore";
    default:
        return "unknown";
    }
}

static const char *wol_policy_name(enum ugreenctl_wol_policy policy)
{
    switch (policy) {
    case UGREENCTL_WOL_ON:
        return "on";
    case UGREENCTL_WOL_OFF:
        return "off";
    default:
        return "unknown";
    }
}

static void print_capabilities(unsigned int capabilities)
{
    bool first = true;
    if (capabilities & UGREENCTL_CAP_FAN) {
        (void)fputs("fan", stdout);
        first = false;
    }
    if (capabilities & UGREENCTL_CAP_LED) {
        (void)fprintf(stdout, "%sled", first ? "" : ", ");
        first = false;
    }
    if (capabilities & UGREENCTL_CAP_POWER) {
        (void)fprintf(stdout, "%spower", first ? "" : ", ");
        first = false;
    }
    if (capabilities & UGREENCTL_CAP_WOL) {
        (void)fprintf(stdout, "%swol", first ? "" : ", ");
        first = false;
    }
    if (first) {
        (void)fputs("none (profile only)", stdout);
    }
}

static int parse_pwm(const char *text, uint8_t *pwm)
{
    char *end;
    long value;

    errno = 0;
    value = strtol(text, &end, 10);
    if (errno != 0 || *text == '\0' || *end != '\0' || value < 0 || value > 255) {
        return -EINVAL;
    }
    *pwm = (uint8_t)value;
    return 0;
}

static enum ugreenctl_startup_policy parse_startup_policy(const char *text)
{
    if (strcmp(text, "on") == 0) {
        return UGREENCTL_STARTUP_ON;
    }
    if (strcmp(text, "off") == 0) {
        return UGREENCTL_STARTUP_OFF;
    }
    if (strcmp(text, "restore") == 0 || strcmp(text, "last") == 0) {
        return UGREENCTL_STARTUP_RESTORE;
    }
    return UGREENCTL_STARTUP_UNKNOWN;
}

static int choose_plugin(const struct ugreenctl_plugin_set *set,
                         const struct cli_options *options,
                         const struct ugreenctl_plugin **chosen)
{
    char product_name[256];
    int dmi_result = ugreenctl_read_dmi_product(product_name, sizeof(product_name));
    const struct ugreenctl_plugin *plugin;

    if (!dmi_result && !strcmp(product_name, "Default string")) {
        struct ugreenctl_machine_identity identity;
        if (!ugreenctl_identity_read(&identity, NULL, 0))
            snprintf(product_name, sizeof(product_name), "%s", ugreenctl_identity_model(&identity));
    }

    if (options->model_id != NULL) {
        plugin = ugreenctl_find_plugin(set, options->model_id);
        if (plugin == NULL) {
            (void)fprintf(stderr, "error: model plugin '%s' was not found\n", options->model_id);
            return -ENOENT;
        }
        if (dmi_result == 0) {
            const char * const *name;
            bool matches = false;
            for (name = plugin->dmi_product_names; name != NULL && *name != NULL; ++name) {
                if (strcmp(*name, product_name) == 0) {
                    matches = true;
                    break;
                }
            }
            if (!matches) {
                (void)fprintf(stderr,
                              "error: selected model '%s' does not match DMI product '%s'; "
                              "hardware writes require an exact DMI match\n",
                              plugin->id, product_name);
                return -ENODEV;
            }
        }
        if (dmi_result != 0) {
            (void)fprintf(stderr,
                          "error: cannot verify the exact DMI product name for model '%s'\n",
                          plugin->id);
            return dmi_result;
        }
        *chosen = plugin;
        return 0;
    }
    if (dmi_result != 0) {
        (void)fprintf(stderr,
                      "error: cannot determine the exact DMI product name; hardware access "
                      "is disabled\n");
        return dmi_result;
    }
    plugin = ugreenctl_match_product(set, product_name);
    if (plugin == NULL) {
        (void)fprintf(stderr,
                      "error: no plugin matches DMI product '%s'; run 'ugreenctl models'\n",
                      product_name);
        return -ENODEV;
    }
    *chosen = plugin;
    return 0;
}

static int require_apply(const struct cli_options *options, const char *description)
{
    if (options->apply) {
        return 0;
    }
    (void)printf("dry-run: would %s\nrerun with --apply to write hardware state\n", description);
    return 1;
}

static void print_fan_status(const struct ugreenctl_fan_status *fan)
{
    (void)printf("%s:", fan->id);
    if (fan->pwm_known) {
        (void)printf(" pwm=%u", fan->pwm);
    } else {
        (void)fputs(" pwm=unknown", stdout);
    }
    if (fan->mode_known) {
        (void)printf(" mode=%s", fan->manual ? "manual" : "auto");
    } else {
        (void)fputs(" mode=unknown", stdout);
    }
    (void)printf(" tach=%u rpm=%lu\n", fan->tachometer, fan->rpm);
}

static void print_status(const struct ugreenctl_status *status)
{
    size_t index;
    (void)printf("controller: %s\n", status->controller);
    (void)printf("startup: %s\n", startup_policy_name(status->startup_policy));
    (void)printf("wol: %s\n", wol_policy_name(status->wol_policy));
    for (index = 0; index < status->fan_count; ++index) {
        (void)fputs("fan ", stdout);
        print_fan_status(&status->fans[index]);
    }
}

static void print_thermal_snapshot(const struct ugreenctl_thermal_snapshot *snapshot)
{
    (void)printf("cpu_celsius=%d cpu_peak_celsius=%d hdd_celsius=%d ssd_celsius=%d\n",
                 snapshot->cpu_celsius, snapshot->cpu_peak_celsius,
                 snapshot->hdd_celsius, snapshot->ssd_celsius);
}

static const char *plugin_controller_name(const struct ugreenctl_plugin *plugin)
{
    if (plugin->abi_version >= UGREENCTL_PLUGIN_ABI_V4 &&
        plugin->controller_name != NULL && plugin->controller_name[0] != '\0') {
        return plugin->controller_name;
    }
    return "model-defined controller";
}

static int plugin_read_fans(const struct ugreenctl_plugin *plugin,
                            const struct ugreenctl_request *request,
                            struct ugreenctl_fan_status *fans, size_t *fan_count,
                            char *error, size_t error_size)
{
    if (plugin->abi_version >= UGREENCTL_PLUGIN_ABI_V4 && plugin->read_fans != NULL) {
        return plugin->read_fans(request, fans, fan_count, error, error_size);
    }
    if (plugin->read_status != NULL) {
        struct ugreenctl_status status;
        int result = plugin->read_status(request, &status, error, error_size);
        if (result == 0) {
            memcpy(fans, status.fans, status.fan_count * sizeof(status.fans[0]));
            *fan_count = status.fan_count;
        }
        return result;
    }
    (void)snprintf(error, error_size, "fan status is not available for %s", plugin->id);
    return -ENOTSUP;
}

static int plugin_read_startup_policy(const struct ugreenctl_plugin *plugin,
                                      const struct ugreenctl_request *request,
                                      enum ugreenctl_startup_policy *policy,
                                      char *error, size_t error_size)
{
    if (plugin->abi_version >= UGREENCTL_PLUGIN_ABI_V4 &&
        plugin->read_startup_policy != NULL) {
        return plugin->read_startup_policy(request, policy, error, error_size);
    }
    if (plugin->read_status != NULL) {
        struct ugreenctl_status status;
        int result = plugin->read_status(request, &status, error, error_size);
        if (result == 0) {
            *policy = status.startup_policy;
        }
        return result;
    }
    (void)snprintf(error, error_size, "startup policy is not available for %s", plugin->id);
    return -ENOTSUP;
}

static int plugin_read_wol_policy(const struct ugreenctl_plugin *plugin,
                                  const struct ugreenctl_request *request,
                                  enum ugreenctl_wol_policy *policy,
                                  char *error, size_t error_size)
{
    if (plugin->abi_version >= UGREENCTL_PLUGIN_ABI_V5 &&
        plugin->read_wol_policy != NULL) {
        return plugin->read_wol_policy(request, policy, error, error_size);
    }
    (void)snprintf(error, error_size, "Wake-on-LAN is not available for %s", plugin->id);
    return -ENOTSUP;
}

static int report_plugin_error(const char *action, int result, const char *error)
{
    if (error != NULL && error[0] != '\0') {
        (void)fprintf(stderr, "error: %s: %s\n", action, error);
    } else if (result < 0) {
        (void)fprintf(stderr, "error: %s: %s\n", action, strerror(-result));
    } else {
        (void)fprintf(stderr, "error: %s failed\n", action);
    }
    return EXIT_FAILURE;
}

int main(int argc, char **argv)
{
    struct cli_options options = {
        .model_directory = ugreenctl_default_model_dir(),
        .model_id = NULL,
        .force = false,
        .apply = false
    };
    struct ugreenctl_plugin_set plugins;
    const struct ugreenctl_plugin *plugin = NULL;
    struct ugreenctl_request request;
    char error[256] = {0};
    int arg = 1;
    int result;

    result = ugreenctl_wake_cli(argc, argv);
    if (result >= 0) return result;

    result = ugreenctl_hardware_cli(argc, argv);
    if (result >= 0) {
        return result;
    }

    while (arg < argc && strncmp(argv[arg], "--", 2) == 0) {
        if (strcmp(argv[arg], "--help") == 0) {
            usage(stdout);
            return EXIT_SUCCESS;
        }
        if (strcmp(argv[arg], "--version") == 0) {
            (void)puts(UGREENCTL_VERSION);
            return EXIT_SUCCESS;
        }
        if (strcmp(argv[arg], "--force") == 0) {
            options.force = true;
            ++arg;
            continue;
        }
        if (strcmp(argv[arg], "--apply") == 0) {
            options.apply = true;
            ++arg;
            continue;
        }
        if ((strcmp(argv[arg], "--model") == 0 || strcmp(argv[arg], "--plugin-dir") == 0) &&
            arg + 1 < argc) {
            if (strcmp(argv[arg], "--model") == 0) {
                options.model_id = argv[arg + 1];
            } else {
                options.model_directory = argv[arg + 1];
            }
            arg += 2;
            continue;
        }
        (void)fprintf(stderr, "error: unknown or incomplete option '%s'\n", argv[arg]);
        usage(stderr);
        return EXIT_FAILURE;
    }
    if (arg >= argc) {
        usage(stderr);
        return EXIT_FAILURE;
    }

    result = ugreenctl_plugins_open(options.model_directory, &plugins, error, sizeof(error));
    if (result != 0) {
        return report_plugin_error("load model plugins", result, error);
    }
    if (strcmp(argv[arg], "models") == 0 && arg + 1 == argc) {
        size_t index;
        for (index = 0; index < plugins.count; ++index) {
            const struct ugreenctl_plugin *item = plugins.items[index].plugin;
            (void)printf("%-14s %-36s ", item->id, item->display_name);
            print_capabilities(item->capabilities);
            {
                const struct ugreenctl_led_profile *profiles;
                size_t count, p;
                profiles = ugreenctl_led_profiles(&count);
                for (p = 0; p < count; ++p) {
                    const char *const *name;
                    for (name = item->dmi_product_names; name != NULL && *name != NULL; ++name) {
                        if (strcmp(*name, profiles[p].product_name) == 0) {
                            (void)printf("; led-profile=%s (experimental%s)", profiles[p].id,
                                         profiles[p].product_version ? ", revision restricted" : "");
                            break;
                        }
                    }
                }
            }
            (void)putchar('\n');
        }
        ugreenctl_plugins_close(&plugins);
        return EXIT_SUCCESS;
    }

    result = choose_plugin(&plugins, &options, &plugin);
    if (result != 0) {
        ugreenctl_plugins_close(&plugins);
        return EXIT_FAILURE;
    }
    request.force = options.force;

    if (strcmp(argv[arg], "model") == 0 && arg + 1 == argc) {
        puts(plugin->id);
        result = EXIT_SUCCESS;
    } else if (strcmp(argv[arg], "thermal") == 0 && arg + 1 < argc &&
               strcmp(argv[arg + 1], "status") == 0 && arg + 2 == argc) {
        struct ugreenctl_thermal_snapshot snapshot;

        result = ugreenctl_read_thermal_snapshot(&snapshot, error, sizeof(error));
        if (result == 0) {
            print_thermal_snapshot(&snapshot);
            result = EXIT_SUCCESS;
        } else {
            result = report_plugin_error("read thermal snapshot", result, error);
        }
    } else if (strcmp(argv[arg], "info") == 0 && arg + 1 == argc) {
        struct ugreenctl_status status;
        char fan_error[256] = {0};
        char startup_error[256] = {0};
        char wol_error[256] = {0};
        int fan_result;
        int startup_result;
        int wol_result;

        memset(&status, 0, sizeof(status));
        (void)snprintf(status.controller, sizeof(status.controller), "%s",
                       plugin_controller_name(plugin));
        fan_result = plugin_read_fans(plugin, &request, status.fans,
                                      &status.fan_count, fan_error, sizeof(fan_error));
        startup_result = plugin_read_startup_policy(plugin, &request,
                                                    &status.startup_policy,
                                                    startup_error,
                                                    sizeof(startup_error));
        wol_result = plugin_read_wol_policy(plugin, &request, &status.wol_policy,
                                            wol_error, sizeof(wol_error));
        if (fan_result != 0 && startup_result != 0) {
            result = report_plugin_error("read fan status", fan_result, fan_error);
            (void)report_plugin_error("read startup policy", startup_result, startup_error);
        } else {
            (void)printf("model: %s (%s)\n", plugin->id, plugin->display_name);
            print_status(&status);
            if (fan_result != 0) {
                (void)fprintf(stderr, "warning: fan status unavailable: %s\n",
                              fan_error[0] != '\0' ? fan_error : strerror(-fan_result));
            }
            if (startup_result != 0) {
                (void)fprintf(stderr, "warning: startup policy unavailable: %s\n",
                              startup_error[0] != '\0' ? startup_error
                                                        : strerror(-startup_result));
            }
            if (wol_result != 0) {
                (void)fprintf(stderr, "warning: Wake-on-LAN unavailable: %s\n",
                              wol_error[0] != '\0' ? wol_error : strerror(-wol_result));
            }
            result = EXIT_SUCCESS;
        }
    } else if (strcmp(argv[arg], "fan") == 0 && arg + 1 < argc &&
               strcmp(argv[arg + 1], "status") == 0 && arg + 2 == argc) {
        struct ugreenctl_fan_status fans[UGREENCTL_MAX_FANS];
        size_t fan_count = 0;
        result = plugin_read_fans(plugin, &request, fans, &fan_count,
                                  error, sizeof(error));
        if (result == 0) {
            size_t index;
            for (index = 0; index < fan_count; ++index) {
                print_fan_status(&fans[index]);
            }
            result = EXIT_SUCCESS;
        } else {
            result = report_plugin_error("read fan status", result, error);
        }
    } else if (strcmp(argv[arg], "fan") == 0 && arg + 4 == argc &&
               strcmp(argv[arg + 1], "set") == 0) {
        uint8_t pwm;
        if (parse_pwm(argv[arg + 3], &pwm) != 0 || plugin->set_fan_pwm == NULL) {
            (void)fprintf(stderr, "error: fan control is not available for %s\n", plugin->id);
            result = EXIT_FAILURE;
        } else if (require_apply(&options, "set the fan PWM") != 0) {
            result = EXIT_SUCCESS;
        } else {
            result = plugin->set_fan_pwm(&request, argv[arg + 2], pwm, error, sizeof(error));
            if (result == 0) {
                (void)printf("%s fan PWM set to %u\n", argv[arg + 2], pwm);
                result = EXIT_SUCCESS;
            } else {
                result = report_plugin_error("set fan PWM", result, error);
            }
        }
    } else if (strcmp(argv[arg], "fan") == 0 && arg + 4 == argc &&
               strcmp(argv[arg + 1], "mode") == 0) {
        if (strcmp(argv[arg + 3], "auto") || plugin->set_fan_mode == NULL) {
            fprintf(stderr, "error: fan mode auto is unavailable for %s\n", plugin->id);
            result = EXIT_FAILURE;
        } else if (require_apply(&options, "restore automatic fan control") != 0) {
            result = EXIT_SUCCESS;
        } else {
            result = plugin->set_fan_mode(&request, argv[arg + 2], true, error, sizeof(error));
            if (result) result = report_plugin_error("restore automatic fan control", result, error);
            else puts("fan automatic mode restored");
        }
    } else if (strcmp(argv[arg], "power") == 0 && arg + 2 < argc &&
               strcmp(argv[arg + 1], "startup") == 0 &&
               strcmp(argv[arg + 2], "get") == 0 && arg + 3 == argc) {
        enum ugreenctl_startup_policy policy = UGREENCTL_STARTUP_UNKNOWN;
        result = plugin_read_startup_policy(plugin, &request, &policy,
                                            error, sizeof(error));
        if (result == 0) {
            (void)puts(startup_policy_name(policy));
            result = EXIT_SUCCESS;
        } else {
            result = report_plugin_error("read startup policy", result, error);
        }
    } else if (strcmp(argv[arg], "power") == 0 && arg + 3 < argc &&
               strcmp(argv[arg + 1], "startup") == 0 &&
               strcmp(argv[arg + 2], "set") == 0 && arg + 4 == argc) {
        enum ugreenctl_startup_policy policy = parse_startup_policy(argv[arg + 3]);
        if (policy == UGREENCTL_STARTUP_UNKNOWN || plugin->set_startup_policy == NULL) {
            (void)fprintf(stderr, "error: use startup policy on, off, or restore\n");
            result = EXIT_FAILURE;
        } else if (require_apply(&options, "change the AC recovery startup policy") != 0) {
            result = EXIT_SUCCESS;
        } else {
            result = plugin->set_startup_policy(&request, policy, error, sizeof(error));
            if (result == 0) {
                if (!strcmp(plugin->id, "dxp4800gt"))
                    (void)printf("startup %s command sent; persistent policy readback unavailable\n", startup_policy_name(policy));
                else
                    (void)printf("startup policy set to %s\n", startup_policy_name(policy));
                result = EXIT_SUCCESS;
            } else {
                result = report_plugin_error("set startup policy", result, error);
            }
        }
    } else if (strcmp(argv[arg], "led") == 0 && arg + 1 < argc &&
               strcmp(argv[arg + 1], "list") == 0 && arg + 2 == argc) {
        (void)fprintf(stderr, "error: LED control is not verified for %s\n", plugin->id);
        result = EXIT_FAILURE;
    } else {
        (void)fprintf(stderr, "error: invalid command\n");
        usage(stderr);
        result = EXIT_FAILURE;
    }

    ugreenctl_plugins_close(&plugins);
    return result;
}
