#include "ec/ec.h"

#include <unistd.h>

bool ugreenctl_ec_transport_present(void)
{
    /*
     * An EC device alone does not describe its vendor command dialect. Model
     * plugins must supply an independently verified protocol before write
     * support is added here.
     */
    return access("/sys/kernel/debug/ec", F_OK) == 0;
}
