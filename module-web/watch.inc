/* Persistent per-client event subscriptions. Never changes SMS routing or
 * acknowledges/deletes messages. Periodic reconciliation is entirely local.
 * Explicit hangup requests use a private socket and the existing VOICE client. */
static volatile sig_atomic_t watch_stop;
static int watch_pipe[2]={-1,-1};
static void watch_signal(int sig){(void)sig;watch_stop=1;}
static uint64_t watch_ms(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000+(unsigned)t.tv_nsec/1000000;}
static void indication(void *client,unsigned message,void *buffer,unsigned size,void *opaque){(void)client;(void)buffer;const char *kind=opaque;if(!kind||size>CAP||((kind[0]=='C'&&message!=0x2e)||(kind[0]=='S'&&message!=1)))return;char b=kind[0];(void)write(watch_pipe[1],&b,1);}
static int watch_connect(Client *c,const char *name,const char *kind){ServiceInfo info={{0}};void *object=dlsym(services,name);if(!object||get_service(object,&info)||init_client(&info,object,(void *)indication,(void *)kind,NULL,&c->handle))return 0;return 1;}
static uint64_t fingerprint(const unsigned char *b,size_t n){uint64_t h=UINT64_C(14695981039346656037);for(size_t i=0;i<n;i++)h=(h^b[i])*UINT64_C(1099511628211);return h;}
typedef struct{unsigned storage,tag;uint32_t index;uint64_t hash;} SeenSms;
static int watch_sms(Client *c,SeenSms *items,size_t *count,char sim[33]){Slot slots[1024];size_t total=0;*count=0;if(!identity(sim))return 0;
 for(unsigned store=0;store<2;store++){Packet req={0},resp={0};unsigned char storage=(unsigned char)store,mode=1;tlv(&req,1,&storage,1);tlv(&req,0x11,&mode,1);if(!request(c,0x31,&req,&resp,1500)||!append_slots(&resp,store,slots,&total))return 0;}
 for(size_t i=0;i<total;i++){unsigned char bytes[MAX_PDU];unsigned tag=0,format=0;size_t length=0;if(!raw_read(c,slots[i].storage,slots[i].index,bytes,&length,&tag,&format))return 0;items[(*count)++]=(SeenSms){slots[i].storage,tag,slots[i].index,fingerprint(bytes,length)};}
 return 1;}
static int watch_calls(Client *c,char *json,size_t capacity,unsigned char ringing[256]){char *buf=NULL;size_t length=0;FILE *stream=open_memstream(&buf,&length);if(!stream)return 0;FILE *previous=output;output=stream;QdcHistoryCall records[255];size_t count=0;int complete=call_list_to(c,records,&count);output=previous;fclose(stream);
 if(buf){buf[strcspn(buf,"\r\n")]=0;length=strlen(buf);}
 int ok=complete&&buf&&length<capacity&&!strncmp(buf,"{\"ok\":true",10);if(ok){snprintf(json,capacity,"%s",buf);memset(ringing,0,256);/* Parse only our own bounded serializer to deduplicate incoming call IDs. */
 const char *p=buf;while((p=strstr(p,"{\"id\":"))){unsigned id,state,direction;if(sscanf(p,"{\"id\":%u,\"state\":%u",&id,&state)==2&&id<256&&(state==2||state==7)){const char *end=strchr(p,'}'),*d=strstr(p,"\"direction\":");if(d&&end&&d<end&&sscanf(d,"\"direction\":%u",&direction)==1&&direction==2)ringing[id]=1;}p++;}}
 if(ok&&!qdc_history_snapshot(private_dir,run,records,count))fprintf(stderr,"Call history checkpoint failed; retry on next successful snapshot.\n");
 free(buf);return ok;}
static int watch_publish(const char *calls_json,const char *epoch,uint64_t call_rev,uint64_t sms_rev,int call_ready,int sms_ready,int call_registered,int sms_registered){char name[768],tmp[832];snprintf(name,sizeof name,"%s/events-state",run);snprintf(tmp,sizeof tmp,"%s.new.%ld",name,(long)getpid());int fd=safe_open(tmp,O_WRONLY|O_CREAT|O_EXCL);if(fd<0)return 0;FILE *f=fdopen(fd,"w");if(!f){close(fd);unlink(tmp);return 0;}
 int ok=fprintf(f,"{\"ok\":true,\"epoch\":\"%s\",\"callRevision\":%llu,\"smsRevision\":%llu,\"callsReady\":%s,\"smsReady\":%s,\"callRegistered\":%s,\"smsRegistered\":%s,\"callState\":%s}\n",epoch,(unsigned long long)call_rev,(unsigned long long)sms_rev,call_ready?"true":"false",sms_ready?"true":"false",call_registered?"true":"false",sms_registered?"true":"false",calls_json)>0;if(fclose(f))ok=0;if(ok&&rename(tmp,name))ok=0;if(!ok)unlink(tmp);return ok;}
static void watch_touch(void){struct timespec health_time;clock_gettime(CLOCK_MONOTONIC,&health_time);char file[768];snprintf(file,sizeof file,"%s/events-health",run);int fd=safe_open(file,O_WRONLY|O_CREAT|O_TRUNC);if(fd>=0){char b[48];int n=snprintf(b,sizeof b,"%llu\n",(unsigned long long)health_time.tv_sec);(void)write(fd,b,(size_t)n);close(fd);}}
/* Fast local enforcement of the generic internet gate. No application/Bark
 * controls and no QMI request. Share the switch lock to preserve snapshots. */
static void watch_data_gate(void){
 char file[768],b[64]={0};snprintf(file,sizeof file,"%s/data-policy",private_dir);int fd=safe_open(file,O_RDONLY);if(fd<0&&errno!=ENOENT)return;
 ssize_t n=-1;if(fd>=0){n=read(fd,b,sizeof b-1);close(fd);}int blocked=n==2&&!memcmp(b,"0\n",2);
 if(!blocked){
  /* A saved roaming OFF is blocked unless the reconciler confirmed home.
   * Missing policy inherits the modem permission and uses its runtime gate.
   * An explicit ON ignores a stale OFF snapshot during a policy change. */
  snprintf(file,sizeof file,"%s/roaming-policy",private_dir);fd=safe_open(file,O_RDONLY);
  int permission=2;if(fd>=0){memset(b,0,sizeof b);n=read(fd,b,sizeof b-1);close(fd);permission=n==2&&b[1]=='\n'&&(b[0]=='0'||b[0]=='1')?b[0]-'0':-1;}
  else if(errno!=ENOENT)permission=-1;
  if(permission==1)return;
  snprintf(file,sizeof file,"%s/roaming-blocked",run);fd=safe_open(file,O_RDONLY);int gate=-1;
  if(fd>=0){memset(b,0,sizeof b);n=read(fd,b,sizeof b-1);close(fd);if(n==2&&b[1]=='\n'&&(b[0]=='0'||b[0]=='1'))gate=b[0]-'0';}
  blocked=permission==0?gate!=0:permission<0||gate==1;if(!blocked)return;
 }
 snprintf(file,sizeof file,"%s/data.lock",run);int lock=safe_open(file,O_RDWR|O_CREAT);if(lock<0)return;if(flock(lock,LOCK_EX|LOCK_NB)){close(lock);return;}
 snprintf(file,sizeof file,"%s/data-job",run);fd=safe_open(file,O_RDONLY);if(fd>=0){memset(b,0,sizeof b);n=read(fd,b,sizeof b-1);close(fd);if(n>0){char *end;struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);uint64_t started=strtoull(b,&end,10),now=(uint64_t)t.tv_sec;if(end!=b&&started<=now&&now-started<60){close(lock);return;}}}
 if(qdc_data_link(-1)==1)(void)qdc_data_link(0);close(lock);
}
static int watch_run(const char *program){alarm(0);signal(SIGTERM,watch_signal);signal(SIGINT,watch_signal);signal(SIGPIPE,SIG_IGN);
 char file[768];snprintf(file,sizeof file,"%s/events-owner.lock",run);int owner=safe_open(file,O_RDWR|O_CREAT);if(owner<0||flock(owner,LOCK_EX|LOCK_NB)){if(owner>=0)close(owner);return 1;}
 if(pipe(watch_pipe)){close(owner);return 1;}for(int i=0;i<2;i++){fcntl(watch_pipe[i],F_SETFD,FD_CLOEXEC);fcntl(watch_pipe[i],F_SETFL,O_NONBLOCK);}
 Client voice={0},sms={0};if(!watch_connect(&voice,"voice_qmi_idl_service_object_v02","C")||!watch_connect(&sms,"wms_qmi_idl_service_object_v01","S")){disconnect(&voice);disconnect(&sms);close(owner);return 1;}
 int control=control_listener();if(control<0){disconnect(&voice);disconnect(&sms);close(watch_pipe[0]);close(watch_pipe[1]);close(owner);return 1;}
 unsigned char yes=1;Packet req={0},resp={0};tlv(&req,0x13,&yes,1);int call_registered=request(&voice,3,&req,&resp,1500);req.n=0;tlv(&req,0x10,&yes,1);int sms_registered=request(&sms,1,&req,&resp,1500);
 char epoch[48];snprintf(epoch,sizeof epoch,"%ld-%llu",(long)getpid(),(unsigned long long)watch_ms());
 char current[16384]="{\"ok\":true,\"calls\":[]}",latest[16384];SeenSms seen[1024],items[1024];size_t seen_count=0;char previous_sim[33]="",sim[33]="";unsigned char ringing[256]={0},new_ringing[256];
 int had_calls=0,had_sms=0,call_ready=0,sms_ready=0,old_call_ready=-1,old_sms_ready=-1;uint64_t call_rev=0,sms_rev=0,next_call=0,next_sms=0,next_health=0,next_config=0,next_data=0;QdcNotify config={0};pid_t notifier=0;unsigned pending=0;int dirty_call=1,dirty_sms=1,failures=0;
 while(!watch_stop){
  struct pollfd command={control,POLLIN,0};if(poll(&command,1,0)>0&&(command.revents&POLLIN)){watch_touch();if(control_handle(control,&voice)==1)dirty_call=1;watch_touch();}
  uint64_t now=watch_ms();int changed=0;
  if(now>=next_data){watch_data_gate();next_data=now+1000;}
  if(now>=next_config){QdcNotify fresh;if(qdc_notify_read(&fresh)){if(fresh.revision!=config.revision||fresh.enabled!=config.enabled){/* Establish a new baseline on enable/change: old messages must not alert. */had_sms=0;dirty_sms=1;pending=0;}config=fresh;}next_config=now+1000;}
  if(dirty_call||dirty_sms||now>=next_call||now>=next_sms){snprintf(file,sizeof file,"%s/comm.lock",run);int lock=safe_open(file,O_RDWR|O_CREAT);
   if(lock>=0&&!flock(lock,LOCK_EX|LOCK_NB)){
    if(dirty_call||now>=next_call){call_ready=watch_calls(&voice,latest,sizeof latest,new_ringing);if(call_ready){if(strcmp(latest,current)){strcpy(current,latest);call_rev++;changed=1;}for(unsigned i=1;i<256;i++)if(had_calls&&new_ringing[i]&&!ringing[i])pending|=1;memcpy(ringing,new_ringing,sizeof ringing);had_calls=1;}dirty_call=0;next_call=now+(strstr(current,"\"state\":")?4000:15000);}
    if(dirty_sms||now>=next_sms){size_t count=0;sms_ready=watch_sms(&sms,items,&count,sim);if(sms_ready){
      size_t previous_count=seen_count;
      int same_sim=had_sms&&!strcmp(sim,previous_sim);int new_message=0;int same=previous_count==count;for(size_t i=0;same&&i<count;i++){int found=0;for(size_t j=0;j<previous_count;j++)if(items[i].storage==seen[j].storage&&items[i].index==seen[j].index&&items[i].hash==seen[j].hash&&items[i].tag==seen[j].tag){found=1;break;}if(!found)same=0;}
      for(size_t i=0;same_sim&&i<count;i++){int found=0;for(size_t j=0;j<previous_count;j++)if(items[i].storage==seen[j].storage&&items[i].index==seen[j].index&&items[i].hash==seen[j].hash){found=1;break;}if(!found&&(items[i].tag==0||items[i].tag==1))new_message=1;}
      if(!had_sms||!same_sim||!same){sms_rev++;changed=1;}if(new_message)pending|=2;
      memcpy(seen,items,count*sizeof *seen);seen_count=count;strcpy(previous_sim,sim);had_sms=1;
     }dirty_sms=0;next_sms=now+30000;}
    close(lock);
   }else{if(lock>=0)close(lock);/* Let API operations finish; do not use the radio while they hold the lock. */}
  }
  if(call_ready!=old_call_ready||sms_ready!=old_sms_ready){changed=1;old_call_ready=call_ready;old_sms_ready=sms_ready;}
  if(changed&&!watch_publish(current,epoch,call_rev,sms_rev,call_ready,sms_ready,call_registered,sms_registered))break;
  if(now>=next_health){watch_touch();next_health=now+5000;}
  if(!call_ready&&!sms_ready){if(++failures>40)break;}else failures=0;
  if(notifier>0){int status;if(waitpid(notifier,&status,WNOHANG)!=0)notifier=0;}
  if(!notifier&&pending){unsigned event=pending&1?1:2;pending&=~event;if(config.enabled&&((event==1&&config.calls)||(event==2&&config.sms))){notifier=fork();if(!notifier){close(control);close(owner);close(watch_pipe[0]);close(watch_pipe[1]);signal(SIGTERM,SIG_DFL);char *args[]={(char *)program,"notify-send",event==1?"call":"sms",NULL};execv(program,args);_exit(127);}if(notifier<0)notifier=0;}}
  struct pollfd p[2]={{watch_pipe[0],POLLIN,0},{control,POLLIN,0}};int ready=poll(p,2,250);if(ready>0&&(p[0].revents&POLLIN)){char b[256];ssize_t n;while((n=read(watch_pipe[0],b,sizeof b))>0)for(ssize_t i=0;i<n;i++){if(b[i]=='C')dirty_call=1;if(b[i]=='S')dirty_sms=1;}}
 }
 if(notifier>0){kill(notifier,SIGTERM);while(waitpid(notifier,NULL,0)<0&&errno==EINTR){}}
 close(control);struct sockaddr_un address;if(control_address(&address))unlink(address.sun_path);
 disconnect(&voice);disconnect(&sms);close(watch_pipe[0]);close(watch_pipe[1]);close(owner);snprintf(file,sizeof file,"%s/events-health",run);unlink(file);return 0;
}
