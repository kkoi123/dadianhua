/* Offline mutation simulator: require ISO channel 5, ICCID and refresh flag. */
#define ESIM_MOCK_SEND reader_send
#include "test_esim_mock.c"
int qmi_client_send_raw_msg_sync(void *h,unsigned msg,void *req,unsigned req_n,void *resp,unsigned resp_n,unsigned *received,unsigned timeout) {
    if(h==&dms_qmi_idl_service_object_v01&&msg==0x25){
        buffer=resp;at=0;unsigned char success[4]={0};tlv(2,success,4);
        tlv(0x11,(const unsigned char *)"490154203237518",15);*received=at;esim_log("imei");return 0;
    }
    unsigned len=0;const unsigned char *a=field(req,req_n,2,&len);
    if(h!=&uim_qmi_idl_service_object_v01||msg!=0x3b||!a||len<10||a[3]!=0xe2||a[7]!=0xbf||
        (a[8]!=0x31&&a[8]!=0x32&&a[8]!=0x33&&a[8]!=0x29&&a[8]!=0x28))return reader_send(h,msg,req,req_n,resp,resp_n,received,timeout);
    if(a[2]!=0xc1||a[0]+256u*a[1]!=len-2||a[4]!=0x91||a[5]||a[6]!=len-7||a[9]!=len-10)return 1;
    unsigned tag=a[8];const char *name=tag==0x31?"enable":tag==0x32?"disable":tag==0x33?"delete":tag==0x29?"rename":"notifications";
    esim_log(name);
    if(tag==0x31||tag==0x32){if(len!=27||a[10]!=0xa0||a[11]!=12||a[12]!=0x5a||a[13]!=10||a[24]!=0x81||a[25]!=1||a[26]!=0xff)return 1;}
    if(tag==0x33||tag==0x29){if(a[10]!=0x5a||a[11]!=10)return 1;}
    const char *scenario=getenv("QDC_TEST_SCENARIO");if(scenario&&!strcmp(scenario,"mutation-transport"))return 5;
    buffer=resp;at=0;unsigned char success[4]={0};tlv(2,success,4);
    unsigned char bytes[]={8,0,0xbf,(unsigned char)tag,3,0x80,1,0,0x90,0};
    if(tag==0x28){bytes[0]=7;bytes[4]=2;bytes[5]=0xa0;bytes[6]=0;bytes[7]=0x90;bytes[8]=0;}
    else if(scenario&&!strcmp(scenario,"mutation-rejected"))bytes[7]=2;
    tlv(0x10,bytes,tag==0x28?9:10);*received=at;return 0;
}
