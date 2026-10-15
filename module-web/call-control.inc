/* Fixed, local-only hangup and read-only audio-check RPCs. The watcher owns
 * the existing VOICE client.
 * No new QMI discovery, command retries, or arbitrary modem operations. */
typedef struct { char magic[8]; uint64_t deadline; char call[4], request[33]; } HangupRequest;
static int control_lstat(const char *path,struct stat *st){
#if defined(__arm__) && defined(__GLIBC__)
 /* Match the same legacy ARM stat ABI used by compatible_fstat(). */
 extern int __lxstat(int,const char *,struct stat *);
 return __lxstat(3,path,st);
#else
 return lstat(path,st);
#endif
}
static uint64_t control_ms(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000+(unsigned)t.tv_nsec/1000000;}
static int control_peer(int fd){
#ifdef __APPLE__
 uid_t uid;gid_t gid;return !getpeereid(fd,&uid,&gid)&&uid==geteuid();
#else
 struct {pid_t pid;uid_t uid;gid_t gid;} cred;socklen_t n=sizeof cred;
 return !getsockopt(fd,SOL_SOCKET,SO_PEERCRED,&cred,&n)&&n==sizeof cred&&cred.uid==geteuid();
#endif
}
static int control_wait(int fd,short events,uint64_t deadline){
 for(;;){uint64_t now=control_ms();if(now>=deadline)return 0;struct pollfd p={fd,events,0};int r=poll(&p,1,(int)(deadline-now));if(r<0&&errno==EINTR)continue;return r>0&&(p.revents&events);}
}
static int control_transfer(int fd,void *buffer,size_t n,int sending,uint64_t deadline){
 size_t used=0;while(used<n){if(!control_wait(fd,sending?POLLOUT:POLLIN,deadline))return 0;
 ssize_t r=sending?send(fd,(char *)buffer+used,n-used,0):recv(fd,(char *)buffer+used,n-used,0);
 if(r<0&&(errno==EINTR||errno==EAGAIN||errno==EWOULDBLOCK))continue;if(r<=0)return 0;used+=(size_t)r;}return 1;
}
static int control_address(struct sockaddr_un *a){memset(a,0,sizeof *a);a->sun_family=AF_UNIX;return snprintf(a->sun_path,sizeof a->sun_path,"%s/call-control.sock",run)<(int)sizeof a->sun_path;}
static int control_socket(void){int fd=socket(AF_UNIX,SOCK_STREAM,0);if(fd>=0){fcntl(fd,F_SETFD,FD_CLOEXEC);fcntl(fd,F_SETFL,O_NONBLOCK);}return fd;}
static int control_listener(void){
 struct sockaddr_un a;struct stat st;if(!control_address(&a))return -1;
 if(!control_lstat(a.sun_path,&st)){if(!S_ISSOCK(st.st_mode)||st.st_uid!=geteuid()||(st.st_mode&077)||unlink(a.sun_path))return -1;}else if(errno!=ENOENT)return -1;
 int fd=control_socket();if(fd<0)return -1;if(bind(fd,(struct sockaddr *)&a,sizeof a)||chmod(a.sun_path,0600)||listen(fd,4)){close(fd);unlink(a.sun_path);return -1;}return fd;
}
static void control_client(const char *call,const char *id){
 if(!digits(call,1,3)||atoi(call)<1||atoi(call)>255||!hex(id,32,32)){error("电话操作参数无效");return;}
 signal(SIGPIPE,SIG_IGN);struct sockaddr_un a;struct stat st;
 if(!control_address(&a)||control_lstat(a.sun_path,&st)||!S_ISSOCK(st.st_mode)||st.st_uid!=geteuid()||(st.st_mode&077)){error("通话后台尚未就绪，请稍后刷新状态");return;}
 int fd=control_socket();uint64_t deadline=control_ms()+20000;
 if(fd<0){error("无法连接通话后台");return;}
 int connected=connect(fd,(struct sockaddr *)&a,sizeof a);if(connected&&errno==EINPROGRESS){int e=0;socklen_t n=sizeof e;connected=!(control_wait(fd,POLLOUT,deadline)&&!getsockopt(fd,SOL_SOCKET,SO_ERROR,&e,&n)&&!e);}
 if(connected||!control_peer(fd)){close(fd);error("通话后台尚未就绪，请稍后刷新状态");return;}
 HangupRequest request={0};memcpy(request.magic,"QDCEND1",8);request.deadline=deadline-8000;snprintf(request.call,sizeof request.call,"%s",call);snprintf(request.request,sizeof request.request,"%s",id);
 TRACE("forwarding hangup to persistent VOICE client");
 if(!control_transfer(fd,&request,sizeof request,1,deadline)){close(fd);error("挂断请求传输未完成，请刷新电话状态");return;}
 shutdown(fd,SHUT_WR);char result[1024];size_t used=0;int complete=0;
 while(used<sizeof result-1&&control_wait(fd,POLLIN,deadline)){ssize_t n=recv(fd,result+used,sizeof result-1-used,0);if(n<0&&(errno==EINTR||errno==EAGAIN))continue;if(n<=0)break;used+=(size_t)n;if(memchr(result,'\n',used)){complete=1;break;}}
 close(fd);result[used]=0;if(complete&&!strncmp(result,"{\"ok\":",6))fprintf(output,"%s",result);else fprintf(output,"{\"ok\":false,\"uncertain\":true,\"error\":\"挂断结果未确认，请刷新电话状态\"}\n");
}
static void control_audio_client(void){
 signal(SIGPIPE,SIG_IGN);struct sockaddr_un a;struct stat st;
 if(!control_address(&a)||control_lstat(a.sun_path,&st)||!S_ISSOCK(st.st_mode)||st.st_uid!=geteuid()||(st.st_mode&077)){error("通话后台尚未就绪");return;}
 int fd=control_socket();uint64_t deadline=control_ms()+8000;
 if(fd<0){error("无法连接通话后台");return;}
 int connected=connect(fd,(struct sockaddr *)&a,sizeof a);if(connected&&errno==EINPROGRESS){int e=0;socklen_t n=sizeof e;connected=!(control_wait(fd,POLLOUT,deadline)&&!getsockopt(fd,SOL_SOCKET,SO_ERROR,&e,&n)&&!e);}
 if(connected||!control_peer(fd)){close(fd);error("无法连接通话后台");return;}
 HangupRequest request={0};memcpy(request.magic,"QDCCK01",8);request.deadline=deadline;
 if(!control_transfer(fd,&request,sizeof request,1,deadline)){close(fd);error("通话状态查询传输超时");return;}
 shutdown(fd,SHUT_WR);char result[1024];size_t used=0;int complete=0;
 while(used<sizeof result-1&&control_wait(fd,POLLIN,deadline)){ssize_t n=recv(fd,result+used,sizeof result-1-used,0);if(n<0&&(errno==EINTR||errno==EAGAIN))continue;if(n<=0)break;used+=(size_t)n;if(memchr(result,'\n',used)){complete=1;break;}}
 close(fd);result[used]=0;if(complete&&!strncmp(result,"{\"ok\":",6))fprintf(output,"%s",result);else error("通话状态查询超时");
}
static int control_handle(int listener,Client *voice){
 int fd=accept(listener,NULL,NULL);if(fd<0)return 0;fcntl(fd,F_SETFD,FD_CLOEXEC);fcntl(fd,F_SETFL,O_NONBLOCK);
 HangupRequest request={0};if(!control_peer(fd)||!control_transfer(fd,&request,sizeof request,0,control_ms()+500)){close(fd);return 0;}
 int check=!memcmp(request.magic,"QDCCK01",8);
 if(check){char zero_call[4]={0},zero_request[33]={0};if(memcmp(request.call,zero_call,sizeof zero_call)||memcmp(request.request,zero_request,sizeof zero_request)){close(fd);return 0;}}
 else if(memcmp(request.magic,"QDCEND1",8)||request.call[3]||request.request[32]||!digits(request.call,1,3)||atoi(request.call)<1||atoi(request.call)>255||!hex(request.request,32,32)){close(fd);return 0;}
 char *buffer=NULL;size_t length=0;FILE *stream=open_memstream(&buffer,&length);if(!stream){close(fd);return 0;}FILE *previous=output;output=stream;transport_error=0;protocol_error=0;
 char file[640];snprintf(file,sizeof file,"%s/comm.lock",run);int lock=safe_open(file,O_RDWR|O_CREAT);
 if(request.deadline<=control_ms()||request.deadline>control_ms()+(check?8200:20000)){error(check?"通话状态查询已过期":"挂断请求已过期，请刷新电话状态");}
 else if(!operation_lock(lock,!check)){error("模块正在处理其他通讯操作，请稍后重试");}
 else if(check){
  /* Leave enough time for the 1500 ms read. A late queued request must not
   * run behind a long SMS operation or keep the shared lock unnecessarily. */
  if(request.deadline<control_ms()+1600)error("通话状态查询已过期");else call_audio_check(voice);
 }
 else if(request.deadline<=control_ms()){error("挂断请求已过期，请刷新电话状态");}
 else {TRACE("hangup using persistent VOICE client");call_action(voice,"call-end",request.call,request.request);}
 if(lock>=0)close(lock);output=previous;fclose(stream);
 if(buffer&&length<1024)(void)control_transfer(fd,buffer,length,1,control_ms()+500);free(buffer);close(fd);return check?2:1;
}
