/* Explicit recovery for a single, identified physical slot in CARD_ERROR.
 * Card-slot power cycle only; no radio/module reset, PIN attempts or retries. */
static int sim_recover_record(const char *id) {
    char name[640];
    if(!hex(id,64,64)||snprintf(name,sizeof name,"%s/sim-recover-request-%s",run,id)>=(int)sizeof name)return 0;
    DIR *d=opendir(run);if(!d)return 0;unsigned count=0;struct dirent *e;
    while((e=readdir(d)))if(!strncmp(e->d_name,"sim-recover-request-",20))count++;
    closedir(d);if(count>=32)return 0;
    int fd=safe_open(name,O_WRONLY|O_CREAT|O_EXCL);if(fd<0)return 0;
    int ok=write_all(fd,"submitted\n");close(fd);return ok;
}
static int sim_recover_uncertain(const Packet *p) {
    size_t n=0;const unsigned char *r=find(p,2,&n);
    return transport_error||!valid_packet(p)||!r||n!=4||!u16(r);
}
static void sim_recover(void) {
    Client uim={0},voice={0};SimPin info={0};char token[2050]={0},id[66]={0};
    if(!fgets(token,sizeof token,stdin)||!strchr(token,'\n')||!fgets(id,sizeof id,stdin)||!strchr(id,'\n')||fgetc(stdin)!=EOF){error("卡片请求不完整，未执行重新识别");goto done;}
    token[strcspn(token,"\n")]=0;id[strcspn(id,"\n")]=0;
    if(!hex(token,2,2048)||!hex(id,64,64)){error("请先读取卡片状态");goto done;}
    if(!connect_client(&uim,"uim_qmi_idl_service_object_v01")||!sim_pin_info(&uim,&info)){error(*failure?failure:"无法确认卡片状态");goto done;}
    if(strcmp(token,info.token)){error("卡片状态已改变，请刷新，未执行重新识别");goto done;}
    if(!info.recoverable){error("当前卡片不是可恢复的读取错误状态，未执行重新识别");goto done;}
    EsimReader reader={0};
    if(!connect_client(&voice,"voice_qmi_idl_service_object_v02")){error("无法确认电话空闲，未执行重新识别");goto done;}
    reader.voice=voice;if(!esim_idle(&reader)){error("请先结束电话，再重新识别 SIM");goto done;}
    if(!sim_recover_record(id)){error("请求已提交或记录不可用，未重复执行，请刷新卡片状态");goto done;}
    Packet req={0},resp={0};unsigned char slot=1;tlv(&req,1,&slot,1);
    int off=request(&uim,0x30,&req,&resp,5000);
    /* A lost response may still have switched power off. Always attempt to
     * restore power once in that case; never repeat the off request. */
    if(!off&&!sim_recover_uncertain(&resp)){error("模块拒绝 SIM 卡槽断电，未执行重新识别；请断电重插模块");goto done;}
    const struct timespec pause={0,500000000};nanosleep(&pause,NULL);
    resp=(Packet){0};
    if(!request(&uim,0x31,&req,&resp,5000)){
        fputs("{\"ok\":false,\"uncertain\":true,\"error\":\"SIM 卡槽恢复供电未确认，请断电重插模块，勿直接重复操作\"}\n",output);goto done;
    }
    int read=0,recovered=0;
    for(unsigned attempt=0;attempt<5;attempt++){
        info=(SimPin){0};read=sim_pin_info(&uim,&info);
        recovered=read&&info.available&&info.state!=0;
        if(recovered)break;
#ifndef COMM_TEST
        if(attempt<4){const struct timespec settle={1,0};nanosleep(&settle,NULL);}
#endif
    }
    fputs("{\"ok\":true,\"powerRestored\":true,\"recovered\":",output);fputs(recovered?"true":"false",output);
    fputs(",\"message\":",output);
    string(recovered?(info.state==1?"SIM 已重新识别，需要输入 PIN 码":"SIM 已重新识别，请刷新状态"):
        "已恢复 SIM 卡槽供电，但卡片仍未就绪；请刷新核对，仍报错时断电重插模块");
    fputs(",\"state\":",output);string(read?sim_pin_state_text(&info):"状态暂未读取");fputs("}\n",output);
done:
    disconnect(&voice);disconnect(&uim);
}
