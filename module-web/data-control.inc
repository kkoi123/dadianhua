/* Mobile-data policy belongs to this application. Fixed vendor CLI commands
 * preserve the modem's default profile/APN; no firmware or radio-mode writes. */
static int data_file(char *p,size_t n,int persistent,const char *name){char suffix[128];snprintf(suffix,sizeof suffix,persistent?"private/%s":"%s",name);return path(p,n,persistent?root:runtime,suffix);}
static int data_policy(void){char p[768],b[8]={0};if(!data_file(p,sizeof p,1,"data-policy"))return -1;int fd=safe_file(p,O_RDONLY);if(fd<0)return errno==ENOENT?1:-1;ssize_t n=read(fd,b,sizeof b-1);close(fd);return n==2&&b[1]=='\n'&&(b[0]=='0'||b[0]=='1')?b[0]-'0':-1;}
static int usb_policy(void){char p[768],b[8]={0};if(!data_file(p,sizeof p,1,"usb-policy"))return -1;int fd=safe_file(p,O_RDONLY);if(fd<0)return errno==ENOENT?1:-1;ssize_t n=read(fd,b,sizeof b-1);close(fd);return n==2&&b[1]=='\n'&&(b[0]=='0'||b[0]=='1')?b[0]-'0':-1;}
/* Forwarding affects USB clients, not local INPUT/OUTPUT, radio or IMS.
 * This firmware has no filter table or policy routing. The cellular switch
 * remains the upstream gate; USB forwarding is an independent downstream gate. */
static int usb_proc(char p[768],int family){
#ifdef DATA_TEST
 const char *test=getenv("QDC_DATA_TEST_SYSCTL");if(test)return snprintf(p,768,"%s/ipv%d",test,family)<768;
#endif
 return snprintf(p,768,"/proc/sys/net/%s",family==4?"ipv4/ip_forward":"ipv6/conf/all/forwarding")<768;
}
static int usb_forward_value(int family){char p[768],b[16]={0};if(!usb_proc(p,family))return -1;int fd=open(p,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);if(fd<0)return -1;ssize_t n=read(fd,b,sizeof b-1);close(fd);int value=-1;char extra;if(n<=0||sscanf(b,"%d %c",&value,&extra)!=1||value<0||value>2)return -1;return value;}
static int usb_forwarding(void){int a=usb_forward_value(4),b=usb_forward_value(6);return a<0||b<0||!!a!=!!b?-1:!!a;}
static int usb_apply(int enabled){
 int a=usb_forward_value(4),b=usb_forward_value(6);if(a<0||b<0)return 0;
 char saved[768],record[32];if(!data_file(saved,sizeof saved,1,"usb-forwarding-baseline"))return 0;
 int first=safe_file(saved,O_RDONLY),on4=1,on6=1;
 if(first<0){if(errno!=ENOENT)return 0;on4=a?a:1;on6=b?b:1;snprintf(record,sizeof record,"%d %d\n",on4,on6);if(!atomic_write(saved,record))return 0;}
 else {char baseline[32]={0};ssize_t n=read(first,baseline,sizeof baseline-1);close(first);char extra;if(n<=0||sscanf(baseline,"%d %d %c",&on4,&on6,&extra)!=2||on4<1||on4>2||on6<1||on6>2)return 0;}
 for(int family=4;family<=6;family+=2){int current=family==4?a:b,wanted=enabled?(family==4?on4:on6):0;if((enabled&&current>0)||current==wanted)continue;
  /* The stock IPv6 sysctl reports only the consumed digit, excluding a
   * trailing newline. Send exactly the digit, then verify both families. */
  char p[768],value[4];if(!usb_proc(p,family))return 0;int fd=open(p,O_WRONLY|O_NOFOLLOW|O_CLOEXEC);if(fd<0)return 0;int n=snprintf(value,sizeof value,"%d",wanted);int ok=write(fd,value,(size_t)n)==n;close(fd);if(!ok)return 0;
 }
 return usb_forwarding()==enabled;
}
static int data_job(void){char p[768],b[64]={0};if(!data_file(p,sizeof p,0,"data-job"))return 0;int fd=safe_file(p,O_RDONLY);if(fd<0)return 0;ssize_t n=read(fd,b,sizeof b-1);close(fd);if(n<=0)return 0;uint64_t started=strtoull(b,NULL,10),now=now_seconds();return started<=now&&now-started<60;}
static int data_lock(void){char p[768];if(!data_file(p,sizeof p,0,"data.lock"))return -1;int fd=safe_file(p,O_RDWR|O_CREAT);if(fd<0)return -1;if(flock(fd,LOCK_EX|LOCK_NB)){close(fd);return -1;}return fd;}
/* Read and change the Linux data routes, including IPv6. The vendor manager
 * owns a different QMI client: its "Disconnected" status does not mean USB
 * traffic is disconnected. Keep the radio/IMS bearer and local USB routes. */
static int data_exec(char *const args[],char *out,size_t capacity,unsigned seconds){
 int pipes[2];if(pipe(pipes))return 0;pid_t pid=fork();if(pid<0){close(pipes[0]);close(pipes[1]);return 0;}
 if(!pid){setsid();dup2(pipes[1],STDOUT_FILENO);close(pipes[0]);close(pipes[1]);int sink=open("/dev/null",O_RDWR);if(sink>=0){dup2(sink,STDIN_FILENO);dup2(sink,STDERR_FILENO);close(sink);}execv(args[0],args);_exit(127);}
 close(pipes[1]);size_t used=0;int complete=0;uint64_t start=now_seconds();
 while(used<capacity-1&&now_seconds()-start<seconds){struct pollfd p={pipes[0],POLLIN,0};int r=poll(&p,1,100);if(r<0&&errno==EINTR)continue;if(r<0)break;if(!r)continue;ssize_t n=read(pipes[0],out+used,capacity-1-used);if(n<0&&errno==EINTR)continue;if(n<0)break;if(!n){complete=1;break;}used+=(size_t)n;}
 close(pipes[0]);if(!complete){kill(-pid,SIGKILL);kill(pid,SIGKILL);}int status=0;while(waitpid(pid,&status,0)<0&&errno==EINTR){}out[used]=0;
 return complete&&WIFEXITED(status)&&!WEXITSTATUS(status);
}
static const char *data_ip(void){
#ifdef DATA_TEST
 if(getenv("QDC_DATA_TEST_IP"))return getenv("QDC_DATA_TEST_IP");
#endif
 return "/sbin/ip";
}
static int data_command(const char *command,const char *profile,const char *family,char *out,size_t capacity,unsigned seconds){
 const char *program="/usr/bin/ql_manager_cli";
#ifdef DATA_TEST
 if(getenv("QDC_DATA_TEST_CLI"))program=getenv("QDC_DATA_TEST_CLI");
#endif
 char *args[]={(char *)program,"data_call",(char *)command,(char *)profile,(char *)family,NULL,NULL};if(!strcmp(command,"up"))args[5]="0";
 return data_exec(args,out,capacity,seconds)&&!strstr(out,"failure")&&!strstr(out,"invalid")&&!strstr(out,"failed");
}
static int data_default(char profile[4],char family[8]){
 char b[2048];if(!data_command("default_profile",NULL,NULL,b,sizeof b,4))return 0;char *p=strstr(b,"profile index:"),*f=strstr(b,"ip family:");unsigned index=0;if(!p||!f||sscanf(p,"profile index: %u",&index)!=1||index<1||index>16||sscanf(f,"ip family: %7s",family)!=1||(strcmp(family,"v4")&&strcmp(family,"v4v6")))return 0;snprintf(profile,4,"%u",index);return 1;
}
static int data_routes(int family,char out[8192]){
 char b[8192],*args[]={(char *)data_ip(),family==4?"-4":"-6","route","show",NULL};
 if(!data_exec(args,b,sizeof b,4))return -1;
 size_t used=0;int connected=0;char *save=NULL;
 for(char *line=strtok_r(b,"\n",&save);line;line=strtok_r(NULL,"\n",&save)){
  /* Read the full table: this firmware's ip show dev filter omits some
   * gateway/default routes. Never touch bridge0, ecm0 or IMS rmnet_data1. */
  char *dev=strstr(line," dev rmnet_data0");if(!dev||!(dev[16]==0||isspace((unsigned char)dev[16])))continue;
  size_t n=strlen(line);if(used+n+2>=8192)return -1;memcpy(out+used,line,n);used+=n;out[used++]='\n';
  if(!strncmp(line,"default ",8))connected=1;
 }
 out[used]=0;return connected;
}
static int data_connected(void){char v4[8192],v6[8192];int up=qdc_data_link(-1),a=data_routes(4,v4),b=data_routes(6,v6);return up<0||a<0||b<0?-1:up&&(a||b);}
static int data_idle(void){char helper[768],b[16385];if(!path(helper,sizeof helper,root,"www/cgi-bin/qdc-comm"))return 0;char *args[]={helper,"call-list",NULL};return data_exec(args,b,sizeof b,5)&&strstr(b,"\"ok\":true")&&strstr(b,"\"calls\":[]");}
/* Save addresses too: this kernel discards IPv6 addresses on link-down. */
static int data_cidr(const char *text,int family){
 char address[128];if(strlen(text)>=sizeof address)return 0;strcpy(address,text);char *slash=strchr(address,'/');if(!slash)return 0;*slash++=0;char *end;long prefix=strtol(slash,&end,10);unsigned char bytes[16];
 return *slash&&!*end&&prefix>=0&&prefix<=(family==4?32:128)&&inet_pton(family==4?AF_INET:AF_INET6,address,bytes)==1;
}
static int data_addresses(int family,char out[4096]){
 char b[8192],*args[]={(char *)data_ip(),family==4?"-4":"-6","addr","show","dev","rmnet_data0",NULL};if(!data_exec(args,b,sizeof b,4))return 0;
 size_t used=0;char *save=NULL;out[0]=0;
 for(char *line=strtok_r(b,"\n",&save);line;line=strtok_r(NULL,"\n",&save)){
  while(isspace((unsigned char)*line))line++;const char *tag=family==4?"inet ":"inet6 ";if(strncmp(line,tag,strlen(tag)))continue;
  char cidr[128];if(sscanf(line+strlen(tag),"%127s",cidr)!=1||!data_cidr(cidr,family))return 0;
  size_t n=strlen(cidr);if(used+n+2>=4096)return 0;memcpy(out+used,cidr,n);used+=n;out[used++]='\n';out[used]=0;
 }return 1;
}
static int data_global6(const char *cidr){
 char address[128];snprintf(address,sizeof address,"%s",cidr);char *slash=strchr(address,'/');if(slash)*slash=0;unsigned char bytes[16];
 return inet_pton(AF_INET6,address,bytes)==1&&bytes[0]!=0xff&&!(bytes[0]==0xfe&&(bytes[1]&0xc0)==0x80);
}
static int data_address_restore(int family){
 char name[40],p[768],b[4096],current[4096];snprintf(name,sizeof name,"data-ipv%d.addresses",family);if(!data_file(p,sizeof p,0,name))return 0;
 int fd=safe_file(p,O_RDONLY);if(fd<0)return errno==ENOENT;ssize_t n=read(fd,b,sizeof b-1);close(fd);if(n<0||n==(ssize_t)sizeof b-1)return 0;b[n]=0;
 if(!data_addresses(family,current))return 0;
 /* Validate the whole snapshot before any mutation. Restore at most one
  * global IPv6 address: stock netmgr may already assign a fresh one. Link
  * locals are regenerated by the kernel. Replaying every privacy address
  * would accumulate obsolete addresses with each OFF/ON cycle. */
 char validation[4096];memcpy(validation,b,(size_t)n+1);char *save=NULL;
 for(char *cidr=strtok_r(validation,"\n",&save);cidr;cidr=strtok_r(NULL,"\n",&save))if(!data_cidr(cidr,family))return 0;
 if(family==6){char observed[4096];strcpy(observed,current);save=NULL;
  for(char *cidr=strtok_r(observed,"\n",&save);cidr;cidr=strtok_r(NULL,"\n",&save))if(data_global6(cidr))return 1;}
 save=NULL;
 for(char *cidr=strtok_r(b,"\n",&save);cidr;cidr=strtok_r(NULL,"\n",&save)){
  if(family==6&&!data_global6(cidr))continue;
  char needle[132];snprintf(needle,sizeof needle,"%s\n",cidr);int exists=!strncmp(current,needle,strlen(needle));char middle[134];snprintf(middle,sizeof middle,"\n%s",needle);exists|=strstr(current,middle)!=NULL;if(exists)continue;
  char result[512],*args[]={(char *)data_ip(),family==4?"-4":"-6","addr","add",cidr,"dev","rmnet_data0",NULL};
  if(!data_exec(args,result,sizeof result,4)){if(!data_addresses(family,current)||(!strstr(current,middle)&&strncmp(current,needle,strlen(needle))))return 0;}
  if(family==6)return 1;
 }return 1;
}
static int data_route_restore(int family){
 char name[32],p[768],b[8192];snprintf(name,sizeof name,"data-ipv%d.routes",family);if(!data_file(p,sizeof p,0,name))return 0;
 int fd=safe_file(p,O_RDONLY);if(fd<0)return errno==ENOENT;ssize_t n=read(fd,b,sizeof b-1);close(fd);if(n<0||n==(ssize_t)sizeof b-1)return 0;b[n]=0;
 /* ip route show prints the default first. After a flush, its gateway is
  * unreachable until the directly connected subnet has been restored. */
 for(int defaults=0;defaults<=1;defaults++){
 char work[8192];memcpy(work,b,(size_t)n+1);char *line_save=NULL;
 for(char *line=strtok_r(work,"\n",&line_save);line;line=strtok_r(NULL,"\n",&line_save)){
  while(*line==' ')line++;if(!*line)continue;
  if((!strncmp(line,"default ",8))!=defaults)continue;
  /* Own mode-0600 runtime snapshots. Still validate tokens and invoke ip
   * directly: no shell parsing, commands, path changes or arbitrary devices. */
  for(char *p=line;*p;p++)if(!isalnum((unsigned char)*p)&&!strchr(" :./_-",*p))return 0;
  char *args[40]={(char *)data_ip(),family==4?"-4":"-6","route","replace"},*save=NULL;unsigned used=4;int device=0;
  for(char *word=strtok_r(line," ",&save);word;word=strtok_r(NULL," ",&save)){
   if(used>=39||!strcmp(word,"table")||!strcmp(word,"nexthop"))return 0;
   if(used>4&&!strcmp(args[used-1],"dev")){if(strcmp(word,"rmnet_data0"))return 0;device=1;}args[used++]=word;
  }
  if(!device)return 0;args[used]=NULL;char result[1024];if(!data_exec(args,result,sizeof result,4))return 0;
 }
 }
 return 1;
}
static int data_apply(int enabled){
 char v4[8192],v6[8192];int up=qdc_data_link(-1),a=data_routes(4,v4),b=data_routes(6,v6);if(up<0||a<0||b<0)return 0;
 if(!enabled){
  /* Capture before link-down, preserving the last useful snapshot on repeat. */
  for(int family=4;up&&family<=6;family+=2){const char *routes=family==4?v4:v6;char name[40],p[768],addresses[4096];
   if(*routes){snprintf(name,sizeof name,"data-ipv%d.routes",family);if(!data_file(p,sizeof p,0,name)||!atomic_write(p,routes))return 0;}
   if(!data_addresses(family,addresses))return 0;
   if(*addresses){snprintf(name,sizeof name,"data-ipv%d.addresses",family);if(!data_file(p,sizeof p,0,name)||!atomic_write(p,addresses))return 0;}
  }
  if(up&&qdc_data_link(0)!=0)return 0;
  /* Clean any retained routes as well. No per-application network controls. */
  for(int family=4;family<=6;family+=2){char result[1024];if(data_routes(family,family==4?v4:v6)<0)return 0;if(!*(family==4?v4:v6))continue;
   char *args[]={(char *)data_ip(),family==4?"-4":"-6","route","flush","dev","rmnet_data0",NULL};if(!data_exec(args,result,sizeof result,5))return 0;
  }
  if(data_routes(4,v4)<0||data_routes(6,v6)<0)return 0;return qdc_data_link(-1)==0&&!*v4&&!*v6;
 }
 if(up&&(a||b))return 1;
 if(qdc_data_link(1)!=1)return 0;
 /* Address assignment can overlap stock netmgr. Restore addresses before
  * subnets/defaults; retry briefly rather than report a premature success. */
 int restored=0;
 for(unsigned i=0;i<8;i++){
  restored=data_address_restore(4)&&data_address_restore(6)&&data_route_restore(4)&&data_route_restore(6);
  if(restored)break;struct timespec pause={0,250000000};nanosleep(&pause,NULL);
 }
 if(!restored)return 0;
 if(data_routes(4,v4)==1)return qdc_data_link(-1)==1;
 /* Cold boot has no snapshot: only the existing configured default profile. */
 char profile[4],family[8],result[2048];if(!data_default(profile,family)||!data_command("up",profile,family,result,sizeof result,12))return 0;
 for(unsigned i=0;i<8;i++){if(data_connected()==1)return 1;struct timespec pause={0,250000000};nanosleep(&pause,NULL);}return 0;
}
static int data_reconcile(int worker){
 if(worker){struct timespec pause={2,0};nanosleep(&pause,NULL);}
 int lock=data_lock();if(lock<0)return 1;if(!worker&&(data_job()||tunnel_job())){close(lock);return 0;}
 int enabled=data_policy(),usb=usb_policy(),roam_policy=roaming_policy();
 int refuse=0;
 if(worker&&!enabled){char p[768],b[64]={0};if(data_file(p,sizeof p,0,"data-job")){int fd=safe_file(p,O_RDONLY);if(fd>=0){read(fd,b,sizeof b-1);close(fd);}}
  if(strstr(b," cellular")&&!data_idle()){if(data_file(p,sizeof p,1,"data-policy")&&atomic_write(p,"1\n"))enabled=1;refuse=1;}}
 if(worker&&roam_policy==0){char p[768],b[64]={0};if(data_file(p,sizeof p,0,"data-job")){int fd=safe_file(p,O_RDONLY);if(fd>=0){read(fd,b,sizeof b-1);close(fd);}}
  if(strstr(b," roaming")&&!data_idle()){if(data_file(p,sizeof p,1,"roaming-policy")&&atomic_write(p,"1\n"))roam_policy=1;refuse=1;}}
 RoamingState roaming=roaming_state_read();int permission=roaming_permission(roam_policy,&roaming);
 int native_ok=roam_policy==2||(roam_policy>=0&&roaming_configure(&roaming,permission));
 int effective=roaming_effective(enabled,permission,roaming.state);
 char gate[768];int published=data_file(gate,sizeof gate,0,"roaming-blocked")&&atomic_write(gate,roaming_effective(1,permission,roaming.state)?"0\n":"1\n");
 int usb_ok=usb>=0&&usb_apply(usb),gate_ok=enabled>=0&&permission>=0&&published&&data_apply(effective);
 int ok=gate_ok&&native_ok;
 if(permission<0||!published){data_apply(0);ok=0;}if(refuse)ok=0;
 if(worker){char p[768],record[64];if(data_file(p,sizeof p,0,"data-result")){snprintf(record,sizeof record,"%d %d\n",enabled,ok);atomic_write(p,record);}if(data_file(p,sizeof p,0,"usb-result")){snprintf(record,sizeof record,"%d %d\n",usb,usb_ok);atomic_write(p,record);}if(data_file(p,sizeof p,0,"data-job"))unlink(p);}
 close(lock);return ok&&usb_ok?0:1;
}
static void data_response(void){
 int pending=data_job();int connected=!pending?data_connected():-1;
 /* A state sample that overlaps a switch must be refreshed once more. */
 if(data_job())pending=1;int enabled=data_policy();
 int last=-1,target=-1;char p[768];if(data_file(p,sizeof p,0,"data-result")){int fd=safe_file(p,O_RDONLY);if(fd>=0){FILE *f=fdopen(fd,"r");if(f){if(fscanf(f,"%d %d",&target,&last)!=2)last=-1;fclose(f);}else close(fd);}}
 int roam_policy=roaming_policy();RoamingState roaming=roaming_state_read();int permission=roaming_permission(roam_policy,&roaming);
 int effective=roaming_effective(enabled,permission,roaming.state),blocked=enabled==1&&!effective;
 if(!pending&&enabled>=0&&connected>=0&&(effective!=connected||(!effective&&qdc_data_link(-1)!=0))){target=enabled;last=0;}
 int usb=usb_policy(),forward=!pending?usb_forwarding():-1,usb_last=-1,usb_target=-1;
 if(data_file(p,sizeof p,0,"usb-result")){int fd=safe_file(p,O_RDONLY);if(fd>=0){FILE *f=fdopen(fd,"r");if(f){if(fscanf(f,"%d %d",&usb_target,&usb_last)!=2)usb_last=-1;fclose(f);}else close(fd);}}
 if(!pending&&usb>=0&&forward>=0&&usb!=forward){usb_target=usb;usb_last=0;}
 headers(200,NULL);printf("{\"ok\":true,\"enabled\":%s,\"connected\":%s,\"pending\":%s,\"lastSuccess\":%s,\"usbEnabled\":%s,\"usbForwarding\":%s,\"usbConnected\":%s,\"usbLastSuccess\":%s",enabled<0?"null":enabled?"true":"false",connected<0?"null":connected?"true":"false",pending?"true":"false",last<0||target!=enabled?"null":last?"true":"false",usb<0?"null":usb?"true":"false",forward<0?"null":forward?"true":"false",forward<0||connected<0?"null":forward&&connected?"true":"false",usb_last<0||usb_target!=usb?"null":usb_last?"true":"false");
 printf(",\"roamingEnabled\":%s,\"roamingApplied\":%s,\"roaming\":%s,\"roamingBlocked\":%s,\"effectiveEnabled\":%s",permission<0||(roam_policy==2&&roaming.permission<0)?"null":permission?"true":"false",roaming.permission<0?"null":roaming.permission?"true":"false",roaming.state<0?"null":roaming.state?"true":"false",blocked?"true":"false",enabled<0||permission<0?"null":effective?"true":"false");compat_fields();puts("}");
}
static void data_set(void){
 if(esim_operation_busy()){error_response(409,"请等待 eSIM 操作完成后再切换联网设置");return;}
 if(!strcmp(field("scope"),"compat")){compat_set();return;}
 const char *scope=field("scope");int usb=!strcmp(scope,"usb"),roam=!strcmp(scope,"roaming");if(*scope&&!usb&&!roam&&strcmp(scope,"cellular")){error_response(400,"联网开关类型无效");return;}
 const char *value=field("enabled");if(strcmp(value,"0")&&strcmp(value,"1")){error_response(400,"数据开关参数无效");return;}
 int lock=data_lock();if(lock<0){error_response(409,"正在切换数据，请稍后刷新");return;}if(data_job()||tunnel_job()){close(lock);error_response(409,"正在切换数据，请稍后刷新");return;}
 /* Refuse a disconnect during a call, including callers bypassing the UI. */
 if(!usb&&!strcmp(value,"0")){char helper[768],b[16385];if(!path(helper,sizeof helper,root,"www/cgi-bin/qdc-comm")){close(lock);error_response(503,"无法确认电话状态，未关闭数据");return;}
 int fds[2];if(pipe(fds)){close(lock);error_response(503,"无法确认电话状态，未关闭数据");return;}pid_t check=fork();if(!check){close(fds[0]);dup2(fds[1],STDOUT_FILENO);close(fds[1]);close(lock);execl(helper,helper,"call-list",NULL);_exit(127);}close(fds[1]);size_t used=0;int done=0;uint64_t start=now_seconds();if(check>0)while(used<sizeof b-1&&now_seconds()-start<5){struct pollfd p={fds[0],POLLIN,0};int r=poll(&p,1,100);if(r<0&&errno==EINTR)continue;if(r<0)break;if(!r)continue;ssize_t n=read(fds[0],b+used,sizeof b-1-used);if(n<=0){done=n==0;break;}used+=(size_t)n;}close(fds[0]);int status=0;if(check>0){if(!done)kill(check,SIGKILL);while(waitpid(check,&status,0)<0&&errno==EINTR){}}b[used]=0;
 if(check<=0||!done||!WIFEXITED(status)||WEXITSTATUS(status)||!strstr(b,"\"ok\":true")||!strstr(b,"\"calls\":[]")){close(lock);error_response(409,"请先结束电话；无法确认空闲时不会关闭数据");return;}}
 if(roam){RoamingState state=roaming_state_read();if(state.permission<0){close(lock);error_response(503,"模块的数据漫游权限未读取，未修改设置");return;}}
 char policy[768],job[768],record[64];int old=roam?roaming_policy():usb?usb_policy():data_policy();if(old<0||!data_file(policy,sizeof policy,1,roam?"roaming-policy":usb?"usb-policy":"data-policy")||!data_file(job,sizeof job,0,"data-job")){close(lock);error_response(503,"数据设置暂不可用");return;}
 snprintf(record,sizeof record,"%llu %s\n",(unsigned long long)now_seconds(),roam?"roaming":usb?"usb":"cellular");if(!atomic_write(job,record)){close(lock);error_response(503,"无法创建数据切换任务");return;}
 char desired[3]={value[0],'\n',0};if(!atomic_write(policy,desired)){if(roam&&old==2)unlink(policy);else{desired[0]=(char)('0'+old);atomic_write(policy,desired);}unlink(job);close(lock);error_response(503,"数据设置保存失败");return;}
 char program[768];if(!path(program,sizeof program,root,"www/cgi-bin/api")){unlink(job);close(lock);error_response(503,"数据程序路径无效");return;}
 pid_t pid=fork();if(!pid){close(lock);setsid();int sink=open("/dev/null",O_RDWR);if(sink>=0){dup2(sink,STDIN_FILENO);dup2(sink,STDOUT_FILENO);dup2(sink,STDERR_FILENO);if(sink>2)close(sink);}execl(program,program,"--data-apply",NULL);_exit(127);}
 if(pid<0){if(roam&&old==2)unlink(policy);else{desired[0]=(char)('0'+old);atomic_write(policy,desired);}unlink(job);close(lock);error_response(503,"无法启动数据切换任务");return;}
 close(lock);headers(202,NULL);printf("{\"ok\":true,\"accepted\":true,\"enabled\":%s,\"usbEnabled\":%s,\"roamingEnabled\":%s,\"pending\":true}\n",(usb||roam?data_policy():value[0]-'0')==1?"true":"false",(usb?value[0]-'0':usb_policy())==1?"true":"false",roam?value[0]=='1'?"true":"false":"null");fflush(stdout);
}
