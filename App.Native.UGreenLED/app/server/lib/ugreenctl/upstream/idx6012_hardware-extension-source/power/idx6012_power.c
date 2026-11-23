#define _GNU_SOURCE
#include "power/idx6012_power.h"
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
/* Vendor ug_it55_functions.c POWER_STATUS_REG, bit 0: 0=on, 1=off.
 * Restricted to the inspected OEM board/BIOS and Intel 8086:7e02 bridge.
 * G3 EC 0xa6 is a different control and is never touched here. */
#define ADDRESS 0xfe001020UL
static int lockfd=-1, memfd=-1;
static void *mapping=MAP_FAILED;
static size_t mapping_size;
static volatile uint32_t *reg;
int idx6012_power_identity(const struct ugreenctl_machine_identity *v)
{
 return v && !strcmp(v->source,"dmi") && !strcmp(v->manufacturer,"UGREEN") &&
   (!strcmp(v->product_name,"Default string") || !strcmp(v->product_name,"iDX6012")) &&
   !strcmp(v->product_version,"iDX6011_iDX6012 V3.0 241112") &&
   !strcmp(v->bios_version,"5.32") && !strcmp(v->bios_date,"11/12/2024") ? 0 : -ENODEV;
}
static int hex_file(const char *path,unsigned expected)
{
 FILE *f=fopen(path,"r"); unsigned n=0; if(!f)return -errno;
 int ok=fscanf(f,"%x",&n);fclose(f);return ok==1 && n==expected ? 0:-ENODEV;
}
static int guard(void)
{
 struct ugreenctl_machine_identity id;
 int r=ugreenctl_identity_read(&id,NULL,0);if(r)return r;
 if((r=idx6012_power_identity(&id)))return r;
 if((r=hex_file("/sys/bus/pci/devices/0000:00:1f.0/vendor",0x8086)))return r;
 if((r=hex_file("/sys/bus/pci/devices/0000:00:1f.0/device",0x7e02)))return r;
 const char *owners[]={"/proc/it86","/sys/module/ug_idx6011_sio","/sys/module/ug_idx6011pro_sio"};
 struct stat st;
 for(size_t n=0;n<sizeof(owners)/sizeof(owners[0]);n++){
  if(!lstat(owners[n],&st))return -EBUSY;
  if(errno!=ENOENT)return -errno;
 }
 DIR *d=opendir("/sys/bus/platform/drivers/intel_pmc_core");
 if(d){ struct dirent *e; while((e=readdir(d))){
   if(e->d_name[0]=='.'||!strcmp(e->d_name,"module"))continue;
   char p[512];snprintf(p,sizeof(p),"/sys/bus/platform/drivers/intel_pmc_core/%s",e->d_name);
   if(lstat(p,&st)||S_ISLNK(st.st_mode)){closedir(d);return -EBUSY;}
  }closedir(d);
 }else if(errno!=ENOENT)return -errno;
 FILE *f=fopen("/proc/iomem","r");char line[512];bool reserved=false;
 if(!f)return -errno;
 while(fgets(line,sizeof(line),f)){
  unsigned long long lo,hi;char name[256];
  if(sscanf(line," %llx-%llx : %255[^\n]",&lo,&hi,name)!=3)continue;
  if(lo<=ADDRESS+3 && hi>=ADDRESS){
   if(strcmp(name,"Reserved")||lo>ADDRESS||hi<ADDRESS+3){fclose(f);return -EBUSY;}
   reserved=true;
  }
 }
 r=ferror(f)?-EIO:reserved?0:-ENODEV;fclose(f);return r;
}
static void close_io(void)
{
 reg=NULL;if(mapping!=MAP_FAILED)munmap(mapping,mapping_size);
 mapping=MAP_FAILED;if(memfd>=0)close(memfd);if(lockfd>=0)close(lockfd);memfd=lockfd=-1;
}
static int open_io(void)
{
#if !defined(__x86_64__) && !defined(__i386__)
 return -ENOTSUP;
#else
 int r=guard();if(r)return r;
 lockfd=open("/run/ugreenctl-idx6012-power.lock",O_CREAT|O_RDWR|O_CLOEXEC|O_NOFOLLOW,0600);
 if(lockfd<0)return -errno;
 struct stat st;
 if(fstat(lockfd,&st)||!S_ISREG(st.st_mode)||st.st_uid!=0||st.st_nlink!=1){close_io();return -EPERM;}
 if(flock(lockfd,LOCK_EX|LOCK_NB)){r=-errno;close_io();return r;}
 if((r=guard())){close_io();return r;}
 memfd=open("/dev/mem",O_RDWR|O_SYNC|O_CLOEXEC|O_NOFOLLOW);
 if(memfd<0){r=-errno;close_io();return r;}
 long page=sysconf(_SC_PAGESIZE);if(page<=0){close_io();return -EINVAL;}
 mapping_size=(size_t)page;
 off_t base=(off_t)(ADDRESS & ~((unsigned long)page-1));
 mapping=mmap(NULL,mapping_size,PROT_READ|PROT_WRITE,MAP_SHARED,memfd,base);
 if(mapping==MAP_FAILED){r=-errno;close_io();return r;}
 reg=(volatile uint32_t *)((char *)mapping+(ADDRESS-(unsigned long)base));return 0;
#endif
}
static int read_io(uint32_t *v){*v=*reg;return *v==UINT32_MAX?-EIO:0;}
static int write_io(uint32_t v){int r=guard();if(r)return r;*reg=v;__sync_synchronize();return 0;}
static struct idx6012_power_io io={open_io,close_io,read_io,write_io};
#ifdef IDX6012_POWER_TESTING
void idx6012_power_test_io(const struct idx6012_power_io *v){io=*v;}
#endif
static int finish(int r,char *e,size_t z){if(r&&e&&z)snprintf(e,z,"iDX6012 startup: %s",strerror(-r));return r;}
int idx6012_read_startup(enum ugreenctl_startup_policy *p,char *e,size_t z)
{
 *p=UGREENCTL_STARTUP_UNKNOWN;int r=io.open();if(r)return finish(r,e,z);
 uint32_t v;r=io.read(&v);if(!r)*p=(v&1)?UGREENCTL_STARTUP_OFF:UGREENCTL_STARTUP_ON;
 io.close();return finish(r,e,z);
}
int idx6012_set_startup(bool force,enum ugreenctl_startup_policy p,char *e,size_t z)
{
 if(!force)return finish(-EPERM,e,z);
 if(p!=UGREENCTL_STARTUP_ON && p!=UGREENCTL_STARTUP_OFF)return finish(-ENOTSUP,e,z);
 int r=io.open();if(r)return finish(r,e,z);
 uint32_t before=0,after=0;bool attempted=false;
 r=io.read(&before);
 if(!r){attempted=true;r=io.write((before&~1U)|(p==UGREENCTL_STARTUP_OFF));}
 if(!r)r=io.read(&after);
 if(!r && (after&1)!=(uint32_t)(p==UGREENCTL_STARTUP_OFF))r=-EIO;
 if(r && attempted){
  int rollback=io.read(&after);
  if(!rollback)rollback=io.write((after&~1U)|(before&1));
  if(!rollback)rollback=io.read(&after);
  if(rollback || (after&1)!=(before&1)){
   io.close();if(e&&z)snprintf(e,z,"iDX6012 startup write failed; rollback unconfirmed");return -EIO;
  }
 }
 io.close();return finish(r,e,z);
}
