/* Native-only protocol simulator: no device access and no shipped code. */
#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
unsigned wms_qmi_idl_service_object_v01, voice_qmi_idl_service_object_v02, dms_qmi_idl_service_object_v01, uim_qmi_idl_service_object_v01;
unsigned nas_qmi_idl_service_object_v01, wds_qmi_idl_service_object_v01;
typedef struct {unsigned info[5];} Info;
int qmi_client_get_any_service(void *object,Info *info){(void)object;memset(info,0,sizeof *info);return 0;}
#ifdef WATCH_MOCK
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>
typedef void (*Indication)(void *,unsigned,void *,unsigned,void *);
typedef struct { void *object,*data;Indication callback;atomic_int live;pthread_t thread; } MockWatch;
static MockWatch watchers[2];
static void mode(char *b,size_t capacity){const char *path=getenv("QDC_WATCH_TEST_STATE");FILE *f=path?fopen(path,"r"):NULL;snprintf(b,capacity,"normal");if(f){if(fgets(b,(int)capacity,f))b[strcspn(b,"\r\n")]=0;fclose(f);}}
static void *watch_thread(void *arg){MockWatch *w=arg;char previous[64]="",current[64];while(atomic_load(&w->live)){mode(current,sizeof current);if(strcmp(previous,current)){strcpy(previous,current);w->callback(w->object,w->object==&voice_qmi_idl_service_object_v02?0x2e:1,NULL,0,w->data);}struct timespec p={0,20000000};nanosleep(&p,NULL);}return NULL;}
static int mock_init(Info *info,void *object,void *callback,void *data,void *os,void **handle){(void)info;(void)os;*handle=object;if(callback){unsigned index=object==&voice_qmi_idl_service_object_v02?0:1;MockWatch *w=&watchers[index];w->object=object;w->data=data;w->callback=(Indication)callback;atomic_store(&w->live,1);if(pthread_create(&w->thread,NULL,watch_thread,w))return 1;}return 0;}
static int mock_release(void *handle){for(unsigned i=0;i<2;i++)if(watchers[i].object==handle&&atomic_exchange(&watchers[i].live,0))pthread_join(watchers[i].thread,NULL);return 0;}
#endif
int qmi_client_init(Info *info,void *object,void *callback,void *data,void *os,void **handle){
#ifdef WATCH_MOCK
return mock_init(info,object,callback,data,os,handle);
#endif
(void)info;(void)callback;(void)data;(void)os;*handle=object;return 0;}
int qmi_client_release(void *handle){
#ifdef WATCH_MOCK
return mock_release(handle);
#else
(void)handle;return 0;
#endif
}
static unsigned at;
static int deleted_storage=-1, delete_sent;
static unsigned char roaming_disallowed;
static unsigned char *buffer;
static void tlv(unsigned type,const void *p,unsigned n){buffer[at++]=(unsigned char)type;buffer[at++]=(unsigned char)n;buffer[at++]=(unsigned char)(n>>8);memcpy(buffer+at,p,n);at+=n;}
static const unsigned char *field(const void *req,unsigned n,unsigned type,unsigned *len){
 const unsigned char *p=req;for(unsigned i=0;i+3<=n;){unsigned size=p[i+1]+256u*p[i+2];if(size>n-i-3)return NULL;if(p[i]==type){*len=size;return p+i+3;}i+=3+size;}return NULL;
}
static void protocol_error(unsigned code){buffer[3]=1;buffer[5]=(unsigned char)code;buffer[6]=(unsigned char)(code>>8);}
int qmi_client_send_raw_msg_sync(void *h,unsigned msg,void *req,unsigned req_n,void *resp,unsigned resp_n,unsigned *received,unsigned timeout){
 (void)resp_n;(void)timeout;buffer=resp;at=0;
 const char *scenario=getenv("QDC_TEST_SCENARIO");if(!scenario)scenario="normal";
#ifdef WATCH_MOCK
char live_mode[64];mode(live_mode,sizeof live_mode);scenario=live_mode;
#endif
 unsigned char success[4]={0};tlv(2,success,4);
 if(h==&nas_qmi_idl_service_object_v01){
   if(req_n)return 1;
   if(msg==0x24){unsigned char state[]={1,1,1,2,1,8},plmn[]={0xcc,1,0,0,4,'C','M','C','C'};
     if(!strcmp(scenario,"radio-searching"))state[0]=2;
     if(!strcmp(scenario,"radio-truncated"))state[4]=9;
     tlv(1,state,sizeof state);tlv(0x12,plmn,sizeof plmn);
     if(strcmp(scenario,"radio-roaming-missing")){
       unsigned char roaming=!strcmp(scenario,"radio-roaming")?0:!strcmp(scenario,"radio-roaming-invalid")?2:1;
       tlv(0x10,&roaming,1);
     }
   }else if(msg==0x20){unsigned char signal[]={162,8};tlv(1,signal,2);}else return 1;
 }
 else if(h==&uim_qmi_idl_service_object_v01&&msg==0x2f){
   if(req_n)return 1;
   unsigned char cards[]={0,0,255,255,255,255,255,255,1,1,3,3,10,0,1,2,7,0,0,0,0,0,0,3,3,10,3,3,10};
   if(!strcmp(scenario,"radio-no-sim")){cards[9]=0;cards[14]=0;tlv(0x10,cards,15);}
   else{if(!strcmp(scenario,"radio-pin")){cards[16]=2;cards[23]=1;}
     if(!strcmp(scenario,"radio-primary-unset")){cards[0]=255;cards[1]=255;}
     tlv(0x10,cards,!strcmp(scenario,"radio-truncated")?25:sizeof cards);}
 }
 else if(h==&wds_qmi_idl_service_object_v01){
   unsigned len=0;
   if(msg==0x2b||msg==0x28){
     const unsigned char *id=field(req,req_n,1,&len);if(!id||len!=2||id[0]||id[1]!=1)return 1;
     if(!strcmp(scenario,"roaming-unsupported")){protocol_error(94);}
     else if(msg==0x2b){if(strcmp(scenario,"roaming-flag-missing"))tlv(0x3e,&roaming_disallowed,1);}
     else {
       const unsigned char *flag=field(req,req_n,0x3e,&len);if(!flag||len!=1||*flag>1||req_n!=9)return 1;
       if(!strcmp(scenario,"roaming-write-error")){protocol_error(94);}
       else if(!strcmp(scenario,"roaming-write-timeout"))return 5;
       else if(strcmp(scenario,"roaming-write-ignored"))roaming_disallowed=*flag;
       const char *log=getenv("QDC_TEST_LOG");if(log){FILE *f=fopen(log,"a");if(f){fputs("wds:28\n",f);fclose(f);}}
     }
   }else if(msg==0x2d){const unsigned char *mask=field(req,req_n,0x10,&len);if(!mask||len!=4||mask[0]!=8||mask[1]!=1||mask[2]||mask[3])return 1;
     if(!strcmp(scenario,"radio-default"))protocol_error(15);
     else if(!strcmp(scenario,"radio-failure"))return 5;
     else{const char *apn="cmnet";unsigned char ip[]={206,20,203,10};tlv(0x14,apn,5);tlv(0x1e,ip,4);}
   }else if(msg==0x2c){const unsigned char *type=field(req,req_n,1,&len);if(!type||len!=1||*type)return 1;
     if(!strcmp(scenario,"radio-failure"))return 5;tlv(0x14,"cmhk",4);
   }else return 1;
 }
 else if(h==&dms_qmi_idl_service_object_v01){if(!strncmp(scenario,"uim",3))protocol_error(94);else{const char *sim=!strcmp(scenario,"swap")?"89860011111111111111":"89860000000000000000";tlv(1,sim,(unsigned)strlen(sim));}}
 else if(h==&uim_qmi_idl_service_object_v01 && msg==0x20){
   unsigned len=0;const unsigned char *session=field(req,req_n,1,&len);
   if(!session||len!=2||session[0]!=0||session[1]!=0)return 1;
   const unsigned char expected[]={0xe2,0x2f,2,0,0x3f};const unsigned char *file=field(req,req_n,2,&len);if(!file||len!=sizeof expected||memcmp(file,expected,len))return 1;
   const unsigned char *read=field(req,req_n,3,&len);if(!read||len!=4||read[0]||read[1]||read[2]!=10||read[3])return 1;
   unsigned char data[]={10,0,0x98,0x68,0,0xa0,0,0,0,0,0,0};
   if(!strcmp(scenario,"uim-swap"))data[11]=1;
   if(!strcmp(scenario,"uim-invalid"))data[2]=0xff;
   tlv(0x11,data,sizeof data);
 }
 else if(h==&wms_qmi_idl_service_object_v01 && msg==0x31){
   unsigned len=0;const unsigned char *mode=field(req,req_n,0x11,&len);if(!mode||len!=1||*mode!=1)return 1;
   const unsigned char *storage=field(req,req_n,1,&len);if(!storage||len!=1||*storage>1)return 1;
   if(delete_sent&&!strcmp(scenario,"delete-verify-error"))return 5;
   unsigned char list[9]={1,0,0,0,7,0,0,0,1};
   if(deleted_storage==*storage||!strcmp(scenario,"delete-already-gone")){list[0]=0;tlv(1,list,4);}else tlv(1,list,9);
 }
 else if(h==&wms_qmi_idl_service_object_v01 && msg==0x22){
   if(!strcmp(scenario,"delete-already-gone"))protocol_error(50);
   else{unsigned char raw[]={1,6,19,0,0,1,0,13,0x91,0x68,0x31,0x08,0x10,0x83,0,0xf0,0,8,4,0x4f,0x60,0x59,0x7d};
     if(!strcmp(scenario,"reused")||(delete_sent&&!strcmp(scenario,"delete-reused")))raw[sizeof raw-1]=0x7e;tlv(1,raw,sizeof raw);}
 }
 else if(h==&voice_qmi_idl_service_object_v02 && msg==0x2f){if(!strcmp(scenario,"call-check-failure"))return 5;unsigned char idle[]={0};unsigned char incoming[]={1,4,2,0,2,4,0,0};unsigned char active[]={1,4,3,0,2,4,0,0};tlv(0x10,!strcmp(scenario,"incoming")?incoming:!strcmp(scenario,"conversation")?active:idle,!strcmp(scenario,"incoming")?sizeof incoming:!strcmp(scenario,"conversation")?sizeof active:sizeof idle);}
 else {
   if(h==&wms_qmi_idl_service_object_v01&&msg==0x24){
     unsigned len=0;const unsigned char *mode=field(req,req_n,0x12,&len);if(!mode||len!=1||*mode!=1||field(req,req_n,0x11,&len))return 1;
     const unsigned char *index=field(req,req_n,0x10,&len);if(!index||len!=4||index[0]!=7||index[1]||index[2]||index[3])return 1;
     const unsigned char *storage=field(req,req_n,1,&len);if(!storage||len!=1||*storage>1)return 1;
     delete_sent=1;
     if(strcmp(scenario,"delete-still-present")&&strcmp(scenario,"delete-verify-error")&&strcmp(scenario,"delete-reused"))deleted_storage=*storage;
   }
   const char *log=getenv("QDC_TEST_LOG");FILE *f=log?fopen(log,"a"):NULL;if(f){fprintf(f,"%s:%x\n",h==&wms_qmi_idl_service_object_v01?"sms":"voice",msg);fclose(f);}
#ifdef WATCH_MOCK
   if(h==&voice_qmi_idl_service_object_v02&&msg==0x21){
     unsigned len=0;const unsigned char *id=field(req,req_n,1,&len);if(!id||len!=1||*id!=4)return 1;
     if(getenv("QDC_HANGUP_TEST_TIMEOUT"))return 5;
     const char *path=getenv("QDC_WATCH_TEST_STATE");FILE *state=path?fopen(path,"w"):NULL;if(state){fputs("normal\n",state);fclose(state);}
   }
#endif
   if(!strcmp(scenario,"timeout"))return 5;
 }
 *received=at;return 0;
}
