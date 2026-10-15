/* The SDK may truncate its working JSON. Keep that file in RAM and commit
 * only complete stable snapshots. Never log private configuration contents. */
#define _GNU_SOURCE
#define _DARWIN_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <sys/stat.h>
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#define LIMIT 8192
static char priv[512]="/usrdata/qdc-web/private", run[512]="/run/qdc-web";
static void ws(const char **p){while(**p==' '||**p=='\n'||**p=='\r'||**p=='\t')++*p;}
static int string(const char **p,char *out,size_t cap){
    if(*(*p)++!='"')return 0;size_t n=0;
    while(**p && **p!='"'){
        unsigned char c=(unsigned char)*(*p)++;if(c<32)return 0;
        if(c=='\\'){
            c=(unsigned char)*(*p)++;if(!c)return 0;
            if(c=='u'){for(int i=0;i<4;i++){if(!isxdigit((unsigned char)**p))return 0;++*p;}c='?';}
            else if(!strchr("\"\\/bfnrt",c))return 0;
        }
        if(out){if(n+1>=cap)return 0;out[n++]=(char)c;}
    }
    if(*(*p)++!='"')return 0;if(out)out[n]=0;return 1;
}
static int value(const char **p,int depth){
    if(depth>16)return 0;ws(p);
    if(**p=='"')return string(p,NULL,0);
    if(**p=='{'||**p=='['){char end=*(*p)++=='{'?'}':']';ws(p);if(**p!=end){for(;;){
        if(end=='}'){if(!string(p,NULL,0))return 0;ws(p);if(*(*p)++!=':')return 0;}
        if(!value(p,depth+1))return 0;ws(p);if(**p==end)break;if(*(*p)++!=',')return 0;ws(p);
    }}if(*(*p)++!=end)return 0;return 1;}
    for(int i=0;i<3;i++){const char *v=i==0?"true":i==1?"false":"null";size_t n=strlen(v);if(!strncmp(*p,v,n)){*p+=n;return 1;}}
    const char *s=*p;if(**p=='-')++*p;
    if(**p=='0')++*p;else{if(**p<'1'||**p>'9')return 0;while(isdigit((unsigned char)**p))++*p;}
    if(**p=='.'){++*p;if(!isdigit((unsigned char)**p))return 0;while(isdigit((unsigned char)**p))++*p;}
    if(**p=='e'||**p=='E'){++*p;if(**p=='+'||**p=='-')++*p;if(!isdigit((unsigned char)**p))return 0;while(isdigit((unsigned char)**p))++*p;}
    return *p>s;
}
static int valid_config(const char *s,char sn[129]){
    const char *p=s;char keys[64][129],auth[257];unsigned count=0,seen=0;ws(&p);if(*p++!='{')return 0;ws(&p);
    if(*p!='}')for(;;){
        if(count>=64||!string(&p,keys[count],sizeof keys[0]))return 0;
        for(unsigned i=0;i<count;i++)if(!strcmp(keys[i],keys[count]))return 0;
        const char *key=keys[count++];ws(&p);if(*p++!=':')return 0;ws(&p);
        if(!strcmp(key,"sn")||!strcmp(key,"auth")){
            char *dest=!strcmp(key,"sn")?sn:auth;size_t cap=!strcmp(key,"sn")?129:sizeof auth;
            if(!string(&p,dest,cap)||!*dest)return 0;
            if(!strcmp(key,"sn")){for(char *q=sn;*q;q++)if(!isalnum((unsigned char)*q)&&*q!='-'&&*q!='_')return 0;seen|=1;}else seen|=2;
        }else{
            const char *start=p;if(!value(&p,0))return 0;
            if(!strcmp(key,"keepalive")||!strcmp(key,"fwkeepalive")||!strcmp(key,"udpkeepalive")){
                if(start==p)return 0;unsigned long n=0;for(const char *q=start;q<p;q++){if(!isdigit((unsigned char)*q)||n>214748364UL)return 0;n=n*10+(unsigned)(*q-'0');if(n>2147483647UL)return 0;}
            }
        }
        ws(&p);if(*p=='}')break;if(*p++!=',')return 0;ws(&p);
    }
    if(*p++!='}')return 0;ws(&p);return !*p&&seen==3;
}
static int valid_origin(const char *s){
    if(strncmp(s,"https://",8))return 0;const char *p=s+8;if(!isalnum((unsigned char)*p))return 0;
    size_t n=0;for(;*p&&*p!='\n';p++,n++)if(!isalnum((unsigned char)*p)&&*p!='.'&&*p!='-')return 0;
    return n>0&&n<254&&(!*p||(!p[1]&&*p=='\n'));
}
static int name(char out[768],int runtime,const char *base){return snprintf(out,768,"%s/%s",runtime?run:priv,base)<768;}
static int read_file(int runtime,const char *base,char out[LIMIT+1]){
    char path[768];if(!name(path,runtime,base))return 0;int fd=open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);if(fd<0)return 0;
    struct stat st;int ok=!fstat(fd,&st)&&S_ISREG(st.st_mode)&&st.st_uid==geteuid()&&!(st.st_mode&077)&&st.st_size>0&&st.st_size<LIMIT;
    size_t n=0;while(ok&&n<LIMIT){ssize_t r=read(fd,out+n,LIMIT-n);if(r<0&&errno==EINTR)continue;if(r<0){ok=0;break;}if(!r)break;n+=(size_t)r;}
    close(fd);out[n]=0;return ok&&n>0&&n<LIMIT&&strlen(out)==n;
}
static int sync_fd(int fd){int rc;do{rc=fsync(fd);}while(rc&&errno==EINTR);return rc==0;}
static int write_file(int runtime,const char *base,const char *s){
    char old[LIMIT+1],path[768],temp[800];if(!name(path,runtime,base))return 0;
    struct stat st;if(!lstat(path,&st)&&(!S_ISREG(st.st_mode)||st.st_uid!=geteuid()||(st.st_mode&077)))return 0;
    if(read_file(runtime,base,old)&&!strcmp(old,s))return 1;
    if(snprintf(temp,sizeof temp,"%s.XXXXXX",path)>=(int)sizeof temp)return 0;
    int fd=mkstemp(temp);if(fd<0)return 0;int ok=fchmod(fd,0600)==0;size_t n=0,len=strlen(s);
    while(ok&&n<len){ssize_t r=write(fd,s+n,len-n);if(r<0&&errno==EINTR)continue;if(r<=0){ok=0;break;}n+=(size_t)r;}
    if(ok)ok=sync_fd(fd);if(close(fd))ok=0;
#ifdef CONFIG_STORE_TEST
    if(getenv("CONFIG_STORE_CRASH")&&!strcmp(getenv("CONFIG_STORE_CRASH"),"before-rename"))_exit(99);
#endif
    if(ok)ok=rename(temp,path)==0;
#ifdef CONFIG_STORE_TEST
    if(getenv("CONFIG_STORE_CRASH")&&!strcmp(getenv("CONFIG_STORE_CRASH"),"after-rename"))_exit(99);
#endif
    if(ok){int dir=open(runtime?run:priv,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);ok=dir>=0&&sync_fd(dir);if(dir>=0)close(dir);}
    if(!ok)unlink(temp);return ok;
}
static int recover_one(const char *base,int origin){
    char saved[LIMIT+1],backup[LIMIT+1],sn[129],last[256];snprintf(last,sizeof last,"%s.last-good",base);
    int good=read_file(0,base,saved)&&(origin?valid_origin(saved):valid_config(saved,sn));
    if(good)return write_file(0,last,saved);
    if(!read_file(0,last,backup)||!(origin?valid_origin(backup):valid_config(backup,sn)))return 0;
    return write_file(0,base,backup);
}
static int checkpoint(int frozen){
    char current[LIMIT+1],saved[LIMIT+1],previous[LIMIT+1],sn[129],saved_sn[129];
    if(!read_file(1,"module-oray.json",current)||!valid_config(current,sn))return 0;
    /* A stale runtime identity must never overwrite a newly imported device. */
    if(!read_file(0,"module-oray.json",saved)||!valid_config(saved,saved_sn)||strcmp(sn,saved_sn))return 0;
    if(!frozen&&(!read_file(1,"module-oray.stable",previous)||strcmp(current,previous)))return write_file(1,"module-oray.stable",current);
    if(!write_file(0,"module-oray.json.last-good",saved))return 0;
    if(!write_file(0,"module-oray.json",current))return 0;
    return write_file(0,"module-oray.json.last-good",current);
}
int main(int argc,char **argv){
    umask(077);
#ifdef CONFIG_STORE_TEST
    const char *p=getenv("CONFIG_STORE_PRIVATE"),*r=getenv("CONFIG_STORE_RUN");if(p)snprintf(priv,sizeof priv,"%s",p);if(r)snprintf(run,sizeof run,"%s",r);
#endif
    if(argc<2)return 2;char lock_path[768];if(!name(lock_path,1,"config-store.lock"))return 1;
    int lock=open(lock_path,O_RDWR|O_CREAT|O_CLOEXEC|O_NOFOLLOW,0600);struct stat st;
    if(lock<0||fstat(lock,&st)||!S_ISREG(st.st_mode)||st.st_uid!=geteuid()||(st.st_mode&077)||flock(lock,LOCK_EX))return 1;
    int ok=0;char s[LIMIT+1],sn[129];
    if(!strcmp(argv[1],"recover")){ok=recover_one("audio-origin",1);/* Missing Oray config must not disable USB web management. */recover_one("module-oray.json",0);}
    else if(!strcmp(argv[1],"seed"))ok=recover_one("module-oray.json",0)&&read_file(0,"module-oray.json",s)&&write_file(1,"module-oray.json",s)&&write_file(1,"module-oray.stable",s);
    else if(!strcmp(argv[1],"checkpoint"))ok=checkpoint(0);
    else if(!strcmp(argv[1],"checkpoint-stopped"))ok=checkpoint(1);
    else if(!strcmp(argv[1],"import"))ok=read_file(1,"module-oray-import.json",s)&&valid_config(s,sn)&&write_file(0,"module-oray.json",s)&&write_file(0,"module-oray.json.last-good",s);
    else if(!strcmp(argv[1],"set-origin")&&argc==3&&strlen(argv[2])<512){snprintf(s,sizeof s,"%s\n",argv[2]);ok=valid_origin(s)&&write_file(0,"audio-origin",s)&&write_file(0,"audio-origin.last-good",s);}
    close(lock);if(!ok)fprintf(stderr,"Configuration was unavailable, invalid or could not be saved.\n");return ok?0:1;
}
