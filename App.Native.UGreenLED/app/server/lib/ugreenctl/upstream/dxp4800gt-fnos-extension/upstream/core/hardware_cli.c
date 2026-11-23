#define _POSIX_C_SOURCE 200809L
#include "core/hardware_cli.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/identity.h"
#include "disk/disk.h"
#include "led/led.h"
#include "led/led_n76_protocol.h"

static int syntax(char *error, size_t size, const char *message)
{
    (void)snprintf(error, size, "%s", message);
    return -2;
}

static int number(const char *text, unsigned long maximum, unsigned int *out)
{
    const unsigned char *p = (const unsigned char *)text;
    unsigned long value = 0;
    if (*p == 0) return -1;
    for (; *p != 0; ++p) {
        if (*p < '0' || *p > '9') return -1;
        value = value * 10 + (*p - '0');
        if (value > maximum) return -1;
    }
    *out = (unsigned int)value;
    return 0;
}

static int target(struct ugreenctl_hardware_options *o, const char *value,
                  char *error, size_t size)
{
    unsigned int ignored;
    const char *suffix = NULL;
    size_t i;
    if (strcmp(value, "power") != 0 && strcmp(value, "netdev") != 0 &&
        strcmp(value, "all") != 0) {
        if (strncmp(value, "disk", 4) == 0) suffix = value + 4;
        if (strncmp(value, "netdev", 6) == 0) suffix = value + 6;
        if (suffix == NULL || number(suffix, 16, &ignored) != 0 || ignored == 0)
            return syntax(error, size, "invalid LED target; use power, netdev, diskN, or all");
    }
    for (i = 0; i < o->target_count; ++i) {
        if (strcmp(o->targets[i], value) == 0) return 0;
        if (strcmp(o->targets[i], "all") == 0 || strcmp(value, "all") == 0)
            return syntax(error, size, "all cannot be combined with individual LED targets");
    }
    if (o->target_count == UGREENCTL_CLI_MAX_TARGETS)
        return syntax(error, size, "too many LED targets");
    o->targets[o->target_count++] = value;
    return 0;
}

static int add_action(struct ugreenctl_hardware_options *o,
                      struct ugreenctl_led_cli_action action,
                      char *error, size_t size)
{
    if (o->action_count == UGREENCTL_CLI_MAX_ACTIONS)
        return syntax(error, size, "too many LED actions");
    o->actions[o->action_count++] = action;
    return 0;
}

static int color(const char *text, struct ugreenctl_led_cli_action *a)
{
    static const struct { const char *name; uint8_t r, g, b; } colors[] = {
        {"red",255,0,0}, {"green",0,255,0}, {"blue",0,0,255},
        {"white",255,255,255}, {"black",0,0,0}, {"yellow",255,255,0},
        {"cyan",0,255,255}, {"magenta",255,0,255}, {"purple",128,0,128},
        {"orange",255,128,0}
    };
    size_t i;
    for (i = 0; i < sizeof(colors) / sizeof(colors[0]); ++i) {
        if (strcmp(text, colors[i].name) == 0) {
            a->red=colors[i].r; a->green=colors[i].g; a->blue=colors[i].b;
            a->named_color=true;
            if (!strcmp(text,"white")) a->color=UGREENCTL_LED_COLOR_WHITE;
            else if (!strcmp(text,"orange")) a->color=UGREENCTL_LED_COLOR_ORANGE;
            else if (!strcmp(text,"red")) a->color=UGREENCTL_LED_COLOR_RED;
            else if (!strcmp(text,"green")) a->color=UGREENCTL_LED_COLOR_GREEN;
            else if (!strcmp(text,"blue")) a->color=UGREENCTL_LED_COLOR_BLUE;
            else if (!strcmp(text,"black")) a->color=UGREENCTL_LED_COLOR_NONE;
            else a->named_color=false;
            return 0;
        }
    }
    if (!strcmp(text,"white-orange") || !strcmp(text,"none") ||
        !strcmp(text,"channel1") || !strcmp(text,"channel2") || !strcmp(text,"both")) {
        a->named_color=true;
        if (!strcmp(text,"none")) a->color=UGREENCTL_LED_COLOR_NONE;
        else if (!strcmp(text,"channel1")) a->color=UGREENCTL_LED_COLOR_CHANNEL1;
        else if (!strcmp(text,"channel2")) a->color=UGREENCTL_LED_COLOR_CHANNEL2;
        else if (!strcmp(text,"both")) a->color=UGREENCTL_LED_COLOR_BOTH;
        else a->color=UGREENCTL_LED_COLOR_WHITE_ORANGE;
        return 0;
    }
    if (*text == '#' && strlen(text) == 7) {
        unsigned int values[3] = {0,0,0}, j;
        for (j = 0; j < 6; ++j) {
            unsigned char c = (unsigned char)text[j + 1];
            unsigned int digit;
            if (c >= '0' && c <= '9') digit = c - '0';
            else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
            else return -1;
            values[j / 2] = values[j / 2] * 16 + digit;
        }
        a->red=(uint8_t)values[0]; a->green=(uint8_t)values[1]; a->blue=(uint8_t)values[2];
        return 0;
    }
    {
        char buffer[32], *first, *second;
        unsigned int r, g, b;
        if (strlen(text) >= sizeof(buffer)) return -1;
        (void)snprintf(buffer, sizeof(buffer), "%s", text);
        first = strchr(buffer, ',');
        if (first == NULL) return -1;
        *first++ = 0;
        second = strchr(first, ',');
        if (second == NULL) return -1;
        *second++ = 0;
        if (number(buffer,255,&r) || number(first,255,&g) || number(second,255,&b)) return -1;
        a->red=(uint8_t)r; a->green=(uint8_t)g; a->blue=(uint8_t)b;
    }
    return 0;
}

static int brightness(const char *text, uint8_t *out)
{
    unsigned int value;
    size_t length = strlen(text);
    if (length > 0 && text[length - 1] == '%') {
        char buffer[8];
        if (length >= sizeof(buffer)) return -1;
        (void)memcpy(buffer, text, length - 1); buffer[length - 1] = 0;
        if (number(buffer,100,&value)) return -1;
        *out=(uint8_t)value;
    } else {
        if (number(text,255,&value)) return -1;
        *out=(uint8_t)value;
    }
    return 0;
}

static int compat_parse(int count, const char **args,
                        struct ugreenctl_hardware_options *o,
                        char *error, size_t size)
{
    int i;
    for (i = 0; i < count; ++i) {
        struct ugreenctl_led_cli_action a = {0};
        const char *arg = args[i];
        unsigned int r,g,b,on,off;
        if (arg[0] != '-') {
            if (o->action_count != 0)
                return syntax(error,size,"put all LED targets before legacy action flags");
            if (target(o,arg,error,size)) return -2;
            continue;
        }
        if (strcmp(arg,"-on")==0) a.kind=UGREENCTL_CLI_ON;
        else if (strcmp(arg,"-off")==0) a.kind=UGREENCTL_CLI_OFF;
        else if (strcmp(arg,"-status")==0) a.kind=UGREENCTL_CLI_STATUS;
        else if (strcmp(arg,"-color")==0) {
            if (i+3>=count || number(args[i+1],255,&r) ||
                number(args[i+2],255,&g) || number(args[i+3],255,&b))
                return syntax(error,size,"-color requires three integers from 0 to 255");
            a.kind=UGREENCTL_CLI_COLOR; a.red=(uint8_t)r; a.green=(uint8_t)g; a.blue=(uint8_t)b;
            i+=3;
        } else if (strcmp(arg,"-brightness")==0) {
            if (++i>=count || number(args[i],255,&r))
                return syntax(error,size,"-brightness requires an integer from 0 to 255");
            a.kind=UGREENCTL_CLI_BRIGHTNESS; a.brightness=(uint8_t)r;
        } else if (strcmp(arg,"-blink")==0 || strcmp(arg,"-breath")==0) {
            if (i+2>=count || number(args[i+1],65535,&on) ||
                number(args[i+2],65535,&off) || on+off>65535 || on+off==0)
                return syntax(error,size,"blink/breath needs ON_MS OFF_MS with a total from 1 to 65535 ms");
            a.kind=strcmp(arg,"-blink")==0 ? UGREENCTL_CLI_BLINK : UGREENCTL_CLI_BREATH;
            a.on_ms=(uint16_t)on; a.off_ms=(uint16_t)off; i+=2;
        } else return syntax(error,size,"unknown legacy flag; see ugreen_leds_cli --help");
        if (add_action(o,a,error,size)) return -2;
    }
    if (o->target_count==0) return syntax(error,size,"specify at least one LED target");
    if (o->action_count==0) {
        struct ugreenctl_led_cli_action a = {0}; a.kind=UGREENCTL_CLI_STATUS;
        return add_action(o,a,error,size);
    }
    return 0;
}

static int new_parse(int count, const char **args,
                     struct ugreenctl_hardware_options *o,
                     char *error, size_t size)
{
    const char *verb;
    bool disk = strcmp(args[0],"disk")==0;
    bool mode_seen=false, timing_seen=false;
    struct ugreenctl_led_cli_action mode = {0};
    unsigned int on=500, off=500;
    int i;
    if (count==1) return syntax(error,size,"missing action; use led --help or disk --help");
    verb=args[1];
    if (disk) {
        if (strcmp(verb,"list")==0 || strcmp(verb,"map")==0) o->command=UGREENCTL_HW_DISK_LIST;
        else if (strcmp(verb,"locate")==0) o->command=UGREENCTL_HW_DISK_LOCATE;
        else return syntax(error,size,"unknown disk action; use list, map, or locate");
    } else {
        if (strcmp(verb,"list")==0) o->command=UGREENCTL_HW_LED_LIST;
        else if (strcmp(verb,"status")==0) o->command=UGREENCTL_HW_LED_STATUS;
        else if (strcmp(verb,"set")==0 || strcmp(verb,"on")==0 || strcmp(verb,"off")==0)
            o->command=UGREENCTL_HW_LED_SET;
        else if (strcmp(verb,"identify")==0) o->command=UGREENCTL_HW_LED_IDENTIFY;
        else return syntax(error,size,"unknown LED action; use list, status, set, on, off, or identify");
        if (strcmp(verb,"on")==0 || strcmp(verb,"off")==0) {
            mode_seen=true; mode.kind=strcmp(verb,"on")==0 ? UGREENCTL_CLI_ON : UGREENCTL_CLI_OFF;
        }
    }
    for (i=2;i<count;++i) {
        const char *arg=args[i];
        struct ugreenctl_led_cli_action a = {0};
        unsigned int value;
        if (arg[0]!='-') {
            if (o->command==UGREENCTL_HW_DISK_LOCATE) {
                if (o->target_count!=0) return syntax(error,size,"disk locate takes exactly one disk selector");
                o->targets[o->target_count++]=arg;
            } else if (o->command==UGREENCTL_HW_LED_LIST || o->command==UGREENCTL_HW_DISK_LIST)
                return syntax(error,size,"list/map does not take targets");
            else if (target(o,arg,error,size)) return -2;
            continue;
        }
        if (strcmp(arg,"--seconds")==0 &&
            (o->command==UGREENCTL_HW_LED_IDENTIFY || o->command==UGREENCTL_HW_DISK_LOCATE)) {
            if (++i>=count || number(args[i],60,&value) || value==0)
                return syntax(error,size,"--seconds requires an integer from 1 to 60");
            o->seconds=value;
            continue;
        }
        if (o->command!=UGREENCTL_HW_LED_SET || strcmp(verb,"set")!=0)
            return syntax(error,size,"unexpected option for this action");
        if (strcmp(arg,"--color")==0) {
            if (++i>=count || color(args[i],&a))
                return syntax(error,size,"--color requires a named color, #RRGGBB, or R,G,B");
            a.kind=UGREENCTL_CLI_COLOR;
            if (add_action(o,a,error,size)) return -2;
        } else if (strcmp(arg,"--brightness")==0) {
            if (++i>=count || brightness(args[i],&a.brightness))
                return syntax(error,size,"--brightness requires 0..255 or 0%..100%");
            a.kind=UGREENCTL_CLI_BRIGHTNESS;
            a.brightness_percent=args[i][strlen(args[i])-1]=='%';
            if (add_action(o,a,error,size)) return -2;
        } else if (strcmp(arg,"--mode")==0) {
            if (mode_seen) return syntax(error,size,"specify --mode only once");
            if (++i>=count) return syntax(error,size,"--mode needs on, off, blink, or breath");
            if (strcmp(args[i],"on")==0) mode.kind=UGREENCTL_CLI_ON;
            else if (strcmp(args[i],"off")==0) mode.kind=UGREENCTL_CLI_OFF;
            else if (strcmp(args[i],"blink")==0) mode.kind=UGREENCTL_CLI_BLINK;
            else if (strcmp(args[i],"breath")==0) mode.kind=UGREENCTL_CLI_BREATH;
            else return syntax(error,size,"--mode needs on, off, blink, or breath");
            mode_seen=true;
        } else if (strcmp(arg,"--on-ms")==0 || strcmp(arg,"--off-ms")==0) {
            if (++i>=count || number(args[i],65535,&value))
                return syntax(error,size,"timing must be an integer from 0 to 65535 ms");
            if (strcmp(arg,"--on-ms")==0) on=value; else off=value;
            timing_seen=true;
        } else return syntax(error,size,"unknown LED set option");
    }
    if (o->command==UGREENCTL_HW_LED_IDENTIFY || o->command==UGREENCTL_HW_DISK_LOCATE) {
        if (o->target_count!=1 || strcmp(o->targets[0],"all")==0)
            return syntax(error,size,"identify/locate requires exactly one target");
    }
    if (o->command==UGREENCTL_HW_LED_STATUS && o->target_count==0) o->targets[o->target_count++]="all";
    if (o->command==UGREENCTL_HW_LED_SET) {
        if (o->target_count==0) return syntax(error,size,"specify at least one LED target");
        if (timing_seen && (!mode_seen || (mode.kind!=UGREENCTL_CLI_BLINK && mode.kind!=UGREENCTL_CLI_BREATH)))
            return syntax(error,size,"--on-ms/--off-ms require --mode blink or breath");
        if (mode_seen) {
            if (on+off>65535 || on+off==0) return syntax(error,size,"blink/breath period must be from 1 to 65535 ms");
            mode.on_ms=(uint16_t)on; mode.off_ms=(uint16_t)off;
            if (add_action(o,mode,error,size)) return -2;
        }
        if (o->action_count==0) return syntax(error,size,"led set needs --color, --brightness, or --mode");
    }
    return 0;
}

/* Global hardware flags and their values have already been removed. Only the
 * first bare command token determines ownership; a later argument named led
 * or disk must not steal a fan/unknown command from main.c. --model is not a
 * supported hardware override, but its value still belongs to that option.
 * Preserve the older global --version short-circuit. */
static int hardware_command_position(int count, const char **args)
{
    int i;
    for (i=0;i<count;++i) {
        if (strcmp(args[i],"--version")==0) return -1;
        if (strcmp(args[i],"--model")==0) {
            if (++i>=count) return -1;
            continue;
        }
        if (args[i][0]=='-') continue;
        return strcmp(args[i],"led")==0 || strcmp(args[i],"disk")==0 ? i : -1;
    }
    return -1;
}

int ugreenctl_hardware_parse(int argc, char **argv, bool compatibility,
                            struct ugreenctl_hardware_options *o,
                            char *error, size_t size)
{
    const char **args;
    int count=0, i, result;
    bool help=false;
    (void)memset(o,0,sizeof(*o)); o->compatibility=compatibility; o->seconds=5;
    if (size>0) error[0]=0;
    args=calloc((size_t)(argc>0 ? argc : 1),sizeof(*args));
    if (args==NULL) return syntax(error,size,"out of memory");
    for (i=1;i<argc;++i) {
        if (strcmp(argv[i],"--json")==0) o->json=true;
        else if (strcmp(argv[i],"--tsv")==0) o->tsv=true;
        else if (strcmp(argv[i],"--apply")==0) o->apply=true;
        else if (strcmp(argv[i],"--force")==0) o->force=true;
        else if (strcmp(argv[i],"--n76-direct")==0) {
            char bus[8]; const char *colon; size_t length;
            if (o->n76_direct || ++i>=argc || !(colon=strchr(argv[i], ':')) ||
                (length=(size_t)(colon-argv[i]))==0 || length>=sizeof(bus)) {
                free(args); return syntax(error,size,"--n76-direct requires one explicit BUS:0x31 or BUS:0x26");
            }
            memcpy(bus,argv[i],length); bus[length]=0;
            if (number(bus,65535,&o->n76_bus) || (strcmp(colon+1,"0x31") && strcmp(colon+1,"0x26"))) {
                free(args); return syntax(error,size,"N76 bus must be numeric and address exactly 0x31 or 0x26");
            }
            o->n76_address=!strcmp(colon+1,"0x31") ? 0x31 : 0x26;
            o->n76_direct=true;
        }
        else if (strcmp(argv[i],"--plugin-dir")==0) {
            if (++i>=argc) { free(args); return syntax(error,size,"--plugin-dir requires a directory"); }
            /* New hardware commands use built-in exact profiles; retain this
             * option so the repository's existing wrapper remains usable. */
        } else if (strcmp(argv[i],"--map-file")==0) {
            if (++i>=argc) { free(args); return syntax(error,size,"--map-file requires a calibration file"); }
            o->map_file=argv[i];
        }
        else if (strcmp(argv[i],"--help")==0 || strcmp(argv[i],"-h")==0) help=true;
        else args[count++]=argv[i];
    }
    if (!compatibility) {
        int command=hardware_command_position(count,args);
        if (command<0) { free(args); return -1; }
        if (command!=0) {
            free(args);
            return syntax(error,size,"unexpected option before LED/disk command");
        }
    }
    if (help || (compatibility && count==0)) { o->command=UGREENCTL_HW_HELP; free(args); return 0; }
    if (compatibility) { o->command=UGREENCTL_HW_LED_COMPAT; result=compat_parse(count,args,o,error,size); }
    else result=new_parse(count,args,o,error,size);
    if (result==0 && o->map_file!=NULL &&
        o->command!=UGREENCTL_HW_DISK_LIST && o->command!=UGREENCTL_HW_DISK_LOCATE)
        result=syntax(error,size,"--map-file is only available for disk commands");
    if (result==0 && o->tsv && (o->command!=UGREENCTL_HW_DISK_LIST || o->json || o->apply))
        result=syntax(error,size,"--tsv is only available for disk list/map without --json or --apply");
    if (result==0 && o->n76_direct &&
        o->command!=UGREENCTL_HW_LED_LIST && o->command!=UGREENCTL_HW_LED_STATUS &&
        o->command!=UGREENCTL_HW_LED_SET && o->command!=UGREENCTL_HW_LED_COMPAT)
        result=syntax(error,size,"N76 direct supports LED list/status/set only; unknown state cannot be restored by identify/locate");
    free(args);
    return result;
}

static const char help_text[] =
    "Usage:\n"
    "  ugreenctl led list\n"
    "  ugreenctl led status [TARGET ...]\n"
    "  ugreenctl led set TARGET ... [--color COLOR] [--brightness VALUE] [--mode MODE]\n"
    "  ugreenctl led on|off TARGET ...\n"
    "  ugreenctl led identify TARGET [--seconds 1..60]\n"
    "  ugreenctl disk list|map\n"
    "  ugreenctl disk locate /dev/sdX|serial:SERIAL|slotN [--seconds 1..60]\n"
    "\n"
    "TARGET: power, netdev, netdev2, diskN, or all; use led list for capabilities.\n"
    "COLOR: blue, red, green, white, yellow, cyan, magenta, purple, orange,\n"
    "       black, '#RRGGBB', or R,G,B for RGB; palette LEDs also accept\n"
    "       white-orange, none, channel1/channel2/both when listed.\n"
    "       Quote hex colors in a shell.\n"
    "VALUE: raw hardware range or 0%..100% of that range. MODE: on/off/blink/breath\n"
    "       where supported by the target.\n"
    "Blink/breath: --on-ms N --off-ms N (default 500/500; total 1..65535 ms).\n"
    "Target-specific minimum durations are shown by led list --json.\n"
    "\n"
    "--json, --apply, --force can appear before or after the command.\n"
    "Writes are previews until --apply; experimental hardware also needs --force.\n"
    "N76: --n76-direct BUS:0x31 (or BUS:0x26) selects explicit userspace I2C.\n"
    "     Exact DH2600/DXP480T Plus only; no scan, module build or loading.\n"
    "     Set requires explicit mode/color; output/PWM/timing remain UNVERIFIED.\n"
    "     Default without this option retains the bound-driver sysfs path.\n"
    "--force never bypasses DMI, MCU identity, driver ownership, or lock checks.\n"
    "Identify restores the complete original LED state after its time limit\n"
    "or SIGINT/SIGTERM. Restoration is best effort if communication fails.\n"
    "Disk mapping uses physical PCI/ATA topology, never sdX enumeration order.\n"
    "disk list/map --tsv emits device, interface, slot_name and led; '-' means absent.\n"
    "Unverified mappings remain unknown; locate refuses them. Calibrated physical\n"
    "maps are read from /etc/ugreenctl/disk-map.conf (or --map-file PATH).\n"
    "Calibration files must be root-owned and not group/world-writable:\n"
    "  model=DXP4800 Plus\n"
    "  slot1 topology=pci-0000:00:17.0-ata-1\n"
    "Record only physical slot/topology pairs confirmed on your machine.\n";

static const char compat_help_text[] =
    "Usage: ugreen_leds_cli TARGET ... [ACTION ...] [--apply] [--force] [--json]\n"
    "TARGET: power, netdev, disk1..disk8, all (availability depends on model).\n"
    "Actions, executed in the given order:\n"
    "  -on | -off\n"
    "  -color R G B          each component 0..255\n"
    "  -brightness N         0..255\n"
    "  -blink ON_MS OFF_MS | -breath ON_MS OFF_MS (total 1..65535 ms)\n"
    "  -status               query complete LED state\n"
    "No action means -status. Put all targets before action flags.\n"
    "\n"
    "This entry point provides argument compatibility with ugreen_leds_cli.\n"
    "Writes default to preview and require --apply; experimental profiles also\n"
    "require --force. Existing scripts must explicitly acknowledge these options.\n"
    "DMI, MCU identity, owner checks and locks always apply.\n";

static void json_string(const char *value)
{
    const unsigned char *p=(const unsigned char *)value;
    (void)putchar('"');
    for (; *p!=0; ++p) {
        if (*p=='"' || *p=='\\') { (void)putchar('\\'); (void)putchar(*p); }
        else if (*p<0x20) (void)printf("\\u%04x",(unsigned int)*p);
        else (void)putchar(*p);
    }
    (void)putchar('"');
}

static int failure(const struct ugreenctl_hardware_options *o,
                   const char *code, const char *message, int exit_code,
                   size_t completed)
{
    if (o->json) {
        (void)fputs("{\"schema_version\":1,\"ok\":false,\"error\":{\"code\":",stdout);
        json_string(code);
        (void)fputs(",\"message\":",stdout); json_string(message);
        (void)printf(",\"completed_operations\":%zu}}\n",completed);
    }
    (void)fprintf(stderr,"error [%s]: %s",code,message);
    if (completed!=0) (void)fprintf(stderr," (%zu operations completed before failure)",completed);
    (void)fputc('\n',stderr);
    return exit_code;
}

static int backend_failure(const struct ugreenctl_hardware_options *o,
                           int result, const char *message, size_t completed)
{
    if (result==-ENOTSUP || result==-ENODEV)
        return failure(o,"unsupported",message,3,completed);
    if (result==-EBUSY)
        return failure(o,"controller_busy",message,4,completed);
    if (result==-EPERM || result==-EACCES)
        return failure(o,"access_denied",message,4,completed);
    if (result==-EINTR)
        return failure(o,"interrupted",message,130,completed);
    return failure(o,"hardware_error",message,5,completed);
}

static void field_names(unsigned int fields)
{
    static const struct { unsigned int bit; const char *name; } names[]={
        {UGREENCTL_LED_PATCH_MODE,"mode"},{UGREENCTL_LED_PATCH_RGB,"rgb"},
        {UGREENCTL_LED_PATCH_BRIGHTNESS,"brightness"},{UGREENCTL_LED_PATCH_TIMING,"timing"},
        {UGREENCTL_LED_PATCH_COLOR,"color"}
    };
    size_t i; bool first=true;
    (void)putchar('[');
    for (i=0;i<sizeof(names)/sizeof(names[0]);++i) if (fields & names[i].bit) {
        if (!first) (void)putchar(',');
        json_string(names[i].name); first=false;
    }
    (void)putchar(']');
}

static void print_value(const struct ugreenctl_led_value *value,
                        const struct ugreenctl_led_descriptor *led, bool json)
{
    if (json) {
        (void)fputs("{\"mode\":",stdout);
        if (value->unknown_fields & UGREENCTL_LED_PATCH_MODE) (void)fputs("null",stdout);
        else json_string(ugreenctl_led_mode_name(value->mode));
        (void)fputs(",\"brightness\":",stdout);
        if (value->unknown_fields & UGREENCTL_LED_PATCH_BRIGHTNESS) (void)fputs("null",stdout);
        else (void)printf("%u",value->brightness);
        (void)fputs(",\"rgb\":",stdout);
        if (!led->rgb || (value->unknown_fields & UGREENCTL_LED_PATCH_RGB)) (void)fputs("null",stdout);
        else (void)printf("[%u,%u,%u]",value->red,value->green,value->blue);
        (void)fputs(",\"color\":",stdout);
        if (led->rgb || (value->unknown_fields & UGREENCTL_LED_PATCH_COLOR)) (void)fputs("null",stdout);
        else json_string(ugreenctl_led_color_name(value->color));
        (void)fputs(",\"on_ms\":",stdout);
        if (value->unknown_fields & UGREENCTL_LED_PATCH_TIMING) (void)fputs("null",stdout);
        else (void)printf("%u",value->on_ms);
        (void)fputs(",\"off_ms\":",stdout);
        if (value->unknown_fields & UGREENCTL_LED_PATCH_TIMING) (void)fputs("null",stdout);
        else (void)printf("%u",value->off_ms);
        (void)fputs(",\"unknown_fields\":",stdout); field_names(value->unknown_fields);
        (void)fputs(",\"cached_fields\":",stdout); field_names(value->cached_fields);
        if (value->n76_mode_valid) {
            (void)printf(",\"n76_mode_registers\":[%u,%u],\"n76_mcu_version\":%u,\"hardware_state_verified\":false",
                value->n76_modes[0],value->n76_modes[1],value->n76_version);
            if (value->unverified_write) (void)fputs(",\"write_result\":\"sent-unverified\",\"verification\":\"mode-registers-only\"",stdout);
        }
        (void)putchar('}');
    } else {
        (void)printf("mode=%s brightness=",value->unknown_fields & UGREENCTL_LED_PATCH_MODE ? "unknown" : ugreenctl_led_mode_name(value->mode));
        if (value->unknown_fields & UGREENCTL_LED_PATCH_BRIGHTNESS) (void)fputs("unknown",stdout);
        else (void)printf("%u",value->brightness);
        if (led->rgb) {
            if (value->unknown_fields & UGREENCTL_LED_PATCH_RGB) (void)fputs(" rgb=unknown",stdout);
            else (void)printf(" rgb=%u,%u,%u",value->red,value->green,value->blue);
        } else (void)printf(" color=%s",value->unknown_fields & UGREENCTL_LED_PATCH_COLOR ? "unknown" : ugreenctl_led_color_name(value->color));
        if (value->unknown_fields & UGREENCTL_LED_PATCH_TIMING) (void)fputs(" timing=unknown",stdout);
        else (void)printf(" on_ms=%u off_ms=%u",value->on_ms,value->off_ms);
        if (value->cached_fields) { (void)fputs(" cached_fields=",stdout); field_names(value->cached_fields); }
        if (value->n76_mode_valid) (void)printf(" n76_modes=%u,%u mcu_version=%u hardware_state=UNVERIFIED%s",
            value->n76_modes[0],value->n76_modes[1],value->n76_version,
            value->unverified_write ? " write_result=sent-unverified (mode registers checked only)" : "");
    }
}

static const char *action_name(enum ugreenctl_led_cli_action_kind kind)
{
    switch (kind) {
    case UGREENCTL_CLI_ON: return "on";
    case UGREENCTL_CLI_OFF: return "off";
    case UGREENCTL_CLI_COLOR: return "color";
    case UGREENCTL_CLI_BRIGHTNESS: return "brightness";
    case UGREENCTL_CLI_BLINK: return "blink";
    case UGREENCTL_CLI_BREATH: return "breath";
    case UGREENCTL_CLI_STATUS: return "status";
    }
    return "unknown";
}

static void patch_for_action(const struct ugreenctl_led_cli_action *action,
                             const struct ugreenctl_led_descriptor *led,
                             struct ugreenctl_led_patch *patch)
{
    (void)memset(patch,0,sizeof(*patch));
    switch (action->kind) {
    case UGREENCTL_CLI_ON:
    case UGREENCTL_CLI_OFF:
        patch->fields=UGREENCTL_LED_PATCH_MODE;
        patch->value.mode=action->kind==UGREENCTL_CLI_ON ? UGREENCTL_LED_MODE_ON : UGREENCTL_LED_MODE_OFF;
        break;
    case UGREENCTL_CLI_COLOR:
        if (action->named_color && (!led->rgb || action->color==UGREENCTL_LED_COLOR_WHITE_ORANGE ||
                                   action->color>=UGREENCTL_LED_COLOR_CHANNEL1)) {
            patch->fields=UGREENCTL_LED_PATCH_COLOR; patch->value.color=action->color;
        } else {
            patch->fields=UGREENCTL_LED_PATCH_RGB;
            patch->value.red=action->red; patch->value.green=action->green; patch->value.blue=action->blue;
        }
        break;
    case UGREENCTL_CLI_BRIGHTNESS:
        patch->fields=UGREENCTL_LED_PATCH_BRIGHTNESS; patch->value.brightness=action->brightness;
        if (action->brightness_percent)
            patch->value.brightness=(action->brightness*led->max_brightness+50U)/100U;
        break;
    case UGREENCTL_CLI_BLINK:
    case UGREENCTL_CLI_BREATH:
        patch->fields=UGREENCTL_LED_PATCH_MODE|UGREENCTL_LED_PATCH_TIMING;
        patch->value.mode=action->kind==UGREENCTL_CLI_BLINK ? UGREENCTL_LED_MODE_BLINK : UGREENCTL_LED_MODE_BREATH;
        patch->value.on_ms=action->on_ms; patch->value.off_ms=action->off_ms;
        break;
    case UGREENCTL_CLI_STATUS: break;
    }
}

static void print_action_parameters(const struct ugreenctl_led_cli_action *a,
                                    const struct ugreenctl_led_descriptor *led, bool json)
{
    struct ugreenctl_led_patch patch;
    if (a->kind==UGREENCTL_CLI_STATUS) return;
    patch_for_action(a,led,&patch);
    if (a->kind==UGREENCTL_CLI_COLOR) {
        if (patch.fields & UGREENCTL_LED_PATCH_COLOR) {
            (void)fputs(json ? ",\"color\":" : " color=",stdout);
            if (json) json_string(ugreenctl_led_color_name(patch.value.color));
            else (void)fputs(ugreenctl_led_color_name(patch.value.color),stdout);
        } else if (json) (void)printf(",\"rgb\":[%u,%u,%u]",a->red,a->green,a->blue);
        else (void)printf(" rgb=%u,%u,%u",a->red,a->green,a->blue);
    } else if (a->kind==UGREENCTL_CLI_BRIGHTNESS) {
        if (json) (void)printf(",\"brightness\":%u",patch.value.brightness);
        else (void)printf(" brightness=%u",patch.value.brightness);
    } else if (a->kind==UGREENCTL_CLI_BLINK || a->kind==UGREENCTL_CLI_BREATH) {
        if (json) (void)printf(",\"on_ms\":%u,\"off_ms\":%u",a->on_ms,a->off_ms);
        else (void)printf(" on_ms=%u off_ms=%u",a->on_ms,a->off_ms);
    }
}

/* New `led set` options describe one intent. Keep legacy ordered actions
 * separate, but combine new options so e.g. a GPIO color and ON reach the
 * hardware together rather than attempting an impossible dormant color. */
static void merged_patch(const struct ugreenctl_hardware_options *request,
                          const struct ugreenctl_led_descriptor *led,
                          struct ugreenctl_led_patch *patch)
{
    size_t i;
    memset(patch,0,sizeof(*patch));
    for (i=0;i<request->action_count;++i) {
        struct ugreenctl_led_patch next;
        patch_for_action(&request->actions[i],led,&next);
        if (next.fields&UGREENCTL_LED_PATCH_RGB) patch->fields&=~UGREENCTL_LED_PATCH_COLOR;
        if (next.fields&UGREENCTL_LED_PATCH_COLOR) patch->fields&=~UGREENCTL_LED_PATCH_RGB;
        patch->fields|=next.fields;
        if (next.fields&UGREENCTL_LED_PATCH_RGB) {
            patch->value.red=next.value.red; patch->value.green=next.value.green; patch->value.blue=next.value.blue;
        }
        if (next.fields&UGREENCTL_LED_PATCH_COLOR) patch->value.color=next.value.color;
        if (next.fields&UGREENCTL_LED_PATCH_BRIGHTNESS) patch->value.brightness=next.value.brightness;
        if (next.fields&UGREENCTL_LED_PATCH_MODE) patch->value.mode=next.value.mode;
        if (next.fields&UGREENCTL_LED_PATCH_TIMING) {
            patch->value.on_ms=next.value.on_ms; patch->value.off_ms=next.value.off_ms;
        }
    }
}

static void print_patch_parameters(const struct ugreenctl_led_patch *patch,bool json)
{
    if (patch->fields&UGREENCTL_LED_PATCH_RGB)
        (void)printf(json ? ",\"rgb\":[%u,%u,%u]" : " rgb=%u,%u,%u",patch->value.red,patch->value.green,patch->value.blue);
    if (patch->fields&UGREENCTL_LED_PATCH_COLOR) {
        (void)fputs(json ? ",\"color\":" : " color=",stdout);
        if (json) json_string(ugreenctl_led_color_name(patch->value.color));
        else (void)fputs(ugreenctl_led_color_name(patch->value.color),stdout);
    }
    if (patch->fields&UGREENCTL_LED_PATCH_BRIGHTNESS)
        (void)printf(json ? ",\"brightness\":%u" : " brightness=%u",patch->value.brightness);
    if (patch->fields&UGREENCTL_LED_PATCH_MODE) {
        (void)fputs(json ? ",\"mode\":" : " mode=",stdout);
        if (json) json_string(ugreenctl_led_mode_name(patch->value.mode));
        else (void)fputs(ugreenctl_led_mode_name(patch->value.mode),stdout);
    }
    if (patch->fields&UGREENCTL_LED_PATCH_TIMING)
        (void)printf(json ? ",\"on_ms\":%u,\"off_ms\":%u" : " on_ms=%u off_ms=%u",patch->value.on_ms,patch->value.off_ms);
}

static void identity_json(const struct ugreenctl_led_identity *identity)
{
    (void)fputs("\"product\":",stdout); json_string(identity->product_name);
    (void)fputs(",\"product_version\":",stdout); json_string(identity->product_version);
    (void)fputs(",\"board_version\":",stdout); json_string(identity->board_version);
    (void)fputs(",\"bios_vendor\":",stdout); if (identity->bios_vendor[0]) json_string(identity->bios_vendor); else (void)fputs("null",stdout);
    (void)fputs(",\"bios_version\":",stdout); if (identity->bios_version[0]) json_string(identity->bios_version); else (void)fputs("null",stdout);
    (void)fputs(",\"bios_date\":",stdout); if (identity->bios_date[0]) json_string(identity->bios_date); else (void)fputs("null",stdout);
    (void)fputs(",\"bios_compatibility\":\"not-independently-validated\"",stdout);
    (void)fputs(",\"profile\":",stdout); json_string(identity->profile->id);
    (void)fputs(",\"protocol\":",stdout); json_string(identity->profile->protocol);
    (void)fputs(",\"identity_source\":",stdout); json_string(identity->source);
    (void)printf(",\"experimental\":%s",identity->profile->experimental ? "true" : "false");
}

static void controller_json(const char *bus, uint16_t chip, bool probed,
                            bool confirmed)
{
    (void)fputs(",\"bus\":",stdout);
    if (probed) json_string(bus); else (void)fputs("null",stdout);
    (void)fputs(",\"chip_id\":",stdout);
    if (probed && chip!=0) (void)printf("%u",chip); else (void)fputs("null",stdout);
    (void)fputs(",\"write_identity_confirmed\":",stdout);
    if (!probed) (void)fputs("null",stdout);
    else (void)fputs(confirmed ? "true" : "false",stdout);
}

static void legacy_status(const struct ugreenctl_led_descriptor *led,
                          const struct ugreenctl_led_value *value)
{
    if (!led->rgb || value->unknown_fields || value->cached_fields) {
        (void)printf("%s: ",led->name); print_value(value,led,false); (void)putchar('\n'); return;
    }
    (void)printf("%s: status = %s, brightness = %u, color = RGB(%u, %u, %u)",
                 led->name,ugreenctl_led_mode_name(value->mode),value->brightness,
                 value->red,value->green,value->blue);
    if (value->mode==UGREENCTL_LED_MODE_BLINK)
        (void)printf(", blink_on = %u ms, blink_off = %u ms",value->on_ms,value->off_ms);
    (void)putchar('\n');
}

static int select_targets(const struct ugreenctl_hardware_options *o,
                          const struct ugreenctl_led_identity *identity,
                          struct ugreenctl_led_descriptor *selected,
                          size_t *count, char *error, size_t size)
{
    struct ugreenctl_led_descriptor descriptors[UGREENCTL_MAX_LEDS];
    size_t n=ugreenctl_led_descriptors(identity->profile,descriptors,UGREENCTL_MAX_LEDS), i,j;
    *count=0;
    if (n==0 || n>UGREENCTL_MAX_LEDS) {
        (void)snprintf(error,size,"profile has no usable LED layout"); return -ENOTSUP;
    }
    if (o->command==UGREENCTL_HW_LED_LIST || strcmp(o->targets[0],"all")==0) {
        (void)memcpy(selected,descriptors,n*sizeof(*selected)); *count=n; return 0;
    }
    for (i=0;i<o->target_count;++i) {
        for (j=0;j<n;++j) if (strcmp(o->targets[i],descriptors[j].name)==0) break;
        if (j==n) {
            (void)snprintf(error,size,"LED '%s' is unsupported by exact profile '%s'",
                           o->targets[i],identity->profile->id); return -ENOTSUP;
        }
        selected[(*count)++]=descriptors[j];
    }
    return 0;
}

static int led_list(const struct ugreenctl_hardware_options *o,
                    const struct ugreenctl_led_identity *identity,
                    const struct ugreenctl_led_descriptor *leds, size_t count)
{
    size_t i;
    if (o->json) {
        (void)fputs("{\"schema_version\":1,\"ok\":true,\"data\":{",stdout);
        identity_json(identity); (void)fputs(",\"leds\":[",stdout);
        for (i=0;i<count;++i) {
            unsigned int mode, color_id; bool first=true;
            if (i) (void)putchar(',');
            (void)fputs("{\"name\":",stdout); json_string(leds[i].name);
            (void)printf(",\"id\":%u,\"bay\":%u,\"rgb\":%s,\"brightness\":%s,\"min_brightness\":%u,\"max_brightness\":%u,\"min_on_ms\":%u,\"min_off_ms\":%u,\"min_breath_on_ms\":%u,\"min_breath_off_ms\":%u,\"max_cycle_ms\":%u,\"timing_quantum_ms\":%u,\"modes\":[",
                         leds[i].id,leds[i].bay,leds[i].rgb ? "true" : "false",
                         leds[i].max_brightness ? "true" : "false",leds[i].min_brightness,leds[i].max_brightness,leds[i].min_on_ms,leds[i].min_off_ms,
                         leds[i].min_breath_on_ms,leds[i].min_breath_off_ms,leds[i].max_cycle_ms ? leds[i].max_cycle_ms : 65535,
                         leds[i].timing_quantum_ms ? leds[i].timing_quantum_ms : 1);
            for (mode=0;mode<=UGREENCTL_LED_MODE_BREATH;++mode) if (leds[i].mode_mask & UGREENCTL_LED_MODE_BIT(mode)) {
                if (!first) (void)putchar(','); first=false;
                json_string(ugreenctl_led_mode_name((enum ugreenctl_led_mode)mode));
            }
            (void)fputs("],\"colors\":[",stdout); first=true;
            for (color_id=0;color_id<=UGREENCTL_LED_COLOR_MAX;++color_id) if (leds[i].color_mask & UGREENCTL_LED_COLOR_BIT(color_id)) {
                if (!first) (void)putchar(','); first=false;
                json_string(ugreenctl_led_color_name((enum ugreenctl_led_color)color_id));
            }
            (void)fputs("]}",stdout);
        }
        (void)fputs("]}}\n",stdout);
    } else {
        (void)printf("%s: %s (%s%s)\n",identity->product_name,identity->profile->id,
                     identity->profile->protocol,identity->profile->experimental ? ", experimental" : "");
        (void)printf("BIOS: %s | %s | %s (version compatibility unverified)\n",
            identity->bios_vendor[0] ? identity->bios_vendor : "unknown",
            identity->bios_version[0] ? identity->bios_version : "unknown",
            identity->bios_date[0] ? identity->bios_date : "unknown");
        for (i=0;i<count;++i) {
            unsigned int c;
            (void)printf("%-8s id=%u bay=%u %s brightness=%u..%u modes=",
                         leds[i].name,leds[i].id,leds[i].bay,leds[i].rgb ? "RGB" : "palette",leds[i].min_brightness,leds[i].max_brightness);
            for (c=0;c<=UGREENCTL_LED_MODE_BREATH;++c) if (leds[i].mode_mask & UGREENCTL_LED_MODE_BIT(c))
                (void)printf("%s ",ugreenctl_led_mode_name((enum ugreenctl_led_mode)c));
            if (!leds[i].rgb) {
                (void)fputs("colors=",stdout);
                for (c=0;c<=UGREENCTL_LED_COLOR_MAX;++c) if (leds[i].color_mask & UGREENCTL_LED_COLOR_BIT(c))
                    (void)printf("%s ",ugreenctl_led_color_name((enum ugreenctl_led_color)c));
            }
            (void)putchar('\n');
        }
    }
    return 0;
}

struct operation_result {
    size_t action, target;
    bool value_known;
    struct ugreenctl_led_value value;
};

static volatile sig_atomic_t identify_cancelled;

static void identify_signal(int signal_number)
{
    identify_cancelled=signal_number;
}

static int identify_run(struct ugreenctl_led_session *session, uint8_t id,
                        unsigned int seconds, char *error, size_t size)
{
    struct sigaction handler, previous_int, previous_term;
    int result;
    (void)memset(&handler,0,sizeof(handler));
    handler.sa_handler=identify_signal; (void)sigemptyset(&handler.sa_mask);
    identify_cancelled=0;
    if (sigaction(SIGINT,&handler,&previous_int)!=0) {
        int saved=errno;
        (void)snprintf(error,size,"cannot install identify interrupt handler: %s",strerror(saved));
        return -saved;
    }
    if (sigaction(SIGTERM,&handler,&previous_term)!=0) {
        int saved=errno; (void)sigaction(SIGINT,&previous_int,NULL);
        (void)snprintf(error,size,"cannot install identify termination handler: %s",strerror(saved));
        return -saved;
    }
    result=ugreenctl_led_identify(session,id,seconds,&identify_cancelled,error,size);
    (void)sigaction(SIGINT,&previous_int,NULL); (void)sigaction(SIGTERM,&previous_term,NULL);
    if (result==0 && identify_cancelled!=0) {
        (void)snprintf(error,size,"identify interrupted; original LED state restored"); result=-EINTR;
    }
    return result;
}

static int led_execute(const struct ugreenctl_hardware_options *o)
{
    struct ugreenctl_led_identity identity;
    struct ugreenctl_led_descriptor leds[UGREENCTL_MAX_LEDS];
    struct ugreenctl_led_session *session=NULL;
    struct ugreenctl_led_access access={.force=o->force,.apply=o->apply,
        .n76_direct=o->n76_direct,.n76_bus=o->n76_bus,.n76_address=o->n76_address};
    struct ugreenctl_led_value initial[UGREENCTL_MAX_LEDS];
    struct operation_result *records=NULL;
    struct ugreenctl_hardware_options request=*o;
    size_t count=0, action_count, a,t,step,completed=0;
    bool writes=false, reads=false, identify=o->command==UGREENCTL_HW_LED_IDENTIFY;
    bool combined=!o->compatibility && o->command==UGREENCTL_HW_LED_SET;
    bool probed=false, confirmed=false;
    uint16_t chip=0;
    char bus[256]={0};
    int result;
    char error[512]={0};
    result=ugreenctl_led_detect(&identity,error,sizeof(error));
    if (result) return backend_failure(o,result,error,0);
    if (o->n76_direct && strcmp(identity.profile->protocol,"sysfs-n76"))
        return failure(o,"unsupported","--n76-direct is restricted to exact DH2600/DXP480T Plus",3,0);
    result=select_targets(o,&identity,leds,&count,error,sizeof(error));
    if (result) return backend_failure(o,result,error,0);
    if (identify && ((leds[0].mode_mask & 3U)!=3U || !leds[0].max_brightness ||
                     (!leds[0].rgb && !leds[0].color_mask)))
        return failure(o,"unsupported","this LED cannot perform a reversible on/off identification",3,0);
    if (o->command==UGREENCTL_HW_LED_LIST) return led_list(o,&identity,leds,count);
    if (o->command==UGREENCTL_HW_LED_STATUS) {
        request.action_count=1; request.actions[0].kind=UGREENCTL_CLI_STATUS;
    }
    action_count=combined ? 1 : request.action_count;
    for (a=0;a<action_count;++a) {
        struct ugreenctl_led_patch patch;
        if (!combined && request.actions[a].kind==UGREENCTL_CLI_STATUS) { reads=true; continue; }
        writes=true;
        for (t=0;t<count;++t) {
            if (combined) merged_patch(&request,&leds[t],&patch);
            else patch_for_action(&request.actions[a],&leds[t],&patch);
            result=ugreenctl_led_validate_target_patch(&leds[t],&patch,error,sizeof(error));
            if (result) return backend_failure(o,result,error,0);
            if (o->n76_direct) {
                struct ugreenctl_n76_plan plan;
                result=ugreenctl_n76_plan(identity.profile,leds[t].id,&patch,&plan,error,sizeof(error));
                if (result) return backend_failure(o,result,error,0);
            }
        }
    }
    if (identify) writes=true;
    if (writes && o->apply && identity.profile->experimental && !o->force)
        return failure(o,"experimental_confirmation","experimental LED writes require --force --apply",4,0);
    if (reads || (writes && o->apply)) {
        result=ugreenctl_led_open(&access,&session,error,sizeof(error));
        if (result) return backend_failure(o,result,error,0);
        probed=true; chip=ugreenctl_led_chip_id(session);
        confirmed=ugreenctl_led_identity_confirmed(session);
        (void)snprintf(bus,sizeof(bus),"%s",ugreenctl_led_bus(session));
        /* Preflight all targets before the first write, so an unreadable later
         * target cannot cause a partially executed request. */
        for (t=0;t<count;++t) {
            result=ugreenctl_led_read(session,leds[t].id,&initial[t],error,sizeof(error));
            if (result) { ugreenctl_led_close(session); return backend_failure(o,result,error,0); }
        }
        if (writes && o->apply && !identify) for (t=0;t<count;++t) {
            struct ugreenctl_led_value planned=initial[t];
            for (a=0;a<action_count;++a) {
                struct ugreenctl_led_patch patch;
                if (!combined && request.actions[a].kind==UGREENCTL_CLI_STATUS) continue;
                if (combined) merged_patch(&request,&leds[t],&patch);
                else patch_for_action(&request.actions[a],&leds[t],&patch);
                result=ugreenctl_led_preflight(session,leds[t].id,&planned,&patch,error,sizeof(error));
                if (result) { ugreenctl_led_close(session); return backend_failure(o,result,error,0); }
                if (combined && !o->n76_direct && !leds[t].rgb && leds[t].max_brightness==1 && (patch.fields&UGREENCTL_LED_PATCH_COLOR) &&
                    (planned.unknown_fields&UGREENCTL_LED_PATCH_COLOR) &&
                    !((patch.fields&UGREENCTL_LED_PATCH_MODE) && patch.value.mode==UGREENCTL_LED_MODE_ON) &&
                    !((patch.fields&UGREENCTL_LED_PATCH_BRIGHTNESS) && patch.value.brightness)) {
                    ugreenctl_led_close(session);
                    return failure(o,"unsupported","a binary GPIO LED has no dormant color register; include --mode on when selecting its color",3,0);
                }
                /* Plan legacy actions in their original order. This only
                 * predicts fields explicitly set by this request; every real
                 * write still rechecks the backend's current state. */
                if (patch.fields&UGREENCTL_LED_PATCH_MODE) planned.mode=patch.value.mode;
                if (patch.fields&UGREENCTL_LED_PATCH_COLOR) planned.color=patch.value.color;
                if (patch.fields&UGREENCTL_LED_PATCH_BRIGHTNESS) planned.brightness=patch.value.brightness;
                planned.unknown_fields&=~patch.fields;
            }
        }
    }
    if (identify) {
        if (o->apply) {
            result=identify_run(session,leds[0].id,o->seconds,error,sizeof(error));
            ugreenctl_led_close(session);
            if (result) return backend_failure(o,result,error,0);
        }
        if (o->json) {
            (void)fputs("{\"schema_version\":1,\"ok\":true,\"data\":{",stdout); identity_json(&identity);
            controller_json(bus,chip,probed,confirmed);
            (void)fputs(",\"target\":",stdout); json_string(leds[0].name);
            (void)printf(",\"action\":\"identify\",\"seconds\":%u,\"dry_run\":%s,\"restored\":%s}}\n",
                         o->seconds,o->apply ? "false" : "true",o->apply ? "true" : "null");
        } else (void)printf("%sidentify %s for %u seconds%s\n",o->apply ? "" : "Preview: ",
                             leds[0].name,o->seconds,o->apply ? "; original state restored" : "; add --apply to execute");
        return 0;
    }
    records=calloc(action_count*count,sizeof(*records));
    if (records==NULL) {
        ugreenctl_led_close(session); return failure(o,"out_of_memory","cannot allocate operation results",5,0);
    }
    for (step=0;step<action_count*count;++step) {
        struct operation_result *record=&records[step];
        struct ugreenctl_led_patch patch;
        /* The legacy tool completes the ordered action sequence for each
         * target before moving to the next target. */
        a=o->compatibility ? step%action_count : step/count;
        t=o->compatibility ? step/action_count : step%count;
        record->action=a; record->target=t;
        if (!combined && request.actions[a].kind==UGREENCTL_CLI_STATUS) {
            result=ugreenctl_led_read(session,leds[t].id,&record->value,error,sizeof(error));
            record->value_known=result==0;
            if (result==0 && o->compatibility && !o->json)
                legacy_status(&leds[t],&record->value);
        } else if (o->apply) {
            if (combined) merged_patch(&request,&leds[t],&patch);
            else patch_for_action(&request.actions[a],&leds[t],&patch);
            result=ugreenctl_led_apply(session,leds[t].id,&patch,&record->value,error,sizeof(error));
            record->value_known=result==0;
            if (result==0) ++completed;
        } else result=0;
        if (result) {
            free(records); ugreenctl_led_close(session); return backend_failure(o,result,error,completed);
        }
    }
    ugreenctl_led_close(session);
    if (o->json) {
        (void)fputs("{\"schema_version\":1,\"ok\":true,\"data\":{",stdout); identity_json(&identity);
        controller_json(bus,chip,probed,confirmed);
        if (o->n76_direct) (void)printf(",\"transport\":\"n76-direct\",\"requested_bus\":%u,\"requested_address\":%u,\"hardware_state_verified\":false",o->n76_bus,o->n76_address);
        (void)printf(",\"dry_run\":%s,\"operations\":[",writes && !o->apply ? "true" : "false");
        for (a=0;a<action_count*count;++a) {
            const struct operation_result *record=&records[a];
            const struct ugreenctl_led_cli_action *action=&request.actions[record->action];
            if (a) (void)putchar(',');
            (void)printf("{\"step\":%zu,\"target\":",record->action+1); json_string(leds[record->target].name);
            (void)fputs(",\"action\":",stdout); json_string(combined ? "set" : action_name(action->kind));
            if (combined) { struct ugreenctl_led_patch patch; merged_patch(&request,&leds[record->target],&patch); print_patch_parameters(&patch,true); }
            else print_action_parameters(action,&leds[record->target],true);
            (void)printf(",\"applied\":%s",o->apply && action->kind!=UGREENCTL_CLI_STATUS ? "true" : "false");
            if (record->value_known) { (void)fputs(",\"state\":",stdout); print_value(&record->value,&leds[record->target],true); }
            (void)putchar('}');
        }
        (void)fputs("]}}\n",stdout);
    } else {
        if (o->n76_direct) (void)printf("N76 direct: /dev/i2c-%u address=0x%02x; output/color/PWM/timing are NOT hardware-verified.\n",o->n76_bus,o->n76_address);
        if (writes && !o->apply) (void)puts("Preview only. Add --apply to write; experimental profiles also require --force.");
        if (!o->compatibility && o->command==UGREENCTL_HW_LED_STATUS) {
            (void)printf("Controller: bus=%s chip_id=",bus);
            if (chip!=0) (void)printf("0x%04x",chip); else (void)fputs("unknown",stdout);
            (void)printf(" write_identity_confirmed=%s\n",confirmed ? "yes" : "no");
        }
        for (a=0;a<action_count*count;++a) {
            const struct operation_result *record=&records[a];
            const struct ugreenctl_led_cli_action *action=&request.actions[record->action];
            if (o->compatibility && action->kind==UGREENCTL_CLI_STATUS && record->value_known) {
                continue; /* Already emitted in legacy execution order. */
            }
            if (o->compatibility && (o->apply || action->kind==UGREENCTL_CLI_STATUS)) continue;
            (void)printf("%-8s %s",leds[record->target].name,combined ? "set" : action_name(action->kind));
            if (combined) { struct ugreenctl_led_patch patch; merged_patch(&request,&leds[record->target],&patch); print_patch_parameters(&patch,false); }
            else print_action_parameters(action,&leds[record->target],false);
            if (record->value_known) { (void)fputs(" -> ",stdout); print_value(&record->value,&leds[record->target],false); }
            (void)putchar('\n');
        }
    }
    free(records); return 0;
}

static int disk_execute(const struct ugreenctl_hardware_options *o)
{
    struct ugreenctl_disk_scan scan;
    struct ugreenctl_machine_identity identity;
    const struct ugreenctl_disk *disk=NULL;
    char product[256]={0}, error[512]={0};
    int result;
    size_t i;
    /* Enumeration stays useful on unknown/non-UGREEN systems. Mapping remains
     * unverified there; no model override is accepted for writes. */
    if (ugreenctl_identity_read(&identity,error,sizeof(error))==0)
        (void)snprintf(product,sizeof(product),"%s",ugreenctl_identity_model(&identity));
    result=ugreenctl_disk_scan(product,o->map_file,&scan,error,sizeof(error));
    if (result) return backend_failure(o,result,error,0);
    if (o->command==UGREENCTL_HW_DISK_LOCATE) {
        struct ugreenctl_hardware_options identify=*o;
        result=ugreenctl_disk_resolve(&scan,o->targets[0],&disk,error,sizeof(error));
        if (result==-ENOTSUP) return backend_failure(o,result,error,0);
        if (result) return failure(o,"unmapped_disk",error,6,0);
        if (!disk->mapped || !disk->led_supported || !disk->led_id[0])
            return failure(o,"unsupported","this physical disk slot has no verified independently controllable LED",3,0);
        identify.command=UGREENCTL_HW_LED_IDENTIFY;
        identify.target_count=1; identify.targets[0]=disk->led_id;
        return led_execute(&identify);
    }
    if (o->tsv) {
        /* Only fixed tokens and kernel block names: no drive-supplied model,
         * serial, or free-form reason can turn into parts of a row. */
        (void)puts("device\tinterface\tslot_name\tled");
        for (i=0;i<scan.count;++i) {
            disk=&scan.disks[i];
            (void)printf("%s\t%s\t%s\t%s\n",disk->device,disk->interface_type,
                disk->mapped ? disk->slot_name : "-",
                disk->mapped && disk->led_supported && disk->led_id[0] ? disk->led_id : "-");
        }
        return 0;
    }
    if (o->json) {
        (void)fputs("{\"schema_version\":1,\"ok\":true,\"data\":{\"product\":",stdout); json_string(product);
        (void)printf(",\"profile_known\":%s,\"profile\":",scan.profile_known ? "true" : "false"); json_string(scan.profile);
        (void)fputs(",\"disks\":[",stdout);
        for (i=0;i<scan.count;++i) {
            disk=&scan.disks[i];
            if (i) (void)putchar(',');
            (void)fputs("{\"device\":",stdout); json_string(disk->device);
            (void)fputs(",\"serial\":",stdout); json_string(disk->serial);
            (void)fputs(",\"wwn\":",stdout); json_string(disk->wwn);
            (void)fputs(",\"namespaces\":[",stdout);
            {
                size_t n; bool first=true;
                for (n=0;n<scan.namespace_count;++n) {
                    char device[80];
                    if (strcmp(scan.namespace_aliases[n].disk_name,disk->name)) continue;
                    if (!first) (void)putchar(',');
                    first=false;
                    (void)snprintf(device,sizeof(device),"/dev/%s",scan.namespace_aliases[n].name);
                    (void)fputs("{\"device\":",stdout); json_string(device);
                    (void)fputs(",\"wwn\":",stdout); json_string(scan.namespace_aliases[n].wwn);
                    (void)putchar('}');
                }
            }
            (void)putchar(']');
            (void)fputs(",\"topology\":",stdout); json_string(disk->topology);
            (void)fputs(",\"interface\":",stdout); json_string(disk->interface_type);
            (void)fputs(",\"slot_name\":",stdout);
            if (disk->mapped) json_string(disk->slot_name); else (void)fputs("null",stdout);
            (void)printf(",\"mapped\":%s,\"slot\":",disk->mapped ? "true" : "false");
            if (disk->mapped) (void)printf("%u",disk->slot); else (void)fputs("null",stdout);
            (void)fputs(",\"led\":",stdout);
            if (disk->mapped && disk->led_supported && disk->led_id[0]) json_string(disk->led_id); else (void)fputs("null",stdout);
            (void)printf(",\"led_supported\":%s",disk->mapped && disk->led_supported && disk->led_id[0] ? "true" : "false");
            (void)fputs(",\"mapping_source\":",stdout); json_string(disk->mapping_source);
            (void)fputs(",\"reason\":",stdout); json_string(disk->reason);
            (void)putchar('}');
        }
        (void)fputs("]}}\n",stdout);
    } else {
        (void)printf("Model: %s; physical map: %s\n",product[0] ? product : "unknown",scan.profile_known ? scan.profile : "unknown");
        for (i=0;i<scan.count;++i) {
            disk=&scan.disks[i];
            (void)printf("%-14s %-10s serial=%s topology=%s",disk->device,
                         disk->mapped ? disk->slot_name : "unmapped",disk->serial,disk->topology);
            if (disk->mapped) (void)printf(" source=%s led=%s",disk->mapping_source,
                disk->led_supported && disk->led_id[0] ? disk->led_id : "unsupported");
            else (void)printf(" reason=%s",disk->reason);
            {
                size_t n; bool first=true;
                for (n=0;n<scan.namespace_count;++n) {
                    if (strcmp(scan.namespace_aliases[n].disk_name,disk->name)) continue;
                    (void)printf("%s%s",first ? " namespaces=" : ",",scan.namespace_aliases[n].name);
                    first=false;
                }
            }
            (void)putchar('\n');
        }
        if (scan.count==0) (void)puts("No physical block devices found.");
    }
    return 0;
}

static int hardware_entry(int argc, char **argv, bool compatibility)
{
    struct ugreenctl_hardware_options options;
    char error[512]={0};
    int result=ugreenctl_hardware_parse(argc,argv,compatibility,&options,error,sizeof(error));
    if (result==-1) return -1;
    if (result) return failure(&options,"invalid_argument",error,2,0);
    if (options.command==UGREENCTL_HW_HELP) {
        const char *help=compatibility ? compat_help_text : help_text;
        if (options.json) {
            (void)fputs("{\"schema_version\":1,\"ok\":true,\"data\":{\"help\":",stdout); json_string(help);
            (void)fputs("}}\n",stdout);
        } else (void)fputs(help,stdout);
        return 0;
    }
    if (options.command==UGREENCTL_HW_DISK_LIST || options.command==UGREENCTL_HW_DISK_LOCATE)
        return disk_execute(&options);
    return led_execute(&options);
}

int ugreenctl_hardware_cli(int argc, char **argv)
{
    return hardware_entry(argc,argv,false);
}

int ugreenctl_led_compat_cli(int argc, char **argv)
{
    return hardware_entry(argc,argv,true);
}
