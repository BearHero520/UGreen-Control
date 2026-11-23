#define _POSIX_C_SOURCE 200809L
#include "led/led_service.h"
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>

static volatile sig_atomic_t stop;
static void stopping(int number) { stop=number; }

int main(int argc,char **argv)
{
    struct ugreenctl_led_access access={.force=false,.apply=false};
    struct sigaction handler;
    bool takeover_flow=false;
    char error[512]={0};
    int result,i;
    for (i=1;i<argc;++i) {
        if (!strcmp(argv[i],"--force")) access.force=true;
        else if (!strcmp(argv[i],"--apply")) access.apply=true;
        else if (!strcmp(argv[i],"--takeover-flow")) takeover_flow=true;
        else if (!strcmp(argv[i],"--help") || !strcmp(argv[i],"-h")) {
            puts("Usage: ugreenctl-ledd --force --apply [--takeover-flow]\n"
                 "Foreground LED holder for verified ARM GPIO models and DXP2800 GT.\n"
                 "Keeps controller resources and locks until SIGINT/SIGTERM.\n"
                 "Root-only clients use /run/ugreenctl-ledd.sock through ugreenctl.\n"
                 "No kernel modules are loaded or unloaded; no implicit backgrounding.\n"
                 "GPIO: stopping releases lines; output persistence is not guaranteed.\n"
                 "GT: --takeover-flow explicitly disables global autonomous effects while running.\n"
                 "GT: requires active Host2EC; restores all four LEDs and original FLOW on clean exit.\n"
                 "The original autonomous animation phase cannot be restored.");
            return 0;
        } else { fprintf(stderr,"unknown option: %s\n",argv[i]); return 2; }
    }
    if (!access.force || !access.apply) { fputs("starting the LED holder requires --force --apply\n",stderr); return 4; }
    memset(&handler,0,sizeof(handler)); handler.sa_handler=stopping; sigemptyset(&handler.sa_mask);
    if (sigaction(SIGINT,&handler,NULL) || sigaction(SIGTERM,&handler,NULL)) { perror("signal handler"); return 5; }
    result=ugreenctl_led_service_run(&access,takeover_flow,&stop,error,sizeof(error));
    if (result) fprintf(stderr,"%s\n",error[0] ? error : "LED holder failed");
    if (!result) return 0;
    if (result==-EPERM || result==-EACCES || result==-EBUSY) return 4;
    if (result==-ENODEV || result==-ENOTSUP) return 3;
    return 5;
}
