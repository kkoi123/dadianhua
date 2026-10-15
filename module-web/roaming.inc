/* NAS supplies the real roaming state. WDS exposes the 3GPP ordinary-data
 * profile's roaming-disallowed bit (confirmed on this device). The Linux gate
 * also closes existing connections; it never touches USB or the IMS interface.
 * Missing policy inherits the existing modem setting, without writing it.
 * An explicit OFF blocks on
 * unknown NAS state too, preventing a failed query from authorizing roaming.
 * All checks are local; the existing supervisor checks about every 20 s. */
static int roaming_policy(void) {
    char p[768],b[8]={0};
    if(!data_file(p,sizeof p,1,"roaming-policy"))return -1;
    int fd=safe_file(p,O_RDONLY);if(fd<0)return errno==ENOENT?2:-1;
    ssize_t n=read(fd,b,sizeof b-1);close(fd);
    return n==2&&b[1]=='\n'&&(b[0]=='0'||b[0]=='1')?b[0]-'0':-1;
}
static int roaming_parse(const char *b,RoamingState *state,int expected) {
    char indicator[6]={0},permission[6]={0},extra;int registration=-1,profile=0;
    /* Fixed helper schema. Never search arbitrary strings for a boolean. */
    if(sscanf(b,"{\"ok\":true,\"roaming\":%5[a-z],\"registrationState\":%d,\"permission\":%5[a-z],\"profile\":%d} %c",indicator,&registration,permission,&profile,&extra)!=4||profile!=expected)return 0;
    state->state=registration!=1?-1:!strcmp(indicator,"true")?1:!strcmp(indicator,"false")?0:-1;
    state->permission=!strcmp(permission,"true")?1:!strcmp(permission,"false")?0:-1;
    state->profile=profile;return 1;
}
static RoamingState roaming_state_read(void) {
    RoamingState state={-1,-1,0};char helper[768],b[256],profile[4],family[8];
    if(!data_default(profile,family)||!path(helper,sizeof helper,root,"www/cgi-bin/qdc-comm"))return state;
    char *args[]={helper,"roaming-state",profile,NULL};
    if(data_exec(args,b,sizeof b,5))roaming_parse(b,&state,atoi(profile));
    return state;
}
static int roaming_configure(const RoamingState *state,int permission) {
    if(state->permission<0||state->profile<1||state->profile>16||permission<0||permission>1)return 0;
    if(state->permission==permission)return 1;
    char helper[768],b[256],profile[4],value[2]={(char)('0'+permission),0},current[4],family[8];
    /* If the default profile changed since sampling, do not write the stale
     * profile. The next reconcile will sample the newly selected profile. */
    if(!data_default(current,family)||atoi(current)!=state->profile||!path(helper,sizeof helper,root,"www/cgi-bin/qdc-comm"))return 0;
    snprintf(profile,sizeof profile,"%d",state->profile);
    char *args[]={helper,"roaming-config",profile,value,NULL};RoamingState applied={-1,-1,0};
    return data_exec(args,b,sizeof b,8)&&roaming_parse(b,&applied,state->profile)&&applied.permission==permission;
}
static int roaming_permission(int policy,const RoamingState *state) {
    /* Unknown inherited state must not change pre-existing operation. Only
     * the UI shows it as unconfirmed; no policy is silently initialized. */
    return policy==2?(state->permission<0?1:state->permission):policy;
}
static int roaming_effective(int enabled,int permission,int state) {
    return enabled>0&&(permission==1||(permission==0&&state==0));
}
