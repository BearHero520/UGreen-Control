#define _DEFAULT_SOURCE
#include "network/wol_interface.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <linux/ethtool.h>
#include <linux/sockios.h>
#include <net/if.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <unistd.h>

static int compare_interface_names(const void *a, const void *b)
{ return strcmp((const char *)a, (const char *)b); }

int ugreenctl_wol_interfaces_at(const char *root, const struct ugreenctl_wol_profile *profile,
                               char (*names)[UGREENCTL_WOL_NAME_SIZE], size_t capacity,
                               size_t *count, char *error, size_t error_size)
{
    char path[1024], identity[256];
    struct dirent *entry;
    DIR *directory;
    if (!root || !profile || !names || !count || !capacity) return -EINVAL;
    *count = 0;
    if (snprintf(path, sizeof(path), "%s/class/net", root) >= (int)sizeof(path)) return -ENAMETOOLONG;
    directory = opendir(path);
    if (!directory) return -errno;
    while ((entry = readdir(directory)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        if (ugreenctl_wol_interface_identity_at(root, profile, entry->d_name,
                identity, sizeof(identity), error, error_size) != 0) continue;
        if (*count == capacity) {
            (void)closedir(directory);
            (void)snprintf(error, error_size, "too many physical Ethernet interfaces");
            return -E2BIG;
        }
        (void)snprintf(names[(*count)++], UGREENCTL_WOL_NAME_SIZE, "%s", entry->d_name);
    }
    (void)closedir(directory);
    qsort(names, *count, sizeof(names[0]), compare_interface_names);
    return 0;
}

int ugreenctl_wol_interface_identity_at(const char *root, const struct ugreenctl_wol_profile *profile,
                                       const char *name, char *identity, size_t size,
                                       char *error, size_t error_size)
{
    char path[1024],line[512],slot[80]="",of_path[256]="";
    unsigned long pci_class=0;
    int dt_compatible=0;
    unsigned type=0;
    FILE *f;
    const char *p;
    if (!root || !profile || !name || !name[0] || strlen(name)>=IFNAMSIZ || !identity || !size) return -EINVAL;
    for (p=name;*p;++p) if (!isalnum((unsigned char)*p) && *p!='_' && *p!='-' && *p!='.') return -EINVAL;
    if (!strcmp(name,".") || !strcmp(name,"..")) return -EINVAL;
    identity[0]=0;
    if (snprintf(path,sizeof(path),"%s/class/net/%s/type",root,name)>=(int)sizeof(path)) return -ENAMETOOLONG;
    f=fopen(path,"r");
    if (!f) goto unsupported;
    if (fscanf(f,"%u",&type)!=1) type=0;
    (void)fclose(f);
    if (type!=1) goto unsupported;
    (void)snprintf(path,sizeof(path),"%s/class/net/%s/wireless",root,name);
    if (!access(path,F_OK)) goto unsupported;
    (void)snprintf(path,sizeof(path),"%s/class/net/%s/device/uevent",root,name);
    f=fopen(path,"r");
    if (!f) goto unsupported;
    while (fgets(line,sizeof(line),f)) {
        line[strcspn(line,"\r\n")]=0;
        if (!strncmp(line,"PCI_CLASS=",10)) {
            char *end;
            errno=0; pci_class=strtoul(line+10,&end,16);
            if (errno || end==line+10 || *end) pci_class=0;
        }
        if (!strncmp(line,"PCI_SLOT_NAME=",14) && strlen(line+14)<sizeof(slot)) strcpy(slot,line+14);
        if (!strncmp(line,"OF_FULLNAME=",12) && strlen(line+12)<sizeof(of_path)) strcpy(of_path,line+12);
        if (!strncmp(line,"OF_COMPATIBLE_",14) && profile->ethernet_compatible) {
            char *equal=strchr(line,'=');
            if (equal && !strcmp(equal+1,profile->ethernet_compatible)) dt_compatible=1;
        }
    }
    (void)fclose(f);
    if ((pci_class>>8)==0x0200 && slot[0]) {
        if (snprintf(identity,size,"pci:%s",slot)>=(int)size) return -ENAMETOOLONG;
        return 0;
    }
    if (profile->ethernet_of_path && dt_compatible && !strcmp(of_path,profile->ethernet_of_path)) {
        if (snprintf(identity,size,"dt:%s",of_path)>=(int)size) return -ENAMETOOLONG;
        return 0;
    }
unsupported:
    (void)snprintf(error,error_size,"%s is not a physical PCI Ethernet device or the model's evidenced DT MAC",name);
    return -ENODEV;
}

int ugreenctl_wol_interface(const struct ugreenctl_wol_profile *profile, const char *name,
                            bool write, enum ugreenctl_wol_policy *policy,
                            char *error, size_t error_size)
{
    char before[256],after[256];
    struct ethtool_wolinfo wol={0};
    struct ifreq request={0};
    unsigned index,desired;
    int fd,result;
    if (!policy || (write && *policy!=UGREENCTL_WOL_ON && *policy!=UGREENCTL_WOL_OFF)) return -EINVAL;
    result=ugreenctl_wol_interface_identity_at("/sys",profile,name,before,sizeof(before),error,error_size);
    if (result) return result;
    index=if_nametoindex(name);
    if (!index) return -ENODEV;
    desired=*policy==UGREENCTL_WOL_ON ? WAKE_MAGIC : 0;
    fd=socket(AF_INET,SOCK_DGRAM | SOCK_CLOEXEC,0);
    if (fd<0) return -errno;
    (void)snprintf(request.ifr_name,sizeof(request.ifr_name),"%s",name);
    request.ifr_data=(char *)&wol;
    wol.cmd=ETHTOOL_GWOL;
    if (ioctl(fd,SIOCETHTOOL,&request)) { result=-errno; goto done; }
    if (!(wol.supported & WAKE_MAGIC)) { result=-EOPNOTSUPP; goto done; }
    if (write) {
        result=ugreenctl_wol_interface_identity_at("/sys",profile,name,after,sizeof(after),error,error_size);
        if (result) goto done;
        if (strcmp(before,after) || if_nametoindex(name)!=index) { result=-ESTALE; goto done; }
        memset(&wol,0,sizeof(wol)); wol.cmd=ETHTOOL_SWOL; wol.wolopts=desired;
        if (ioctl(fd,SIOCETHTOOL,&request)) { result=-errno; goto done; }
        memset(&wol,0,sizeof(wol)); wol.cmd=ETHTOOL_GWOL;
        if (ioctl(fd,SIOCETHTOOL,&request)) { result=-errno; goto done; }
        if (!(wol.supported & WAKE_MAGIC) || wol.wolopts!=desired) { result=-EIO; goto done; }
        result=ugreenctl_wol_interface_identity_at("/sys",profile,name,after,sizeof(after),error,error_size);
        if (result) goto done;
        if (strcmp(before,after) || if_nametoindex(name)!=index) { result=-ESTALE; goto done; }
    }
    *policy=(wol.wolopts & WAKE_MAGIC) ? UGREENCTL_WOL_ON : UGREENCTL_WOL_OFF;
    result=0;
done:
    (void)close(fd);
    if (result && error_size && !error[0])
        (void)snprintf(error,error_size,"WOL %s failed on %s: %s; state not confirmed",write ? "write/readback" : "query",name,strerror(-result));
    return result;
}
