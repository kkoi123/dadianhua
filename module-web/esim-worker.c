/* Module LPA worker. libeuicc implements GSMA ES9+/ES10; the host owns QMI,
 * bounded HTTPS, card identity checks, and private progress records.
 * No raw APDU or shell-command web interface. No activation codes on disk.
 */
#define main qdc_unused_comm_main
#include "comm.c"
#undef main
#include <arpa/inet.h>
#include <netdb.h>
#include <spawn.h>
#include <ctype.h>
#include <euicc/euicc.h>
#include <euicc/es10c.h>
#include <euicc/es9p.h>
#include <cjson/cJSON.h>

extern char **environ;
#define ESIM_HTTP_LIMIT (1024u*1024u)
typedef struct {
    char action[24],aid[33],eid[33],iccid[23],name[65];
    char activation[1025],confirmation[129],imei[16];
} EsimOperation;
static EsimReader reader;
static char job_id[65];
static const char *stage="checking";
static int mutation_started, channel_close_failed;
static uint64_t last_idle;

static int allowed_aid(const char *s) {
    static const char *const ids[]={"a06573746b6d65ffff4953442d522030","a06573746b6d65ffff4953442d522031",
        "a0000005591010ffffffff8900000100","a0000005591010000000008900000300",
        "a0000005591010000000890000000300","a0000005591010ffffffff8900050500",
        "a0000005591010ffffffff8900000177","a000000559104c696e6b736669656c64",
        "a0000006281010ffffffff8900000100","a06573746b6d65ffffffff4953442d52"};
    for(unsigned i=0;i<sizeof ids/sizeof *ids;i++)if(!strcmp(s,ids[i]))return 1;
    return 0;
}
static int operation_name(const char *s) {
    return !strcmp(s,"enable")||!strcmp(s,"disable")||!strcmp(s,"rename")||
        !strcmp(s,"delete")||!strcmp(s,"download")||!strcmp(s,"notifications");
}
static int plain(const char *s) {
    for(const unsigned char *p=(const unsigned char *)s;*p;p++)if(*p<32||*p==127)return 0;
    return 1;
}
static int read_line(char *out,size_t cap) {
    size_t n=0;int c;while((c=getchar())!=EOF&&c!='\n'){
        if(!c||n+1>=cap)return 0;out[n++]=(char)c;
    }
    out[n]=0;return c=='\n'&&plain(out);
}
static int read_operation(EsimOperation *o) {
    return read_line(o->action,sizeof o->action)&&read_line(o->aid,sizeof o->aid)&&
        read_line(o->eid,sizeof o->eid)&&read_line(o->iccid,sizeof o->iccid)&&
        read_line(o->name,sizeof o->name)&&read_line(o->activation,sizeof o->activation)&&
        read_line(o->confirmation,sizeof o->confirmation)&&read_line(o->imei,sizeof o->imei)&&
        getchar()==EOF&&operation_name(o->action)&&allowed_aid(o->aid)&&digits(o->eid,32,32);
}
static void job_record(const char *state,const char *message,int uncertain,int refresh) {
    if(!*job_id)return;
    char dest[640],temp[680];snprintf(dest,sizeof dest,"%s/esim-result",run);
    snprintf(temp,sizeof temp,"%s.tmp.%ld",dest,(long)getpid());
    int fd=open(temp,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);if(fd<0)return;
    FILE *f=fdopen(fd,"w");if(!f){close(fd);unlink(temp);return;}
    FILE *saved=output;output=f;
    fputs("{\"ok\":true,\"jobId\":",f);string(job_id);fputs(",\"state\":",f);string(state);
    fputs(",\"stage\":",f);string(stage);fputs(",\"message\":",f);string(message);
    fprintf(f,",\"pid\":%ld,\"uncertain\":%s,\"refreshRequired\":%s}\n",(long)getpid(),uncertain?"true":"false",refresh?"true":"false");
    output=saved;int ok=!fflush(f)&&!fsync(fd);if(fclose(f))ok=0;
    if(ok)rename(temp,dest);else unlink(temp);
}
static void progress(const char *name,const char *message) {stage=name;job_record("running",message,0,0);}
static int idle_checkpoint(void) {
    if(esim_ms()>=reader.deadline){reader.error="eSIM 操作超时，请重新读卡核对状态";return 0;}
    if(esim_ms()-last_idle<1000)return 1;
    last_idle=esim_ms();return esim_idle(&reader);
}
static int card_connect(struct euicc_ctx *ctx) {(void)ctx;return 0;}
static void card_disconnect(struct euicc_ctx *ctx) {(void)ctx;}
static int card_open(struct euicc_ctx *ctx,const uint8_t *aid,uint8_t len) {
    (void)ctx;char text[33];if(len>16)return -1;encode(aid,len,text);
    if(!allowed_aid(text)||!esim_open(&reader,text))return -1;
    return reader.channel;
}
static void card_close(struct euicc_ctx *ctx,uint8_t channel) {
    (void)ctx;if(channel!=reader.channel||!esim_close(&reader))channel_close_failed=1;
}
static int card_transmit(struct euicc_ctx *ctx,uint8_t **rx,uint32_t *rx_len,const uint8_t *tx,uint32_t tx_len) {
    (void)ctx;*rx=NULL;*rx_len=0;
    if(tx_len<5||tx_len>260||(tx[1]!=0xe2&&tx[1]!=0xc0)||!idle_checkpoint())return -1;
    /* libeuicc uses a basic channel nibble; apply full ISO channel encoding. */
    unsigned char cmd[260],response[1024];memcpy(cmd,tx,tx_len);cmd[0]=esim_cla(0x80,reader.channel);
    size_t n=sizeof response;if(!esim_apdu(&reader,cmd,tx_len,response,&n))return -1;
    *rx=malloc(n);if(!*rx)return -1;memcpy(*rx,response,n);*rx_len=(uint32_t)n;return 0;
}

/* SM-DP+ destinations are HTTPS on port 443, public IPv4, DNS pinned per
 * request. No redirects, proxies, credentials in URLs, or LAN destinations. */
static int public_ipv4(uint32_t ip) {
    unsigned a=ip>>24,b=(ip>>16)&255;
    return a&&a!=10&&a!=127&&a<224&&!(a==169&&b==254)&&
        !(a==172&&b>=16&&b<=31)&&!(a==192&&b==168)&&!(a==100&&b>=64&&b<=127)&&
        !(a==198&&(b==18||b==19))&&!(a==192&&b==0)&&!(a==198&&b==51)&&!(a==203&&b==0);
}
static int hostname(const char *s) {
    size_t n=strlen(s);if(n<3||n>253||!strchr(s,'.'))return 0;
    size_t label=0;for(size_t i=0;i<n;i++){
        unsigned c=(unsigned char)s[i];if(c=='.'){if(!label||label>63||s[i-1]=='-')return 0;label=0;continue;}
        if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-')||(!label&&c=='-'))return 0;
        label++;
    }
    return label&&label<=63&&s[n-1]!='-';
}
static int http_destination(const char *url,char host[254],char resolved[320]) {
    if(strncmp(url,"https://",8))return 0;
    const char *slash=strchr(url+8,'/');if(!slash||slash-(url+8)>253)return 0;
    memcpy(host,url+8,(size_t)(slash-(url+8)));host[slash-(url+8)]=0;
    if(!hostname(host)||strchr(slash,'?')||strchr(slash,'#'))return 0;
    static const char *const paths[]={"/gsma/rsp2/es9plus/initiateAuthentication",
        "/gsma/rsp2/es9plus/authenticateClient","/gsma/rsp2/es9plus/getBoundProfilePackage",
        "/gsma/rsp2/es9plus/cancelSession","/gsma/rsp2/es9plus/handleNotification"};
    int allowed=0;for(unsigned i=0;i<sizeof paths/sizeof *paths;i++)if(!strcmp(paths[i],slash))allowed=1;
    if(!allowed)return 0;
    struct addrinfo hints={0},*list=NULL;hints.ai_family=AF_INET;hints.ai_socktype=SOCK_STREAM;
    if(getaddrinfo(host,"443",&hints,&list))return 0;
    int valid=1;char address[INET_ADDRSTRLEN]={0};
    for(struct addrinfo *p=list;p;p=p->ai_next){struct sockaddr_in *a=(struct sockaddr_in *)p->ai_addr;
        if(!public_ipv4(ntohl(a->sin_addr.s_addr))){valid=0;break;}
        if(!*address&&!inet_ntop(AF_INET,&a->sin_addr,address,sizeof address))valid=0;
    }
    freeaddrinfo(list);if(!valid||!*address)return 0;
    snprintf(resolved,320,"%s:443:%s",host,address);return 1;
}
static int http_transmit(struct euicc_ctx *ctx,const char *url,uint32_t *rcode,uint8_t **rx,uint32_t *rx_len,
    const uint8_t *tx,uint32_t tx_len,const char **headers) {
    (void)ctx;(void)headers;*rx=NULL;*rx_len=0;*rcode=0;
    char host[254],resolved[320];
    if(tx_len>ESIM_HTTP_LIMIT||!idle_checkpoint()||!http_destination(url,host,resolved))return -1;
    int in[2],out[2];if(pipe(in))return -1;if(pipe(out)){close(in[0]);close(in[1]);return -1;}
    posix_spawn_file_actions_t acts;posix_spawn_file_actions_init(&acts);
    posix_spawn_file_actions_adddup2(&acts,in[0],STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&acts,out[1],STDOUT_FILENO);
    posix_spawn_file_actions_addopen(&acts,STDERR_FILENO,"/dev/null",O_WRONLY,0600);
    posix_spawn_file_actions_addclose(&acts,in[0]);posix_spawn_file_actions_addclose(&acts,in[1]);
    posix_spawn_file_actions_addclose(&acts,out[0]);posix_spawn_file_actions_addclose(&acts,out[1]);
    /* The curl child must not retain the web operation lock. */
    if(in[0]!=3&&in[1]!=3&&out[0]!=3&&out[1]!=3)posix_spawn_file_actions_addclose(&acts,3);
    char *argv[]={"curl","--disable","--noproxy","*","--silent","--show-error","--proto","=https",
        "--proto-redir","=https","--connect-timeout","12","--max-time","45",
        "--resolve",resolved,"--request","POST","--header","Content-Type: application/json",
        "--header","User-Agent: gsma-rsp-lpad","--header","X-Admin-Protocol: gsma/rsp/v2.2.2",
        "--data-binary","@-","--write-out","\n%{http_code}",(char *)url,NULL};
    pid_t pid;int spawn=posix_spawn(&pid,"/usr/bin/curl",&acts,NULL,argv,environ);posix_spawn_file_actions_destroy(&acts);
    close(in[0]);close(out[1]);if(spawn){close(in[1]);close(out[0]);return -1;}
    fcntl(in[1],F_SETFL,O_NONBLOCK);fcntl(out[0],F_SETFL,O_NONBLOCK);
    unsigned char *buf=NULL;size_t used=0,capacity=0,sent=0;int eof=0,ok=1;
    uint64_t deadline=esim_ms()+50000;
    while(!eof&&esim_ms()<deadline&&idle_checkpoint()){
        if(in[1]>=0&&sent==tx_len){close(in[1]);in[1]=-1;}
        struct pollfd fds[2]={{out[0],POLLIN,0},{in[1],POLLOUT,0}};
        int ready=poll(fds,2,100);if(ready<0){if(errno==EINTR)continue;ok=0;break;}
        if(in[1]>=0&&(fds[1].revents&(POLLOUT|POLLERR|POLLHUP))){
            ssize_t n=write(in[1],tx+sent,tx_len-sent);if(n>0)sent+=(size_t)n;
            else if(n<0&&errno!=EAGAIN&&errno!=EINTR){ok=0;break;}
        }
        if(fds[0].revents&(POLLIN|POLLHUP|POLLERR)){
            unsigned char bytes[8192];ssize_t n=read(out[0],bytes,sizeof bytes);
            if(n==0){eof=1;break;}if(n<0){if(errno==EAGAIN||errno==EINTR)continue;ok=0;break;}
            if(used+(size_t)n>ESIM_HTTP_LIMIT+4){ok=0;break;}
            if(used+(size_t)n+1>capacity){size_t size=used+(size_t)n+8192;unsigned char *next=realloc(buf,size);if(!next){ok=0;break;}buf=next;capacity=size;}
            memcpy(buf+used,bytes,(size_t)n);used+=(size_t)n;
        }
    }
    if(in[1]>=0)close(in[1]);close(out[0]);if(!eof||!ok)kill(pid,SIGKILL);
    int status=0;while(waitpid(pid,&status,0)<0&&errno==EINTR){}
    if(!ok||!eof||!WIFEXITED(status)||WEXITSTATUS(status)||used<4||buf[used-4]!='\n'||
        buf[used-3]<'0'||buf[used-3]>'9'||buf[used-2]<'0'||buf[used-2]>'9'||buf[used-1]<'0'||buf[used-1]>'9')goto fail;
    *rcode=(buf[used-3]-'0')*100+(buf[used-2]-'0')*10+buf[used-1]-'0';used-=4;
    if(memchr(buf,0,used))goto fail;buf[used]=0;*rx=buf;*rx_len=(uint32_t)used;return 0;
fail:free(buf);return -1;
}
static const struct euicc_apdu_interface apdu_if={card_connect,card_disconnect,card_open,card_close,card_transmit,NULL};
static const struct euicc_http_interface http_if={http_transmit,NULL};
static int read_profiles(EsimCard *card) {
    static const unsigned char p[]={0xbf,0x2d,8,0x5c,6,0x5a,0x9f,0x70,0x90,0x91,0x92};
    unsigned char b[8192];size_t n=sizeof b;
    return esim_read_command(&reader,p,sizeof p,b,&n)&&esim_profiles(b,n,card);
}
static int valid_imei(const char *s) {
    if(!digits(s,15,15))return 0;unsigned sum=0;
    for(unsigned i=0;i<15;i++){unsigned v=s[i]-'0';if(i%2){v*=2;if(v>9)v-=9;}sum+=v;}
    return sum%10==0;
}
static int read_imei(char out[16]) {
    Client dms={0};Packet req={0},resp={0};int ok=0;
    if(connect_client(&dms,"dms_qmi_idl_service_object_v01")&&request(&dms,0x25,&req,&resp,1800)){
        size_t n=0;const unsigned char *p=find(&resp,0x11,&n);if(p&&n==15){memcpy(out,p,15);out[15]=0;ok=valid_imei(out);}}
    disconnect(&dms);return ok;
}
static int activation(EsimOperation *o,char **server,char **matching) {
    if(strncmp(o->activation,"LPA:1$",6))return 0;
    char *parts[5];size_t count=0;char *p=o->activation+4;
    while(count<5){parts[count++]=p;char *next=strchr(p,'$');if(!next)break;*next=0;p=next+1;}
    if(count<3||strchr(parts[count-1],'$')||strcmp(parts[0],"1")||!hostname(parts[1])||!plain(parts[2]))return 0;
    if(count==5&&(*parts[4]&&strcmp(parts[4],"1")))return 0;
    if(count==5&&!strcmp(parts[4],"1")&&!*o->confirmation)return 0;
    if(!*o->imei&&!read_imei(o->imei))return 0;
    if(!valid_imei(o->imei))return 0;*server=parts[1];*matching=parts[2];return 1;
}
static int notifications(struct euicc_ctx *ctx) {
    struct es10b_notification_metadata_list *list=NULL;int ok=es10b_list_notification(ctx,&list)==0;
    unsigned count=0;for(struct es10b_notification_metadata_list *p=list;ok&&p;p=p->next){
        if(++count>64||!idle_checkpoint()){ok=0;break;}
        struct es10b_pending_notification pending={0};
        if(es10b_retrieve_notifications_list(ctx,&pending,p->seqNumber)||!pending.notificationAddress||!pending.b64_PendingNotification){ok=0;es10b_pending_notification_free(&pending);break;}
        ctx->http.server_address=pending.notificationAddress;
        ok=!es9p_handle_notification(ctx,pending.b64_PendingNotification);
        if(ok)ok=!es10b_remove_notification_from_list(ctx,p->seqNumber);
        ctx->http.server_address=NULL;es10b_pending_notification_free(&pending);
    }
    es10b_notification_metadata_list_free_all(list);return ok;
}
static int download(struct euicc_ctx *ctx,EsimOperation *o) {
    char *server=NULL,*matching=NULL;if(!activation(o,&server,&matching)){reader.error="激活码、确认码或 IMEI 格式无效，请检查后再下载";return 0;}
    ctx->http.server_address=server;
    progress("connecting","正在连接运营商并验证下载…");
    if(es10b_get_euicc_challenge_and_info(ctx)||es9p_initiate_authentication(ctx)||
        es10b_authenticate_server(ctx,matching,o->imei)||es9p_authenticate_client(ctx))goto cancel;
    progress("preparing","正在准备号码配置…");
    if(es10b_prepare_download(ctx,o->confirmation)||es9p_get_bound_profile_package(ctx))goto cancel;
    if(!idle_checkpoint())goto cancel;
    progress("installing","正在写入号码配置，请勿拔出模块…");mutation_started=1;
    struct es10b_load_bound_profile_package_result result={0};
    int ok=!es10b_load_bound_profile_package(ctx,&result);free(result.iccid);return ok;
cancel:
    /* End an incomplete authentication session; never retry a download or
     * cancel after profile installation has started. Incoming calls take priority. */
    if(ctx->http._internal.transaction_id_bin&&idle_checkpoint()&&
        !es10b_cancel_session(ctx,ES10B_CANCEL_SESSION_REASON_ENDUSERREJECTION))es9p_cancel_session(ctx);
    return 0;
}
static void operate(EsimOperation *o,int inspect) {
    struct euicc_ctx ctx={0};unsigned char aid[16];char *actual=NULL;int initialized=0,ok=0;
    const char *message="eSIM 操作未确认成功，请重新读卡核对状态；请勿直接重复下载";
    reader=(EsimReader){.open_message=0x42,.deadline=esim_ms()+(inspect?27000:900000)};
    if(!connect_client(&reader.voice,"voice_qmi_idl_service_object_v02")||!connect_client(&reader.uim,"uim_qmi_idl_service_object_v01")){message="模块卡片接口不可用";goto done;}
    if(!esim_idle(&reader)||!esim_card_slot(&reader)){message=reader.error;goto done;}
    ctx.aid=aid;ctx.aid_len=(uint8_t)decode(o->aid,aid,sizeof aid);ctx.es10x_mss=120;ctx.apdu.interface=&apdu_if;ctx.http.interface=&http_if;
    if(euicc_init(&ctx)){message="无法打开指定卡片，请重新读卡";goto done;}initialized=1;
    if(es10c_get_eid(&ctx,&actual)||!actual||!digits(actual,32,32)){message="卡片身份读取失败，未执行修改";goto done;}
    if(!inspect&&strcmp(actual,o->eid)){message="卡片已更换，请重新读卡后再操作";goto done;}
    EsimCard card={0};if(!read_profiles(&card)){message="号码配置未完整读取，未执行修改";goto done;}
    if(inspect){unsigned enabled=0;for(unsigned i=0;i<card.count;i++)enabled+=card.profiles[i].enabled;
        fprintf(output,"{\"ok\":true,\"profileCount\":%u,\"enabledCount\":%u}\n",card.count,enabled);ok=1;goto done;}
    int profile_action=strcmp(o->action,"download")&&strcmp(o->action,"notifications");EsimProfile *target=NULL;
    if(profile_action){for(unsigned i=0;i<card.count;i++)if(!strcmp(card.profiles[i].iccid,o->iccid))target=&card.profiles[i];
        if(!target){message="号码配置已变化，请重新读卡";goto done;}
        if(!strcmp(o->action,"delete")&&target->enabled){message="不能删除正在使用的配置，请先切换或停用";goto done;}
        if(!strcmp(o->action,"rename")){char name[65];EsimTLV t={0,(const unsigned char *)o->name,strlen(o->name)};
            if(!*o->name||!esim_text(t,name,sizeof name)){message="名称不能为空，最多 64 字节";goto done;}}
    }
    if(!esim_idle(&reader)){message=reader.error;goto done;}
    progress("applying","正在处理号码配置…");
    if(!strcmp(o->action,"download")){ok=download(&ctx,o);message=ok?"下载完成，号码配置尚未自动启用":reader.error?reader.error:message;}
    else if(!strcmp(o->action,"notifications")){ok=notifications(&ctx);message=ok?"运营商回执已补发":"回执补发未完成，可联网后重试";}
    else {
        mutation_started=1;
        if(!strcmp(o->action,"enable"))ok=!es10c_enable_profile(&ctx,o->iccid,1);
        else if(!strcmp(o->action,"disable"))ok=!es10c_disable_profile(&ctx,o->iccid,1);
        else if(!strcmp(o->action,"delete"))ok=!es10c_delete_profile(&ctx,o->iccid);
        else ok=!es10c_set_nickname(&ctx,o->iccid,o->name);
        if(ok)message=!strcmp(o->action,"enable")||!strcmp(o->action,"disable")?
            "卡片切换已提交，网络可能短暂断开，请稍后读卡核对":"配置操作完成，请重新读卡";
    }
    if(ok&&(!strcmp(o->action,"download")||!strcmp(o->action,"delete"))){
        progress("notifying","正在发送运营商回执…");if(!notifications(&ctx))message="配置操作完成；运营商回执未完成，可联网后补发";
    }
done:
    if(initialized)euicc_fini(&ctx);else if(reader.channel&&!esim_close(&reader))channel_close_failed=1;
    euicc_http_cleanup(&ctx);free(actual);disconnect(&reader.uim);disconnect(&reader.voice);
    if(channel_close_failed){ok=0;message="卡片通道未确认关闭，请重新读卡核对状态";}
    if(!inspect)job_record(ok?"complete":"failed",message?message:"eSIM 操作失败",!ok&&mutation_started,mutation_started);
    else if(!ok)error(message?message:"读卡验证失败");
}
#ifndef ESIM_WORKER_TEST_MAIN
int main(int argc,char **argv) {
    umask(0077);signal(SIGPIPE,SIG_IGN);alarm(930);
    int outfd=dup(STDOUT_FILENO),sink=open("/dev/null",O_RDWR);output=fdopen(outfd,"w");if(!output||sink<0)return 1;
    dup2(sink,STDOUT_FILENO);dup2(sink,STDERR_FILENO);close(sink);
    const char *root=getenv("QDC_WEB_ROOT"),*runtime=getenv("QDC_WEB_RUN");
    if(snprintf(private_dir,sizeof private_dir,"%s/private",root?root:"/usrdata/qdc-web")>=(int)sizeof private_dir||
        snprintf(run,sizeof run,"%s",runtime?runtime:"/run/qdc-web")>=(int)sizeof run)return 1;
    int inspect=(argc==2||argc==3)&&!strcmp(argv[1],"--check");
    if(!inspect&&(argc!=2||!hex(argv[1],64,64)))return 1;
    if(!inspect)strcpy(job_id,argv[1]);
    EsimOperation op={0};if(inspect){const char *aid=argc==3?argv[2]:"a06573746b6d65ffff4953442d522030";if(!allowed_aid(aid))return 1;strcpy(op.aid,aid);}
    else if(!read_operation(&op)){job_record("failed","eSIM 操作参数无效",0,0);return 1;}
    char lockname[640];snprintf(lockname,sizeof lockname,"%s/comm.lock",run);int lock=safe_open(lockname,O_RDWR|O_CREAT);
    if(!operation_lock(lock,0)){job_record("failed","模块正在处理其他通讯操作，请稍后重试",0,0);if(inspect)error("模块忙");return 1;}
    if(!library()){job_record("failed","模块卡片接口不可用",0,0);if(inspect)error("模块卡片接口不可用");return 1;}
    operate(&op,inspect);memset(&op,0,sizeof op);close(lock);fclose(output);return 0;
}
#endif
