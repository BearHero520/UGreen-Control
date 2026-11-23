#ifndef UGREENCTL_LOADER_H
#define UGREENCTL_LOADER_H

#include <stddef.h>

#include "ugreenctl.h"

#define UGREENCTL_MAX_PLUGINS 32U

struct ugreenctl_loaded_plugin {
    void *handle;
    const struct ugreenctl_plugin *plugin;
};

struct ugreenctl_plugin_set {
    struct ugreenctl_loaded_plugin items[UGREENCTL_MAX_PLUGINS];
    size_t count;
};

int ugreenctl_plugins_open(const char *directory, struct ugreenctl_plugin_set *set,
                           char *error, size_t error_size);
void ugreenctl_plugins_close(struct ugreenctl_plugin_set *set);
const struct ugreenctl_plugin *ugreenctl_find_plugin(
    const struct ugreenctl_plugin_set *set, const char *id);
const struct ugreenctl_plugin *ugreenctl_match_product(
    const struct ugreenctl_plugin_set *set, const char *product_name);
int ugreenctl_read_dmi_product(char *product_name, size_t product_name_size);
const char *ugreenctl_default_model_dir(void);

#endif
