#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#endif
#include "notify.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
static const char *envdir(const char *key,const char *fallback){const char *v=getenv(key);return v&&*v?v:fallback;}
static int path(char *b,size_t n,int runtime,const char *file){int k=snprintf(b,n,"%s/%s",envdir(runtime?"QDC_WEB_RUN":"QDC_WEB_ROOT",runtime?"/run/qdc-web":"/usrdata/qdc-web"),file);return k>0&&(size_t)k<n;}
/* Zig's ARM glibc headers use stat version 3; the module's older libc
 * fstat wrapper selects a different version. Preserve full ownership/link
 * checks by explicitly using the matching ABI, as the QMI helper already does. */
static int notify_fstat(int fd,struct stat *st){
#if defined(__arm__) && defined(__GLIBC__)
 extern int __fxstat(int,int,struct stat *);
 _Static_assert(offsetof(struct stat,st_mode)==16,"ARM glibc stat layout");
 _Static_assert(offsetof(struct stat,st_uid)==24,"ARM glibc stat ownership layout");
 return __fxstat(3,fd,st);
#else
 return fstat(fd,st);
#endif
}
static int safe(const char *name,int flags){int fd=open(name,flags|O_NOFOLLOW|O_CLOEXEC,0600);struct stat st;if(fd<0)return -1;if(notify_fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_uid!=geteuid()||(st.st_mode&077)||st.st_nlink!=1){close(fd);errno=EACCES;return -1;}return fd;}
static int key_ok(const char *key){size_t n=strlen(key);return n>=5&&n<=128&&strspn(key,"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-")==n;}
int qdc_notify_read(QdcNotify *c){memset(c,0,sizeof *c);c->calls=c->sms=1;char name[768];if(!path(name,sizeof name,0,"private/notify"))return 0;int fd=safe(name,O_RDONLY);if(fd<0)return errno==ENOENT;FILE *f=fdopen(fd,"r");if(!f){close(fd);return 0;}char tag[24],extra;
 int ok=fscanf(f,"%23s %u %u %u %" SCNu64 " %128s",tag,&c->enabled,&c->calls,&c->sms,&c->revision,c->key)==6&&!strcmp(tag,"notify-v1")&&c->enabled<2&&c->calls<2&&c->sms<2;
 if(!strcmp(c->key,"-"))c->key[0]=0;else ok=ok&&key_ok(c->key);if(c->enabled&&!*c->key)ok=0;if(ok&&fscanf(f," %c",&extra)!=EOF)ok=0;fclose(f);return ok;}
int qdc_notify_write(const QdcNotify *c){if(c->enabled>1||c->calls>1||c->sms>1||(*c->key&&!key_ok(c->key))||(c->enabled&&!*c->key))return 0;char name[768],tmp[832];if(!path(name,sizeof name,0,"private/notify"))return 0;snprintf(tmp,sizeof tmp,"%s.new.%ld",name,(long)getpid());int fd=safe(tmp,O_WRONLY|O_CREAT|O_EXCL);if(fd<0)return 0;FILE *f=fdopen(fd,"w");if(!f){close(fd);unlink(tmp);return 0;}
 int ok=fprintf(f,"notify-v1 %u %u %u %" PRIu64 " %s\n",c->enabled,c->calls,c->sms,c->revision,*c->key?c->key:"-")>0;if(fflush(f)||fsync(fd))ok=0;if(fclose(f))ok=0;if(ok&&rename(tmp,name))ok=0;if(!ok)unlink(tmp);if(ok){char directory[768];if(!path(directory,sizeof directory,0,"private"))return 0;int parent=open(directory,O_RDONLY|O_DIRECTORY|O_CLOEXEC);if(parent<0)return 0;ok=fsync(parent)==0;close(parent);}return ok;}
static void result(int ok){char name[768],tmp[832];if(!path(name,sizeof name,1,"notify-result"))return;snprintf(tmp,sizeof tmp,"%s.new.%ld",name,(long)getpid());int fd=safe(tmp,O_WRONLY|O_CREAT|O_EXCL);if(fd<0)return;char b[80];int n=snprintf(b,sizeof b,"notify-result-v1 %lld %d\n",(long long)time(NULL),ok);int saved=write(fd,b,(size_t)n)==n;close(fd);if(saved)rename(tmp,name);else unlink(tmp);}
/* Keep numeric send diagnostics only; never retain payload/key/response. */
static void diagnostic(const char *kind,int exit_code,int http,int code){char name[768],tmp[832];if(!path(name,sizeof name,1,"notify-diagnostic"))return;snprintf(tmp,sizeof tmp,"%s.new.%ld",name,(long)getpid());int fd=safe(tmp,O_WRONLY|O_CREAT|O_EXCL);if(fd<0)return;char b[120];int n=snprintf(b,sizeof b,"notify-diagnostic-v1 %lld %s %d %d %d\n",(long long)time(NULL),kind,exit_code,http,code);int saved=write(fd,b,(size_t)n)==n;close(fd);if(saved)rename(tmp,name);else unlink(tmp);}
typedef struct{int ok,exit_code,http_code,server_code;} NotifyHTTP;
static NotifyHTTP post_message(const char *payload,size_t length){NotifyHTTP answer={0,125,0,0};
 int in[2],out[2];if(pipe(in)){return answer;}if(pipe(out)){close(in[0]);close(in[1]);return answer;}
 const char *curl="/usr/bin/curl";
#ifdef NOTIFY_TEST
 curl=envdir("QDC_NOTIFY_TEST_CURL",curl);
#endif
 pid_t child=fork();if(child==0){close(in[1]);close(out[0]);dup2(in[0],STDIN_FILENO);dup2(out[1],STDOUT_FILENO);close(in[0]);close(out[1]);int sink=open("/dev/null",O_WRONLY);if(sink>=0){dup2(sink,STDERR_FILENO);close(sink);}execl(curl,curl,"--noproxy","*","--proto","=https","--tlsv1.2","--cacert","/etc/ssl/certs/ca-certificates.crt","--connect-timeout","4","--max-time","8","--fail","--silent","--max-filesize","4096","--write-out","\nQDC_HTTP=%{http_code}\n","--header","Content-Type: application/json","--data-binary","@-","https://api.day.app/push",NULL);_exit(127);}
 close(in[0]);close(out[1]);if(child<0){close(in[1]);close(out[0]);return answer;}size_t offset=0;while(offset<(size_t)length){ssize_t wrote=write(in[1],payload+offset,(size_t)length-offset);if(wrote>0)offset+=(size_t)wrote;else if(wrote<0&&errno==EINTR)continue;else break;}close(in[1]);
 char response[4097];size_t used=0;while(used<sizeof response-1){ssize_t got=read(out[0],response+used,sizeof response-1-used);if(got>0)used+=(size_t)got;else if(got<0&&errno==EINTR)continue;else break;}close(out[0]);int status=0;while(waitpid(child,&status,0)<0&&errno==EINTR){}response[used]=0;
 char *code=strstr(response,"\"code\"");if(code)code=strchr(code,':');int server_code=code?atoi(code+1):0;char *http=strstr(response,"QDC_HTTP=");int http_code=http?atoi(http+9):0;int exit_code=WIFEXITED(status)?WEXITSTATUS(status):128;int ok=offset==(size_t)length&&exit_code==0&&server_code==200;answer=(NotifyHTTP){ok,exit_code,http_code,server_code};return answer;
}
int qdc_notify_send(const char *kind){signal(SIGPIPE,SIG_IGN);int call=!strcmp(kind,"call"),sms=!strcmp(kind,"sms"),test=!strcmp(kind,"test");if(!call&&!sms&&!test)return 0;QdcNotify c;if(!qdc_notify_read(&c)||!*c.key)return 0;if(!test&&(!c.enabled||(call&&!c.calls)||(sms&&!c.sms)))return 2;
 char name[768];if(!path(name,sizeof name,1,"notify-send.lock"))return 0;int lock=safe(name,O_RDWR|O_CREAT);if(lock<0||flock(lock,LOCK_EX|LOCK_NB)){if(lock>=0)close(lock);return 0;}
 /* Throttle user test clicks, including concurrent sessions. */
 if(test){char b[48]={0};(void)pread(lock,b,sizeof b-1,0);time_t last=(time_t)strtoll(b,NULL,10),now=time(NULL);if(last&&now-last<15){close(lock);return 0;}int n=snprintf(b,sizeof b,"%lld\n",(long long)now);if(ftruncate(lock,0)||pwrite(lock,b,(size_t)n,0)!=n){close(lock);return 0;}}
 char origin[256]={0};if(!path(name,sizeof name,0,"private/audio-origin")){close(lock);return 0;}int origin_fd=safe(name,O_RDONLY);if(origin_fd<0){close(lock);return 0;}ssize_t n=read(origin_fd,origin,sizeof origin-1);close(origin_fd);if(n<=0){close(lock);return 0;}origin[strcspn(origin,"\r\n")]=0;
 if(strncmp(origin,"https://",8)||strlen(origin)<9||strspn(origin+8,"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-")!=strlen(origin+8)){close(lock);return 0;}
 const char *body=call?"模块有来电，点击打开电话页面":sms?"模块收到新短信，点击查看":"提醒已连接，点击打开通讯网页";
 char payload[1024];int length=snprintf(payload,sizeof payload,"{\"device_key\":\"%s\",\"title\":\"4G 模块管理\",\"body\":\"%s\",\"url\":\"%s/module/#%s\",\"group\":\"QDC507\",\"level\":\"timeSensitive\",\"isArchive\":0%s}",c.key,body,origin,call?"phone":sms?"sms":"settings",call?",\"call\":\"1\"":"");uint64_t config_revision=c.revision;memset(&c,0,sizeof c);
 if(length<0||(size_t)length>=sizeof payload){close(lock);return 0;}
 int attempts=0;NotifyHTTP answer;
 do {
  answer=post_message(payload,(size_t)length);attempts++;
  diagnostic(kind,answer.exit_code,answer.http_code,answer.server_code);
  if(answer.ok||test||attempts>=2)break;
  int transient=answer.exit_code==6||answer.exit_code==7||answer.exit_code==28||answer.exit_code==35||answer.exit_code==52||answer.exit_code==55||answer.exit_code==56||answer.http_code==408||answer.http_code==429||answer.http_code==500||answer.http_code==502||answer.http_code==503||answer.http_code==504;
  if(!transient)break;
  struct timespec pause={1,0};nanosleep(&pause,NULL);
  QdcNotify fresh;int allowed=qdc_notify_read(&fresh)&&fresh.revision==config_revision&&fresh.enabled&&((call&&fresh.calls)||(sms&&fresh.sms));memset(&fresh,0,sizeof fresh);if(!allowed)break;
 }while(1);
 memset(payload,0,sizeof payload);result(answer.ok);close(lock);return answer.ok;}
