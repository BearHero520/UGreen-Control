#define _GNU_SOURCE
#include "led/led_service.h"
#include "led/led_gpio.h"
#include "led/led_cs201x.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

/* Fixed-width, versioned local wire format. All integers are network order;
 * no pointers, native enum layout, paths or caller-provided file names cross
 * the socket. Only the holder opens hardware and only root may connect. */
#define MAGIC 0x55474c44U
#define VERSION 1U
#define INFO 1U
#define READ 2U
#define APPLY 3U
#define BEGIN_IDENTIFY 4U
#define END_IDENTIFY 5U
struct request { uint32_t word[16]; };
struct response { uint32_t word[16]; char profile[64], error[256]; };
_Static_assert(sizeof(struct request)==64,"stable LED request layout");
_Static_assert(sizeof(struct response)==384,"stable LED response layout");
struct ugreenctl_led_service_client {
    int fd;
    bool confirmed;
    struct ugreenctl_led_access access;
    const struct ugreenctl_led_profile *profile;
};
struct service_ops {
    void *context;
    const struct ugreenctl_led_identity *identity;
    bool (*confirmed)(void *);
    int (*read)(void *,uint8_t,struct ugreenctl_led_value *,char *,size_t);
    int (*apply)(void *,uint8_t,const struct ugreenctl_led_patch *,struct ugreenctl_led_value *,char *,size_t);
    void (*client)(void *,pid_t);
    int (*begin_identify)(void *,uint8_t,char *,size_t);
    int (*end_identify)(void *,uint8_t,char *,size_t);
};
struct connection_state { bool saved,restore_failed; uint8_t saved_id; };

static int fail(char *error,size_t size,int result,const char *message)
{
    if (error && size) (void)snprintf(error,size,"%s",message);
    return result;
}

static int transfer(int fd,void *buffer,size_t length,bool writing,int seconds,
                    const volatile sig_atomic_t *stop)
{
    unsigned char *bytes=buffer;
    size_t done=0;
    struct timespec now;
    int64_t deadline;
    if (clock_gettime(CLOCK_MONOTONIC,&now)) return -errno;
    deadline=(int64_t)now.tv_sec*1000+now.tv_nsec/1000000+(int64_t)seconds*1000;
    while (done<length) {
        struct pollfd pending={fd,writing ? POLLOUT : POLLIN,0};
        int64_t remaining;
        int ready;
        ssize_t count;
        if (stop && *stop) return -EINTR;
        if (clock_gettime(CLOCK_MONOTONIC,&now)) return -errno;
        remaining=deadline-((int64_t)now.tv_sec*1000+now.tv_nsec/1000000);
        if (remaining<=0) return -ETIMEDOUT;
        ready=poll(&pending,1,(int)remaining);
        if (ready<0) { if (errno==EINTR) continue; return -errno; }
        if (!ready) return -ETIMEDOUT;
        count=writing ? send(fd,bytes+done,length-done,MSG_NOSIGNAL|MSG_DONTWAIT) : recv(fd,bytes+done,length-done,MSG_DONTWAIT);
        if (count<0) {
            /* Complete a client RPC before processing identify cancellation:
             * the holder may already have changed GPIO. Preserve framing and
             * identity so the same exclusive connection can restore it.
             * The daemon's stop flag still interrupts its idle session. */
            if (errno==EINTR || errno==EAGAIN || errno==EWOULDBLOCK) continue;
            return -errno;
        }
        if (!count) return -ECONNRESET;
        done+=(size_t)count;
    }
    return 0;
}

static int timeout(int fd,int seconds)
{
    struct timeval limit={seconds,0};
    if (setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&limit,sizeof(limit)) ||
        setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&limit,sizeof(limit))) return -errno;
    return 0;
}

static bool root_peer(int fd)
{
    struct ucred credentials;
    socklen_t length=sizeof(credentials);
    return getsockopt(fd,SOL_SOCKET,SO_PEERCRED,&credentials,&length)==0 &&
           length==sizeof(credentials) && credentials.uid==0;
}

static void encode_value(uint32_t *word,const struct ugreenctl_led_value *value)
{
    word[5]=htonl((unsigned int)value->mode); word[6]=htonl(value->brightness);
    word[7]=htonl(value->red); word[8]=htonl(value->green); word[9]=htonl(value->blue);
    word[10]=htonl(value->on_ms); word[11]=htonl(value->off_ms);
    word[12]=htonl((unsigned int)value->color);
    word[13]=htonl(value->unknown_fields); word[14]=htonl(value->cached_fields);
}

static void decode_value(const uint32_t *word,struct ugreenctl_led_value *value)
{
    memset(value,0,sizeof(*value));
    value->mode=(enum ugreenctl_led_mode)ntohl(word[5]); value->brightness=ntohl(word[6]);
    value->red=ntohl(word[7]); value->green=ntohl(word[8]); value->blue=ntohl(word[9]);
    value->on_ms=ntohl(word[10]); value->off_ms=ntohl(word[11]);
    value->color=(enum ugreenctl_led_color)ntohl(word[12]);
    value->unknown_fields=ntohl(word[13]); value->cached_fields=ntohl(word[14]);
}

static int serve_requests(int fd,const struct service_ops *ops,const volatile sig_atomic_t *stop,
                          struct connection_state *session)
{
    int result;
    /* A connection is the exclusive session. 75s permits the CLI's maximum
     * 60s identify wait, while a stalled client cannot hold the server forever. */
    if ((result=timeout(fd,75))<0) return result;
    while (!stop || !*stop) {
        struct request request;
        struct response response;
        struct ugreenctl_led_value value={0};
        unsigned int opcode,id;
        memset(&response,0,sizeof(response));
        result=transfer(fd,&request,sizeof(request),false,75,stop);
        if (result<0) return result==-ECONNRESET ? 0 : result;
        response.word[0]=htonl(MAGIC); response.word[1]=htonl(VERSION);
        (void)snprintf(response.profile,sizeof(response.profile),"%s",ops->identity->profile->id);
        opcode=ntohl(request.word[2]); id=ntohl(request.word[3]);
        result=0;
        if (ntohl(request.word[0])!=MAGIC || ntohl(request.word[1])!=VERSION ||
            request.word[14] || request.word[15] || id>255) result=-EPROTO;
        else if (!ops->confirmed(ops->context)) result=-ENOTSUP;
        else if (opcode==INFO) {
            for (size_t i=3;i<16;++i) if (request.word[i]) result=-EPROTO;
        } else if (opcode==READ) {
            for (size_t i=4;i<16;++i) if (request.word[i]) result=-EPROTO;
            if (!result) result=ops->read(ops->context,(uint8_t)id,&value,response.error,sizeof(response.error));
        } else if (opcode==BEGIN_IDENTIFY || opcode==END_IDENTIFY) {
            for (size_t i=4;i<16;++i) if (i!=13 && request.word[i]) result=-EPROTO;
            if (!result && ntohl(request.word[13])!=3U) result=-EPERM;
            if (!result && (!ops->begin_identify || !ops->end_identify)) result=-ENOTSUP;
            if (!result && opcode==BEGIN_IDENTIFY) {
                bool present=false;
                for (size_t i=0;i<ops->identity->profile->led_count;++i)
                    if (ops->identity->profile->leds[i].id==id) present=true;
                if (!present) result=-ENOTSUP;
            }
            if (!result && opcode==BEGIN_IDENTIFY) {
                if (session->saved) result=-EBUSY;
                else {
                    result=ops->begin_identify(ops->context,(uint8_t)id,response.error,sizeof(response.error));
                    if (!result) { session->saved=true; session->saved_id=(uint8_t)id; }
                    else {
                        char restore_error[256]={0};
                        int restored=ops->end_identify(ops->context,(uint8_t)id,restore_error,sizeof(restore_error));
                        /* An effect may have been partly applied even though
                         * BEGIN failed. ENOENT means no snapshot was taken. */
                        if (restored && restored!=-ENOENT) {
                            session->saved=true; session->saved_id=(uint8_t)id; session->restore_failed=true;
                            (void)snprintf(response.error,sizeof(response.error),"identify start and restoration failed: %.200s",restore_error);
                        }
                    }
                }
            } else if (!result) {
                if (!session->saved || id!=session->saved_id) result=-EINVAL;
                else {
                    result=ops->end_identify(ops->context,(uint8_t)id,response.error,sizeof(response.error));
                    if (!result) session->saved=false;
                    else session->restore_failed=true;
                }
            }
        } else if (opcode==APPLY) {
            struct ugreenctl_led_patch patch;
            memset(&patch,0,sizeof(patch));
            /* Both acknowledgements are required on every write RPC. */
            if (ntohl(request.word[13])!=3U) result=-EPERM;
            else if (session->saved) result=-EBUSY;
            else {
                patch.fields=ntohl(request.word[4]);
                decode_value(request.word,&patch.value);
                patch.value.unknown_fields=patch.value.cached_fields=0;
                result=ugreenctl_led_validate_patch(&patch,response.error,sizeof(response.error));
                if (!result) result=ops->apply(ops->context,(uint8_t)id,&patch,&value,response.error,sizeof(response.error));
            }
        } else result=-EINVAL;
        if (result && !response.error[0]) (void)snprintf(response.error,sizeof(response.error),"LED holder request refused: %s",strerror(-result));
        response.word[2]=htonl((uint32_t)(int32_t)result);
        response.word[3]=htonl(ops->confirmed(ops->context) ? 1U : 0U);
        if (!result) encode_value(response.word,&value);
        if (transfer(fd,&response,sizeof(response),true,75,stop)<0) return -EPIPE;
        if (result==-EPROTO) return result;
        if (session->restore_failed) return result ? result : -EIO;
    }
    return 0;
}

static int serve_connection(int fd,const struct service_ops *ops,const volatile sig_atomic_t *stop,
                            bool *restore_failed,char *error,size_t size)
{
    struct ucred credentials;
    socklen_t length=sizeof(credentials);
    int result;
    struct connection_state session={0};
    if (restore_failed) *restore_failed=false;
    if (getsockopt(fd,SOL_SOCKET,SO_PEERCRED,&credentials,&length) ||
        length!=sizeof(credentials) || credentials.uid!=0 || credentials.pid<=0) return -EPERM;
    if (ops->client) ops->client(ops->context,credentials.pid);
    result=serve_requests(fd,ops,stop,&session);
    /* A disconnected or cancelled client must not leave an identify effect
     * behind. No raw bytes come from the caller; the holder saved them. */
    if (session.saved) {
        int restored=ops->end_identify(ops->context,session.saved_id,error,size);
        if (restored) { session.restore_failed=true; result=restored; }
    }
    if (restore_failed) *restore_failed=session.restore_failed;
    /* During daemon shutdown the accepted socket remains open until the
     * complete controller baseline has been restored. Keep that authenticated
     * compatibility client excluded only for that still-active connection. */
    if (ops->client && !(restore_failed && (session.restore_failed || (stop && *stop))))
        ops->client(ops->context,0);
    return result;
}

static int exchange(struct ugreenctl_led_service_client *client,struct request *request,
                     struct ugreenctl_led_value *value,char *error,size_t size)
{
    struct response response;
    int result;
    if (!client || client->fd<0) return fail(error,size,-ENOTCONN,"LED holder session is disconnected");
    request->word[0]=htonl(MAGIC); request->word[1]=htonl(VERSION);
    result=transfer(client->fd,request,sizeof(*request),true,2,NULL);
    if (!result) result=transfer(client->fd,&response,sizeof(response),false,2,NULL);
    if (result) {
        /* A timed-out or partial frame cannot be reused: a late response has
         * no sequence number and could otherwise impersonate the next RPC's
         * readback. Closing also triggers pending identify restoration. */
        close(client->fd); client->fd=-1; client->confirmed=false;
        return fail(error,size,result,"LED holder connection failed or is busy; no direct write fallback is permitted");
    }
    if (ntohl(response.word[0])!=MAGIC || ntohl(response.word[1])!=VERSION ||
        !memchr(response.profile,0,sizeof(response.profile)) || !memchr(response.error,0,sizeof(response.error)) ||
        strcmp(response.profile,client->profile->id)) {
        close(client->fd); client->fd=-1; client->confirmed=false;
        return fail(error,size,-EPROTO,"LED holder identity or protocol mismatch");
    }
    client->confirmed=ntohl(response.word[3])==1;
    result=(int)(int32_t)ntohl(response.word[2]);
    if (result>0) {
        close(client->fd); client->fd=-1; client->confirmed=false;
        return fail(error,size,-EPROTO,"invalid LED holder result");
    }
    if (result) return fail(error,size,result,response.error);
    if (!client->confirmed) return fail(error,size,-ENOTSUP,"LED holder hardware identity is unconfirmed");
    if (value) decode_value(response.word,value);
    return 0;
}

static int connect_fd(int fd,const struct ugreenctl_led_identity *identity,
                       const struct ugreenctl_led_access *access,
                       struct ugreenctl_led_service_client **output,char *error,size_t size)
{
    struct ugreenctl_led_service_client *client;
    struct request request={{0}};
    int result;
    if (!identity || !identity->profile || !access || !output) { close(fd); return -EINVAL; }
    *output=NULL;
    if (!root_peer(fd)) { close(fd); return fail(error,size,-EPERM,"LED holder peer is not root"); }
    client=calloc(1,sizeof(*client));
    if (!client) { close(fd); return -ENOMEM; }
    client->fd=fd; client->profile=identity->profile; client->access=*access;
    result=timeout(fd,2);
    request.word[2]=htonl(INFO);
    if (!result) result=exchange(client,&request,NULL,error,size);
    if (result) {
        ugreenctl_led_service_close(client);
        return result==-ETIMEDOUT ? fail(error,size,-EBUSY,"LED holder is serving another exclusive LED session") : result;
    }
    *output=client;
    return 0;
}

int ugreenctl_led_service_connect(const struct ugreenctl_led_identity *identity,
                                  const struct ugreenctl_led_access *access,
                                  struct ugreenctl_led_service_client **output,char *error,size_t size)
{
    struct stat info;
    struct sockaddr_un address;
    int fd,result;
    if (output) *output=NULL;
    if (lstat(UGREENCTL_LED_SERVICE_SOCKET,&info)) return -errno;
    if (!S_ISSOCK(info.st_mode) || info.st_uid!=0 || (info.st_mode & 077))
        return fail(error,size,-EPERM,"LED holder socket ownership or permissions are unsafe");
    /* A full listener queue must report busy immediately. The exclusive
     * holder can be serving a 60-second identify session; do not block here
     * before the bounded RPC transfer has even started. */
    fd=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC|SOCK_NONBLOCK,0);
    if (fd<0) return -errno;
    memset(&address,0,sizeof(address)); address.sun_family=AF_UNIX;
    (void)snprintf(address.sun_path,sizeof(address.sun_path),"%s",UGREENCTL_LED_SERVICE_SOCKET);
    if (connect(fd,(struct sockaddr *)&address,sizeof(address))) {
        result=(errno==EAGAIN || errno==EWOULDBLOCK || errno==EINPROGRESS) ? -EBUSY : -errno;
        close(fd);
        return fail(error,size,result,"LED holder socket exists but cannot be reached; do not bypass it");
    }
    return connect_fd(fd,identity,access,output,error,size);
}

bool ugreenctl_led_service_identity_confirmed(const struct ugreenctl_led_service_client *client)
{
    return client && client->fd>=0 && client->confirmed;
}

int ugreenctl_led_service_read(struct ugreenctl_led_service_client *client,uint8_t id,
                               struct ugreenctl_led_value *value,char *error,size_t size)
{
    struct request request={{0}};
    if (!client || !value) return -EINVAL;
    request.word[2]=htonl(READ); request.word[3]=htonl(id);
    return exchange(client,&request,value,error,size);
}

int ugreenctl_led_service_apply(struct ugreenctl_led_service_client *client,uint8_t id,
                                const struct ugreenctl_led_patch *patch,
                                struct ugreenctl_led_value *value,char *error,size_t size)
{
    struct request request={{0}};
    int result;
    if (!client || !patch) return -EINVAL;
    if (!client->access.force || !client->access.apply) return fail(error,size,-EPERM,"LED holder writes require --force --apply");
    result=ugreenctl_led_validate_patch(patch,error,size);
    if (result) return result;
    encode_value(request.word,&patch->value);
    request.word[2]=htonl(APPLY); request.word[3]=htonl(id); request.word[4]=htonl(patch->fields);
    request.word[13]=htonl(3U); request.word[14]=request.word[15]=0;
    return exchange(client,&request,value,error,size);
}

void ugreenctl_led_service_close(struct ugreenctl_led_service_client *client)
{
    if (client) { if (client->fd>=0) close(client->fd); free(client); }
}

static int identify_boundary(struct ugreenctl_led_service_client *client,uint8_t id,
                              unsigned int opcode,char *error,size_t size)
{
    struct request request={{0}};
    if (!client || !client->access.force || !client->access.apply)
        return fail(error,size,-EPERM,"LED holder identify requires --force --apply");
    request.word[2]=htonl(opcode); request.word[3]=htonl(id); request.word[13]=htonl(3U);
    return exchange(client,&request,NULL,error,size);
}

int ugreenctl_led_service_identify(struct ugreenctl_led_service_client *client,uint8_t id,
                                   unsigned int seconds,const volatile sig_atomic_t *cancelled,
                                   char *error,size_t size)
{
    int result,restored;
    char restore_error[256]={0};
    if (seconds<1 || seconds>60) return fail(error,size,-EINVAL,"identify duration must be 1..60 seconds");
    result=identify_boundary(client,id,BEGIN_IDENTIFY,error,size);
    if (result) return result;
    /* The holder saves raw target bytes and starts a fixed, validated effect.
     * It rejects ordinary writes until END or disconnect restoration. */
    for (unsigned int tick=0;tick<seconds*10U;++tick) {
        struct timespec delay={0,100000000};
        if (cancelled && *cancelled) break;
        if (nanosleep(&delay,NULL)<0 && errno!=EINTR) { result=-errno; break; }
    }
    restored=identify_boundary(client,id,END_IDENTIFY,restore_error,sizeof(restore_error));
    if (restored) {
        /* Closing the connection triggers an independent holder restoration,
         * including when a response was lost after a hardware transaction. */
        if (client && client->fd>=0) { close(client->fd); client->fd=-1; client->confirmed=false; }
        return fail(error,size,restored,restore_error[0] ? restore_error : "LED identify restoration failed");
    }
    return result;
}

static bool gpio_confirmed(void *context) { return ugreenctl_led_gpio_identity_confirmed(context); }
static int gpio_read(void *context,uint8_t id,struct ugreenctl_led_value *value,char *error,size_t size)
{ return ugreenctl_led_gpio_read(context,id,value,error,size); }
static int gpio_apply(void *context,uint8_t id,const struct ugreenctl_led_patch *patch,struct ugreenctl_led_value *value,char *error,size_t size)
{ return ugreenctl_led_gpio_apply(context,id,patch,value,error,size); }
static void gpio_client(void *context,pid_t pid) { ugreenctl_led_gpio_set_client(context,pid); }
static bool cs_confirmed(void *context) { return ugreenctl_led_cs201x_identity_confirmed(context); }
static int cs_read(void *context,uint8_t id,struct ugreenctl_led_value *value,char *error,size_t size)
{ return ugreenctl_led_cs201x_read(context,id,value,error,size); }
static int cs_apply(void *context,uint8_t id,const struct ugreenctl_led_patch *patch,struct ugreenctl_led_value *value,char *error,size_t size)
{ return ugreenctl_led_cs201x_apply(context,id,patch,value,error,size); }
static void cs_client(void *context,pid_t pid) { ugreenctl_led_cs201x_set_client(context,pid); }
static int cs_begin_identify(void *context,uint8_t id,char *error,size_t size)
{ return ugreenctl_led_cs201x_begin_identify(context,id,error,size); }
static int cs_end_identify(void *context,uint8_t id,char *error,size_t size)
{ return ugreenctl_led_cs201x_end_identify(context,id,error,size); }

int ugreenctl_led_service_run(const struct ugreenctl_led_access *access,
                              bool takeover_flow,
                              const volatile sig_atomic_t *stop,char *error,size_t size)
{
    struct ugreenctl_led_gpio *gpio=NULL;
    struct ugreenctl_led_cs201x *cs=NULL;
    struct ugreenctl_led_identity identity;
    struct sockaddr_un address;
    struct stat old, bound={0};
    struct service_ops ops={0};
    mode_t previous;
    int listener=-1,shutdown_client=-1,result;
    bool owns_socket=false;
    if (geteuid()!=0) return fail(error,size,-EPERM,"LED holder must run as root");
    if (!access || !access->force || !access->apply) return fail(error,size,-EPERM,"starting the LED holder requires --force --apply");
    result=ugreenctl_led_detect(&identity,error,size);
    if (result) return result;
    if (!strcmp(identity.profile->protocol,"cs201x-sio")) {
        result=ugreenctl_led_cs201x_open_holder(access,takeover_flow,&cs,error,size);
        if (!result) {
            ops.context=cs; ops.identity=ugreenctl_led_cs201x_identity(cs);
            ops.confirmed=cs_confirmed; ops.read=cs_read; ops.apply=cs_apply; ops.client=cs_client;
            ops.begin_identify=cs_begin_identify; ops.end_identify=cs_end_identify;
        }
    } else if (!strcmp(identity.profile->protocol,"sysfs-gpio")) {
        if (takeover_flow) return fail(error,size,-EINVAL,"--takeover-flow is only valid for DXP2800 GT");
        result=ugreenctl_led_gpio_open(access,&gpio,error,size);
        if (!result) {
            ops.context=gpio; ops.identity=ugreenctl_led_gpio_identity(gpio);
            ops.confirmed=gpio_confirmed; ops.read=gpio_read; ops.apply=gpio_apply; ops.client=gpio_client;
        }
    } else return fail(error,size,-ENOTSUP,"the LED holder supports exact GPIO models and DXP2800 GT only");
    if (result) return result;
    /* The transport owns fixed controller locks, protecting stale-socket
     * cleanup and the complete global takeover/restore lifecycle. */
    if (!lstat(UGREENCTL_LED_SERVICE_SOCKET,&old)) {
        int check;
        if (!S_ISSOCK(old.st_mode) || old.st_uid!=0 || (old.st_mode & 077)) { result=-EPERM; goto done; }
        check=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC|SOCK_NONBLOCK,0);
        if (check<0) { result=-errno; goto done; }
        memset(&address,0,sizeof(address)); address.sun_family=AF_UNIX;
        strcpy(address.sun_path,UGREENCTL_LED_SERVICE_SOCKET);
        if (connect(check,(struct sockaddr *)&address,sizeof(address))==0) { close(check); result=-EBUSY; goto done; }
        result=-errno; close(check);
        if (result!=-ECONNREFUSED) goto done;
        if (unlink(UGREENCTL_LED_SERVICE_SOCKET)) { result=-errno; goto done; }
    } else if (errno!=ENOENT) { result=-errno; goto done; }
    listener=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0);
    if (listener<0) { result=-errno; goto done; }
    memset(&address,0,sizeof(address)); address.sun_family=AF_UNIX;
    strcpy(address.sun_path,UGREENCTL_LED_SERVICE_SOCKET);
    previous=umask(0077);
    result=bind(listener,(struct sockaddr *)&address,sizeof(address));
    umask(previous);
    if (result) { result=-errno; goto done; }
    owns_socket=true;
    if (lstat(UGREENCTL_LED_SERVICE_SOCKET,&bound) || chmod(UGREENCTL_LED_SERVICE_SOCKET,0600) || listen(listener,8)) { result=-errno; goto done; }
    result=0;
    while (!stop || !*stop) {
        struct pollfd poller={listener,POLLIN,0};
        int ready=poll(&poller,1,500),client;
        if (ready<0) { if (errno==EINTR) continue; result=-errno; break; }
        if (!ready) continue;
        client=accept4(listener,NULL,NULL,SOCK_CLOEXEC);
        if (client<0) { if (errno==EINTR) continue; result=-errno; break; }
        bool restore_failed=false;
        int served=serve_connection(client,&ops,stop,&restore_failed,error,size);
        if (restore_failed || (stop && *stop)) {
            shutdown_client=client;
            result=restore_failed ? (served ? served : -EIO) : 0;
            break;
        }
        close(client);
    }
done:
    if (listener>=0) close(listener);
    if (owns_socket) {
        struct stat current;
        if (!lstat(UGREENCTL_LED_SERVICE_SOCKET,&current) && current.st_uid==0 && S_ISSOCK(current.st_mode) &&
            current.st_ino==bound.st_ino && current.st_dev==bound.st_dev)
            (void)unlink(UGREENCTL_LED_SERVICE_SOCKET);
    }
    ugreenctl_led_gpio_close(gpio);
    if (cs) {
        char restore_error[256]={0};
        int restored=ugreenctl_led_cs201x_restore_close(cs,restore_error,sizeof(restore_error));
        if (restored) { result=restored; (void)fail(error,size,restored,restore_error[0] ? restore_error : "GT holder restoration failed; hardware state may have changed"); }
    }
    if (shutdown_client>=0) close(shutdown_client);
    if (result && error && size && !error[0]) (void)snprintf(error,size,"LED holder failed: %s",strerror(-result));
    return result;
}

#ifdef UGREENCTL_LED_SERVICE_TESTING
#include "led/led_service_test.h"
int ugreenctl_led_service_test_serve(int fd,const struct ugreenctl_led_service_test_ops *test)
{
    const struct service_ops ops={test->context,test->identity,test->confirmed,test->read,test->apply,test->client,
        test->begin_identify,test->end_identify};
    return serve_connection(fd,&ops,NULL,NULL,NULL,0);
}
int ugreenctl_led_service_test_connect(int fd,const struct ugreenctl_led_identity *identity,
                                      const struct ugreenctl_led_access *access,
                                      struct ugreenctl_led_service_client **client,char *error,size_t size)
{ return connect_fd(fd,identity,access,client,error,size); }
#endif
