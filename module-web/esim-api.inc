/* Only validated named operations reach a detached worker. Secret inputs are
 * sent through a pipe; job files contain a hash, ID, and fixed progress text. */
static int esim_operation_lock(void) {
    char p[768];if(!path(p,sizeof p,runtime,"esim-operation.lock"))return -1;
    return safe_file(p,O_RDWR|O_CREAT);
}
static int esim_operation_busy(void) {
    int fd=esim_operation_lock();if(fd<0)return 1;
    int busy=flock(fd,LOCK_EX|LOCK_NB)!=0;close(fd);return busy;
}
static int esim_plain(const char *s,size_t max) {
    if(strlen(s)>max)return 0;
    for(const unsigned char *p=(const unsigned char *)s;*p;p++)if(*p<32||*p==127)return 0;
    return 1;
}
static int esim_decimal_id(const char *s,size_t min,size_t max) {
    size_t n=strlen(s);return n>=min&&n<=max&&strspn(s,"0123456789")==n;
}
static int esim_job_file(char *out,size_t cap,const char *suffix) {return path(out,cap,runtime,suffix);}
static void esim_job_response(void) {
    char p[768],record[2048];if(!esim_job_file(p,sizeof p,"esim-result")){error_response(503,"无法读取 eSIM 状态");return;}
    int fd=safe_file(p,O_RDONLY);if(fd<0){headers(200,NULL);puts("{\"ok\":true,\"state\":\"idle\"}");return;}
    ssize_t n=read(fd,record,sizeof record-1);close(fd);if(n<=0||n>=(ssize_t)sizeof record-1){error_response(503,"eSIM 状态记录不完整");return;}record[n]=0;
    if((strstr(record,"\"state\":\"running\"")||strstr(record,"\"state\":\"queued\""))&&!esim_operation_busy()){
        headers(200,NULL);puts("{\"ok\":true,\"state\":\"interrupted\",\"uncertain\":true,\"refreshRequired\":true,\"message\":\"eSIM 操作已中断，请重新读卡核对状态；请勿直接重复下载\"}");return;
    }
    headers(200,NULL);fputs(record,stdout);
}
static void esim_operation_start(void) {
    static const char *const keys[]={"operation","aid","eid","iccid","name","activationCode","confirmationCode","imei","requestId","csrfToken"};
    for(size_t i=0;i<field_count;i++){int known=0;for(unsigned j=0;j<sizeof keys/sizeof *keys;j++)if(!strcmp(fields[i].name,keys[j]))known=1;
        if(!known){error_response(400,"eSIM 操作包含无效字段");return;}}
    const char *op=field("operation"),*id=field("requestId");
    int download=!strcmp(op,"download"),notice=!strcmp(op,"notifications"),rename_op=!strcmp(op,"rename");
    if(!download&&!notice&&!rename_op&&strcmp(op,"enable")&&strcmp(op,"disable")&&strcmp(op,"delete")){error_response(400,"不支持的 eSIM 操作");return;}
    if(!valid_hex(id,64)||!esim_decimal_id(field("eid"),32,32)||strlen(field("aid"))<10||strlen(field("aid"))>32||
        strlen(field("aid"))%2||strspn(field("aid"),"0123456789abcdef")!=strlen(field("aid"))||
        (!download&&!notice&&!esim_decimal_id(field("iccid"),19,20))||
        !esim_plain(field("name"),64)||!esim_plain(field("activationCode"),1024)||
        !esim_plain(field("confirmationCode"),128)||!esim_plain(field("imei"),15)||
        (rename_op&&!*field("name"))||(download&&strncmp(field("activationCode"),"LPA:1$",6))){error_response(400,"eSIM 操作参数无效");return;}
    char wire[1536];int len=snprintf(wire,sizeof wire,"%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n",op,field("aid"),field("eid"),field("iccid"),field("name"),field("activationCode"),field("confirmationCode"),field("imei"));
    if(len<0||(size_t)len>=sizeof wire){error_response(400,"eSIM 操作参数过长");return;}
    unsigned char digest[32];char hash[65],ledger[768],suffix[96],record[128];
    if(mbedtls_md(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),(unsigned char *)wire,(size_t)len,digest)){error_response(503,"无法校验 eSIM 请求");return;}
    encode_hex(digest,32,hash);snprintf(suffix,sizeof suffix,"esim-request-%s",id);
    if(!esim_job_file(ledger,sizeof ledger,suffix)){error_response(503,"无法记录 eSIM 请求");return;}
    int lock=esim_operation_lock();if(lock<0){error_response(503,"无法锁定 eSIM 操作");return;}
    if(flock(lock,LOCK_EX|LOCK_NB)){
        int old=safe_file(ledger,O_RDONLY);ssize_t n=old>=0?read(old,record,sizeof record-1):-1;if(old>=0)close(old);
        if(n>0){record[n]=0;if(!strcmp(record,hash)){headers(202,NULL);printf("{\"ok\":true,\"accepted\":true,\"duplicate\":true,\"jobId\":\"%s\"}\n",id);close(lock);return;}}
        close(lock);error_response(409,"模块正在处理 eSIM 操作，请等待完成");return;
    }
    int previous=safe_file(ledger,O_RDONLY);if(previous>=0){ssize_t n=read(previous,record,sizeof record-1);close(previous);record[n>0?n:0]=0;
        close(lock);if(!strcmp(record,hash)){headers(202,NULL);printf("{\"ok\":true,\"accepted\":true,\"duplicate\":true,\"jobId\":\"%s\"}\n",id);}else error_response(409,"请求编号已用于其他操作，未重复执行");return;}
    /* Bound RAM records without forgetting accepted IDs during this boot. */
    DIR *directory=opendir(runtime);unsigned records=0;struct dirent *entry;
    if(!directory){close(lock);error_response(503,"无法检查请求记录，未执行操作");return;}
    while((entry=readdir(directory)))if(!strncmp(entry->d_name,"esim-request-",13)&&valid_hex(entry->d_name+13,64))records++;
    closedir(directory);if(records>=128){close(lock);error_response(503,"本次启动的 eSIM 请求记录已满，未执行操作");return;}
    char data_lock[768],helper[768];path(data_lock,sizeof data_lock,runtime,"data.lock");int data=safe_file(data_lock,O_RDWR|O_CREAT);
    if(data<0||flock(data,LOCK_EX|LOCK_NB)||data_job()){if(data>=0)close(data);close(lock);error_response(409,"模块正在切换联网状态，请稍后重试");return;}
    if(!path(helper,sizeof helper,root,"www/bin/qdc-esim")||access(helper,X_OK)){close(data);close(lock);error_response(503,"eSIM 管理程序未安装");return;}
    int pipes[2];if(pipe(pipes)){close(data);close(lock);error_response(503,"无法创建 eSIM 操作");return;}
    if(!atomic_write(ledger,hash)){close(pipes[0]);close(pipes[1]);close(data);close(lock);error_response(503,"无法记录请求，未执行操作");return;}
    char result[768],initial[512];esim_job_file(result,sizeof result,"esim-result");
    snprintf(initial,sizeof initial,"{\"ok\":true,\"jobId\":\"%s\",\"state\":\"queued\",\"stage\":\"checking\",\"message\":\"正在核对卡片…\",\"uncertain\":false}\n",id);
    if(!atomic_write(result,initial)){unlink(ledger);close(pipes[0]);close(pipes[1]);close(data);close(lock);error_response(503,"无法记录操作状态");return;}
    pid_t child=fork();if(!child){
        close(data);close(pipes[1]);setsid();dup2(pipes[0],STDIN_FILENO);close(pipes[0]);
        int sink=open("/dev/null",O_RDWR);if(sink>=0){dup2(sink,STDOUT_FILENO);dup2(sink,STDERR_FILENO);if(sink>2&&sink!=lock)close(sink);}
        dup2(lock,3);fcntl(3,F_SETFD,0);if(lock!=3)close(lock);
        execl(helper,helper,id,NULL);_exit(127);
    }
    close(pipes[0]);close(data);
    int sent=child>0&&write(pipes[1],wire,(size_t)len)==len;close(pipes[1]);memset(wire,0,sizeof wire);close(lock);
    if(!sent){error_response(503,"eSIM 操作启动未确认，请查看状态，勿重复提交");return;}
    headers(202,NULL);printf("{\"ok\":true,\"accepted\":true,\"jobId\":\"%s\"}\n",id);
}
