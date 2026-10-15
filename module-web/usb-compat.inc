/* USB forwarding compatibility is opt-in. Status GET never loads a driver. */
static int compat_policy(void) {
    char p[768], b[8]={0};
    if (!data_file(p,sizeof p,1,"usb-compat-policy")) return -1;
    int fd=safe_file(p,O_RDONLY);
    if (fd<0) return errno==ENOENT?0:-1;
    ssize_t n=read(fd,b,sizeof b-1); close(fd);
    return n==2&&b[1]=='\n'&&(b[0]=='0'||b[0]=='1')?b[0]-'0':-1;
}
static int compat_parameter(char p[768],const char *name) {
#ifdef DATA_TEST
    const char *test=getenv("QDC_COMPAT_TEST_PARAMS");
    if(test) return snprintf(p,768,"%s/%s",test,name)<768;
#endif
    return snprintf(p,768,"/sys/module/qdc_usb_compat/parameters/%s",name)<768;
}
static int compat_active(void) {
    char p[768],b[8]={0};
    if(!compat_parameter(p,"enabled"))return -1;
    int fd=open(p,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
    if(fd<0)return errno==ENOENT?0:-1;
    ssize_t n=read(fd,b,sizeof b-1);close(fd);
    return n==2&&b[1]=='\n'&&(b[0]=='Y'||b[0]=='N')?b[0]=='Y':-1;
}
static int compat_supported(void) {
    struct utsname u;char p[768];struct stat st;
    int kernel=!uname(&u)&&!strcmp(u.release,"3.18.44")&&!strcmp(u.machine,"armv7l");
#ifdef DATA_TEST
    if(getenv("QDC_COMPAT_TEST_PARAMS"))kernel=1;
#endif
    return kernel&&path(p,sizeof p,root,"usb-compat/qdc_usb_compat.ko")&&
        !lstat(p,&st)&&S_ISREG(st.st_mode);
}
static int compat_apply(int enabled) {
    char p[768],result[512],ko[768];
    if(!compat_parameter(p,"enabled"))return 0;
    if(compat_active()<0)return 0;
    if(enabled&&!compat_supported())return 0;
    if(enabled&&compat_active()==1)return 1;
    int loaded=access(p,F_OK)==0,newly_loaded=0;
    if(enabled&&!loaded){
        if(!path(ko,sizeof ko,root,"usb-compat/qdc_usb_compat.ko"))return 0;
        const char *loader="/sbin/insmod";
#ifdef DATA_TEST
        if(getenv("QDC_COMPAT_TEST_INSMOD"))loader=getenv("QDC_COMPAT_TEST_INSMOD");
#endif
        char *args[]={(char *)loader,ko,"enabled=0",NULL};
        if(!data_exec(args,result,sizeof result,5))return 0;
        loaded=newly_loaded=1;
    }
    if(!loaded)return !enabled;
    int fd=open(p,O_WRONLY|O_NOFOLLOW|O_CLOEXEC);
    int ok=fd>=0&&write(fd,enabled?"Y":"N",1)==1;
    if(fd>=0)close(fd);
    ok=ok&&compat_active()==enabled;
    if(!enabled||(!ok&&newly_loaded)){
        const char *unloader="/sbin/rmmod";
#ifdef DATA_TEST
        if(getenv("QDC_COMPAT_TEST_RMMOD"))unloader=getenv("QDC_COMPAT_TEST_RMMOD");
#endif
        char *args[]={(char *)unloader,"qdc_usb_compat",NULL};
        int removed=data_exec(args,result,sizeof result,5);
        /* Disabled but still loaded is harmless; never claim enabled after
         * an unconfirmed sysfs write. */
        if(!enabled)ok=compat_active()==0&&(ok||removed);
    }
    return ok;
}
static int compat_reconcile(void) {
    int lock=data_lock();if(lock<0)return 0;
    if(data_job()||tunnel_job()){close(lock);return 0;}
    int desired=compat_policy(),ok=desired>=0&&compat_apply(desired);
    close(lock);return ok?0:1;
}
static void compat_fields(void) {
    int desired=compat_policy(),actual=compat_active();
    printf(",\"compatEnabled\":%s,\"compatApplied\":%s,\"compatSupported\":%s",
        desired<0?"null":desired?"true":"false",
        actual<0?"null":actual?"true":"false",compat_supported()?"true":"false");
}
static void compat_set(void) {
    const char *value=field("enabled");
    if(strcmp(value,"0")&&strcmp(value,"1")){error_response(400,"兼容开关参数无效");return;}
    int lock=data_lock();
    if(lock<0){error_response(409,"正在切换网络，请稍后重试");return;}
    if(data_job()||tunnel_job()){close(lock);error_response(409,"正在切换网络，请稍后重试");return;}
    int old=compat_policy(),actual=compat_active(),desired=value[0]-'0';char p[768];
    if(old<0||actual<0||!data_file(p,sizeof p,1,"usb-compat-policy")){
        close(lock);error_response(503,"兼容设置状态无法确认");return;
    }
    if(!compat_apply(desired)){
        compat_apply(actual);close(lock);error_response(503,"USB 漫游兼容切换失败，请刷新确认");return;
    }
    char record[3]={value[0],'\n',0};
    if(!atomic_write(p,record)){
        /* A directory fsync can fail after rename has already committed.
         * Restore the policy as well as runtime state before reporting failure. */
        char previous[3]={(char)('0'+old),'\n',0};
        atomic_write(p,previous);
        compat_apply(actual);close(lock);error_response(503,"设置保存失败，已请求恢复原状态");return;
    }
    close(lock);data_response();
}
