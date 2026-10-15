/* Local simulator only: validate QMI/APDU layouts, segmentation and cleanup. */
#define qmi_client_send_raw_msg_sync base_send
#include "test_mock_qmi.c"
#undef qmi_client_send_raw_msg_sync
static unsigned char card_reply[8192];static size_t reply_n,reply_at;
static void esim_log(const char *name) {
    const char *path=getenv("QDC_ESIM_LOG");FILE *f=path?fopen(path,"a"):NULL;
    if(f){fprintf(f,"%s\n",name);fclose(f);}
}
static int fixture(const char *name) {
    const char *dir=getenv("QDC_ESIM_FIXTURES");char path[1024];
    if(!dir||snprintf(path,sizeof path,"%s/%s",dir,name)>=(int)sizeof path)return 0;
    FILE *f=fopen(path,"rb");if(!f)return 0;
    reply_n=fread(card_reply,1,sizeof card_reply,f);int ok=!ferror(f)&&feof(f);fclose(f);reply_at=0;return ok;
}
#ifndef ESIM_MOCK_SEND
#define ESIM_MOCK_SEND qmi_client_send_raw_msg_sync
#endif
int ESIM_MOCK_SEND(void *h,unsigned msg,void *req,unsigned req_n,void *resp,unsigned resp_n,unsigned *received,unsigned timeout) {
    if(h!=&uim_qmi_idl_service_object_v01||msg==0x2f)return base_send(h,msg,req,req_n,resp,resp_n,received,timeout);
    const char *scenario=getenv("QDC_TEST_SCENARIO");if(!scenario)scenario="normal";
    buffer=resp;at=0;unsigned char success[4]={0};tlv(2,success,4);
    unsigned len=0;const unsigned char *slot=field(req,req_n,1,&len);if(!slot||len!=1||*slot!=1)return 1;
    if(msg==0x42||msg==0x3f){
        const unsigned char *channel=field(req,req_n,0x11,&len);
        if(msg==0x3f&&channel){if(len!=1||*channel!=5)return 1;esim_log("close");
            if(!strcmp(scenario,"close-error"))protocol_error(94);
        }else{
            const unsigned char *aid=field(req,req_n,0x10,&len);if(!aid||len<2||aid[0]!=len-1)return 1;
            esim_log("open");
            if(!strcmp(scenario,"fallback")&&msg==0x42)protocol_error(71);
            else if(!strcmp(scenario,"open-transport"))return 5;
            else if(!strcmp(scenario,"unsupported")){unsigned char sw[]={0x6a,0x82};protocol_error(82);tlv(0x11,sw,2);}
            else {unsigned char id=5,sw[]={0x90,0};tlv(0x10,&id,1);tlv(0x11,sw,2);}
        }
    }else if(msg==0x3b){
        const unsigned char *channel=field(req,req_n,0x10,&len);if(!channel||len!=1||*channel!=5)return 1;
        const unsigned char *apdu=field(req,req_n,2,&len);
        if(!apdu||len<7||apdu[0]+256u*apdu[1]!=len-2||apdu[2]!=0xc1)return 1;
        if(!strcmp(scenario,"apdu-transport"))return 5;
        if(apdu[3]==0xe2){
            if(len<10||apdu[4]!=0x91||apdu[5]||apdu[6]!=len-7)return 1;
            static const unsigned char eid[]={0xbf,0x3e,3,0x5c,1,0x5a};
            static const unsigned char profiles[]={0xbf,0x2d,8,0x5c,6,0x5a,0x9f,0x70,0x90,0x91,0x92};
            static const unsigned char info[]={0xbf,0x22,0};const char *name=NULL;
            if(len-7==sizeof eid&&!memcmp(apdu+7,eid,sizeof eid)){name="eid";esim_log("eid");}
            else if(len-7==sizeof profiles&&!memcmp(apdu+7,profiles,sizeof profiles)){name="profiles";esim_log("profiles");}
            else if(len-7==sizeof info&&!memcmp(apdu+7,info,sizeof info)){name="info";esim_log("info");}
            if(!name||!fixture(name))return 1;
            if(!strcmp(name,"info")&&!strcmp(scenario,"info-unsupported")){unsigned char data[]={2,0,0x6d,0};tlv(0x10,data,4);*received=at;return 0;}
        }else if(apdu[3]==0xc0){if(len!=7||apdu[4]||apdu[5])return 1;esim_log("get-response");}
        else return 1;
        size_t bytes=reply_n-reply_at;if(bytes>60)bytes=60;
        unsigned char data[64];data[0]=(unsigned char)(bytes+2);data[1]=0;
        memcpy(data+2,card_reply+reply_at,bytes);reply_at+=bytes;
        data[2+bytes]=reply_at<reply_n?0x61:0x90;data[3+bytes]=reply_at<reply_n?60:0;
        tlv(0x10,data,(unsigned)bytes+4);
    }else return 1;
    *received=at;return 0;
}
