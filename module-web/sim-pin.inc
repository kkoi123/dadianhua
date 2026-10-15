/* QMI UIM Verify PIN (0x26), PIN1 or its UPIN replacement only.
 * PIN never appears in argv, files or debug output. One explicit submission,
 * no fallback command and no retry after an uncertain result. */
typedef struct { unsigned session,id,state,retries,puk; int available,recoverable; char description[64],token[2049]; unsigned char session_aid[34]; } SimPin;
static int sim_pin_info(Client *uim,SimPin *pin) {
    Packet req={0},resp={0};size_t n=0;
    if(!request(uim,0x2f,&req,&resp,1800))return 0;
    const unsigned char *p=find(&resp,0x10,&n);
    if(!p||n<9||n>1024||p[8]>8){snprintf(failure,sizeof failure,"卡片状态不完整");return 0;}
    unsigned primary=u16(p);size_t off=9;int selected=0,provisioned=0;
    snprintf(pin->description,sizeof pin->description,"未插卡");
    for(unsigned i=0;i<p[8];i++){
        if(n-off<6)goto malformed;
        size_t base=off;unsigned cs=p[off],apps=p[off+5];off+=6;
        if(apps>16)goto malformed;
        if(p[8]==1&&i==0&&cs==2)pin->recoverable=1;
        if(!selected&&i==0)snprintf(pin->description,sizeof pin->description,"%s",cs==0?"未插卡":cs==2?"SIM 读取错误，请断电后重新插稳卡片":"SIM 初始化中，请稍后刷新");
        for(unsigned j=0;j<apps;j++){
            if(n-off<7)goto malformed;
            unsigned type=p[off],as=p[off+1],aid=p[off+6];
            if(aid>n-off-7||n-off-7-aid<7)goto malformed;
            size_t pos=off+7+aid;int match=primary!=65535&&(primary>>8)==i&&(primary&255)==j;
            if(cs==1&&(type==1||type==2)&&(!selected||(!provisioned&&match))){
                if(i>1||aid>32){snprintf(failure,sizeof failure,"当前 SIM 应用暂不支持解锁");return 0;}
                /* Legacy GSM SIM has no AID and may have no GW session before
                 * PIN verification. Use its card-slot session, not an eUICC AID.
                 * USIM uses its provisioned GW or exact nonprovisioning AID. */
                pin->session=match?0:!aid?6+i:4+i;
                memset(pin->session_aid,0,sizeof pin->session_aid);pin->session_aid[0]=pin->session;
                pin->session_aid[1]=!match&&aid?aid:0;
                if(!match&&aid)memcpy(pin->session_aid+2,p+off+7,aid);
                pin->id=p[pos]?3:1;pin->state=p[pos]?p[base+1]:p[pos+1];
                pin->retries=p[pos]?p[base+2]:p[pos+2];pin->puk=p[pos]?p[base+3]:p[pos+3];
                if(as==3||as==5)pin->state=4;
                if(as==6){pin->state=0;snprintf(pin->description,sizeof pin->description,"SIM 应用不可用");}else *pin->description=0;
                pin->available=1;selected=1;provisioned=match;
            }
            off=pos+7;
        }
    }
    if(off!=n)goto malformed;
    encode(p,n,pin->token);return 1;
malformed:
    snprintf(failure,sizeof failure,"卡片状态不完整");return 0;
}
static const char *sim_pin_state_text(const SimPin *p) {
    return *p->description?p->description:p->state==1?"需要 PIN 码":p->state==2?"PIN 已验证":p->state==3?"PIN 保护未开启":p->state==4?"需要 PUK 码":p->state==5?"SIM 已永久锁定":"PIN 状态尚未就绪，请稍后刷新";
}
static void sim_pin_status_json(const SimPin *p) {
    fprintf(output,"{\"ok\":true,\"state\":");string(sim_pin_state_text(p));
    fprintf(output,",\"required\":%s,\"retries\":",p->available&&p->state==1&&p->retries?"true":"false");
    if(p->available)fprintf(output,"%u",p->retries);else fputs("null",output);
    fputs(",\"pukRetries\":",output);if(p->available)fprintf(output,"%u",p->puk);else fputs("null",output);
    fprintf(output,",\"recoverable\":%s",p->recoverable?"true":"false");
    fputs(",\"stateToken\":",output);string(p->token);fputs("}\n",output);
}
static int sim_pin_record(const char *id) {
    char filename[640];if(!hex(id,64,64)||snprintf(filename,sizeof filename,"%s/sim-pin-request-%s",run,id)>=(int)sizeof filename)return 0;
    DIR *d=opendir(run);if(!d)return 0;unsigned count=0;struct dirent *e;
    while((e=readdir(d)))if(!strncmp(e->d_name,"sim-pin-request-",16))count++;
    closedir(d);if(count>=128)return 0;
    int fd=safe_open(filename,O_WRONLY|O_CREAT|O_EXCL);if(fd<0)return 0;
    int ok=write_all(fd,"submitted\n");close(fd);return ok;
}
static void sim_pin(int verify) {
    Client uim={0},voice={0};SimPin info={0};char pin[10]={0},token[2050]={0},id[66]={0};
    if(verify){
        if(!fgets(pin,sizeof pin,stdin)||!strchr(pin,'\n')||!fgets(token,sizeof token,stdin)||!strchr(token,'\n')||!fgets(id,sizeof id,stdin)||!strchr(id,'\n')||fgetc(stdin)!=EOF){error("PIN 请求不完整，未执行解锁");goto done;}
        pin[strcspn(pin,"\n")]=0;token[strcspn(token,"\n")]=0;id[strcspn(id,"\n")]=0;
        if(!digits(pin,4,8)||!hex(token,2,2048)||!hex(id,64,64)){error("PIN 应为 4 至 8 位数字");goto done;}
    }
    if(!connect_client(&uim,"uim_qmi_idl_service_object_v01")||!sim_pin_info(&uim,&info)){error(*failure?failure:"无法读取卡片 PIN 状态");goto done;}
    if(!verify){sim_pin_status_json(&info);goto done;}
    if(strcmp(token,info.token)){error("卡片状态已改变，请重新读取 PIN 状态，未提交密码");goto done;}
    if(!info.available||info.state!=1||!info.retries){error(info.state==4?"卡片需要 PUK 码，请联系运营商":"当前卡片不需要 PIN 解锁，未提交密码");goto done;}
    EsimReader reader={0};
    if(!connect_client(&voice,"voice_qmi_idl_service_object_v02")){error("无法确认电话是否空闲，未提交 PIN");goto done;}
    reader.voice=voice;if(!esim_idle(&reader)){error("请先结束电话，再解锁 SIM 卡");goto done;}
    if(!sim_pin_record(id)){error("请求已提交或记录不可用，请重新读取卡片状态，未重复提交 PIN");goto done;}
    unsigned char value[10]={info.id,(unsigned char)strlen(pin)};
    memcpy(value+2,pin,strlen(pin));Packet req={0},resp={0};tlv(&req,1,info.session_aid,2+info.session_aid[1]);tlv(&req,2,value,2+strlen(pin));
    int ok=request(&uim,0x26,&req,&resp,5000);memset(pin,0,sizeof pin);memset(value,0,sizeof value);memset(req.b,0,sizeof req.b);
    if(ok){fputs("{\"ok\":true,\"message\":\"PIN 已验证，请刷新 SIM 卡状态\"}\n",output);goto done;}
    size_t result_n=0;const unsigned char *result=find(&resp,2,&result_n);
    int uncertain=transport_error||!valid_packet(&resp)||!result||result_n!=4||!u16(result);
    size_t n=0;const unsigned char *retries=find(&resp,0x10,&n);
    fputs("{\"ok\":false,\"error\":",output);
    string(uncertain?"PIN 验证结果未确认，请先刷新卡片状态，不要直接重试":protocol_error==12?"PIN 不正确，请核对后再输入":"模块拒绝 PIN 验证，请刷新卡片状态");
    fprintf(output,",\"uncertain\":%s,\"retries\":",uncertain?"true":"false");
    if(!uncertain&&retries&&n==2)fprintf(output,"%u",retries[0]);else fputs("null",output);fputs("}\n",output);
done:
    memset(pin,0,sizeof pin);disconnect(&voice);disconnect(&uim);
}
