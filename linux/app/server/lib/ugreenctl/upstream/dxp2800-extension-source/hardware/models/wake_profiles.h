#ifndef UGREENCTL_WAKE_PROFILES_H
#define UGREENCTL_WAKE_PROFILES_H
#include <stddef.h>
#include "core/identity.h"

/* Independent capability tables: RTC eligibility never depends on WOL. */
struct ugreenctl_rtc_profile {
    const char *id, *product, *dt_model, *dt_compatible;
    const char *rtc_name, *rtc_of_path;
    unsigned int resolution_seconds;
};
struct ugreenctl_wol_profile {
    const char *id, *product, *dt_model, *dt_compatible;
    const char *ethernet_of_path, *ethernet_compatible;
    size_t automatic_pci_count;
};
const struct ugreenctl_rtc_profile *ugreenctl_rtc_profiles(size_t *count);
const struct ugreenctl_wol_profile *ugreenctl_wol_profiles(size_t *count);
const struct ugreenctl_rtc_profile *ugreenctl_rtc_match(const struct ugreenctl_machine_identity *identity);
const struct ugreenctl_wol_profile *ugreenctl_wol_match(const struct ugreenctl_machine_identity *identity);
#endif
