#include <stdio.h>
#include <stdlib.h>
#include "led/led_gt_bus.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
struct fixture { int values[16]; unsigned int calls; };
static int probe(void *opaque, unsigned int bus)
{
    struct fixture *f = opaque;
    CHECK(bus < 16);
    ++f->calls;
    return f->values[bus];
}
int main(void)
{
    unsigned int buses[] = {2,3,4}, reverse[] = {4,3,2}, selected = 99;
    struct fixture f = {0};
    CHECK(gt_bus_name("SMBus PIIX4 adapter port 0 at 0b00"));
    CHECK(gt_bus_name("SMBus PIIX4 adapter port 2 at 0b00"));
    CHECK(gt_bus_name("SMBus PIIX4 adapter port 1 at 0b20"));
    CHECK(gt_bus_name("Synopsys DesignWare I2C adapter"));
    CHECK(!gt_bus_name("AMDGPU DM i2c hw bus 0"));
    CHECK(!gt_bus_name("SMBus I801 adapter at efa0"));
    CHECK(!gt_bus_name("SMBus PIIX4 adapter-other"));
    for (unsigned int winner=2; winner<=4; ++winner) {
        f = (struct fixture){0}; f.values[winner]=1;
        CHECK(!gt_select_bus(buses,3,probe,&f,&selected));
        CHECK(selected==winner && f.calls==3);
        CHECK(!gt_select_bus(reverse,3,probe,&f,&selected));
        CHECK(selected==winner);
    }
    f = (struct fixture){0}; selected=99;
    CHECK(gt_select_bus(buses,3,probe,&f,&selected)==-ENODEV && selected==99);
    CHECK(gt_select_bus(buses,0,probe,&f,&selected)==-ENODEV);
    f.values[2]=f.values[4]=1;
    CHECK(gt_select_bus(buses,3,probe,&f,&selected)==-ENOTUNIQ && selected==99);
    const int fatal[] = {-EBUSY,-EACCES,-ETIMEDOUT,-EIO,-EOPNOTSUPP};
    for (size_t i=0;i<sizeof(fatal)/sizeof(fatal[0]);++i) {
        f=(struct fixture){0}; f.values[2]=1; f.values[4]=fatal[i];
        CHECK(gt_select_bus(buses,3,probe,&f,&selected)==fatal[i]);
        CHECK(selected==99);
    }
    puts("DXP4800 GT multi-bus selection passed");
    return 0;
}
