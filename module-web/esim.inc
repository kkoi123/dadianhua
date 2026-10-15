/* Read-only ES10c over the module's UIM CCI service. This exposes no raw APDU
 * endpoint. SELECT opens a private logical channel; only these three fixed
 * requests and GET RESPONSE may be sent. Always close before returning JSON.
 * QMI layouts: linux-mobile-broadband/libqmi data/qmi-service-uim.json.
 */
#include "esim-reader.h"
typedef struct {
    Client uim,voice; unsigned char slot,channel; unsigned open_message;
    uint64_t deadline; const char *error; int absent;
} EsimReader;
static uint64_t esim_ms(void) {
    struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t))return 0;
    return (uint64_t)t.tv_sec*1000+(unsigned long)t.tv_nsec/1000000;
}
static int esim_query(EsimReader *r,unsigned msg,Packet *req,Packet *resp) {
    if(!esim_ms()||esim_ms()>=r->deadline){r->error="读卡超时，请稍后重新读卡";return 0;}
    if(!request(&r->uim,msg,req,resp,1800)){
        r->error=transport_error?"模块卡片接口未及时返回，请稍后读卡":"模块卡片接口拒绝查询，可能不支持此卡片";return 0;}
    return 1;
}
static int esim_idle(EsimReader *r) {
    Packet resp={0};if(!calls(&r->voice,&resp)){r->error="无法确认电话是否空闲，请稍后读卡";return 0;}
    size_t n=0;const unsigned char *p=find(&resp,0x10,&n);
    if(!p||n!=1+(size_t)p[0]*7){r->error="电话状态不完整，暂不读卡";return 0;}
    for(unsigned i=0;i<p[0];i++)if(p[1+i*7+1]!=9){r->error="请在电话空闲时读卡";return 0;}
    return 1;
}
static int esim_card_slot(EsimReader *r) {
    Packet req={0},resp={0};Radio info={0};
    if(!esim_query(r,0x2f,&req,&resp))return 0;
    if(!card_status(&info,&resp)||strcmp(info.sim,"已就绪")){r->error="SIM 卡尚未就绪，请检查卡片或 PIN 状态";return 0;}
    size_t n=0;const unsigned char *p=find(&resp,0x10,&n);unsigned primary=u16(p);
    /* With no provisioned application, slot 0 was selected by card_status. */
    r->slot=(unsigned char)(primary==65535?1:(primary>>8)+1);return 1;
}
static int esim_close(EsimReader *r) {
    if(!r->channel)return 1;
    unsigned char channel=r->channel;Packet req={0},resp={0};
    tlv(&req,1,&r->slot,1);tlv(&req,0x11,&channel,1);
    /* Cleanup retains its own timeout, even after the read deadline expires. */
    int ok=request(&r->uim,0x3f,&req,&resp,1800);r->channel=0;
    if(!ok)r->error="卡片通道未确认关闭，请稍后读卡";
    return ok;
}
static int esim_open(EsimReader *r,const char *aid) {
    unsigned char bytes[33];size_t n=decode(aid,bytes+1,sizeof bytes-1);
    if(!n)return 0;bytes[0]=(unsigned char)n;
    Packet req={0},resp={0};tlv(&req,1,&r->slot,1);tlv(&req,0x10,bytes,n+1);
    int ok=esim_query(r,r->open_message,&req,&resp);
    /* Old UIM services may only provide the combined open/close message. */
    if(!ok&&!transport_error&&(protocol_error==71||protocol_error==94)&&r->open_message==0x42){
        r->open_message=0x3f;resp=(Packet){0};ok=esim_query(r,r->open_message,&req,&resp);
    }
    size_t cn=0,sn=0;const unsigned char *channel=find(&resp,0x10,&cn),*sw=find(&resp,0x11,&sn);
    if(channel&&cn==1&&*channel>=1&&*channel<=19)r->channel=*channel;
    size_t rn=0;const unsigned char *result=find(&resp,2,&rn);
    r->absent=!transport_error&&valid_packet(&resp)&&result&&rn==4&&sw&&sn==2&&sw[0]==0x6a&&sw[1]==0x82;
    if(!ok||!r->channel||(sw&&(sn!=2||(sw[0]!=0x61&&(sw[0]!=0x90||sw[1]))))){
        if(r->channel&&!esim_close(r))r->absent=0;
        if(!r->absent&&!r->error)r->error="未能打开卡片管理接口";
        return 0;
    }
    r->error=NULL;return 1;
}
static unsigned char esim_cla(unsigned char base,unsigned char channel) {
    return channel<4?(base&0x9c)|channel:(base&0xb0)|0x40|(channel-4);
}
static int esim_apdu(EsimReader *r,const unsigned char *b,size_t n,unsigned char *out,size_t *out_n) {
    Packet req={0},resp={0};unsigned char data[260];
    if(n<5||n>sizeof data-2||!r->channel){r->error="读卡指令无效";return 0;}
    put16(data,(unsigned)n);memcpy(data+2,b,n);
    tlv(&req,1,&r->slot,1);tlv(&req,2,data,n+2);tlv(&req,0x10,&r->channel,1);
    if(!esim_query(r,0x3b,&req,&resp))return 0;
    size_t bytes=0;const unsigned char *p=find(&resp,0x10,&bytes);
    if(!p||bytes<4||u16(p)!=bytes-2||bytes-2>*out_n){r->error="卡片响应不完整或过大";return 0;}
    memcpy(out,p+2,bytes-2);*out_n=bytes-2;return 1;
}
static int esim_read_command(EsimReader *r,const unsigned char *payload,size_t payload_n,unsigned char *out,size_t *out_n) {
    /* Fixed allowlist: EID, profile metadata, and eUICC capacity/firmware. */
    static const unsigned char eid[]={0xbf,0x3e,3,0x5c,1,0x5a};
    static const unsigned char profiles[]={0xbf,0x2d,8,0x5c,6,0x5a,0x9f,0x70,0x90,0x91,0x92};
    static const unsigned char info[]={0xbf,0x22,0};
    if(!((payload_n==sizeof eid&&!memcmp(payload,eid,payload_n))||
         (payload_n==sizeof profiles&&!memcmp(payload,profiles,payload_n))||
         (payload_n==sizeof info&&!memcmp(payload,info,payload_n))))return 0;
    unsigned char cmd[32]={esim_cla(0x80,r->channel),0xe2,0x91,0,(unsigned char)payload_n};
    memcpy(cmd+5,payload,payload_n);size_t cmd_n=5+payload_n,total=0,capacity=*out_n;
    for(unsigned iteration=0;iteration<64;iteration++){
        unsigned char response[1024];size_t n=sizeof response;
        if(!esim_apdu(r,cmd,cmd_n,response,&n))return 0;
        unsigned sw1=response[n-2],sw2=response[n-1];n-=2;
        if(sw1==0x6c&&cmd[1]==0xc0&&!n){cmd[4]=(unsigned char)sw2;continue;}
        if(sw1!=0x61&&(sw1!=0x90||sw2)){r->error="卡片未接受 eSIM 信息查询";return 0;}
        if(n>capacity-total){r->error="卡片配置列表过大，未显示部分结果";return 0;}
        memcpy(out+total,response,n);total+=n;
        if(sw1==0x90){*out_n=total;return 1;}
        cmd[0]=esim_cla(0x80,r->channel);cmd[1]=0xc0;cmd[2]=cmd[3]=0;cmd[4]=(unsigned char)sw2;cmd_n=5;
    }
    r->error="卡片返回过多分段，读卡已停止";return 0;
}
static void esim_list(void) {
    static const char *const aids[]={
        "a06573746b6d65ffff4953442d522030","a06573746b6d65ffff4953442d522031",
        "a0000005591010ffffffff8900000100","a0000005591010000000008900000300",
        "a0000005591010000000890000000300","a0000005591010ffffffff8900050500",
        "a0000005591010ffffffff8900000177","a000000559104c696e6b736669656c64",
        "a0000006281010ffffffff8900000100","a06573746b6d65ffffffff4953442d52"};
    EsimReader r={.open_message=0x42,.deadline=esim_ms()+27000};
    EsimCard card={0};unsigned char response[8192];int found_card=0,complete=0,unsupported=0;const char *selected=NULL;
    if(!connect_client(&r.voice,"voice_qmi_idl_service_object_v02")||!connect_client(&r.uim,"uim_qmi_idl_service_object_v01")){r.error="模块卡片接口不可用";goto done;}
    if(!esim_idle(&r)||!esim_card_slot(&r))goto done;
    for(unsigned i=0;i<sizeof aids/sizeof *aids;i++){
        if(!esim_idle(&r))goto done;
        if(!esim_open(&r,aids[i])){if(r.absent)continue;goto done;}
        static const unsigned char eid[]={0xbf,0x3e,3,0x5c,1,0x5a};size_t n=sizeof response;
        if(!esim_read_command(&r,eid,sizeof eid,response,&n)||!esim_eid(response,n,card.eid)){
            if(!r.error)r.error="卡片未返回有效 EID";goto done;
        }
        found_card=1;selected=aids[i];
        static const unsigned char profiles[]={0xbf,0x2d,8,0x5c,6,0x5a,0x9f,0x70,0x90,0x91,0x92};n=sizeof response;
        if(!esim_idle(&r)||!esim_read_command(&r,profiles,sizeof profiles,response,&n)||!esim_profiles(response,n,&card)){
            if(!r.error)r.error="卡片配置列表不完整，请重新读卡";goto done;
        }
        static const unsigned char info[]={0xbf,0x22,0};n=sizeof response;
        /* Capacity is optional. Keep valid EID/profiles if unsupported. */
        if(esim_read_command(&r,info,sizeof info,response,&n)){
            if(!esim_info(response,n,&card)){*card.firmware=0;card.has_free=0;}
        }else if(transport_error||esim_ms()>=r.deadline)goto done;
        r.error=NULL;complete=1;break;
    }
    if(!found_card)unsupported=1;
done:
    if(r.channel&&!esim_close(&r))complete=0;
    disconnect(&r.uim);disconnect(&r.voice);
    if(!complete){
        if(unsupported){fputs("{\"ok\":true,\"state\":\"unsupported\",\"cards\":[],\"message\":\"未发现受支持的 eUICC 管理接口。可能是普通 SIM，或卡片型号暂未支持。\"}\n",output);return;}
        error(r.error?r.error:"卡片读取失败，请稍后读卡");return;
    }
    fputs("{\"ok\":true,\"state\":\"ready\",\"cards\":[{\"aid\":",output);string(selected);
    fputs(",\"eid\":",output);string(card.eid);fputs(",\"firmware\":",output);string(card.firmware);
    fputs(",\"freeBytes\":",output);if(card.has_free)fprintf(output,"%u",card.free_bytes);else fputs("null",output);
    fputs(",\"profiles\":[",output);
    for(unsigned i=0;i<card.count;i++){EsimProfile *p=&card.profiles[i];if(i)fputc(',',output);
        fputs("{\"iccid\":",output);string(p->iccid);fputs(",\"name\":",output);string(p->name);
        fputs(",\"provider\":",output);string(p->provider);fprintf(output,",\"enabled\":%s}",p->enabled?"true":"false");}
    fputs("]}]}\n",output);
}
