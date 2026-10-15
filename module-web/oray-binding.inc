/* Fixed official binding endpoints. SDK passwords and credential-bearing URLs
 * remain server-side. No account reset, device registration or service restart. */
static int oray_skip(const char **p, unsigned depth) {
    if (depth > 8) return 0;
    whitespace(p);
    if (**p == '"') { char s[4097]; return parse_string(p, s, sizeof s); }
    if (**p == '{' || **p == '[') {
        char end = **p == '{' ? '}' : ']'; ++*p; whitespace(p);
        if (**p == end) { ++*p; return 1; }
        for (;;) {
            if (end == '}') { char key[129]; if (!parse_string(p,key,sizeof key)) return 0; whitespace(p); if (*(*p)++ != ':') return 0; }
            if (!oray_skip(p,depth+1)) return 0;
            whitespace(p); if (**p == end) { ++*p; return 1; }
            if (*(*p)++ != ',') return 0; whitespace(p);
        }
    }
    const char *start = *p;
    while (**p && !strchr(",}] \r\n\t", **p)) ++*p;
    size_t n = (size_t)(*p-start);
    if ((n==4 && (!memcmp(start,"true",4)||!memcmp(start,"null",4))) || (n==5 && !memcmp(start,"false",5))) return 1;
    if (!n || n>24) return 0;
    char value[25]; memcpy(value,start,n); value[n]=0; char *end;
    (void)strtod(value,&end); return end!=value && !*end;
}
static int oray_member(const char *json,const char *wanted,char *value,size_t size,int string) {
    const char *p=json; whitespace(&p); if (*p++!='{') return 0; whitespace(&p);
    int found=0; if (*p=='}') return 0;
    for (;;) {
        char key[129]; if (!parse_string(&p,key,sizeof key)) return 0; whitespace(&p); if (*p++!=':') return 0;
        whitespace(&p); const char *start=p; if (!oray_skip(&p,0)) return 0;
        if (!strcmp(key,wanted)) {
            if (found++) return 0;
            if (string) { const char *s=start; if (!parse_string(&s,value,size) || s!=p) return 0; }
            else { size_t n=(size_t)(p-start); if(n>=size) return 0; memcpy(value,start,n);value[n]=0; }
        }
        whitespace(&p); if (*p=='}') { ++p; whitespace(&p); return found==1 && !*p; }
        if (*p++!=',') return 0; whitespace(&p);
    }
}
static int oray_number(const char *json,const char *key,int *n) {
    char b[25],*end; if(!oray_member(json,key,b,sizeof b,0))return 0;
    long v=strtol(b,&end,10); if(*end||v<0||v>2147483647L)return 0; *n=(int)v;return 1;
}
static int oray_escape(const char *in,char *out,size_t size) {
    size_t n=0;
    for(const unsigned char *p=(const unsigned char *)in;*p;p++) {
        if(*p<32)return 0;
        if(*p=='"'||*p=='\\'){if(n+2>=size)return 0;out[n++]='\\';}
        if(n+1>=size)return 0;out[n++]=(char)*p;
    }
    out[n]=0;return 1;
}
static int oray_fetch(const char *url,const char *payload,unsigned char *out,size_t limit,size_t *length,int local) {
    int in[2],result[2]; if(pipe(in))return 0;
    if(pipe(result)){close(in[0]);close(in[1]);return 0;}
    const char *curl="/usr/bin/curl";
#ifdef ORAY_TEST
    const char *test=getenv("QDC_ORAY_TEST_CURL");if(test&&*test)curl=test;
#endif
    char maximum[32];snprintf(maximum,sizeof maximum,"%zu",limit);
    char *args[40];size_t a=0;
    args[a++]=(char *)curl;args[a++]="--noproxy";args[a++]="*";args[a++]="--silent";args[a++]="--fail";
    args[a++]="--connect-timeout";args[a++]="3";args[a++]="--max-time";args[a++]=local?"3":"10";
    args[a++]="--max-filesize";args[a++]=maximum;args[a++]="--proto";args[a++]=local?"=http":"=https";
    if(!local){args[a++]="--tlsv1.2";args[a++]="--cacert";args[a++]="/etc/ssl/certs/ca-certificates.crt";}
    if(payload){args[a++]="--header";args[a++]="Content-Type: application/json";args[a++]="--data-binary";args[a++]="@-";}
    args[a++]=(char *)url;args[a]=NULL;
    pid_t child=fork();
    if(!child){close(in[1]);close(result[0]);dup2(in[0],0);dup2(result[1],1);close(in[0]);close(result[1]);int sink=open("/dev/null",O_WRONLY);if(sink>=0){dup2(sink,2);close(sink);}execv(curl,args);_exit(127);}
    close(in[0]);close(result[1]);if(child<0){close(in[1]);close(result[0]);return 0;}
    signal(SIGPIPE,SIG_IGN); size_t sent=0,total=payload?strlen(payload):0;
    while(sent<total){ssize_t n=write(in[1],payload+sent,total-sent);if(n<0&&errno==EINTR)continue;if(n<=0)break;sent+=(size_t)n;}
    close(in[1]);size_t used=0;int ok=1;uint64_t start=now_seconds();
    while(used<limit){
        if(now_seconds()-start>=12){ok=0;break;}
        struct pollfd poller={result[0],POLLIN,0};int ready=poll(&poller,1,500);
        if(ready<0&&errno==EINTR)continue;if(ready<0){ok=0;break;}if(!ready)continue;
        ssize_t n=read(result[0],out+used,limit-used);if(n<0&&errno==EINTR)continue;if(n<0){ok=0;break;}if(!n)break;used+=(size_t)n;
    }
    if(used==limit)ok=0;
    close(result[0]);if(!ok)kill(child,SIGKILL);int status=0;while(waitpid(child,&status,0)<0&&errno==EINTR){}
    /* Padding bounds parse_string's four-byte Unicode escape reads. */
    memset(out+used,0,8);*length=used;
    return ok&&sent==total&&WIFEXITED(status)&&!WEXITSTATUS(status)&&used>0;
}
typedef struct {int online,known,bound;char sn[129],account[257];} OrayBinding;
static int oray_rpc(const char *method,char *data,size_t size) {
    unsigned char raw[16392];size_t n;char url[128];int code;
    snprintf(url,sizeof url,"http://127.0.0.1:16063/ora_service/%s",method);
    int ok=oray_fetch(url,NULL,raw,16384,&n,1)&&oray_number((char *)raw,"result_code",&code)&&code==0&&oray_member((char *)raw,"data",data,size,0);
    memset(raw,0,sizeof raw);return ok;
}
static void oray_binding_status(void) {
    OrayBinding b={0};char data[8192];int status;
    int available=oray_rpc("getsn",data,sizeof data);
    if(available){available=oray_number(data,"status",&status)&&oray_member(data,"device_sn",b.sn,sizeof b.sn,1);b.online=available&&status==1;}
    memset(data,0,sizeof data);
    if(available&&oray_rpc("getmgrurl",data,sizeof data)) {
        b.known=oray_member(data,"account",b.account,sizeof b.account,1)&&oray_number(data,"status",&status);
        b.bound=b.known&&*b.account;
    }
    memset(data,0,sizeof data);
    headers(200,NULL);printf("{\"ok\":true,\"rpcAvailable\":%s,\"online\":%s,\"bindingKnown\":%s,\"bound\":%s,\"deviceSN\":",available?"true":"false",b.online?"true":"false",b.known?"true":"false",b.bound?"true":"false");json_string(b.sn);printf(",\"account\":");json_string(b.account);puts("}");
}
static void oray_qrcode(void) {
    char lockfile[768];if(!path(lockfile,sizeof lockfile,runtime,"oray-binding.lock")){error_response(503,"绑定功能暂不可用");return;}
    int lock=safe_file(lockfile,O_RDWR|O_CREAT);if(lock<0||flock(lock,LOCK_EX|LOCK_NB)){if(lock>=0)close(lock);error_response(429,"二维码正在生成，请稍后重试");return;}
    char rate[48]={0};unsigned long long last=0;ssize_t readn=pread(lock,rate,sizeof rate-1,0);if(readn>0)last=strtoull(rate,NULL,10);
    uint64_t now=now_seconds();if(last&&now>=last&&now-last<15){close(lock);error_response(429,"请等待 15 秒后再刷新二维码");return;}
    char data[8192],sn[129],password[257],escapedSN[260],escapedPassword[520],payload[850];int status;
    int ok=oray_rpc("getsn",data,sizeof data)&&oray_number(data,"status",&status)&&status==1&&oray_member(data,"device_sn",sn,sizeof sn,1)&&*sn&&oray_member(data,"device_sn_pwd",password,sizeof password,1)&&*password&&oray_escape(sn,escapedSN,sizeof escapedSN)&&oray_escape(password,escapedPassword,sizeof escapedPassword);
    memset(data,0,sizeof data);memset(password,0,sizeof password);
    if(!ok){close(lock);error_response(503,"花生壳未连接服务器，请先开启服务并确认蜂窝联网");return;}
    snprintf(payload,sizeof payload,"{\"sn\":\"%s\",\"password\":\"%s\"}",escapedSN,escapedPassword);memset(escapedPassword,0,sizeof escapedPassword);
    int len=snprintf(rate,sizeof rate,"%llu\n",(unsigned long long)now);if(ftruncate(lock,0)||pwrite(lock,rate,(size_t)len,0)!=len){memset(payload,0,sizeof payload);close(lock);error_response(503,"无法生成二维码");return;}
    unsigned char raw[16392];size_t n=0;char url[4097];int code=0,ttl=300;
    ok=oray_fetch("https://hsk-api.oray.com/devices/qrcode",payload,raw,16384,&n,0);memset(payload,0,sizeof payload);
    if(ok&&oray_number((char *)raw,"code",&code)&&code==401002){memset(raw,0,sizeof raw);close(lock);error_response(409,"设备密码与花生壳服务器不一致，请先在花生壳后台核对设备密码");return;}
    ok=ok&&oray_member((char *)raw,"qrcodeimg",url,sizeof url,1);
    if(ok){int value;if(oray_number((char *)raw,"ttl",&value))ttl=value;}
    memset(raw,0,sizeof raw);ttl=ttl<1?1:ttl>600?600:ttl;
    const char *prefix="https://cphapi.oray.net/";
    ok=ok&&!strncmp(url,prefix,strlen(prefix))&&strspn(url,"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_/?=&%+.,:-")==strlen(url);
    unsigned char *png=ok?malloc(262152):NULL;
    if(!png||!oray_fetch(url,NULL,png,262144,&n,0)||n<8||memcmp(png,"\211PNG\r\n\032\n",8)){free(png);memset(url,0,sizeof url);close(lock);error_response(503,"二维码获取失败，请检查网络或稍后重试");return;}
    memset(url,0,sizeof url);close(lock);
    headers(200,NULL);printf("{\"ok\":true,\"deviceSN\":");json_string(sn);printf(",\"expiresIn\":%d,\"image\":\"data:image/png;base64,",ttl);
    const char *alphabet="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for(size_t i=0;i<n;i+=3){unsigned v=(unsigned)png[i]<<16;if(i+1<n)v|=(unsigned)png[i+1]<<8;if(i+2<n)v|=png[i+2];putchar(alphabet[(v>>18)&63]);putchar(alphabet[(v>>12)&63]);putchar(i+1<n?alphabet[(v>>6)&63]:'=');putchar(i+2<n?alphabet[v&63]:'=');}
    puts("\"}");free(png);
}
