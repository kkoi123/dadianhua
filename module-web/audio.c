/* Module-native, bounded PCM websocket. It never dials or changes APN.
 * The only hardware writer is the owned qdc-pcm child after session and call checks.
 * SHA-1 below is only the RFC6455 handshake, not authentication. */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#endif
#include <arpa/inet.h>
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <mbedtls/base64.h>
#include <mbedtls/sha1.h>
#define VERSION "0.3.5"
#define FRAME 320
#define MAX_WS 512
static volatile sig_atomic_t stopping;
static char root[512], run[512], origin[256], pcm_path[768], comm_path[768];
static int listener=-1;
static uint64_t millis(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000+(unsigned)t.tv_nsec/1000000;}
static void terminate(int sig){(void)sig;stopping=1;}
static int hex(const char *s){if(strlen(s)!=64)return 0;for(int i=0;i<64;i++)if(!strchr("0123456789abcdef",s[i]))return 0;return 1;}
static int filename(char *dst,size_t n,const char *base,const char *suffix){int k=snprintf(dst,n,"%s/%s",base,suffix);return k>0&&(size_t)k<n;}
static int secure_open(const char *name,int flags){int fd=open(name,flags|O_NOFOLLOW|O_CLOEXEC,0600);struct stat st;if(fd<0)return -1;if(fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_uid!=geteuid()||(st.st_mode&077)||st.st_nlink!=1){close(fd);return -1;}return fd;}
static int read_private(const char *name,char *buf,size_t size){int fd=secure_open(name,O_RDONLY);if(fd<0)return 0;ssize_t n=read(fd,buf,size-1);close(fd);if(n<=0||(size_t)n==size-1)return 0;buf[n]=0;return 1;}
static int write_all(int fd,const void *data,size_t n,unsigned deadline){const unsigned char *p=data;uint64_t end=millis()+deadline;while(n&&!stopping){ssize_t k=send(fd,p,n,0);if(k>0){p+=k;n-=(size_t)k;continue;}if(k<0&&errno==EINTR)continue;if(k<0&&(errno==EAGAIN||errno==EWOULDBLOCK)){uint64_t now=millis();if(now>=end)return 0;int left=(int)(end-now);struct pollfd f={fd,POLLOUT,0};if(poll(&f,1,left)>0)continue;}return 0;}return n==0;}
static int ws_send(int fd,unsigned opcode,const void *data,size_t n){unsigned char buf[MAX_WS+4];if(n>MAX_WS)return 0;buf[0]=0x80|opcode;size_t h=2;if(n<126)buf[1]=(unsigned char)n;else{buf[1]=126;buf[2]=(unsigned char)(n>>8);buf[3]=(unsigned char)n;h=4;}memcpy(buf+h,data,n);return write_all(fd,buf,h+n,250);}
static void ws_close(int fd,unsigned code,const char *reason){unsigned char b[125];size_t n=strlen(reason);if(n>123)n=123;b[0]=code>>8;b[1]=code;memcpy(b+2,reason,n);(void)ws_send(fd,8,b,n+2);}
/* Incremental parser: bounded, masked client frames, FIN required, no extensions. */
typedef struct{unsigned char bytes[4096];size_t used;} Parser;
static int ws_next(Parser *p,unsigned *op,unsigned char out[MAX_WS],size_t *length){
    if(p->used<2)return 0;unsigned char *b=p->bytes;size_t h=2,n=b[1]&127;
    if((b[0]&0x70)||!(b[0]&0x80)||!(b[1]&0x80))return -1;
    *op=b[0]&15;if(*op!=1&&*op!=2&&*op!=8&&*op!=9&&*op!=10)return -1;
    if(n==127)return -1;if(n==126){if(p->used<4)return 0;n=((size_t)b[2]<<8)|b[3];h=4;if(n<126)return -1;}
    if(n>MAX_WS||(*op>=8&&n>125)||(*op==8&&n==1))return -1;
    if(p->used<h+4+n)return 0;for(size_t i=0;i<n;i++)out[i]=b[h+4+i]^b[h+i%4];
    *length=n;size_t total=h+4+n;p->used-=total;memmove(b,b+total,p->used);return 1;
}
static int ws_read(int fd,Parser *p){if(p->used==sizeof p->bytes)return 0;ssize_t n=recv(fd,p->bytes+p->used,sizeof p->bytes-p->used,0);if(n>0){p->used+=(size_t)n;return 1;}return n<0&&(errno==EAGAIN||errno==EWOULDBLOCK||errno==EINTR);}
static int session_valid(const char *sid,const char *generation){
    char name[768],buf[512],csrf[65],gen[65],salt[33],hash[65];unsigned iterations=0;unsigned long long expires=0;
    if(!hex(sid)||!hex(generation))return 0;
    snprintf(name,sizeof name,"%s/sessions/%s",run,sid);
    if(!read_private(name,buf,sizeof buf)||sscanf(buf,"session-v1 %llu %64s %64s",&expires,csrf,gen)!=3||expires<=millis()/1000||strcmp(gen,generation)||!hex(csrf))return 0;
    if(!filename(name,sizeof name,root,"private/credential")||!read_private(name,buf,sizeof buf))return 0;
    return sscanf(buf,"pbkdf2-sha256-v1 %u %32s %64s %64s",&iterations,salt,hash,gen)==4&&iterations==600000&&!strcmp(gen,generation)&&hex(hash);
}
static int consume_ticket(const char *ticket,char sid[65],char gen[65]){
    if(!hex(ticket))return 0;char name[768],buf[256];int k=snprintf(name,sizeof name,"%s/audio-tickets/%s",run,ticket);if(k<=0||(size_t)k>=sizeof name)return 0;
    int fd=secure_open(name,O_RDONLY);if(fd<0)return 0;if(flock(fd,LOCK_EX|LOCK_NB)){close(fd);return 0;}
    struct stat st;if(fstat(fd,&st)||st.st_nlink!=1){close(fd);return 0;}
    ssize_t n=read(fd,buf,sizeof buf-1);unsigned long long expires=0;int ok=0;
    if(n>0&&(size_t)n<sizeof buf-1){buf[n]=0;ok=sscanf(buf,"audio-v1 %llu %64s %64s",&expires,sid,gen)==3&&expires>millis()/1000&&expires<=millis()/1000+60&&session_valid(sid,gen);}
    if(unlink(name))ok=0;close(fd);return ok;
}
static int header(char *request,const char *wanted,char *value,size_t cap){
    unsigned count=0;char *line=strstr(request,"\r\n");if(!line)return 0;line+=2;
    while(*line){char *end=strstr(line,"\r\n"),*colon=strchr(line,':');if(!end)return 0;if(end==line)break;
        if(colon&&colon<end&&(size_t)(colon-line)==strlen(wanted)&&!strncasecmp(line,wanted,colon-line)){
            if(++count>1)return 0;char *start=colon+1;while(start<end&&(*start==' '||*start=='\t'))start++;while(end>start&&(end[-1]==' '||end[-1]=='\t'))end--;
            size_t n=end-start;if(n>=cap)return 0;memcpy(value,start,n);value[n]=0;
        }line=strstr(line,"\r\n")+2;
    }return count==1;
}
static int upgrade(int fd){
    char request[4096];size_t used=0;uint64_t deadline=millis()+3000;
    while(used<sizeof request-1&&millis()<deadline&&!stopping){struct pollfd p={fd,POLLIN,0};if(poll(&p,1,100)<=0)continue;ssize_t n=recv(fd,request+used,1,0);if(n<=0)return 0;used++;request[used]=0;if(used>=4&&!memcmp(request+used-4,"\r\n\r\n",4))break;}
    if(used<4||memcmp(request+used-4,"\r\n\r\n",4))return 0;
    if(!strncmp(request,"GET /healthz HTTP/1.1\r\n",23)){const char msg[]="HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n{\"ok\":true,\"version\":\"" VERSION "\"}";(void)write_all(fd,msg,sizeof msg-1,250);return 0;}
    char o[256],key[64],u[32],c[64],v[16];
    if(strncmp(request,"GET /audio HTTP/1.1\r\n",21)||!header(request,"Origin",o,sizeof o)||strcmp(o,origin)||!header(request,"Upgrade",u,sizeof u)||strcasecmp(u,"websocket")||!header(request,"Connection",c,sizeof c)||strcasecmp(c,"Upgrade")||!header(request,"Sec-WebSocket-Version",v,sizeof v)||strcmp(v,"13")||!header(request,"Sec-WebSocket-Key",key,sizeof key)){
        const char denied[]="HTTP/1.1 403 Forbidden\r\nConnection: close\r\nContent-Length: 0\r\n\r\n";(void)write_all(fd,denied,sizeof denied-1,250);return 0;
    }
    unsigned char decoded[32];size_t len=0;if(strlen(key)!=24||mbedtls_base64_decode(decoded,sizeof decoded,&len,(unsigned char *)key,strlen(key))||len!=16)return 0;
    char merged[128],response[256];unsigned char sha[20],accept[64];size_t alen=0;
    snprintf(merged,sizeof merged,"%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11",key);
    if(mbedtls_sha1_ret((unsigned char *)merged,strlen(merged),sha)||mbedtls_base64_encode(accept,sizeof accept-1,&alen,sha,sizeof sha))return 0;accept[alen]=0;
    int n=snprintf(response,sizeof response,"HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n",accept);
    return n>0&&(size_t)n<sizeof response&&write_all(fd,response,n,250);
}
static int auth(int fd,Parser *p,char sid[65],char gen[65]){
    uint64_t deadline=millis()+5000;while(millis()<deadline&&!stopping){unsigned op;size_t n;unsigned char data[MAX_WS];int r=ws_next(p,&op,data,&n);if(r<0)return 0;if(r){if(op!=1||n!=69||memcmp(data,"AUTH ",5))return 0;data[n]=0;return consume_ticket((char *)data+5,sid,gen);}
        struct pollfd f={fd,POLLIN,0};int ready=poll(&f,1,100);if(ready>0&&!ws_read(fd,p))return 0;if(ready<0&&errno!=EINTR)return 0;
    }return 0;
}
static void stop_child_grace(pid_t *pid,unsigned grace){if(*pid<=0)return;pid_t target=*pid;int status;pid_t r=waitpid(target,&status,WNOHANG);if(r==0){kill(target,SIGTERM);uint64_t end=millis()+grace;while(millis()<end){r=waitpid(target,&status,WNOHANG);if(r==target||r<0)break;struct timespec t={0,20000000};nanosleep(&t,NULL);}if(r==0){kill(target,SIGKILL);while(waitpid(target,&status,0)<0&&errno==EINTR){}fprintf(stderr,"audio: owned child required forced stop\n");}}*pid=0;}
static void stop_child(pid_t *pid){stop_child_grace(pid,1500);}
static int child_alive(pid_t *pid){int s;if(*pid<=0)return 0;pid_t r=waitpid(*pid,&s,WNOHANG);if(r==0)return 1;*pid=0;return 0;}
static pid_t spawn(const char *program,const char *arg,int input,int output){pid_t p=fork();if(p)return p;
    if(listener>=0)close(listener);if(input>=0){if(input!=3)dup2(input,3);fcntl(3,F_SETFD,0);}if(output>=0)dup2(output,STDOUT_FILENO);
    int sink=open("/dev/null",O_RDWR);if(sink>=0){dup2(sink,STDIN_FILENO);if(output<0)dup2(sink,STDOUT_FILENO);if(input<0)dup2(sink,STDERR_FILENO);if(sink>3)close(sink);}
    /* PCM errors go to the existing private service log, never to the wire. */
    if(input>=0)execl(program,program,"--socket-fd","3","--verbose",NULL);else execl(program,program,arg,NULL);_exit(127);
}
typedef struct{pid_t pid;int fd;char result[1024];size_t used;uint64_t started;} Check;
static int check_start(Check *c){int f[2];if(pipe(f))return 0;fcntl(f[0],F_SETFD,FD_CLOEXEC);fcntl(f[1],F_SETFD,FD_CLOEXEC);c->pid=spawn(comm_path,"call-audio-check",-1,f[1]);close(f[1]);if(c->pid<0){close(f[0]);c->pid=0;return 0;}c->fd=f[0];fcntl(c->fd,F_SETFL,O_NONBLOCK);c->used=0;c->started=millis();return 1;}
/* 0 pending, 1 active, 2 inactive, -1 unavailable. */
static int check_poll(Check *c,unsigned *id,unsigned long long *key){if(c->pid<=0)return -1;int eof=0;for(;;){ssize_t n=read(c->fd,c->result+c->used,sizeof c->result-1-c->used);if(n>0){c->used+=n;if(c->used==sizeof c->result-1){eof=1;break;}continue;}if(n==0)eof=1;else if(errno!=EAGAIN&&errno!=EINTR)eof=1;break;}
    if(!eof&&millis()-c->started<8500)return 0;close(c->fd);c->fd=-1;stop_child(&c->pid);c->result[c->used]=0;
    if(sscanf(c->result,"{\"ok\":true,\"active\":true,\"callId\":%u,\"callKey\":\"%16llx\"}",id,key)==2&&*id>0&&*id<=255)return 1;
    if(!strcmp(c->result,"{\"ok\":true,\"active\":false,\"callId\":0,\"callKey\":\"0000000000000000\"}\n"))return 2;
    /* Log only the failure schema, never phone numbers or call identities. */
    int transport=0;unsigned protocol=0;const char *p=strstr(c->result,"\"transportError\":");if(p)sscanf(p,"\"transportError\":%d",&transport);
    p=strstr(c->result,"\"qmiError\":");if(p)sscanf(p,"\"qmiError\":%u",&protocol);
    const char *message="empty or invalid result";size_t length=strlen(message);
    if(!strncmp(c->result,"{\"ok\":false",11)&&(p=strstr(c->result,"\"error\":\""))){p+=9;const char *end=strchr(p,'\"');if(end){message=p;length=(size_t)(end-p);if(length>180)length=180;}}
    fprintf(stderr,"audio: call check failed elapsed=%llu bytes=%zu transport=%d qmi=%u error=%.*s\n",(unsigned long long)(millis()-c->started),c->used,transport,protocol,(int)length,message);return -1;
}
static unsigned nonzero(const unsigned char *pcm,size_t n){unsigned count=0;for(size_t i=0;i+1<n;i+=2)if(pcm[i]||pcm[i+1])count++;return count;}
/* Refuse an existing legacy bridge. Never kill a bridge owned by another app. */
static int legacy_audio_busy(void){
#ifndef AUDIO_TEST
    DIR *d=opendir("/proc");if(!d)return 1;struct dirent *e;int busy=0;
    while((e=readdir(d))){char *end=NULL;long pid=strtol(e->d_name,&end,10);if(!*e->d_name||*end||pid<=0)continue;
        char name[128],cmd[2048];snprintf(name,sizeof name,"/proc/%ld/cmdline",pid);int fd=open(name,O_RDONLY|O_CLOEXEC);if(fd<0)continue;ssize_t n=read(fd,cmd,sizeof cmd-1);close(fd);if(n<=0)continue;cmd[n]=0;
        for(ssize_t i=0;i<n;i++)if(!cmd[i])cmd[i]=' ';
        if(strstr(cmd,"mavo-pcm-bridge")||strstr(cmd,"voc_bridge")||strstr(cmd,"/bin/qdc-pcm")){busy=1;break;}
    }closedir(d);return busy;
#else
    return 0;
#endif
}
static void connection(int fd){Parser parser={0};char sid[65],gen[65];pid_t route=0,bridge=0;int pcm=-1,route_log=-1;Check check={.fd=-1};int owner=-1;
    unsigned up_frames=0,down_frames=0,up_nonzero=0,down_nonzero=0,up_dropped=0;
    const char *reason="音频连接已结束";unsigned code=4000;
    if(!upgrade(fd))goto cleanup;if(!auth(fd,&parser,sid,gen)){reason="音频登录已失效";code=4001;goto closed;}
    char lockname[768];filename(lockname,sizeof lockname,run,"audio-owner.lock");owner=secure_open(lockname,O_RDWR|O_CREAT);if(owner<0||flock(owner,LOCK_EX|LOCK_NB)){reason="音频正在其他页面使用";goto closed;}
    if(!check_start(&check)){reason="电话状态查询不可用";goto closed;}
    unsigned call_id=0;unsigned long long call_key=0;int result=-1;while(!stopping&&(result=check_poll(&check,&call_id,&call_key))==0){struct pollfd f={fd,POLLIN,0};poll(&f,1,20);if(f.revents&POLLHUP)goto closed;}
    if(stopping||result!=1){reason="请先接通电话，再启动音频";goto closed;}
    if(legacy_audio_busy()){reason="其他应用正在使用模块音频，请先停止原通话音频";goto closed;}
    int logs[2];if(pipe(logs)){reason="无法启动音频路由";goto closed;}
    fcntl(logs[0],F_SETFD,FD_CLOEXEC);fcntl(logs[1],F_SETFD,FD_CLOEXEC);
    /* The route program logs readiness on stderr. */
    route=fork();if(route==0){close(listener);dup2(logs[1],STDERR_FILENO);int null=open("/dev/null",O_RDWR);dup2(null,STDIN_FILENO);dup2(null,STDOUT_FILENO);close(null);execl(pcm_path,pcm_path,"--network-route-session",NULL);_exit(127);}close(logs[1]);route_log=logs[0];fcntl(route_log,F_SETFL,O_NONBLOCK);
    if(route<0){route=0;reason="无法启动音频路由";goto closed;}
    char logbuf[4096]={0};size_t used=0;uint64_t deadline=millis()+6500;int ready=0;
    while(!stopping&&millis()<deadline&&child_alive(&route)){ssize_t n=read(route_log,logbuf+used,sizeof logbuf-1-used);if(n>0){used+=n;logbuf[used]=0;if(strstr(logbuf,"route session active")){ready=1;break;}if(used==sizeof logbuf-1){memmove(logbuf,logbuf+used-512,512);used=512;}}struct pollfd f={fd,POLLIN,0};poll(&f,1,20);if(f.revents&POLLHUP)break;}
    if(!ready||!session_valid(sid,gen)){fprintf(stderr,"audio: route startup failed: %.*s\n",(int)used,logbuf);reason="模块语音路由未能就绪";goto closed;}
    close(route_log);route_log=-1;
    /* Ensure the same call is still connected before opening PCM after setup. */
    if(!check_start(&check))goto closed;unsigned confirmed=0;unsigned long long confirmed_key=0;while(!stopping&&(result=check_poll(&check,&confirmed,&confirmed_key))==0){struct timespec t={0,20000000};nanosleep(&t,NULL);}if(stopping||result!=1||(confirmed!=call_id||confirmed_key!=call_key)){reason="电话已结束或变化";goto closed;}
    int sockets[2];if(socketpair(AF_UNIX,SOCK_STREAM,0,sockets)){reason="无法建立音频通道";goto closed;}fcntl(sockets[0],F_SETFD,FD_CLOEXEC);fcntl(sockets[1],F_SETFD,FD_CLOEXEC);
    bridge=spawn(pcm_path,NULL,sockets[1],-1);close(sockets[1]);pcm=sockets[0];fcntl(pcm,F_SETFL,O_NONBLOCK);if(bridge<=0){bridge=0;reason="无法启动模块音频";goto closed;}
    if(!ws_send(fd,1,"{\"type\":\"ready\"}",16))goto closed;
    unsigned char down[FRAME],up[FRAME*5];size_t down_used=0,up_used=0;unsigned seq=0;
    uint64_t started=millis(),next_check=started+2000,next_stats=started+1000,last_client=started,last_pcm=started,last_confirmed=started;
    while(!stopping){
        if(!child_alive(&route)||!child_alive(&bridge)){reason="模块音频进程已停止";break;}
        uint64_t now=millis();if(now-last_client>15000){reason="网页音频连接超时";break;}if(now-last_pcm>8000){reason="模块未返回音频数据";break;}
        if(!check.pid&&now>=next_check){if(!session_valid(sid,gen)){reason="登录已失效，音频停止";code=4001;break;}if(!check_start(&check)){reason="无法确认电话状态";break;}next_check=now+2000;}
        if(check.pid){unsigned current=0;unsigned long long current_key=0;int r=check_poll(&check,&current,&current_key);if(r==2||(r==1&&(current!=call_id||current_key!=call_key))){reason="电话已结束或变化";code=1000;break;}if(r==1)last_confirmed=millis();if(r<0)next_check=millis()+1000;}
        /* A busy QMI operation is not a hangup. Still fail closed after a
         * bounded interval without confirmation, and on any confirmed change.
         * A single confirmation can occupy several seconds while the watcher is
         * busy (for example a full SMS scan holding comm.lock), so the old ten
         * second window covered barely two retries and cut healthy calls. The
         * bound stays, the window is widened to survive a busy watcher. */
        if(millis()-last_confirmed>=30000){reason="无法确认电话状态，音频停止";break;}
        if(now>=next_stats){char stats[256];int n=snprintf(stats,sizeof stats,"{\"type\":\"stats\",\"upFrames\":%u,\"downFrames\":%u,\"upNonzeroSamples\":%u,\"downNonzeroSamples\":%u,\"upDroppedFrames\":%u}",up_frames,down_frames,up_nonzero,down_nonzero,up_dropped);if(!ws_send(fd,1,stats,n))break;next_stats=now+1000;}
        struct pollfd f[3]={{fd,POLLIN,0},{pcm,POLLIN|(up_used?POLLOUT:0),0},{listener,POLLIN,0}};int r=poll(f,3,20);if(r<0){if(errno==EINTR)continue;break;}
        if(f[2].revents&POLLIN){int other=accept(listener,NULL,NULL);if(other>=0){fcntl(other,F_SETFL,O_NONBLOCK);const char busy[]="HTTP/1.1 409 Conflict\r\nConnection: close\r\nContent-Length: 0\r\n\r\n";(void)write_all(other,busy,sizeof busy-1,250);close(other);}}
        if(f[0].revents&(POLLHUP|POLLERR|POLLNVAL))break;
        if(f[0].revents&POLLIN){if(!ws_read(fd,&parser))break;}
        for(unsigned quota=0;quota<16;quota++){unsigned op;size_t n;unsigned char data[MAX_WS];int w=ws_next(&parser,&op,data,&n);if(w<0){reason="音频协议无效";goto closed;}if(!w)break;last_client=millis();if(op==8){code=1000;goto closed;}if(op==9){if(!ws_send(fd,10,data,n))goto closed;continue;}if(op==10)continue;if(op==1&&n==4&&!memcmp(data,"PING",4)){if(!ws_send(fd,1,"{\"type\":\"pong\"}",15))goto closed;continue;}
            if(op!=2||n!=FRAME+7||data[0]!=2){reason="音频帧格式无效";goto closed;}
            /* TCP/tunnels deliver bursts. Keep at most 100 ms of pending
             * speech, discard an old complete 20 ms block rather than killing
             * both directions. Dropping an even byte count preserves samples
             * even when the previous socket write ended inside a sample. */
            if(up_used+FRAME>sizeof up){memmove(up,up+FRAME,up_used-FRAME);up_used-=FRAME;up_dropped++;}
            memcpy(up+up_used,data+7,FRAME);up_used+=FRAME;up_frames++;up_nonzero+=nonzero(data+7,FRAME);
        }
        if(up_used){ssize_t n=write(pcm,up,up_used);if(n>0){up_used-=n;memmove(up,up+n,up_used);}else if(n<0&&errno!=EAGAIN&&errno!=EINTR)break;}
        if(f[1].revents&(POLLHUP|POLLERR|POLLNVAL))break;
        if(f[1].revents&POLLIN){ssize_t n=read(pcm,down+down_used,FRAME-down_used);if(n>0){down_used+=n;last_pcm=millis();if(down_used==FRAME){unsigned char frame[FRAME+7];frame[0]=1;frame[1]=seq>>8;frame[2]=seq++;uint32_t elapsed=(uint32_t)(millis()-started);for(int i=0;i<4;i++)frame[3+i]=elapsed>>(24-i*8);memcpy(frame+7,down,FRAME);down_nonzero+=nonzero(down,FRAME);down_frames++;down_used=0;if(!ws_send(fd,2,frame,sizeof frame))break;}}else if(!n||(errno!=EAGAIN&&errno!=EINTR))break;}
    }
closed:
    fprintf(stderr,"audio: %s; up=%u down=%u up_nonzero=%u down_nonzero=%u up_dropped=%u\n",reason,up_frames,down_frames,up_nonzero,down_nonzero,up_dropped);
    ws_close(fd,code,reason);
cleanup:
    if(pcm>=0)close(pcm);if(route_log>=0)close(route_log);if(check.fd>=0)close(check.fd);stop_child(&check.pid);stop_child_grace(&bridge,5500);stop_child_grace(&route,4000);if(owner>=0)close(owner);
}
int main(int argc,char **argv){
    if(getenv("REQUEST_METHOD")||argc!=2||strcmp(argv[1],"--serve"))return 2;
    umask(0077);const char *r=getenv("QDC_WEB_ROOT"),*s=getenv("QDC_WEB_RUN");if(snprintf(root,sizeof root,"%s",r?r:"/usrdata/qdc-web")>=(int)sizeof root||snprintf(run,sizeof run,"%s",s?s:"/run/qdc-web")>=(int)sizeof run)return 2;
    char name[768];if(!filename(name,sizeof name,root,"private/audio-origin")||!read_private(name,origin,sizeof origin))return 2;origin[strcspn(origin,"\r\n")]=0;
#ifndef AUDIO_TEST
    if(strncmp(origin,"https://",8)||strchr(origin+8,'/')||strpbrk(origin," \t?#"))return 2;
#endif
    if(!filename(pcm_path,sizeof pcm_path,root,"www/bin/qdc-pcm")||!filename(comm_path,sizeof comm_path,root,"www/cgi-bin/qdc-comm"))return 2;
    struct sigaction action={0};action.sa_handler=terminate;sigemptyset(&action.sa_mask);sigaction(SIGTERM,&action,NULL);sigaction(SIGINT,&action,NULL);signal(SIGPIPE,SIG_IGN);
    listener=socket(AF_INET,SOCK_STREAM,0);if(listener<0)return 1;fcntl(listener,F_SETFD,FD_CLOEXEC);int reuse=1;setsockopt(listener,SOL_SOCKET,SO_REUSEADDR,&reuse,sizeof reuse);
    struct sockaddr_in address={.sin_family=AF_INET,.sin_port=htons(8789)};inet_pton(AF_INET,"192.168.225.1",&address.sin_addr);
#ifdef AUDIO_TEST
    address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);const char *port=getenv("QDC_AUDIO_TEST_PORT");address.sin_port=htons(port?atoi(port):8789);
#endif
    if(bind(listener,(struct sockaddr *)&address,sizeof address)||listen(listener,4)){close(listener);return 1;}
    fprintf(stderr,"qdc-audio " VERSION " listening; audio is idle until authenticated call\n");
    while(!stopping){struct pollfd p={listener,POLLIN,0};if(poll(&p,1,200)<=0)continue;int fd=accept(listener,NULL,NULL);if(fd<0)continue;fcntl(fd,F_SETFD,FD_CLOEXEC);fcntl(fd,F_SETFL,O_NONBLOCK);connection(fd);close(fd);}
    close(listener);return 0;
}
