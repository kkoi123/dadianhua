/* Independent WMS/VOICE client. Uses the module's existing Qualcomm libraries;
 * does not open an occupied AT port or change modem configuration.
 * Wire layouts: primary libqmi definitions plus the module's vendor IDL
 * tables (the WMS list mode tag differs from generic libqmi).
 * Legacy CCI ABI: qmi_service_info is five uint32 values; Linux OS parameters
 * are unused by direct qmi_client_init (no notifier or guessed OS struct).
 */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#endif
#include <dlfcn.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <net/if.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <signal.h>
#include <poll.h>
#include <sys/wait.h>
#include "notify.h"
#include "call-history.h"
#include "data-link.h"
#include <unistd.h>

#define CAP 16384
#define PAGE 8
#define MAX_PDU 256
typedef struct { unsigned int info[5]; } ServiceInfo;
typedef int (*GetService)(void *, ServiceInfo *);
typedef int (*Init)(ServiceInfo *, void *, void *, void *, void *, void **);
typedef int (*Send)(void *, unsigned int, void *, unsigned int, void *, unsigned int, unsigned int *, unsigned int);
typedef int (*Release)(void *);
typedef struct { void *handle; } Client;
typedef struct { unsigned char storage, tag; uint32_t index; } Slot;
typedef struct { unsigned char b[CAP]; size_t n; } Packet;
static GetService get_service;
static Init init_client;
static Send send_raw;
static Release release_client;
static void *services;
static FILE *output;
static char run[512], private_dir[512], failure[256];
static unsigned protocol_error;
static int transport_error;
static int debug;
#define TRACE(s) do {if(debug)fprintf(stderr,"qdc-comm: %s\n",s);}while(0)

static uint16_t u16(const unsigned char *p) { return (uint16_t)p[0] | (uint16_t)p[1] << 8; }
static uint32_t u32(const unsigned char *p) { return (uint32_t)u16(p) | (uint32_t)u16(p+2) << 16; }
static void put16(unsigned char *p, unsigned v) { p[0]=(unsigned char)v; p[1]=(unsigned char)(v>>8); }
static void put32(unsigned char *p, uint32_t v) { put16(p,v); put16(p+2,v>>16); }
static int tlv(Packet *p, unsigned type, const void *data, size_t n) {
    if (n>65535 || p->n+n+3>sizeof p->b) return 0;
    p->b[p->n]=(unsigned char)type; put16(p->b+p->n+1,(unsigned)n);
    if(n) memcpy(p->b+p->n+3,data,n);
    p->n+=3+n; return 1;
}
static const unsigned char *find(const Packet *p, unsigned type, size_t *length) {
    for(size_t i=0;i+3<=p->n;) {
        size_t n=u16(p->b+i+1);
        if(n>p->n-i-3) return NULL;
        if(p->b[i]==type) { *length=n; return p->b+i+3; }
        i+=3+n;
    }
    return NULL;
}
static int valid_packet(const Packet *p) {
    size_t i=0;
    while(i<p->n) { if(p->n-i<3) return 0; size_t n=u16(p->b+i+1); if(n>p->n-i-3)return 0; i+=3+n; }
    return i==p->n;
}
static void string(const char *s) {
    fputc('"',output);
    for(const unsigned char *p=(const unsigned char *)s;*p;p++) {
        if(*p=='"'||*p=='\\') fprintf(output,"\\%c",*p);
        else if(*p<32) fprintf(output,"\\u%04x",*p);
        else fputc(*p,output);
    }
    fputc('"',output);
}
static void error(const char *s) {
    fprintf(output,"{\"ok\":false,\"error\":");
    /* Chinese error messages are literals encoded as UTF-8. */
    fprintf(output,"\"%s\",\"qmiError\":%u,\"transportError\":%d}\n",s,protocol_error,transport_error);
}
static int digits(const char *s, size_t min, size_t max) {
    size_t n=strlen(s); if(n<min || n>max)return 0;
    for(size_t i=0;i<n;i++)if(s[i]<'0'||s[i]>'9')return 0;
    return 1;
}
static int number(const char *s) { return digits(s+(*s=='+'),1,20); }
static int hex(const char *s, size_t min, size_t max) {
    size_t n=strlen(s); if(n<min||n>max||n%2)return 0;
    for(size_t i=0;i<n;i++)if(!strchr("0123456789abcdef",s[i]))return 0;
    return 1;
}
static int sim_identity(const char *s) {
    return digits(s,10,32)||(!strncmp(s,"ef:",3)&&hex(s+3,20,20));
}
static void encode(const unsigned char *b,size_t n,char *s) {
    static const char h[]="0123456789abcdef";
    for(size_t i=0;i<n;i++) {s[2*i]=h[b[i]>>4];s[2*i+1]=h[b[i]&15];}s[n*2]=0;
}
static size_t decode(const char *s,unsigned char *b,size_t cap) {
    if(!hex(s,2,cap*2))return 0;
    size_t n=strlen(s)/2;
    for(size_t i=0;i<n;i++)b[i]=(unsigned char)(((strchr("0123456789abcdef",s[2*i])-"0123456789abcdef")<<4)|(strchr("0123456789abcdef",s[2*i+1])-"0123456789abcdef"));
    return n;
}
static int library(void) {
    const char *cci="/usr/lib/libqmi_cci.so.1", *objects="/usr/lib/libqmiservices.so.1";
#ifdef COMM_TEST
    cci=getenv("QDC_TEST_LIBRARY"); objects=cci;
#endif
    TRACE("loading CCI library");
    void *h=dlopen(cci,RTLD_NOW|RTLD_GLOBAL);
    TRACE("loading service objects");
    services=dlopen(objects,RTLD_NOW|RTLD_GLOBAL);
    if(!h||!services) { snprintf(failure,sizeof failure,"模块 QMI 动态库不可用"); return 0; }
    get_service=(GetService)dlsym(h,"qmi_client_get_any_service");
    init_client=(Init)dlsym(h,"qmi_client_init");
    send_raw=(Send)dlsym(h,"qmi_client_send_raw_msg_sync");
    release_client=(Release)dlsym(h,"qmi_client_release");
    TRACE("QMI symbols loaded");return get_service&&init_client&&send_raw&&release_client;
}
static int connect_client(Client *c,const char *name) {
    ServiceInfo info={{0}}; void *obj=dlsym(services,name);
    if(!obj) {snprintf(failure,sizeof failure,"模块未提供所需 QMI 服务");return 0;}
    TRACE("discovering service");transport_error=get_service(obj,&info);
    if(debug)fprintf(stderr,"qdc-comm: service discovery result=%d\n",transport_error);
    if(!transport_error){TRACE("initializing QMI client");transport_error=init_client(&info,obj,NULL,NULL,NULL,&c->handle);
        if(debug)fprintf(stderr,"qdc-comm: client initialization result=%d\n",transport_error);}
    if(transport_error) {snprintf(failure,sizeof failure,"无法连接模块 QMI 服务");return 0;}
    return 1;
}
static int request(Client *c,unsigned msg,const Packet *req,Packet *resp,unsigned timeout) {
    unsigned received=0;resp->n=0; protocol_error=0;
    TRACE("sending state query");transport_error=send_raw(c->handle,msg,(void *)req->b,(unsigned)req->n,resp->b,sizeof resp->b,&received,timeout);
    TRACE("QMI response returned");
    if(debug)fprintf(stderr,"qdc-comm: message=0x%x transport=%d bytes=%u\n",msg,transport_error,received);
    if(transport_error) {snprintf(failure,sizeof failure,"模块未及时返回结果，请勿直接重试发送或拨号");return 0;}
    if(received>sizeof resp->b) return 0;
    resp->n=received;
    size_t n=0;const unsigned char *result=find(resp,2,&n);
    if(!valid_packet(resp)||!result||n!=4){snprintf(failure,sizeof failure,"模块返回的数据不完整");return 0;}
    protocol_error=u16(result+2);
    if(debug)fprintf(stderr,"qdc-comm: result=%u protocol=%u\n",u16(result),protocol_error);
    if(u16(result)){snprintf(failure,sizeof failure,"模块拒绝操作，请检查 SIM 卡和运营商状态");return 0;}
    return 1;
}
static void disconnect(Client *c) {TRACE("releasing QMI client");if(c->handle)release_client(c->handle);c->handle=NULL;TRACE("QMI client released");}
/* Read-only NAS/UIM/WDS wire layouts checked against this device's IDL.
 * No AT port, network start/stop, PIN verification or profile writes. */
typedef struct {
    char operator_name[128], type[32], sim[64], apn[128], apn_source[16], address[32];
    int signal, has_signal, registration, roaming;
} Radio;
static const char *rat_name(unsigned rat) {
    switch(rat) {case 1:return "CDMA";case 2:return "EVDO";case 4:return "GSM";
    case 5:return "UMTS";case 8:return "LTE";case 9:return "TD-SCDMA";case 12:return "NR";default:return "";}
}
static const char *carrier(unsigned mcc,unsigned mnc) {
    if(mcc!=460)return "";
    switch(mnc) {case 0:case 2:case 4:case 7:case 8:return "中国移动";
    case 1:case 6:case 9:return "中国联通";
    case 3:case 5:case 11:case 12:return "中国电信";case 15:return "中国广电";default:return "";}
}
static int ascii_value(char *out,size_t cap,const unsigned char *p,size_t n) {
    if(!p||n>=cap)return 0;
    for(size_t i=0;i<n;i++)if(p[i]<32||p[i]>126)return 0;
    memcpy(out,p,n);out[n]=0;return 1;
}
static void serving(Radio *r,const Packet *resp) {
    size_t n=0;const unsigned char *p=find(resp,1,&n);
    if(!p||n<5||n!=5+(size_t)p[4]||p[0]>4)return;
    r->registration=p[0];
    if(p[0]!=1)return; /* Unregistered firmware can still return an old PLMN. */
    for(size_t i=5;i<n;i++)if(*rat_name(p[i])) {
        snprintf(r->type,sizeof r->type,"%s",rat_name(p[i]));
        if(p[i]==8||p[i]==12)break;
    }
    /* NAS roaming is authoritative; never infer it from operator names or
     * MCC comparisons (roaming SIMs can use several home-equivalent PLMNs).
     * QMI NAS roaming indicator 0=ON, 1=OFF; other values are unknown. */
    p=find(resp,0x10,&n);
    if(p&&n==1&&p[0]<=1)r->roaming=p[0]==0;
    p=find(resp,0x12,&n);
    if(!p||n<5||n!=5+(size_t)p[4]||u16(p)>999||u16(p+2)>999)return;
    const char *name=carrier(u16(p),u16(p+2));
    if(*name)snprintf(r->operator_name,sizeof r->operator_name,"%s",name);
    else if(!ascii_value(r->operator_name,sizeof r->operator_name,p+5,p[4])||!*r->operator_name)
        snprintf(r->operator_name,sizeof r->operator_name,"%03u-%03u",u16(p),u16(p+2));
}
static int card_status(Radio *r,const Packet *resp) {
    size_t n=0;const unsigned char *p=find(resp,0x10,&n);
    if(!p||n<9||p[8]>8)return 0;
    unsigned primary=u16(p),slot=primary==65535?0:primary>>8,app=primary&255;
    if(!p[8]){snprintf(r->sim,sizeof r->sim,"未插卡");return n==9;}
    if(slot>=p[8])return 0;
    char state[64]="";size_t off=9;
    for(unsigned i=0;i<p[8];i++) {
        if(n-off<6)return 0;
        unsigned cs=p[off],upin=p[off+1],apps=p[off+5],selected=0;off+=6;
        if(apps>16)return 0;
        if(i==slot)snprintf(state,sizeof state,"%s",cs==0?"未插卡":cs==2?"SIM 错误":cs==1?"初始化中":"");
        for(unsigned j=0;j<apps;j++) {
            if(n-off<7)return 0;
            unsigned type=p[off],as=p[off+1],aid=p[off+6];
            if(aid>n-off-7||n-off-7-aid<7)return 0;
            size_t pin=off+7+aid;unsigned ps=p[pin]?upin:p[pin+1];
            if(i==slot&&cs==1&&(j==app||(primary==65535&&(type==1||type==2)&&!selected))) {
                const char *s=ps==5?"SIM 已永久锁定":ps==4||as==3?"需要 PUK 码":
                    ps==1||as==2?"需要 PIN 码":as==5?"PIN 已锁定":as==6?"SIM 不可用":
                    as==4?"SIM 网络锁定":as==7?"已就绪":"初始化中";
                snprintf(state,sizeof state,"%s",s);selected=1;
            }
            off=pin+7;
        }
    }
    if(off!=n)return 0;
    snprintf(r->sim,sizeof r->sim,"%s",state);return *state!=0;
}
static int apn_value(Radio *r,const Packet *resp,const char *source) {
    size_t n=0;const unsigned char *p=find(resp,0x14,&n);
    if(!p||!ascii_value(r->apn,sizeof r->apn,p,n))return 0;
    if(!n)snprintf(r->apn,sizeof r->apn,"自动（空 APN）");
    snprintf(r->apn_source,sizeof r->apn_source,"%s",source);return 1;
}
static void nullable(const char *s) {if(*s)string(s);else fputs("null",output);}
static void radio(void) {
    Radio r={.registration=-1,.roaming=-1};Client c={0};Packet req={0},resp={0};
    if(connect_client(&c,"nas_qmi_idl_service_object_v01")) {
        if(request(&c,0x24,&req,&resp,1200))serving(&r,&resp);
        if(request(&c,0x20,&req,&resp,1200)&&r.registration==1) {
            size_t n=0;const unsigned char *p=find(&resp,1,&n);
            if(p&&n==2&&(int8_t)p[0]<0&&(int8_t)p[0]>-128&&*rat_name(p[1])) {
                r.signal=(int8_t)p[0];r.has_signal=1;
            }
        }
    }
    disconnect(&c);req=(Packet){0};
    if(connect_client(&c,"uim_qmi_idl_service_object_v01")&&request(&c,0x2f,&req,&resp,1200))card_status(&r,&resp);
    disconnect(&c);
    if(connect_client(&c,"wds_qmi_idl_service_object_v01")) {
        unsigned char mask[4];put32(mask,(1u<<3)|(1u<<8));req=(Packet){0};tlv(&req,0x10,mask,4);
        if(request(&c,0x2d,&req,&resp,1200)&&apn_value(&r,&resp,"current")) {
            size_t n=0;const unsigned char *p=find(&resp,0x1e,&n);
            if(p&&n==4){uint32_t ip=u32(p);snprintf(r.address,sizeof r.address,"%u.%u.%u.%u",(ip>>24)&255,(ip>>16)&255,(ip>>8)&255,ip&255);}
        }
        if(!*r.apn_source) {
            unsigned char type=0;req=(Packet){0};tlv(&req,1,&type,1);
            if(request(&c,0x2c,&req,&resp,1200))apn_value(&r,&resp,"configured");
        }
    }
    disconnect(&c);
    fprintf(output,"{\"ok\":true,\"stateSource\":\"qmi\",\"updatedAt\":%lld,\"network\":{\"operator\":",(long long)time(NULL));nullable(r.operator_name);
    fputs(",\"networkType\":",output);nullable(r.type);fputs(",\"simState\":",output);nullable(r.sim);
    fputs(",\"signalDbm\":",output);if(r.has_signal)fprintf(output,"%d",r.signal);else fputs("null",output);
    fputs(",\"signalMetric\":\"RSSI\",\"apn\":",output);nullable(r.apn);
    fputs(",\"apnSource\":",output);nullable(r.apn_source);fputs(",\"apnAddress\":",output);nullable(r.address);
    fprintf(output,",\"roaming\":%s,\"registrationState\":%d},\"complete\":%s}\n",r.roaming<0?"null":r.roaming?"true":"false",r.registration,
        *r.operator_name&&*r.type&&*r.sim&&r.has_signal&&*r.apn?"true":"false");
}
/* Read the ordinary-data profile's permission and NAS roaming state locally.
 * The explicit setter below changes only the profile's roaming-disallowed bit;
 * status reads never modify the modem or create external traffic. */
static int roaming_profile(Client *c,unsigned index) {
    Packet req={0},resp={0};unsigned char id[2]={0,(unsigned char)index};size_t n=0;
    tlv(&req,1,id,2);
    if(!request(c,0x2b,&req,&resp,1200))return -1;
    const unsigned char *flag=find(&resp,0x3e,&n);
    return flag&&n==1&&*flag<=1?!*flag:-1;
}
static void roaming_state(const char *profile,const char *setting) {
    unsigned index=profile&&digits(profile,1,2)?(unsigned)atoi(profile):0;
    if(profile&&(index<1||index>16)){error("上网配置编号无效");return;}
    int permission=-1;
    Client wds={0};
    if(index&&connect_client(&wds,"wds_qmi_idl_service_object_v01")){
        permission=roaming_profile(&wds,index);
        if(setting){
            if((strcmp(setting,"0")&&strcmp(setting,"1"))||permission<0){disconnect(&wds);error("模块未提供可确认的数据漫游设置，未修改");return;}
            int desired=setting[0]-'0';
            if(permission!=desired){
                Packet req={0},resp={0};unsigned char id[2]={0,(unsigned char)index},flag=(unsigned char)!desired;
                /* Modify ONLY the default ordinary-data profile's roaming
                 * disallowed bit. No APN, auth, IMS profile or radio fields.
                 * A matching setting is a read-only no-op, avoiding NV wear. */
                tlv(&req,1,id,2);tlv(&req,0x3e,&flag,1);
                if(!request(&wds,0x28,&req,&resp,2500)){disconnect(&wds);error("漫游设置结果未确认，请刷新核对");return;}
                permission=roaming_profile(&wds,index);
                if(permission!=desired){disconnect(&wds);error("漫游设置尚未确认生效，请刷新核对");return;}
            }
        }
    }
    disconnect(&wds);
    if(setting&&permission<0){error("数据漫游设置无法读取，未修改");return;}
    Radio r={.registration=-1,.roaming=-1};Client c={0};Packet req={0},resp={0};
    if(connect_client(&c,"nas_qmi_idl_service_object_v01")&&request(&c,0x24,&req,&resp,1200))serving(&r,&resp);
    disconnect(&c);
    fprintf(output,"{\"ok\":true,\"roaming\":%s,\"registrationState\":%d,\"permission\":%s,\"profile\":%u}\n",
        r.roaming<0?"null":r.roaming?"true":"false",r.registration,permission<0?"null":permission?"true":"false",index);
}
static int identity(char sim[33]) {
    Client c={0};Packet req={0},resp={0};int ok=0;
    if(connect_client(&c,"dms_qmi_idl_service_object_v01")&&request(&c,0x3c,&req,&resp,1500)) {
        size_t n=0;const unsigned char *s=find(&resp,1,&n);
        if(s&&n>=10&&n<=32) {memcpy(sim,s,n);sim[n]=0;ok=digits(sim,10,32);}
    }
    disconnect(&c);
    if(!ok&&connect_client(&c,"uim_qmi_idl_service_object_v01")) {
        /* Read only EF_ICCID (2FE2) under MF (3F00), active GW session.
         * Newer firmware no longer implements the legacy DMS UIM command. */
        req=(Packet){0};resp=(Packet){0};
        unsigned char session[]={0,0},file[]={0xe2,0x2f,2,0,0x3f},read_info[]={0,0,10,0};
        tlv(&req,1,session,sizeof session);tlv(&req,2,file,sizeof file);tlv(&req,3,read_info,sizeof read_info);
        if(request(&c,0x20,&req,&resp,1500)) {
            size_t n=0;const unsigned char *p=find(&resp,0x11,&n);
            if(debug)fprintf(stderr,"qdc-comm: SIM read result bytes=%zu prefix=%u\n",n,p&&n>=2?u16(p):0);
            if(p&&n>=7&&n<=18&&u16(p)==n-2) {
                size_t at=0;int valid=1;
                for(size_t i=2;i<n&&valid;i++)for(unsigned shift=0;shift<=4;shift+=4) {
                    unsigned digit=(p[i]>>shift)&15;
                    if(digit<=9)sim[at++]=(char)('0'+digit);
                    else if(!(digit==15&&i==n-1&&shift==4))valid=0;
                }
                sim[at]=0;ok=valid&&digits(sim,10,32);
                /* Some cards include nondecimal nibbles in their ICCID.
                 * For comparison only, preserve all ten EF bytes as an
                 * opaque identity; never present it as a phone number. */
                if(!ok&&n==12&&p[2]==0x98) {
                    memcpy(sim,"ef:",3);encode(p+2,10,sim+3);ok=1;
                }
                if(debug)fprintf(stderr,"qdc-comm: SIM identity confirmed=%d\n",ok);
            }
        }
        disconnect(&c);
    }
    if(!ok){sim[0]=0;snprintf(failure,sizeof failure,"无法读取当前 SIM 身份，暂不允许发送或删除短信");}return ok;
}
static int append_slots(const Packet *resp,unsigned storage,Slot slots[1024],size_t *count) {
    size_t n=0;const unsigned char *p=find(resp,1,&n);
    if(!p||n<4||u32(p)>1024||n!=4+(size_t)u32(p)*5)return 0;
    for(uint32_t i=0;i<u32(p);i++) {
        uint32_t index=u32(p+4+i*5);int duplicate=0;
        for(size_t j=0;j<*count;j++)if(slots[j].storage==storage&&slots[j].index==index){duplicate=1;break;}
        if(!duplicate){if(*count==1024)return 0;slots[(*count)++]=(Slot){(unsigned char)storage,p[8+i*5],index};}
    }
    return 1;
}
static int raw_read(Client *c, unsigned storage,uint32_t index,unsigned char *data,size_t *n,unsigned *tag,unsigned *format) {
    Packet req={0},resp={0};unsigned char id[5]={(unsigned char)storage},mode=1;
    put32(id+1,index);tlv(&req,1,id,sizeof id);tlv(&req,0x10,&mode,1);
    if(!request(c,0x22,&req,&resp,1500))return 0;
    size_t size=0;const unsigned char *p=find(&resp,1,&size);
    if(!p||size<4||u16(p+2)>MAX_PDU||size!=4+(size_t)u16(p+2))return 0;
    *tag=p[0];*format=p[1];*n=u16(p+2);memcpy(data,p+4,*n);return 1;
}
static void sms_list(Client *c,const char *cursor_arg) {
    if(!digits(cursor_arg,1,4)){error("短信分页参数无效");return;}
    unsigned cursor=(unsigned)strtoul(cursor_arg,NULL,10);Slot slots[1024];size_t count=0;unsigned stores=0,errors=0;
    char sim[33]="";int sim_ok=identity(sim);
    for(unsigned storage=0;storage<2;storage++) {
        Packet req={0},resp={0};unsigned char st=(unsigned char)storage,mode=1;
        /* Verified against this module's vendor IDL table: mode is 0x11,
         * optional tag is 0x10. Generic libqmi's list schema differs. */
        tlv(&req,1,&st,1);tlv(&req,0x11,&mode,1);
        if(request(c,0x31,&req,&resp,1500)) {
            if(append_slots(&resp,storage,slots,&count))stores++;else errors++;
        } else errors++;
    }
    if(!stores){error("短信存储读取失败，请检查 SIM 卡状态");return;}
    fprintf(output,"{\"ok\":true,\"sim\":");string(sim);
    fprintf(output,",\"canMutate\":%s,\"storageErrors\":%u,\"total\":%zu,\"messages\":[",sim_ok?"true":"false",errors,count);
    size_t end=cursor+PAGE;if(end>count)end=count;unsigned printed=0,failed=0;
    for(size_t i=cursor;i<end;i++) {
        unsigned char b[MAX_PDU];size_t n=0;unsigned tag=0,format=0;char raw[MAX_PDU*2+1];
        Slot s=slots[i];
        if(!raw_read(c,s.storage,s.index,b,&n,&tag,&format)){failed++;continue;}
        encode(b,n,raw);if(printed++)fputc(',',output);
        fprintf(output,"{\"storage\":%u,\"index\":%u,\"tag\":%u,\"format\":%u,\"pdu\":\"%s\"}",s.storage,s.index,tag,format,raw);
    }
    fprintf(output,"],\"readErrors\":%u,\"next\":",failed);
    if(end<count)fprintf(output,"%zu",end);else fprintf(output,"null");
    fprintf(output,"}\n");
}
/* Check actual storage after a successful delete response. Never repeat the
 * mutation: a slot may already have been reused for a newly received SMS. */
static int deleted_slot(Client *c,unsigned storage,uint32_t index,const char *raw) {
    Packet req={0},resp={0};unsigned char st=(unsigned char)storage,mode=1;
    tlv(&req,1,&st,1);tlv(&req,0x11,&mode,1);
    if(!request(c,0x31,&req,&resp,1500))return -1;
    Slot slots[1024];size_t count=0;
    if(!append_slots(&resp,storage,slots,&count))return -1;
    int present=0;
    for(size_t i=0;i<count;i++)if(slots[i].index==index){present=1;break;}
    if(!present)return 1;
    unsigned char data[MAX_PDU];size_t n=0;unsigned tag=0,format=0;char pdu[MAX_PDU*2+1];
    if(!raw_read(c,storage,index,data,&n,&tag,&format))return -1;
    encode(data,n,pdu);return strcmp(raw,pdu)?2:0;
}
static void delete_result(int verified,const char *message,int uncertain) {
    fprintf(output,"{\"ok\":%s,\"deleted\":%s,\"verified\":%s,\"uncertain\":%s",
        verified?"true":"false",verified?"true":"false",verified?"true":"false",uncertain?"true":"false");
    if(message){fputs(",\"error\":",output);string(message);}
    fprintf(output,",\"qmiError\":%u,\"transportError\":%d}\n",protocol_error,transport_error);
}
static void sms_delete(Client *c,const char *storage_arg,const char *index_arg,const char *raw,const char *expected_sim) {
    if(!digits(storage_arg,1,1)||atoi(storage_arg)>1||!digits(index_arg,1,8)||!hex(raw,2,MAX_PDU*2)||!sim_identity(expected_sim)){error("短信删除参数无效");return;}
    char sim[33];if(!identity(sim)){error(failure);return;}
    if(strcmp(sim,expected_sim)){error("SIM 卡已变化，请刷新短信列表");return;}
    unsigned char b[MAX_PDU];size_t n=0;unsigned tag=0,format=0;char current[MAX_PDU*2+1];
    unsigned st=(unsigned)atoi(storage_arg);uint32_t idx=(uint32_t)strtoul(index_arg,NULL,10);
    if(!raw_read(c,st,idx,b,&n,&tag,&format)) {
        /* Deleting a mirrored copy can also remove this exact storage slot.
         * Treat absence as verified; errors and reused slots remain failures. */
        if(deleted_slot(c,st,idx,raw)==1){delete_result(1,NULL,0);return;}
        error("短信已变化或无法读取，请刷新列表");return;
    }
    encode(b,n,current);if(strcmp(raw,current)){error("该存储位置已换成其他短信，未执行删除");return;}
    Packet req={0},resp={0};unsigned char storage=(unsigned char)st,mode=1,id[4];put32(id,idx);
    /* Vendor IDL: storage=0x01, index=0x10, optional tag=0x11,
     * message mode=0x12. Do not send a tag/category delete filter. */
    tlv(&req,1,&storage,1);tlv(&req,0x10,id,4);tlv(&req,0x12,&mode,1);
    if(!request(c,0x24,&req,&resp,2500)) {
        delete_result(0,transport_error?"删除结果未确认，请刷新短信列表，不要直接重复删除":failure,transport_error!=0);return;
    }
    int gone=0;
    for(unsigned attempt=0;attempt<3;attempt++) {
        gone=deleted_slot(c,st,idx,raw);
        if(gone!=0)break;
        if(attempt<2){const struct timespec pause={0,250000000};nanosleep(&pause,NULL);}
    }
    if(gone<0){delete_result(0,"删除请求已提交，但无法复查模块存储，请刷新列表确认",1);return;}
    if(!gone){delete_result(0,"模块仍保留这条短信，删除未成功；请刷新列表后重试",0);return;}
    if(!identity(sim)||strcmp(sim,expected_sim)) {
        delete_result(0,"删除后 SIM 状态已变化，无法确认结果，请刷新列表",1);return;
    }
    delete_result(1,NULL,0);
}
/* Only accept SMS-SUBMIT UCS2 point-to-point PDUs generated by our encoder.
 * Check lengths, address, segmentation UDH and surrogate boundaries before
 * passing data to WMS. No arbitrary QMI message or binary PDU API. */
static int submit_valid(const unsigned char *p,size_t n,unsigned seq,unsigned total,const unsigned char *first) {
    if(n<12||p[0]!=0||p[1]!=(total>1?0x41:1)||p[2]!=0||p[3]<1||p[3]>20||(p[4]!=0x81&&p[4]!=0x91))return 0;
    size_t addr=(p[3]+1)/2,at=5+addr;
    if(at+3>n||p[at]!=0||p[at+1]!=8||p[at+2]!=n-at-3||p[at+2]>140)return 0;
    for(size_t i=0;i<addr;i++){unsigned low=p[5+i]&15,high=p[5+i]>>4;if(low>9||(high>9&&!(i==addr-1&&p[3]%2&&high==15)))return 0;}
    if(p[3]%2 && (p[at-1]>>4)!=15)return 0;
    at+=3;
    if(total>1) {
        if(at+6>n||p[at]!=5||p[at+1]!=0||p[at+2]!=3||p[at+4]!=total||p[at+5]!=seq)return 0;
        if(first && (memcmp(p+3,first+3,2+addr)||p[at+3]!=first[at+3]))return 0;
        at+=6;
    }
    if(at==n||(n-at)%2)return 0;
    for(size_t i=at;i<n;i+=2) {
        unsigned u=(unsigned)p[i]*256+p[i+1];
        if(u>=0xd800&&u<=0xdbff){if(i+3>=n)return 0;unsigned next=(unsigned)p[i+2]*256+p[i+3];if(next<0xdc00||next>0xdfff)return 0;i+=2;}
        else if(u>=0xdc00&&u<=0xdfff)return 0;
    }
    return 1;
}
static int compatible_fstat(int fd,struct stat *st) {
#if defined(__arm__) && defined(__GLIBC__)
    /* Zig's compatibility shim passes version 0; the module's glibc 2.22
     * ARM ABI requires Linux stat version 3. Keep ownership checks intact.
     * glibc-2.22/sysdeps/unix/sysv/linux/bits/stat.h defines this ABI. */
    extern int __fxstat(int,int,struct stat *);
    _Static_assert(offsetof(struct stat,st_mode)==16,"ARM glibc stat layout");
    _Static_assert(offsetof(struct stat,st_uid)==24,"ARM glibc stat ownership layout");
    return __fxstat(3,fd,st);
#else
    return fstat(fd,st);
#endif
}
static int safe_open(const char *p,int flags) {
    int fd=open(p,flags|O_NOFOLLOW|O_CLOEXEC,0600);struct stat st;
    if(fd<0){if(debug)fprintf(stderr,"qdc-comm: safe open failed errno=%d\n",errno);return -1;}
    int result=compatible_fstat(fd,&st);
    if(result||!S_ISREG(st.st_mode)||st.st_uid!=geteuid()||(st.st_mode&077)){
        if(debug)fprintf(stderr,"qdc-comm: file check result=%d errno=%d\n",result,errno);
        close(fd);return -1;
    }return fd;
}
static int write_all(int fd,const char *s) {
    size_t n=strlen(s),i=0;
    while(i<n){ssize_t r=write(fd,s+i,n-i);if(r<0&&errno==EINTR)continue;if(r<=0)return 0;i+=(size_t)r;}return !fsync(fd);
}
/* Hangup must survive a short audio watchdog query holding the shared lock.
 * Wait for the owner to finish; never kill it or resend a modem command. */
static int operation_lock(int fd,int wait_for_hangup) {
    if(fd<0)return 0;
    struct timespec started,now;clock_gettime(CLOCK_MONOTONIC,&started);
    for(;;){
        if(!flock(fd,LOCK_EX|LOCK_NB))return 1;
        if(errno!=EWOULDBLOCK&&errno!=EAGAIN&&errno!=EINTR)return 0;
        if(!wait_for_hangup)return 0;
        clock_gettime(CLOCK_MONOTONIC,&now);
        if((now.tv_sec-started.tv_sec)*1000+(now.tv_nsec-started.tv_nsec)/1000000>=5000)return 0;
        struct timespec pause={0,20000000};nanosleep(&pause,NULL);
    }
}
static int begin_action(const char *kind,const char *id,const char *payload,char filename[640]) {
    if(!hex(id,32,32))return -1;
    if(snprintf(filename,640,"%s/action-%s-%s",private_dir,kind,id)>=640)return -1;
    int fd=safe_open(filename,O_RDONLY);
    if(fd>=0) {
        char buf[8192];ssize_t n=read(fd,buf,sizeof buf-1);close(fd);if(n<=0)return -1;buf[n]=0;
        char *newline=strchr(buf,'\n');if(!newline)return -1;*newline=0;
        if(strcmp(buf,payload)){error("重复请求编号对应不同内容，已拒绝操作");return 0;}
        fprintf(output,"%s\n",newline+1);return 0;
    }
    DIR *dir=opendir(private_dir);if(!dir)return -1;unsigned records=0;struct dirent *entry;
    while((entry=readdir(dir)))if(!strncmp(entry->d_name,"action-",7))records++;
    closedir(dir);
    if(records>=512){error("通讯请求记录已满，请先导出并清理请求记录");return 0;}
    fd=safe_open(filename,O_WRONLY|O_CREAT|O_EXCL);
    if(fd<0) {
        fd=safe_open(filename,O_RDONLY);if(fd<0)return -1;
        char buf[8192];ssize_t n=read(fd,buf,sizeof buf-1);close(fd);if(n<=0)return -1;buf[n]=0;
        char *newline=strchr(buf,'\n');if(!newline)return -1;*newline=0;
        if(strcmp(buf,payload)){error("重复请求编号对应不同内容，已拒绝操作");return 0;}
        fprintf(output,"%s\n",newline+1);return 0;
    }
    /* Keep at most 512 actions. Do not prune old IDs: eviction would allow an
       old retry to send twice. User can explicitly clear records when needed. */
    char record[8192];snprintf(record,sizeof record,"%s\n{\"ok\":false,\"uncertain\":true,\"error\":\"上次请求可能已执行，请核对后再决定是否重新操作\"}",payload);
    int ok=write_all(fd,record);close(fd);
    int dirfd=open(private_dir,O_RDONLY|O_DIRECTORY);if(dirfd<0||fsync(dirfd))ok=0;if(dirfd>=0)close(dirfd);
    return ok?1:-1;
}
static void finish_action(const char *filename,const char *payload,const char *result) {
    char tmp[704],record[8192];snprintf(tmp,sizeof tmp,"%s.tmp-%ld",filename,(long)getpid());
    snprintf(record,sizeof record,"%s\n%s",payload,result);
    int fd=safe_open(tmp,O_WRONLY|O_CREAT|O_EXCL);if(fd<0)return;
    int ok=write_all(fd,record);close(fd);if(ok)rename(tmp,filename);unlink(tmp);
}
static void sms_send(Client *c,const char *id,const char *pdus,const char *expected_sim) {
    if(strlen(pdus)>3072||!sim_identity(expected_sim)){error("短信发送参数无效");return;}
    char split[3073];strcpy(split,pdus);unsigned char parts[10][MAX_PDU];size_t sizes[10];unsigned count=0;
    char *save=NULL,*token=strtok_r(split,",",&save);
    while(token){if(count>=10||!(sizes[count]=decode(token,parts[count],MAX_PDU))){error("短信编码无效");return;}count++;token=strtok_r(NULL,",",&save);}
    if(!count||pdus[0]==','||pdus[strlen(pdus)-1]==','||strstr(pdus,",,")){error("短信编码无效");return;}
    for(unsigned i=0;i<count;i++)if(!submit_valid(parts[i],sizes[i],i+1,count,i?parts[0]:NULL)){error("短信编码或分段不完整");return;}
    char payload[3200],filename[640];snprintf(payload,sizeof payload,"%s:%s",expected_sim,pdus);
    int begin=begin_action("sms",id,payload,filename);
    if(begin<=0){if(begin<0)error("无法保存请求记录，未发送短信");return;}
    char sim[33];
    if(!identity(sim)||strcmp(sim,expected_sim)){const char *r="{\"ok\":false,\"error\":\"SIM 卡已变化或未就绪，未发送短信\"}";finish_action(filename,payload,r);fprintf(output,"%s\n",r);return;}
    unsigned sent=0;int uncertain=0;
    time_t started=time(NULL);
    for(unsigned i=0;i<count;i++) {
        if(time(NULL)-started>=22){uncertain=0;break;}
        Packet req={0},resp={0};unsigned char value[MAX_PDU+3];value[0]=6;put16(value+1,(unsigned)sizes[i]);memcpy(value+3,parts[i],sizes[i]);tlv(&req,1,value,sizes[i]+3);
        if(!request(c,0x20,&req,&resp,10000)){uncertain=transport_error!=0;break;}sent++;
    }
    char result[512];
    if(sent==count)snprintf(result,sizeof result,"{\"ok\":true,\"sentParts\":%u,\"totalParts\":%u}",sent,count);
    else snprintf(result,sizeof result,"{\"ok\":false,\"sentParts\":%u,\"totalParts\":%u,\"uncertain\":%s,\"qmiError\":%u,\"error\":\"%s\"}",sent,count,uncertain?"true":"false",protocol_error,uncertain?"发送结果未确认，请核对收件人；请勿直接重发":"部分或全部短信未发送，请核对发送结果");
    finish_action(filename,payload,result);fprintf(output,"%s\n",result);
}
static int calls(Client *c,Packet *resp) {Packet req={0};return request(c,0x2f,&req,resp,1500);}
static const char *call_state(unsigned s) {
    static const char *labels[]={"未知","拨号中","来电","通话中","连接中","对方振铃","保留","呼叫等待","正在挂断","已结束","来电准备","来电准备"};
    return s<sizeof labels/sizeof labels[0]?labels[s]:"未知";
}
static int call_list_to(Client *c,QdcHistoryCall *records,size_t *count) {
    if(count)*count=0;
    Packet resp={0};if(!calls(c,&resp)){error(failure);return 0;}
    size_t n=0;const unsigned char *p=find(&resp,0x10,&n);
    if(!p||n<1||n!=1+(size_t)p[0]*7){error("电话状态返回不完整");return 0;}
    size_t numbers_n=0;const unsigned char *numbers=find(&resp,0x11,&numbers_n);
    fprintf(output,"{\"ok\":true,\"audioAvailable\":false,\"calls\":[");
    for(unsigned i=0;i<p[0];i++) {
        const unsigned char *call=p+1+i*7;char num[81]="";
        if(numbers&&numbers_n>=1) {
            size_t off=1;
            for(unsigned k=0;k<numbers[0]&&off+3<=numbers_n;k++) {
                unsigned len=numbers[off+2];if(len>numbers_n-off-3)break;
                if(numbers[off]==call[0]&&len<sizeof num){memcpy(num,numbers+off+3,len);num[len]=0;}
                off+=3+len;
            }
        }
        if(records){records[i]=(QdcHistoryCall){.id=call[0],.state=call[1],.direction=call[3]};snprintf(records[i].number,sizeof records[i].number,"%s",num);}
        if(i)fputc(',',output);
        fprintf(output,"{\"id\":%u,\"state\":%u,\"stateText\":\"%s\",\"direction\":%u,\"number\":",call[0],call[1],call_state(call[1]),call[3]);string(num);fprintf(output,"}");
    }
    fprintf(output,"]}\n");if(count)*count=p[0];return 1;
}
static void call_list(Client *c){(void)call_list_to(c,NULL,NULL);}
/* Internal daemon query: deliberately not exposed as a CGI action. */
static void call_audio_check(Client *c) {
    Packet resp={0}; if(!calls(c,&resp)){error(failure);return;}
    size_t n=0;const unsigned char *p=find(&resp,0x10,&n);
    if(!p||n<1||n!=1+(size_t)p[0]*7){error("电话状态返回不完整");return;}
    unsigned active=0,id=0;uint64_t key=UINT64_C(14695981039346656037);
    size_t numbers_n=0;const unsigned char *numbers=find(&resp,0x11,&numbers_n);
    for(unsigned i=0;i<p[0];i++){const unsigned char *call=p+1+i*7;if(call[1]==3){
        active++;id=call[0];
        /* Stable call identity: id, type, direction and remote digits. Never logged. */
        for(unsigned k=0;k<4;k++){if(k==1)continue;key=(key^call[k])*UINT64_C(1099511628211);}
        if(numbers&&numbers_n>=1){size_t off=1;for(unsigned j=0;j<numbers[0]&&off+3<=numbers_n;j++){
            unsigned len=numbers[off+2];if(len>numbers_n-off-3)break;
            if(numbers[off]==id)for(unsigned k=0;k<len;k++)key=(key^numbers[off+3+k])*UINT64_C(1099511628211);
            off+=3+len;
        }}
    }}
    /* More than one conversation is ambiguous: do not bridge the wrong call. */
    fprintf(output,"{\"ok\":true,\"active\":%s,\"callId\":%u,\"callKey\":\"%016llx\"}\n",active==1?"true":"false",active==1?id:0,(unsigned long long)(active==1?key:0));
}
static void call_action(Client *c,const char *action,const char *value,const char *id) {
    int dial=!strcmp(action,"call-dial");
    if((dial&&!number(value))||(!dial&&(!digits(value,1,3)||atoi(value)>255||atoi(value)<1))){error("电话操作参数无效");return;}
    Packet state={0};if(!calls(c,&state)){error(failure);return;}
    size_t n=0;const unsigned char *p=find(&state,0x10,&n);
    if(!p||n<1||n!=1+(size_t)p[0]*7){error("无法确认当前电话状态");return;}
    int matched=0,active=0;
    for(unsigned i=0;i<p[0];i++) {
        const unsigned char *call=p+1+i*7;
        if(call[1]!=9)active=1;
        if(!dial&&call[0]==atoi(value)&&call[1]!=9&&
            (!strcmp(action,"call-end")||call[1]==2||call[1]==7))matched=1;
    }
    if(dial&&active){error("当前已有电话，未执行新的拨号");return;}
    if(!dial&&!matched){error("电话已变化，请刷新页面");return;}
    char filename[640],payload[96];snprintf(payload,sizeof payload,"%s:%s",action,value);
    if(dial){int begin=begin_action("dial",id,payload,filename);if(begin<=0){if(begin<0)error("无法保存请求记录，未拨号");return;}}
    Packet req={0},resp={0};unsigned char cid=(unsigned char)atoi(value);unsigned msg=dial?0x20:!strcmp(action,"call-answer")?0x22:0x21;
    tlv(&req,1,dial?(const void *)value:(const void *)&cid,dial?strlen(value):1);
    int ok=request(c,msg,&req,&resp,5000);
    char result[512];snprintf(result,sizeof result,ok?"{\"ok\":true}":"{\"ok\":false,\"uncertain\":true,\"error\":\"电话操作结果未确认，请刷新状态；请勿直接重复拨号\"}");
    if(dial)finish_action(filename,payload,result);fprintf(output,"%s\n",result);
}
#include "call-control.inc"
#include "watch.inc"
#include "esim.inc"
#include "sim-pin.inc"
#include "sim-recover.inc"
int main(int argc,char **argv) {
    umask(0077);alarm(35);
    int outfd=dup(STDOUT_FILENO),sink=open("/dev/null",O_RDWR);output=fdopen(outfd,"w");
    if(!output||sink<0)return 1;
    debug=getenv("QDC_COMM_DEBUG")&&!strcmp(getenv("QDC_COMM_DEBUG"),"1");
    dup2(sink,STDOUT_FILENO);if(!debug)dup2(sink,STDERR_FILENO);close(sink);
    TRACE("helper started");
    if(argc<2){error("缺少操作名称");return 0;}
    const char *root=getenv("QDC_WEB_ROOT"),*runtime=getenv("QDC_WEB_RUN");
    if(snprintf(private_dir,sizeof private_dir,"%s/private",root?root:"/usrdata/qdc-web")>=(int)sizeof private_dir||snprintf(run,sizeof run,"%s",runtime?runtime:"/run/qdc-web")>=(int)sizeof run){error("程序路径无效");return 0;}
    /* Re-exec the notifier before any QMI threads exist. It is internal only,
     * never exposed as a CGI action; avoid libc operations after a threaded fork. */
    if(!strcmp(argv[1],"notify-send")&&argc==3)return qdc_notify_send(argv[2])==1?0:1;
    if(!strcmp(argv[1],"watch")&&argc==2){if(!library())return 1;return watch_run(argv[0]);}
    if(!strcmp(argv[1],"call-end")){if(argc==4)control_client(argv[2],argv[3]);else error("操作参数无效");fclose(output);return 0;}
    /* Audio checks use the watcher's existing VOICE client. Do not load CCI
     * or acquire comm.lock while waiting for the local socket response. */
    if(!strcmp(argv[1],"call-audio-check")){if(argc==2)control_audio_client();else error("操作参数无效");fclose(output);return 0;}
    char lockname[640];snprintf(lockname,sizeof lockname,"%s/comm.lock",run);
    int lock=safe_open(lockname,O_RDWR|O_CREAT);
    if(!operation_lock(lock,!strcmp(argv[1],"call-end")&&argc==4)){if(lock>=0)close(lock);error("模块正在处理其他通讯操作，请稍后重试");return 0;}
    int sms=!strncmp(argv[1],"sms-",4),phone=!strncmp(argv[1],"call-",5);
    int radio_query=!strcmp(argv[1],"radio")&&argc==2;
    int pin_action=(!strcmp(argv[1],"sim-pin-state")||!strcmp(argv[1],"sim-pin-verify"))&&argc==2;
    int recover_action=!strcmp(argv[1],"sim-recover")&&argc==2;
    int esim_query_action=!strcmp(argv[1],"esim-list")&&argc==2;
    int roaming_query=(!strcmp(argv[1],"roaming-state")&&(argc==2||argc==3))||(!strcmp(argv[1],"roaming-config")&&argc==4);
    if(!sms&&!phone&&!radio_query&&!roaming_query&&!esim_query_action&&!pin_action&&!recover_action){error("操作不支持");return 0;}
    if(!library()){error(*failure?failure:"QMI 接口不可用");return 0;}
    if(radio_query){radio();close(lock);fclose(output);return 0;}
    if(roaming_query){roaming_state(argc>=3?argv[2]:NULL,argc==4?argv[3]:NULL);close(lock);fclose(output);return 0;}
    if(recover_action){sim_recover();close(lock);fclose(output);return 0;}
    if(pin_action){sim_pin(!strcmp(argv[1],"sim-pin-verify"));close(lock);fclose(output);return 0;}
    if(esim_query_action){esim_list();close(lock);fclose(output);return 0;}
    Client c={0};if(!connect_client(&c,sms?"wms_qmi_idl_service_object_v01":"voice_qmi_idl_service_object_v02")){error(failure);return 0;}
    if(!strcmp(argv[1],"sms-list")&&argc==3)sms_list(&c,argv[2]);
    else if(!strcmp(argv[1],"sms-delete")&&argc==6)sms_delete(&c,argv[2],argv[3],argv[4],argv[5]);
    else if(!strcmp(argv[1],"sms-send")&&argc==5)sms_send(&c,argv[2],argv[3],argv[4]);
    else if(!strcmp(argv[1],"call-list")&&argc==2)call_list(&c);
    else if(!strcmp(argv[1],"call-audio-check")&&argc==2)call_audio_check(&c);
    else if((!strcmp(argv[1],"call-dial")||!strcmp(argv[1],"call-answer")||!strcmp(argv[1],"call-end"))&&argc==4)call_action(&c,argv[1],argv[2],argv[3]);
    else error("操作参数无效");
    disconnect(&c);close(lock);fclose(output);return 0;
}
