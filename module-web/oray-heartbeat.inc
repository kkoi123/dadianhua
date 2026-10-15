/* PHTunnel numeric configuration: TCP timeout is four times the send
 * interval; UDP uses the interval directly. SDK identity remains private. */
#define HEARTBEAT_CONFIG_LIMIT 8192
static int heartbeat_read(char out[HEARTBEAT_CONFIG_LIMIT + 8]) {
    char p[768]; if (!data_file(p,sizeof p,1,"module-oray.json")) return 0;
    int fd=safe_file(p,O_RDONLY); if(fd<0)return 0;
    size_t used=0; int ok=1;
    while(used<HEARTBEAT_CONFIG_LIMIT){ssize_t n=read(fd,out+used,HEARTBEAT_CONFIG_LIMIT-used);if(n<0&&errno==EINTR)continue;if(n<0){ok=0;break;}if(!n)break;used+=(size_t)n;}
    close(fd);memset(out+used,0,8);
    return ok&&used>0&&used<HEARTBEAT_CONFIG_LIMIT&&strlen(out)==used;
}
/* Preserve every other member's raw value, including nested auth and forward
 * objects. Never reconstruct or return credentials through browser fields. */
static int heartbeat_config(const char *json,char *out,size_t size,int tcp,int udp,
                            int *control_ms,int *forward_ms,int *udp_ms,int *custom) {
    const char *p=json;whitespace(&p);if(*p++!='{')return 0;whitespace(&p);
    size_t used=1;unsigned seen=0;int kept=0;out[0]='{';
    *control_ms=120000;*forward_ms=120000;*udp_ms=10000;*custom=0;
    if(*p!='}')for(;;){
        const char *start=p;char key[129];if(!parse_string(&p,key,sizeof key))return 0;
        whitespace(&p);if(*p++!=':')return 0;whitespace(&p);const char *value=p;
        if(!oray_skip(&p,0))return 0;
        unsigned bit=!strcmp(key,"keepalive")?1:!strcmp(key,"fwkeepalive")?2:!strcmp(key,"udpkeepalive")?4:0;
        if(bit){
            if(seen&bit)return 0;seen|=bit;*custom=1;
            if(value==p||*value<'0'||*value>'9')return 0;
            unsigned long number=0;
            for(const char *v=value;v<p;v++){if(*v<'0'||*v>'9'||number>214748364UL)return 0;number=number*10+(unsigned)(*v-'0');if(number>2147483647UL)return 0;}
            if(number){if(bit==1)*control_ms=(int)number;else if(bit==2)*forward_ms=(int)number;else *udp_ms=(int)number;}
        }else{
            size_t n=(size_t)(p-start);if(used+n+2>=size)return 0;
            if(kept++)out[used++]=',';memcpy(out+used,start,n);used+=n;
        }
        whitespace(&p);if(*p=='}')break;if(*p++!=',')return 0;whitespace(&p);
    }
    if(*p++!='}')return 0;whitespace(&p);if(*p)return 0;
    if(tcp){int n=snprintf(out+used,size-used,"%s\"keepalive\":%d,\"fwkeepalive\":%d,\"udpkeepalive\":%d",kept?",":"",tcp*4000,tcp*4000,udp*1000);if(n<0||(size_t)n>=size-used)return 0;used+=(size_t)n;}
    if(used+3>=size||used+2>=HEARTBEAT_CONFIG_LIMIT)return 0;out[used++]='}';out[used++]='\n';out[used]=0;return 1;
}
static int heartbeat_seconds(const char *s,int *value) {
    if(!*s||strlen(s)>4)return 0;int n=0;
    for(;*s;s++){if(*s<'0'||*s>'9')return 0;n=n*10+*s-'0';}
    if(n<10||n>1800)return 0;*value=n;return 1;
}
static void heartbeat_response(void) {
    char config[HEARTBEAT_CONFIG_LIMIT+8],scratch[HEARTBEAT_CONFIG_LIMIT+256];int a,b,c,custom;
    if(!heartbeat_read(config)||!heartbeat_config(config,scratch,sizeof scratch,0,0,&a,&b,&c,&custom)){
        memset(config,0,sizeof config);memset(scratch,0,sizeof scratch);error_response(503,"花生壳心跳配置无法读取");return;
    }
    memset(config,0,sizeof config);memset(scratch,0,sizeof scratch);
    char p[768],result[32]={0};int last=-1;
    if(data_file(p,sizeof p,0,"heartbeat-result")){int fd=safe_file(p,O_RDONLY);if(fd>=0){ssize_t n=read(fd,result,sizeof result-1);close(fd);if(n==2&&(result[0]=='0'||result[0]=='1')&&result[1]=='\n')last=result[0]-'0';}}
    headers(200,NULL);
    printf("{\"ok\":true,\"tcpSeconds\":%.3f,\"forwardSeconds\":%.3f,\"udpSeconds\":%.3f,\"custom\":%s,\"pending\":%s,\"lastSuccess\":%s,\"minSeconds\":10,\"maxSeconds\":1800}\n",a/4000.0,b/4000.0,c/1000.0,custom?"true":"false",tunnel_job()?"true":"false",tunnel_job()||last<0?"null":last?"true":"false");
}
static int heartbeat_apply(void) {
    struct timespec delay={2,0};nanosleep(&delay,NULL);
    int lock=-1;for(unsigned i=0;i<20&&lock<0;i++){lock=data_lock();if(lock<0){struct timespec pause={0,100000000};nanosleep(&pause,NULL);}}
    char job[768],candidate[768],result[768],maintenance[768],config_path[768];
    int paths=data_file(job,sizeof job,0,"tunnel-job")&&data_file(candidate,sizeof candidate,0,"heartbeat-request")&&data_file(result,sizeof result,0,"heartbeat-result")&&data_file(maintenance,sizeof maintenance,0,"standalone-maintenance")&&data_file(config_path,sizeof config_path,1,"module-oray.json");
    char old[HEARTBEAT_CONFIG_LIMIT+8]={0},next[HEARTBEAT_CONFIG_LIMIT+256]={0},command[256],request[32]={0},extra;
    int tcp=0,udp=0,a,b,c,custom,ok=0,stopped=0,changed=0;
    if(lock<0||!paths)goto done;
    int fd=safe_file(candidate,O_RDONLY);if(fd<0)goto done;ssize_t n=read(fd,request,sizeof request-1);close(fd);
    if(n<=0||sscanf(request,"%d %d %c",&tcp,&udp,&extra)!=2||tcp<10||tcp>1800||udp<10||udp>1800)goto done;
    if(data_job()||access(maintenance,F_OK)==0||!data_idle()||!heartbeat_read(old)||!heartbeat_config(old,next,sizeof next,tcp,udp,&a,&b,&c,&custom))goto done;
    if(!tunnel_command("tunnel-status",command,sizeof command))goto done;
    if(!strcmp(command,"running\n")){
        if(!tunnel_command("tunnel-stop",command,sizeof command))goto done;
        stopped=1;
        /* Read again after freezing the SDK, preserving its latest auth. */
        if(!heartbeat_read(old)||!heartbeat_config(old,next,sizeof next,tcp,udp,&a,&b,&c,&custom))goto done;
    }
    if(!atomic_write(config_path,next))goto done;changed=1;
    ok=tunnel_command("tunnel-apply",command,sizeof command);
done:
    if(!ok&&paths&&changed){tunnel_command("tunnel-stop",command,sizeof command);atomic_write(config_path,old);}
    if(!ok&&stopped)tunnel_command("tunnel-apply",command,sizeof command);
    if(paths){atomic_write(result,ok?"1\n":"0\n");unlink(candidate);unlink(job);}
    memset(old,0,sizeof old);memset(next,0,sizeof next);
    if(lock>=0)close(lock);return ok?0:1;
}
static void heartbeat_set(void) {
    int tcp,udp;if(!heartbeat_seconds(field("tcpSeconds"),&tcp)||!heartbeat_seconds(field("udpSeconds"),&udp)){error_response(400,"心跳间隔须为 10–1800 秒的整数");return;}
    int lock=data_lock();if(lock<0){error_response(409,"服务正在切换，请稍后重试");return;}
    char p[768];if(!data_file(p,sizeof p,0,"standalone-maintenance")||access(p,F_OK)==0||data_job()||tunnel_job()){close(lock);error_response(409,"联网或花生壳服务正在切换，请稍后重试");return;}
    char config[HEARTBEAT_CONFIG_LIMIT+8],scratch[HEARTBEAT_CONFIG_LIMIT+256];int a,b,c,custom;
    int valid=tunnel_configured()&&heartbeat_read(config)&&heartbeat_config(config,scratch,sizeof scratch,tcp,udp,&a,&b,&c,&custom);
    memset(config,0,sizeof config);memset(scratch,0,sizeof scratch);
    if(!valid){close(lock);error_response(503,"花生壳配置无效，未修改");return;}
    if(a==tcp*4000&&b==tcp*4000&&c==udp*1000){close(lock);headers(200,NULL);puts("{\"ok\":true,\"unchanged\":true,\"pending\":false}");return;}
    if(!data_idle()){close(lock);error_response(409,"请先结束电话；无法确认空闲时不会重连花生壳");return;}
    char job[768],candidate[768],program[768],record[64];
    if(!data_file(job,sizeof job,0,"tunnel-job")||!data_file(candidate,sizeof candidate,0,"heartbeat-request")||!path(program,sizeof program,root,"www/cgi-bin/api")){close(lock);error_response(503,"心跳设置暂不可用");return;}
    snprintf(record,sizeof record,"%d %d\n",tcp,udp);if(!atomic_write(candidate,record)){close(lock);error_response(503,"心跳设置保存失败");return;}
    snprintf(record,sizeof record,"%llu\n",(unsigned long long)now_seconds());if(!atomic_write(job,record)){unlink(candidate);close(lock);error_response(503,"无法创建重连任务");return;}
    pid_t pid=fork();if(!pid){close(lock);setsid();int sink=open("/dev/null",O_RDWR);if(sink>=0){dup2(sink,0);dup2(sink,1);dup2(sink,2);if(sink>2)close(sink);}execl(program,program,"--heartbeat-apply",NULL);_exit(127);}
    if(pid<0){unlink(candidate);unlink(job);close(lock);error_response(503,"无法启动重连任务");return;}
    close(lock);headers(202,NULL);printf("{\"ok\":true,\"accepted\":true,\"pending\":true,\"tcpSeconds\":%d,\"udpSeconds\":%d}\n",tcp,udp);fflush(stdout);
}
