#define _GNU_SOURCE
#include "call-history.h"
#include <sys/stat.h>
#include <sys/file.h>
#include <fcntl.h>
#include <dirent.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <inttypes.h>
/* The module's dynamic ARM glibc helper needs the old stat ABI, just as comm.c. */
static int hstat(int fd,struct stat *s) {
#if defined(__GLIBC__) && defined(__arm__)
 extern int __fxstat(int,int,struct stat *);return __fxstat(3,fd,s);
#else
 return fstat(fd,s);
#endif
}
static uint64_t wall(void){struct timespec t;clock_gettime(CLOCK_REALTIME,&t);return (uint64_t)t.tv_sec*1000+(unsigned)t.tv_nsec/1000000u;}
static uint64_t tick(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000+(unsigned)t.tv_nsec/1000000u;}
/* Bound duration to 49 days; use a 32-bit divide to keep the proprietary
 * glibc helper free of compiler-rt/libc interposition. */
static uint64_t seconds_from_ms(uint64_t ms){uint32_t bounded=ms>UINT32_MAX?UINT32_MAX:(uint32_t)ms;return bounded/1000u;}
static int number_ok(const char *s){size_t n=strlen(s);if(!n)return 1;if(n>80)return 0;for(size_t i=0;i<n;i++)if((s[i]<'0'||s[i]>'9')&&!(i==0&&s[i]=='+'))return 0;return strcmp(s,"+")!=0;}
static int hex_id(const char *s){if(strlen(s)!=32)return 0;for(unsigned i=0;i<32;i++)if(!((s[i]>='0'&&s[i]<='9')||(s[i]>='a'&&s[i]<='f')))return 0;return 1;}
static int boot_id(char boot[37]) {
#ifdef HISTORY_TEST
 const char *mock=getenv("QDC_HISTORY_BOOT");if(mock){if(strlen(mock)!=36)return 0;memcpy(boot,mock,37);return 1;}
#endif
 int fd=open("/proc/sys/kernel/random/boot_id",O_RDONLY|O_CLOEXEC);if(fd<0){
#ifdef HISTORY_TEST
 strcpy(boot,"00000000-0000-0000-0000-000000000000");return 1;
#else
 return 0;
#endif
 }char b[38]={0};ssize_t n=read(fd,b,37);close(fd);if(n<36)return 0;b[36]=0;memcpy(boot,b,37);return 1;
}
static int safe(const char *p,int flags){int fd=open(p,flags|O_NOFOLLOW|O_CLOEXEC,0600);struct stat s;if(fd<0)return -1;if(hstat(fd,&s)||!S_ISREG(s.st_mode)||s.st_uid!=geteuid()||(s.st_mode&077)||s.st_nlink!=1){close(fd);errno=EPERM;return -1;}return fd;}
static int name(char p[768],const char *dir,const char *suffix){return snprintf(p,768,"%s/%s",dir,suffix)<768;}
static int lock_history(const char *run){char p[768];if(!name(p,run,"call-history.lock"))return -1;int fd=safe(p,O_RDWR|O_CREAT);if(fd<0)return -1;/* Never block the radio watcher behind a CGI request. */if(flock(fd,LOCK_EX|LOCK_NB)){close(fd);return -1;}return fd;}
static int load(const char *dir,QdcHistory *h){memset(h,0,sizeof *h);char p[768];if(!name(p,dir,"call-history"))return 0;errno=0;int fd=safe(p,O_RDONLY);if(fd<0)return errno==ENOENT;FILE *f=fdopen(fd,"r");if(!f){close(fd);return 0;}char line[512],extra;unsigned count;int ok=fgets(line,sizeof line,f)&&sscanf(line,"QDCH1 %" SCNu64 " %u %c",&h->cleared,&count,&extra)==2&&count<=QDC_HISTORY_LIMIT;
 for(unsigned i=0;ok&&i<count;i++) {QdcHistoryRow *r=&h->rows[i];ok=fgets(line,sizeof line,f)&&sscanf(line,"%32s %36s %80s %u %u %u %u %u %" SCNu64 " %" SCNu64 " %" SCNu64 " %" SCNu64 " %" SCNu64 " %c",r->id,r->boot,r->number,&r->call_id,&r->direction,&r->status,&r->hidden,&r->imported,&r->started,&r->ended,&r->answered,&r->answer_tick,&r->duration,&extra)==13;
  if(!strcmp(r->number,"-"))r->number[0]=0;
  if(!hex_id(r->id)||strlen(r->boot)!=36||!number_ok(r->number)||r->call_id>255||(r->direction!=1&&r->direction!=2)||r->status>3||r->hidden>1||r->imported>1||!r->started||(r->ended&&r->ended<r->started))ok=0;
  for(unsigned j=0;ok&&j<i;j++)if(!strcmp(r->id,h->rows[j].id))ok=0;
 }
 if(ok&&(fgetc(f)!=EOF||ferror(f)))ok=0;fclose(f);if(ok)h->count=count;return ok;
}
static void cleanup_temps(const char *dir){DIR *d=opendir(dir);if(!d)return;struct dirent *e;while((e=readdir(d))){if(strncmp(e->d_name,".call-history-",14)||strlen(e->d_name)!=20)continue;int valid=1;for(unsigned i=14;i<20;i++)if(!((e->d_name[i]>='0'&&e->d_name[i]<='9')||(e->d_name[i]>='a'&&e->d_name[i]<='z')||(e->d_name[i]>='A'&&e->d_name[i]<='Z')))valid=0;if(!valid)continue;char p[768];if(!name(p,dir,e->d_name))continue;int fd=safe(p,O_RDONLY);if(fd>=0){close(fd);unlink(p);}}closedir(d);}
static int save(const char *dir,const QdcHistory *h){char target[768],temp[768];if(!name(target,dir,"call-history")||!name(temp,dir,".call-history-XXXXXX"))return 0;
 /* Refuse replacing unsafe existing files. mkstemp makes interrupted writers safe. */
 errno=0;int check=safe(target,O_RDONLY);if(check>=0)close(check);else if(errno!=ENOENT)return 0;
 cleanup_temps(dir);int directory=open(dir,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);if(directory<0)return 0;int fd=mkstemp(temp);if(fd<0){close(directory);return 0;}fcntl(fd,F_SETFD,FD_CLOEXEC);FILE *f=fdopen(fd,"w");if(!f){close(fd);unlink(temp);close(directory);return 0;}
 int ok=fprintf(f,"QDCH1 %" PRIu64 " %u\n",h->cleared,h->count)>0;
 for(unsigned i=0;ok&&i<h->count;i++){const QdcHistoryRow *r=&h->rows[i];ok=fprintf(f,"%s %s %s %u %u %u %u %u %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64 "\n",r->id,r->boot,*r->number?r->number:"-",r->call_id,r->direction,r->status,r->hidden,r->imported,r->started,r->ended,r->answered,r->answer_tick,r->duration)>0;}
 if(fflush(f)||fsync(fd))ok=0;if(fclose(f))ok=0;if(ok&&rename(temp,target))ok=0;if(ok&&fsync(directory))ok=0;if(!ok)unlink(temp);close(directory);return ok;
}
static int random_id(char id[33]){unsigned char b[16];int fd=open("/dev/urandom",O_RDONLY|O_CLOEXEC);if(fd<0)return 0;size_t n=0;while(n<sizeof b){ssize_t x=read(fd,b+n,sizeof b-n);if(x<0&&errno==EINTR)continue;if(x<=0){close(fd);return 0;}n+=(size_t)x;}close(fd);for(unsigned i=0;i<16;i++)snprintf(id+2*i,3,"%02x",b[i]);return 1;}
static int recover(QdcHistory *h,const char *boot){int changed=0;for(unsigned i=0;i<h->count;i++){QdcHistoryRow *r=&h->rows[i];if(!r->status&&strcmp(r->boot,boot)){r->status=2;r->ended=0;r->duration=0;changed=1;}}return changed;}
static void remove_row(QdcHistory *h,unsigned i){memmove(h->rows+i,h->rows+i+1,(h->count-i-1)*sizeof *h->rows);h->count--;}
static QdcHistoryRow *insert(QdcHistory *h){if(h->count==QDC_HISTORY_LIMIT){int found=-1;for(unsigned i=0;i<h->count;i++)if(h->rows[i].status&&(found<0||h->rows[i].started<h->rows[found].started))found=(int)i;if(found<0)return NULL;remove_row(h,(unsigned)found);}QdcHistoryRow *r=&h->rows[h->count++];memset(r,0,sizeof *r);return r;}
int qdc_history_snapshot(const char *dir,const char *run,const QdcHistoryCall *calls,size_t count){if(count>255)return 0;char boot[37];if(!boot_id(boot))return 0;int lock=lock_history(run);if(lock<0)return 0;QdcHistory *h=calloc(1,sizeof *h);int ok=h&&load(dir,h);if(!ok){free(h);close(lock);return 0;}int changed=recover(h,boot);uint64_t now=wall(),mono=tick();
 for(unsigned i=0;i<h->count;i++){QdcHistoryRow *r=&h->rows[i];if(r->status)continue;const QdcHistoryCall *c=NULL;for(size_t j=0;j<count;j++)if(calls[j].id==r->call_id&&calls[j].direction==r->direction){c=&calls[j];break;}
  if(!c||c->state==9){r->status=1;r->ended=now<r->started?r->started:now;if(r->answer_tick&&mono>=r->answer_tick)r->duration=seconds_from_ms(mono-r->answer_tick);changed=1;}
 }
 for(size_t j=0;j<count;j++){const QdcHistoryCall *c=&calls[j];if(c->id>255||(c->direction!=1&&c->direction!=2)||c->state==9)continue;QdcHistoryRow *r=NULL;for(unsigned i=0;i<h->count;i++)if(!h->rows[i].status&&h->rows[i].call_id==c->id&&h->rows[i].direction==c->direction){r=&h->rows[i];break;}
  if(!r){r=insert(h);if(!r||!random_id(r->id)){ok=0;break;}strcpy(r->boot,boot);r->call_id=c->id;r->direction=c->direction;r->started=now;changed=1;}
  if(number_ok(c->number)&&*c->number&&strcmp(r->number,c->number)){strcpy(r->number,c->number);changed=1;}
  if((c->state==3||c->state==6)&&!r->answered){r->answered=now;r->answer_tick=mono;changed=1;}
 }
 for(unsigned i=0;i<h->count;)if(h->rows[i].hidden&&h->rows[i].status){remove_row(h,i);changed=1;}else i++;
 if(ok&&changed)ok=save(dir,h);free(h);close(lock);return ok;
}
int qdc_history_read(const char *dir,const char *run,QdcHistory *h){char boot[37];if(!boot_id(boot))return 0;int lock=lock_history(run);if(lock<0)return 0;int ok=load(dir,h);if(ok){int changed=recover(h,boot);if(changed)ok=save(dir,h);uint64_t mono=tick();for(unsigned i=0;i<h->count;i++){QdcHistoryRow *r=&h->rows[i];if(!r->status&&r->answer_tick&&mono>=r->answer_tick)r->duration=seconds_from_ms(mono-r->answer_tick);}}close(lock);return ok;}
int qdc_history_clear(const char *dir,const char *run){int lock=lock_history(run);if(lock<0)return 0;QdcHistory *h=calloc(1,sizeof *h);int ok=h&&load(dir,h);if(ok){h->cleared=wall();for(unsigned i=0;i<h->count;)if(h->rows[i].status)remove_row(h,i);else {h->rows[i].hidden=1;i++;}ok=save(dir,h);}free(h);close(lock);return ok;}
int qdc_history_import(const char *dir,const char *run,const QdcHistoryRow *rows,size_t count){if(count>20)return 0;char boot[37];if(!boot_id(boot))return 0;uint64_t now=wall();for(size_t i=0;i<count;i++)if(!number_ok(rows[i].number)||(rows[i].direction!=1&&rows[i].direction!=2)||!rows[i].started||rows[i].started>now+86400000||rows[i].ended>now+86400000||(rows[i].ended&&rows[i].ended<rows[i].started))return 0;
 int lock=lock_history(run);if(lock<0)return 0;QdcHistory *h=calloc(1,sizeof *h);int ok=h&&load(dir,h),changed=0;
 for(size_t i=0;ok&&i<count;i++){const QdcHistoryRow *r=&rows[i];if(r->started<=h->cleared)continue;int seen=0;for(unsigned j=0;j<h->count;j++)if(h->rows[j].imported&&h->rows[j].started==r->started&&h->rows[j].direction==r->direction&&!strcmp(h->rows[j].number,r->number)){seen=1;break;}if(seen)continue;/* Never evict newer module records to import older browser records. */
  if(h->count==QDC_HISTORY_LIMIT){uint64_t oldest=UINT64_MAX;for(unsigned j=0;j<h->count;j++)if(h->rows[j].status&&h->rows[j].started<oldest)oldest=h->rows[j].started;if(r->started<=oldest)continue;}
  QdcHistoryRow *dest=insert(h);if(!dest||!random_id(dest->id)){ok=0;break;}strcpy(dest->boot,boot);strcpy(dest->number,r->number);dest->direction=r->direction;dest->started=r->started;dest->ended=r->ended;dest->status=3;dest->imported=1;changed=1;
 }
 if(ok&&changed)ok=save(dir,h);free(h);close(lock);return ok;
}
