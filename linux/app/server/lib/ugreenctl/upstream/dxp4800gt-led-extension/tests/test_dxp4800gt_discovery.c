/* Exercise the actual production find_bus with simulated Linux syscalls. */
#define open fake_open
#define close fake_close
#define fstat fake_fstat
#define ioctl fake_ioctl
#define opendir fake_opendir
#define readdir fake_readdir
#define closedir fake_closedir
#define fopen fake_fopen
#include "led/led.c"
#undef fopen
#undef open
#undef close
#undef fstat
#undef ioctl
#undef opendir
#undef readdir
#undef closedir
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x); exit(1); } } while(0)
static int ids[6], busy=-1, denied=-1, timeout_bus=-1, wrong_node=-1;
static unsigned int cursor, opened, closed, reads, writes, owners_calls;
static int change_id;
static const char *names[] = {"Synopsys DesignWare I2C adapter", "Synopsys DesignWare I2C adapter",
 "SMBus PIIX4 adapter port 0 at 0b00", "SMBus PIIX4 adapter port 2 at 0b00",
 "SMBus PIIX4 adapter port 1 at 0b20", "AMDGPU DM i2c hw bus 0"};
DIR *fake_opendir(const char *path) { CHECK(!strcmp(path,"/sys/class/i2c-dev")); cursor=0; return (DIR *)1; }
struct dirent *fake_readdir(DIR *d) {
    static struct dirent entry; CHECK(d==(DIR *)1);
    if (cursor==6) return NULL;
    snprintf(entry.d_name,sizeof(entry.d_name),"i2c-%u",cursor++); return &entry;
}
int fake_closedir(DIR *d) { CHECK(d==(DIR *)1); return 0; }
FILE *fake_fopen(const char *path,const char *mode) {
    unsigned int bus; FILE *f;
    CHECK(!strcmp(mode,"r")); CHECK(sscanf(path,"/sys/class/i2c-dev/i2c-%u/name",&bus)==1 && bus<6);
    f=tmpfile(); CHECK(f); fputs(names[bus],f); rewind(f); return f;
}
int fake_open(const char *path,int flags,...) {
    unsigned int bus; CHECK(sscanf(path,"/dev/i2c-%u",&bus)==1 && bus<5);
    CHECK((flags & O_NOFOLLOW) && (flags & O_CLOEXEC));
    if ((int)bus==denied) { errno=EACCES; return -1; }
    ++opened; return 100+(int)bus;
}
int fake_close(int fd) { CHECK(fd>=100 && fd<105); ++closed; return 0; }
int fake_fstat(int fd,struct stat *st) {
    memset(st,0,sizeof(*st)); st->st_mode=fd-100==wrong_node?S_IFREG:S_IFCHR;
    st->st_rdev=makedev(89,fd-100); return 0;
}
int fake_ioctl(int fd,unsigned long request,...) {
    va_list ap; int bus=fd-100; va_start(ap,request); CHECK(bus>=0 && bus<5);
    if (request==I2C_FUNCS) {
        unsigned long *f=va_arg(ap,unsigned long *);
        *f=I2C_FUNC_SMBUS_READ_WORD_DATA|I2C_FUNC_SMBUS_READ_BYTE_DATA|I2C_FUNC_SMBUS_READ_I2C_BLOCK|I2C_FUNC_SMBUS_WRITE_BLOCK_DATA;
    } else if (request==I2C_SLAVE) {
        CHECK(va_arg(ap,int)==0x3a);
        if (bus==busy) { va_end(ap); errno=EBUSY; return -1; }
    } else {
        struct i2c_smbus_ioctl_data *t=va_arg(ap,struct i2c_smbus_ioctl_data *);
        CHECK(request==I2C_SMBUS);
        if (t->read_write!=I2C_SMBUS_READ) ++writes;
        CHECK(t->read_write==I2C_SMBUS_READ && t->command==0x5a && t->size==I2C_SMBUS_WORD_DATA);
        ++reads;
        if (bus==timeout_bus || !ids[bus]) { va_end(ap); errno=bus==timeout_bus?ETIMEDOUT:ENXIO; return -1; }
        t->data->word=(change_id && reads>5)?0x1234:ids[bus];
    }
    va_end(ap); return 0;
}
static int owners(void *opaque) { (void)opaque; ++owners_calls; return 0; }
static void reset(void) {
    memset(ids,0,sizeof(ids)); ids[3]=0xc5b2;
    busy=denied=timeout_bus=wrong_node=-1; change_id=0;
    opened=closed=reads=writes=owners_calls=0;
}
static int discover(const char *product) {
    struct ugreenctl_led_session s={0}; char error[256]; int result;
    strcpy(s.identity.product_name,product);
    s.identity.profile=ugreenctl_led_profile_find(product,""); CHECK(s.identity.profile);
    s.fd=-1; s.locked=true; s.opaque=&s; s.io.owners=owners; s.io.read_chip_id=production_chip_id;
    result=find_bus(&s,error,sizeof(error));
    if (!result) CHECK(s.bus_number==3);
    if (s.fd>=0) fake_close(s.fd);
    CHECK(opened==closed && writes==0);
    return result;
}
int main(void) {
    reset(); CHECK(!discover("DXP4800 GT")); CHECK(reads==6 && owners_calls>=7);
    reset(); ids[2]=0xc5b2; CHECK(discover("DXP4800 GT")==-ENOTUNIQ);
    reset(); ids[3]=0x1234; CHECK(discover("DXP4800 GT")==-ENODEV);
    reset(); busy=4; CHECK(discover("DXP4800 GT")==-EBUSY);
    reset(); denied=4; CHECK(discover("DXP4800 GT")==-EACCES);
    reset(); timeout_bus=4; CHECK(discover("DXP4800 GT")==-ETIMEDOUT);
    reset(); wrong_node=4; CHECK(discover("DXP4800 GT")==-ENODEV);
    reset(); change_id=1; CHECK(discover("DXP4800 GT")==-ESTALE);
    reset(); CHECK(discover("DXP6800")==-ENOTSUP); CHECK(!opened && !reads);
    puts("DXP4800 GT production discovery: identity, uniqueness, owners, read-only IO and cleanup passed");
    return 0;
}
