/* Display the durable SMS idempotency journal. Hiding a row must never erase
 * the original action record, otherwise an old retry could send twice. */
static int outbox_id(const char *id) { return valid_hex(id,32); }
static int outbox_path(char p[768],const char *prefix,const char *id) {
    char suffix[128];if(!outbox_id(id))return 0;
    snprintf(suffix,sizeof suffix,"private/%s%s",prefix,id);return path(p,768,root,suffix);
}
static int outbox_lock(void) {
    char p[768];if(!path(p,sizeof p,runtime,"sms-outbox.lock"))return -1;
    int fd=safe_file(p,O_RDWR|O_CREAT);if(fd<0)return -1;
    if(flock(fd,LOCK_EX|LOCK_NB)){close(fd);return -1;}return fd;
}
static int outbox_sync(void) {
    char p[768];if(!path(p,sizeof p,root,"private"))return 0;
    int fd=open(p,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);if(fd<0)return 0;
    int ok=!fsync(fd);close(fd);return ok;
}
static int outbox_hidden(const char *id) {
    char p[768],b[4]={0};if(!outbox_path(p,"sms-hidden-",id))return -1;
    errno=0;int fd=safe_file(p,O_RDONLY);if(fd<0)return errno==ENOENT?0:-1;
    ssize_t n=read(fd,b,sizeof b);close(fd);return n==2&&!memcmp(b,"1\n",2)?1:-1;
}
/* Timestamp is separate from the action's mtime, which changes on completion.
 * Failed preflight requests may have a stamp but no action and are not listed. */
static int outbox_stamp(void) {
    const char *id=field("requestId"),*pdus=field("pdus"),*sim=field("sim");
    if(!outbox_id(id)||!*pdus||strlen(pdus)>3072||strlen(sim)>32)return 0;
    char p[768];if(!outbox_path(p,"sms-start-",id))return 0;
    int lock=outbox_lock();if(lock<0)return 0;
    errno=0;int fd=safe_file(p,O_RDONLY);if(fd>=0){close(fd);close(lock);return 1;}
    if(errno!=ENOENT){close(lock);return 0;}
    char record[64];snprintf(record,sizeof record,"%lld\n",(long long)time(NULL));
    int ok=atomic_write(p,record)&&outbox_sync();close(lock);return ok;
}
typedef struct {char id[33];long long timestamp;int exact;} OutboxIndex;
static long long outbox_time(const char *id,const struct stat *info,int *exact) {
    char p[768],b[64]={0},extra;long long stamp=0;*exact=0;
    if(outbox_path(p,"sms-start-",id)){int fd=safe_file(p,O_RDONLY);if(fd>=0){ssize_t n=read(fd,b,sizeof b-1);close(fd);if(n>0&&sscanf(b,"%lld %c",&stamp,&extra)==1&&stamp>0){*exact=1;return stamp;}}}
    return (long long)info->st_mtime;
}
static int outbox_compare(const void *x,const void *y) {
    const OutboxIndex *a=x,*b=y;if(a->timestamp!=b->timestamp)return a->timestamp>b->timestamp?-1:1;return strcmp(a->id,b->id);
}
static int outbox_record(const char *id,char sim[34],char pdus[3073],char result[4097]) {
    char p[768],raw[8192]={0};if(!outbox_path(p,"action-sms-",id))return 0;
    int fd=safe_file(p,O_RDONLY);if(fd<0)return 0;size_t used=0;int ok=1;
    while(used<sizeof raw-1){ssize_t n=read(fd,raw+used,sizeof raw-1-used);if(n<0&&errno==EINTR)continue;if(n<0){ok=0;break;}if(!n)break;used+=(size_t)n;}close(fd);
    if(!ok||used==sizeof raw-1||strlen(raw)!=used)return 0;
    char *newline=strchr(raw,'\n');if(!newline)return 0;*newline=0;char *colon=strrchr(raw,':');
    if(!colon||colon-raw<1||colon-raw>32||strlen(colon+1)>3072||!*++colon)return 0;
    size_t n=(size_t)(colon-raw-1);memcpy(sim,raw,n);sim[n]=0;
    const char *part=colon;unsigned parts=1;size_t digits=0;
    for(const char *c=part;*c;c++){if(*c==','){if(!digits||(digits&1)||digits>512||++parts>10)return 0;digits=0;}else{if(!isxdigit((unsigned char)*c))return 0;digits++;}}
    if(!digits||(digits&1)||digits>512)return 0;
    strcpy(pdus,part);
    const char *r=newline+1;whitespace(&r);size_t length=strlen(r);
    if(length>=4089)return 0;memcpy(result,r,length);memset(result+length,0,8);
    const char *end=result;if(*end!='{'||!oray_skip(&end,0))return 0;whitespace(&end);return !*end;
}
static void outbox_list(void) {
    const char *cursor=field("cursor");char *end;unsigned long offset=strtoul(*cursor?cursor:"0",&end,10);
    if(*end||offset>512){error_response(400,"发送记录分页参数无效");return;}
    char dirpath[768];if(!path(dirpath,sizeof dirpath,root,"private")){error_response(503,"无法读取发送记录");return;}
    DIR *dir=opendir(dirpath);if(!dir){error_response(503,"无法读取发送记录");return;}
    OutboxIndex index[512];size_t count=0;int errors=0;struct dirent *entry;
    while((entry=readdir(dir))){
        if(strncmp(entry->d_name,"action-sms-",11)||!outbox_id(entry->d_name+11))continue;
        int hidden=outbox_hidden(entry->d_name+11);if(hidden){if(hidden<0)errors++;continue;}
        char p[768];outbox_path(p,"action-sms-",entry->d_name+11);int fd=safe_file(p,O_RDONLY);struct stat info;
        if(fd<0){errors++;continue;}int ok=!fstat(fd,&info);close(fd);if(!ok||count>=512){errors++;continue;}
        strcpy(index[count].id,entry->d_name+11);index[count].timestamp=outbox_time(index[count].id,&info,&index[count].exact);count++;
    }
    closedir(dir);qsort(index,count,sizeof index[0],outbox_compare);
    headers(200,NULL);printf("{\"ok\":true,\"total\":%zu,\"messages\":[",count);
    size_t limit=offset+8;if(limit>count)limit=count;int emitted=0;
    for(size_t i=offset;i<limit;i++){
        char sim[34],pdus[3073],result[4097],value[32],detail[1024]={0};int sent=-1,total=1;
        if(!outbox_record(index[i].id,sim,pdus,result)){errors++;continue;}
        for(char *c=pdus;*c;c++)if(*c==',')total++;
        int known=oray_member(result,"ok",value,sizeof value,0)&&(!strcmp(value,"true")||!strcmp(value,"false"));
        int success=known&&!strcmp(value,"true");
        int uncertain=!known||(oray_member(result,"uncertain",value,sizeof value,0)&&!strcmp(value,"true"));
        if(!oray_number(result,"sentParts",&sent))sent=success?total:0;
        int known_total;if(oray_number(result,"totalParts",&known_total)&&known_total>0&&known_total<=10)total=known_total;
        if(sent<0||sent>total){sent=0;uncertain=1;success=0;}
        oray_member(result,"error",detail,sizeof detail,1);
        const char *status=success?"submitted":uncertain?"uncertain":sent>0?"partial":"failed";
        if(emitted++)putchar(',');printf("{\"id\":");json_string(index[i].id);printf(",\"pdus\":");json_string(pdus);
        printf(",\"timestamp\":%lld,\"timeExact\":%s,\"currentSIM\":%s,\"status\":",index[i].timestamp,index[i].exact?"true":"false",!*field("sim")?"null":!strcmp(sim,field("sim"))?"true":"false");json_string(status);
        printf(",\"sentParts\":%d,\"totalParts\":%d,\"error\":",sent,total);json_string(detail);putchar('}');
    }
    printf("],\"readErrors\":%d,\"next\":",errors);if(limit<count)printf("%zu",limit);else printf("null");puts("}");
}
static void outbox_delete(void) {
    const char *id=field("id");if(!outbox_id(id)){error_response(400,"发送记录编号无效");return;}
    int lock=outbox_lock();if(lock<0){error_response(409,"发送记录正在更新，请稍后重试");return;}
    char p[768],sim[34],pdus[3073],result[4097];int hidden=outbox_hidden(id);
    if(hidden<0||(!hidden&&!outbox_record(id,sim,pdus,result))){close(lock);error_response(404,"发送记录不存在或暂不可读取");return;}
    int ok=hidden==1||(outbox_path(p,"sms-hidden-",id)&&atomic_write(p,"1\n")&&outbox_sync());close(lock);
    if(!ok){error_response(503,"发送记录删除未确认，请刷新核对");return;}
    headers(200,NULL);puts("{\"ok\":true,\"deleted\":true}");
}
