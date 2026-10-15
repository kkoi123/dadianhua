/* Authenticated SSE with no full-state polling across the mobile link. */
static int event_text(char *buf,size_t capacity){char file[768];if(!path(file,sizeof file,runtime,"events-state"))return 0;int fd=safe_file(file,O_RDONLY);if(fd<0)return 0;ssize_t n=read(fd,buf,capacity-1);close(fd);if(n<=0||(size_t)n==capacity-1||buf[0]!='{')return 0;buf[n]=0;buf[strcspn(buf,"\r\n")]=0;return 1;}
static int events_healthy(void){char file[768],b[64];if(!path(file,sizeof file,runtime,"events-health"))return 0;int fd=safe_file(file,O_RDONLY);if(fd<0)return 0;ssize_t n=read(fd,b,sizeof b-1);close(fd);if(n<=0)return 0;b[n]=0;uint64_t value=strtoull(b,NULL,10),now=now_seconds();return value<=now&&now-value<=45;}
static void events_response(const char *sid){char file[768];int lock=-1;for(int i=0;i<2;i++){int n=snprintf(file,sizeof file,"%s/events-client-%s-%d",runtime,sid,i);if(n<0||(size_t)n>=sizeof file)break;int fd=safe_file(file,O_RDWR|O_CREAT);if(fd>=0){if(!flock(fd,LOCK_EX|LOCK_NB)){lock=fd;break;}close(fd);}}
 if(lock<0){error_response(429,"已打开多个实时页面，请关闭多余页面再试");return;}
 alarm(0);signal(SIGPIPE,SIG_IGN);printf("Status: 200\r\nContent-Type: text/event-stream; charset=utf-8\r\nCache-Control: no-store\r\nX-Accel-Buffering: no\r\nX-Content-Type-Options: nosniff\r\n\r\nretry: 10000\n\n");fflush(stdout);
 char previous[18000]="",snapshot[18000];int old_health=-1;
 for(;;){struct pollfd output={STDOUT_FILENO,POLLOUT,0};if(poll(&output,1,0)<0||output.revents&(POLLERR|POLLHUP|POLLNVAL))break;
  Credential credential;Session session;char current_sid[65];if(!authenticate(&credential,&session,current_sid)||strcmp(sid,current_sid)){fputs("event: expired\ndata: {}\n\n",stdout);fflush(stdout);break;}
  int healthy=events_healthy();int have=event_text(snapshot,sizeof snapshot);
  if(healthy!=old_health){printf("event: availability\ndata: {\"ready\":%s}\n\n",healthy&&have?"true":"false");old_health=healthy;}
  if(healthy&&have&&strcmp(previous,snapshot)){printf("event: state\ndata: %s\n\n",snapshot);strcpy(previous,snapshot);}
  if(fflush(stdout)||ferror(stdout))break;
#ifdef EVENTS_TEST
  if(getenv("QDC_EVENTS_TEST_ONCE"))break;
#endif
  struct timespec pause={0,250000000};nanosleep(&pause,NULL);
 }
 close(lock);
}
static void notify_response(void){QdcNotify config;if(!qdc_notify_read(&config)){error_response(503,"提醒设置读取失败");return;}char file[768];long long sent=0;int ok=-1;if(path(file,sizeof file,runtime,"notify-result")){int fd=safe_file(file,O_RDONLY);if(fd>=0){FILE *f=fdopen(fd,"r");if(f){if(fscanf(f,"notify-result-v1 %lld %d",&sent,&ok)!=2){sent=0;ok=-1;}fclose(f);}else close(fd);}}
 headers(200,NULL);printf("{\"ok\":true,\"enabled\":%s,\"calls\":%s,\"sms\":%s,\"configured\":%s,\"watchReady\":%s,\"lastAttempt\":%lld,\"lastSuccess\":%s}\n",config.enabled?"true":"false",config.calls?"true":"false",config.sms?"true":"false",*config.key?"true":"false",events_healthy()?"true":"false",sent,ok<0?"null":ok?"true":"false");memset(&config,0,sizeof config);}
static int flag_field(const char *name){return !strcmp(field(name),"0")||!strcmp(field(name),"1");}
static void notify_settings(void){if(!flag_field("enabled")||!flag_field("calls")||!flag_field("sms")||!flag_field("removeKey")){error_response(400,"提醒选项无效");return;}
 char file[768];if(!path(file,sizeof file,runtime,"notify-config.lock")){error_response(503,"提醒设置暂不可用");return;}int lock=safe_file(file,O_RDWR|O_CREAT);if(lock<0||flock(lock,LOCK_EX|LOCK_NB)){if(lock>=0)close(lock);error_response(503,"正在保存提醒设置，请稍后重试");return;}
 QdcNotify config;if(!qdc_notify_read(&config)){close(lock);error_response(503,"提醒设置读取失败");return;}
 const char *url=field("barkURL");if(!strcmp(field("removeKey"),"1"))config.key[0]=0;
 if(*url){const char *prefix="https://api.day.app/";if(strncmp(url,prefix,strlen(prefix))){close(lock);error_response(400,"请填写 Bark 官方推送地址 https://api.day.app/设备密钥");return;}const char *key=url+strlen(prefix);size_t n=strlen(key);if(n&&key[n-1]=='/')n--;if(n<5||n>128||strspn(key,"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-")!=n){close(lock);error_response(400,"Bark 地址应只包含设备密钥，不要附加内容或参数");return;}memcpy(config.key,key,n);config.key[n]=0;}
 config.enabled=(unsigned)atoi(field("enabled"));config.calls=(unsigned)atoi(field("calls"));config.sms=(unsigned)atoi(field("sms"));config.revision++;
 if(config.enabled&&!*config.key){memset(&config,0,sizeof config);close(lock);error_response(400,"请先填写 Bark 推送地址");return;}
 int ok=qdc_notify_write(&config);memset(&config,0,sizeof config);close(lock);if(!ok){error_response(503,"提醒设置保存失败");return;}notify_response();}
static void notify_test(void){int ok=qdc_notify_send("test");if(ok!=1){error_response(503,"测试提醒未发送成功，请检查推送地址和网络；两次测试请间隔至少 15 秒");return;}headers(200,NULL);puts("{\"ok\":true}");}
