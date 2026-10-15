/* Local route-netlink observer. No Internet probes, credentials or audio data. */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "network-watch-state.h"

#define ROOT "/usrdata/qdc-web"
#define RUN "/run/qdc-web"
#define DEVICE "rmnet_data0"
#define LOWER_UP 0x10000u
static volatile sig_atomic_t stopped;
static void stop(int signal_number) { (void)signal_number;stopped=1; }
static int64_t now_ms(void) {
    struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t))return 0;
    return (int64_t)t.tv_sec*1000+t.tv_nsec/1000000;
}
static int compare(const void *a,const void *b) { return strcmp(a,b); }

/* This SDK's public control/forward transports use IPv4. Ignore USB, IMS
 * rmnet_data1 and unrelated IPv6 renewals, which cannot change those sockets. */
static int snapshot(char result[512],unsigned *index) {
    result[0]=0;*index=if_nametoindex(DEVICE);
    if(!*index)return 1; /* Removed interface is a valid offline observation. */
    struct ifaddrs *list=NULL;
    if(getifaddrs(&list))return 0;
    char addresses[16][INET_ADDRSTRLEN];unsigned count=0;int up=0,overflow=0;
    for(struct ifaddrs *a=list;a;a=a->ifa_next) {
        if(strcmp(a->ifa_name,DEVICE))continue;
        if((a->ifa_flags&IFF_UP)&&(a->ifa_flags&LOWER_UP))up=1;
        if(!a->ifa_addr||a->ifa_addr->sa_family!=AF_INET)continue;
        const struct sockaddr_in *v=(const void *)a->ifa_addr;
        uint32_t host=ntohl(v->sin_addr.s_addr);
        if(!host||(host>>24)==127||(host>>16)==0xa9fe)continue;
        if(count==16){overflow=1;break;}
        if(inet_ntop(AF_INET,&v->sin_addr,addresses[count],sizeof addresses[0]))count++;
    }
    freeifaddrs(list);
    if(overflow)return 0;
    if(!up||!count)return 1;
    FILE *routes=fopen("/proc/net/route","r");if(!routes)return 0;
    char line[512],iface[32];unsigned long dest,gateway,mask;unsigned flags;unsigned long gateways[16];unsigned ng=0;
    while(fgets(line,sizeof line,routes)) {
        if(sscanf(line,"%31s %lx %lx %x %*u %*u %*u %lx",iface,&dest,&gateway,&flags,&mask)!=5)continue;
        if(!strcmp(iface,DEVICE)&&!dest&&!mask&&(flags&1)&&!(flags&0x200)) {
            if(ng==16){overflow=1;break;}gateways[ng++]=gateway;
        }
    }
    int failed=ferror(routes);fclose(routes);
    if(failed||overflow)return 0;
    if(!ng)return 1;
    qsort(addresses,count,sizeof addresses[0],compare);
    /* Multiple default routes are uncommon; sort so dump order cannot reconnect. */
    for(unsigned a=0;a<ng;a++)for(unsigned b=a+1;b<ng;b++)if(gateways[b]<gateways[a]){unsigned long t=gateways[a];gateways[a]=gateways[b];gateways[b]=t;}
    size_t n=(size_t)snprintf(result,512,"%u",*index);
    for(unsigned i=0;i<count;i++){int written=snprintf(result+n,512-n,"|%s",addresses[i]);if(written<0||(size_t)written>=512-n)return 0;n+=(size_t)written;}
    for(unsigned i=0;i<ng;i++){int written=snprintf(result+n,512-n,"|g%lx",gateways[i]);if(written<0||(size_t)written>=512-n)return 0;n+=(size_t)written;}
    return 1;
}

static int netlink_open(void) {
    int fd=socket(AF_NETLINK,SOCK_RAW|SOCK_CLOEXEC,NETLINK_ROUTE);if(fd<0)return -1;
    int buffer=262144;setsockopt(fd,SOL_SOCKET,SO_RCVBUF,&buffer,sizeof buffer);
    struct sockaddr_nl local={0};local.nl_family=AF_NETLINK;
    local.nl_groups=RTMGRP_LINK|RTMGRP_IPV4_IFADDR|RTMGRP_IPV4_ROUTE;
    if(bind(fd,(struct sockaddr *)&local,sizeof local)){close(fd);return -1;}
    return fd;
}
static int relevant(const struct nlmsghdr *h,unsigned index,int *lost) {
    if(h->nlmsg_type==RTM_NEWLINK||h->nlmsg_type==RTM_DELLINK) {
        if(h->nlmsg_len<NLMSG_LENGTH(sizeof(struct ifinfomsg)))return 0;
        const struct ifinfomsg *m=NLMSG_DATA(h);int own=(unsigned)m->ifi_index==index && index;
        int len=IFLA_PAYLOAD(h);
        for(const struct rtattr *a=IFLA_RTA(m);RTA_OK(a,len);a=RTA_NEXT(a,len))
            if(a->rta_type==IFLA_IFNAME && RTA_PAYLOAD(a)==sizeof DEVICE && !memcmp(RTA_DATA(a),DEVICE,sizeof DEVICE))own=1;
        if(own&&(h->nlmsg_type==RTM_DELLINK||!(m->ifi_flags&IFF_UP)||!(m->ifi_flags&LOWER_UP))) *lost=1;
        return own;
    }
    if(h->nlmsg_type==RTM_NEWADDR||h->nlmsg_type==RTM_DELADDR) {
        if(h->nlmsg_len<NLMSG_LENGTH(sizeof(struct ifaddrmsg)))return 0;
        const struct ifaddrmsg *m=NLMSG_DATA(h);
        int own=m->ifa_family==AF_INET && index && m->ifa_index==index;
        if(own&&h->nlmsg_type==RTM_DELADDR)*lost=1;
        return own;
    }
    if(h->nlmsg_type==RTM_NEWROUTE||h->nlmsg_type==RTM_DELROUTE) {
        if(h->nlmsg_len<NLMSG_LENGTH(sizeof(struct rtmsg)))return 0;
        const struct rtmsg *m=NLMSG_DATA(h);
        if(m->rtm_family!=AF_INET||m->rtm_dst_len||m->rtm_table!=RT_TABLE_MAIN)return 0;
        int own=0,len=RTM_PAYLOAD(h);
        for(const struct rtattr *a=RTM_RTA(m);RTA_OK(a,len);a=RTA_NEXT(a,len))
            if(a->rta_type==RTA_OIF&&RTA_PAYLOAD(a)==sizeof(unsigned)){unsigned out;memcpy(&out,RTA_DATA(a),sizeof out);if(index&&out==index)own=1;}
        if(own&&h->nlmsg_type==RTM_DELROUTE)*lost=1;
        return own;
    }
    return 0;
}
static int restart(const char *reason) {
    pid_t child=fork();if(child<0)return -1;
    if(!child){
        setsid();
        int null=open("/dev/null",O_RDWR|O_CLOEXEC);
        if(null>=0){dup2(null,0);dup2(null,1);dup2(null,2);if(null>2)close(null);}
        execl("/bin/sh","sh",ROOT "/standalone/service.sh","tunnel-network-reconnect",reason,(char *)NULL);_exit(127);
    }
    int status;int64_t deadline=now_ms()+20000;
    while(!stopped&&now_ms()<deadline) {
        pid_t got=waitpid(child,&status,WNOHANG);
        if(got==child)return WIFEXITED(status)?WEXITSTATUS(status):-1;
        if(got<0&&errno!=EINTR)return -1;
        struct timespec delay={0,100000000};nanosleep(&delay,NULL);
    }
    /* The command has its own session and cannot leave a delayed stop request. */
    kill(-child,SIGKILL);kill(child,SIGKILL);while(waitpid(child,&status,0)<0&&errno==EINTR){}
    return -1;
}
static int self_test(void) {
    /* Synthetic kernel message parsing; no events are sent to the kernel. */
    struct {struct nlmsghdr h;struct ifinfomsg link;} link={0};int lost=0;
    link.h.nlmsg_type=RTM_NEWLINK;link.h.nlmsg_len=NLMSG_LENGTH(sizeof link.link);
    link.link.ifi_index=7;link.link.ifi_flags=IFF_UP|LOWER_UP;
    if(!relevant(&link.h,7,&lost)||lost)return 1;
    if(relevant(&link.h,8,&lost))return 1;
    link.link.ifi_flags=IFF_UP;
    if(!relevant(&link.h,7,&lost)||!lost)return 1;
    struct {struct nlmsghdr h;struct ifaddrmsg addr;} addr={0};lost=0;
    addr.h.nlmsg_type=RTM_DELADDR;addr.h.nlmsg_len=NLMSG_LENGTH(sizeof addr.addr);
    addr.addr.ifa_index=7;addr.addr.ifa_family=AF_INET6;
    if(relevant(&addr.h,7,&lost)||lost)return 1;
    addr.addr.ifa_family=AF_INET;
    if(!relevant(&addr.h,7,&lost)||!lost)return 1;
    struct {struct nlmsghdr h;struct rtmsg route;struct rtattr attr;unsigned index;} route={0};lost=0;
    route.h.nlmsg_type=RTM_DELROUTE;route.h.nlmsg_len=NLMSG_LENGTH(sizeof route.route)+RTA_LENGTH(sizeof route.index);
    route.route.rtm_family=AF_INET;route.route.rtm_table=RT_TABLE_MAIN;
    route.attr.rta_type=RTA_OIF;route.attr.rta_len=RTA_LENGTH(sizeof route.index);route.index=7;
    if(!relevant(&route.h,7,&lost)||!lost)return 1;
    lost=0;route.index=8;
    if(relevant(&route.h,7,&lost)||lost)return 1;
    puts("Netlink parsing passed: own/unrelated link, carrier loss, IPv4/IPv6 address filter, own/unrelated default route.");return 0;
}
static void record(const char *event,const char *reason) {
    char text[128],temp[]=RUN "/network-watch-result.XXXXXX";
    int n=snprintf(text,sizeof text,"%lld %s %s\n",(long long)(now_ms()/1000),event,reason);
    int fd=mkstemp(temp);if(fd<0)return;
    if(write(fd,text,(size_t)n)==n){close(fd);if(!rename(temp,RUN "/network-watch-result"))return;}else close(fd);
    unlink(temp);
}
int main(int argc,char **argv) {
    if(argc==2&&!strcmp(argv[1],"--version")){puts("qdc-network-watch 1.0.0");return 0;}
    if(argc==2&&!strcmp(argv[1],"--self-test"))return self_test();
    if(argc==2&&!strcmp(argv[1],"--snapshot")) {
        char signature[512];unsigned index=0;
        if(!snapshot(signature,&index))return 1;
        printf("{\"online\":%s,\"interfaceIndex\":%u,\"signature\":\"%s\"}\n",signature[0]?"true":"false",index,signature);return 0;
    }
    if(argc!=1)return 2;
    signal(SIGTERM,stop);signal(SIGINT,stop);
    int fd=netlink_open();if(fd<0){perror("route netlink");return 1;}
    struct network_state state={0};char signature[512];unsigned index=0;
    int ok=snapshot(signature,&index);if(ok)network_observe(&state,signature,0);
    int64_t due=now_ms()+2000,refresh=now_ms()+30000,retry=0;int event_lost=0;
    record("listening","local-ipv4");
    while(!stopped) {
        int64_t now=now_ms();int wait=1000;
        if(due&&due>now&&due-now<wait)wait=(int)(due-now);
        struct pollfd p={fd,POLLIN,0};int result=poll(&p,1,wait);
        if(result<0&&errno!=EINTR){close(fd);return 1;}
        if(p.revents&(POLLHUP|POLLNVAL)){close(fd);return 1;}
        if(p.revents&(POLLERR|POLLIN)) {
            _Alignas(struct nlmsghdr) char data[32768];struct sockaddr_nl sender={0};struct iovec io={data,sizeof data};
            struct msghdr message={0};message.msg_name=&sender;message.msg_namelen=sizeof sender;message.msg_iov=&io;message.msg_iovlen=1;
            ssize_t n=recvmsg(fd,&message,MSG_DONTWAIT);
            if(n<0&&errno==ENOBUFS)due=now_ms()+2000;
            else if(n>0&&sender.nl_pid==0) {
                if(message.msg_flags&MSG_TRUNC)due=now_ms()+2000;
                else {
                    size_t offset=0,length=(size_t)n;
                    while(length-offset>=sizeof(struct nlmsghdr)) {
                        struct nlmsghdr *h=(void *)(data+offset);
                        if(h->nlmsg_len<sizeof *h||h->nlmsg_len>length-offset)break;
                        if(h->nlmsg_type==NLMSG_OVERRUN||relevant(h,index,&event_lost))due=now_ms()+2000;
                        size_t step=NLMSG_ALIGN(h->nlmsg_len);
                        if(step>length-offset)break;
                        offset+=step;
                    }
                }
            }
        }
        now=now_ms();
        if((due&&now>=due)||now>=refresh) {
            due=0;refresh=now+30000;unsigned new_index=0;
            if(!snapshot(signature,&new_index)){due=now+5000;continue;}
            int needed=network_observe(&state,signature,event_lost);event_lost=0;
            if(new_index)index=new_index; /* Retain old index to match queued deletion events. */
            if(needed) {
                if(now<retry){due=retry;continue;}
                const char *reason=state.lost?"network-restored":"address-changed";
                int status=restart(reason);retry=now_ms()+10000;
                if(status==0||status==3){network_ack(&state,signature);record(status==0?"reconnected":"disabled",reason);}
                else {due=retry;record("deferred",reason);}
            }
        }
    }
    close(fd);return 0;
}
