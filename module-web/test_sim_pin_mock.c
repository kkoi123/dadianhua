#define qmi_client_send_raw_msg_sync pin_base_send
#include "test_mock_qmi.c"
#undef qmi_client_send_raw_msg_sync
static int recovery_on=0;
int qmi_client_send_raw_msg_sync(void *h,unsigned msg,void *req,unsigned req_n,void *resp,unsigned resp_n,unsigned *received,unsigned timeout) {
    if(h==&voice_qmi_idl_service_object_v02&&msg==0x2f&&getenv("QDC_TEST_SCENARIO")&&!strcmp(getenv("QDC_TEST_SCENARIO"),"recover-call")){
        buffer=resp;at=0;unsigned char result[4]={0},active[]={1,4,3,0,2,4,0,0};tlv(2,result,4);tlv(0x10,active,sizeof active);*received=at;return 0;
    }
    if(h!=&uim_qmi_idl_service_object_v01)return pin_base_send(h,msg,req,req_n,resp,resp_n,received,timeout);
    const char *s=getenv("QDC_TEST_SCENARIO");if(!s)s="pin";
    buffer=resp;at=0;unsigned char success[4]={0};tlv(2,success,4);
    if(msg==0x2f){
        unsigned char p[]={0,0,255,255,255,255,255,255,1,1,3,3,10,0,1,2,2,0,0,0,0,0,0,1,3,10,3,3,10};
        if(!strncmp(s,"recover-",8)){
            if(recovery_on&&!strcmp(s,"recover-ok")){p[16]=7;p[23]=2;}
            else {p[9]=2;p[14]=0;tlv(0x10,p,15);*received=at;return 0;}
        }
        if(!strcmp(s,"ordinary")){p[0]=p[1]=255;p[15]=1;}
        if(!strcmp(s,"puk-required")){p[16]=5;p[23]=1;p[24]=0;}
        if(!strcmp(s,"absent")||!strcmp(s,"card-error")||!strcmp(s,"initializing")){p[9]=!strcmp(s,"absent")?0:!strcmp(s,"card-error")?2:1;p[14]=0;tlv(0x10,p,15);*received=at;return 0;}
        if(!strcmp(s,"ready")){p[16]=7;p[23]=2;}
        if(!strcmp(s,"blocked")){p[16]=3;p[23]=4;p[24]=0;}
        if(!strcmp(s,"upin")){p[10]=1;p[22]=1;}
        if(!strcmp(s,"nonprov")){p[0]=p[1]=255;unsigned char full[31];memcpy(full,p,22);full[21]=2;full[22]=0xa0;full[23]=0;memcpy(full+24,p+22,7);tlv(0x10,full,31);*received=at;return 0;}
        tlv(0x10,p,!strcmp(s,"truncated")?25:sizeof p);
    }else if(msg==0x30||msg==0x31){
        unsigned n=0;const unsigned char *slot=field(req,req_n,1,&n);if(n!=1||!slot||slot[0]!=1)return 1;
        const char *log=getenv("QDC_TEST_LOG");FILE *f=log?fopen(log,"a"):NULL;if(f){fprintf(f,"power-%s\n",msg==0x30?"off":"on");fclose(f);}
        if(msg==0x30&&!strcmp(s,"recover-off-rejected"))protocol_error(94);
        if(msg==0x30&&!strcmp(s,"recover-off-timeout"))return 5;
        if(msg==0x31&&!strcmp(s,"recover-on-timeout"))return 5;
        if(msg==0x31)recovery_on=1;
    }else if(msg==0x26){
        unsigned len=0;const unsigned char *session=field(req,req_n,1,&len);
        unsigned char primary[]={0,0},ordinary[]={6,0},nonprov[]={4,2,0xa0,0};
        const unsigned char *expected=!strcmp(s,"nonprov")?nonprov:!strcmp(s,"ordinary")?ordinary:primary;unsigned expected_n=!strcmp(s,"nonprov")?4:2;
        if(!session||len!=expected_n||memcmp(session,expected,len))return 1;
        const unsigned char *pin=field(req,req_n,2,&len);
        if(!pin||len!=6||pin[0]!=(strcmp(s,"upin")?1:3)||pin[1]!=4||memcmp(pin+2,"1234",4))return 1;
        const char *log=getenv("QDC_TEST_LOG");FILE *f=log?fopen(log,"a"):NULL;if(f){fputs("verify-once\n",f);fclose(f);}
        if(!strcmp(s,"timeout"))return 5;
        if(!strcmp(s,"malformed-reply")){*received=2;return 0;}
        if(!strcmp(s,"wrong")){protocol_error(12);unsigned char retries[]={2,10};tlv(0x10,retries,2);}
    }else return 1;
    *received=at;return 0;
}
