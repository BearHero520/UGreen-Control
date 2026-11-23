#include "loader.h"

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#ifndef UGREENCTL_DEFAULT_MODEL_DIR
#define UGREENCTL_DEFAULT_MODEL_DIR "/usr/lib/ugreenctl/models"
#endif

static void set_error(char *error, size_t error_size, const char *message)
{
    if (error != NULL && error_size > 0) {
        (void)snprintf(error, error_size, "%s", message);
    }
}

static bool has_shared_object_suffix(const char *name)
{
    size_t length = strlen(name);
    return length > 3 && strcmp(name + length - 3, ".so") == 0;
}

static bool product_matches(const struct ugreenctl_plugin *plugin, const char *product_name)
{
    const char * const *name;
    if (plugin->dmi_product_names == NULL || product_name == NULL) {
        return false;
    }
    for (name = plugin->dmi_product_names; *name != NULL; ++name) {
        if (strcmp(*name, product_name) == 0) {
            return true;
        }
    }
    return false;
}

int ugreenctl_plugins_open(const char *directory, struct ugreenctl_plugin_set *set,
                           char *error, size_t error_size)
{
    DIR *dir;
    struct dirent *entry;

    memset(set, 0, sizeof(*set));
    dir = opendir(directory);
    if (dir == NULL) {
        (void)snprintf(error, error_size, "cannot open model directory '%s': %s",
                       directory, strerror(errno));
        return -errno;
    }
    while ((entry = readdir(dir)) != NULL) {
        char path[4096];
        void *handle;
        ugreenctl_plugin_entrypoint entrypoint;
        const struct ugreenctl_plugin *plugin;
        unsigned int expected_abi;

        if (!has_shared_object_suffix(entry->d_name)) {
            continue;
        }
        if (set->count == UGREENCTL_MAX_PLUGINS) {
            set_error(error, error_size, "too many model plugins");
            (void)closedir(dir);
            ugreenctl_plugins_close(set);
            return -E2BIG;
        }
        if (snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name) >=
            (int)sizeof(path)) {
            set_error(error, error_size, "model plugin path is too long");
            (void)closedir(dir);
            ugreenctl_plugins_close(set);
            return -ENAMETOOLONG;
        }
        handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
        if (handle == NULL) {
            (void)fprintf(stderr, "warning: skipping %s: %s\n", path, dlerror());
            continue;
        }
        dlerror();
        entrypoint = (ugreenctl_plugin_entrypoint)dlsym(handle, "ugreenctl_plugin_v5");
        if (dlerror() == NULL && entrypoint != NULL) {
            expected_abi = UGREENCTL_PLUGIN_ABI_V5;
        } else {
            dlerror();
            entrypoint = (ugreenctl_plugin_entrypoint)dlsym(handle, "ugreenctl_plugin_v4");
            if (dlerror() == NULL && entrypoint != NULL) {
                expected_abi = UGREENCTL_PLUGIN_ABI_V4;
            } else {
                dlerror();
                entrypoint = (ugreenctl_plugin_entrypoint)dlsym(handle, "ugreenctl_plugin_v3");
                if (dlerror() == NULL && entrypoint != NULL) {
                    expected_abi = UGREENCTL_PLUGIN_ABI_V3;
                } else {
                    dlerror();
                    entrypoint = (ugreenctl_plugin_entrypoint)dlsym(handle, "ugreenctl_plugin_v2");
                    if (dlerror() != NULL || entrypoint == NULL) {
                        (void)fprintf(stderr,
                                      "warning: skipping %s: missing ugreenctl_plugin_v5/v4/v3/v2\n",
                                      path);
                        (void)dlclose(handle);
                        continue;
                    }
                    expected_abi = UGREENCTL_PLUGIN_ABI_V2;
                }
            }
        }
        plugin = entrypoint();
        if (plugin == NULL || plugin->abi_version != expected_abi ||
            plugin->id == NULL || plugin->display_name == NULL) {
            (void)fprintf(stderr, "warning: skipping %s: incompatible plugin\n", path);
            (void)dlclose(handle);
            continue;
        }
        set->items[set->count].handle = handle;
        set->items[set->count].plugin = plugin;
        ++set->count;
    }
    (void)closedir(dir);
    if (set->count == 0) {
        (void)snprintf(error, error_size, "no usable .so plugins found in '%s'", directory);
        return -ENOENT;
    }
    return 0;
}

void ugreenctl_plugins_close(struct ugreenctl_plugin_set *set)
{
    size_t index;
    for (index = 0; index < set->count; ++index) {
        if (set->items[index].handle != NULL) {
            (void)dlclose(set->items[index].handle);
        }
    }
    memset(set, 0, sizeof(*set));
}

const struct ugreenctl_plugin *ugreenctl_find_plugin(
    const struct ugreenctl_plugin_set *set, const char *id)
{
    size_t index;
    for (index = 0; index < set->count; ++index) {
        if (strcmp(set->items[index].plugin->id, id) == 0) {
            return set->items[index].plugin;
        }
    }
    return NULL;
}

const struct ugreenctl_plugin *ugreenctl_match_product(
    const struct ugreenctl_plugin_set *set, const char *product_name)
{
    size_t index;
    for (index = 0; index < set->count; ++index) {
        if (product_matches(set->items[index].plugin, product_name)) {
            return set->items[index].plugin;
        }
    }
    return NULL;
}

int ugreenctl_read_dmi_product(char *product_name, size_t product_name_size)
{
    FILE *file;
    size_t length;

    if (product_name_size == 0) {
        return -EINVAL;
    }
    file = fopen("/sys/class/dmi/id/product_name", "r");
    if (file == NULL) {
        return -errno;
    }
    if (fgets(product_name, (int)product_name_size, file) == NULL) {
        int error = ferror(file) ? -errno : -ENOENT;
        (void)fclose(file);
        return error;
    }
    (void)fclose(file);
    length = strcspn(product_name, "\r\n");
    product_name[length] = '\0';
    return product_name[0] == '\0' ? -ENOENT : 0;
}

const char *ugreenctl_default_model_dir(void)
{
    return UGREENCTL_DEFAULT_MODEL_DIR;
}
