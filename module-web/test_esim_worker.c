/* Offline orchestration and parser boundaries, no card/network writes. */
#define ESIM_WORKER_TEST_MAIN
#define COMM_TEST
#define HISTORY_TEST
#define NOTIFY_TEST
#define es10b_get_euicc_challenge_and_info fake_challenge
#define es9p_initiate_authentication fake_initiate
#define es10b_authenticate_server fake_authserver
#define es9p_authenticate_client fake_authclient
#define es10b_prepare_download fake_prepare
#define es9p_get_bound_profile_package fake_package
#define es10b_load_bound_profile_package fake_install
#define es10b_cancel_session fake_cancel_card
#define es9p_cancel_session fake_cancel_server
#include "esim-worker.c"
#include <euicc/derutil.h>
#include <assert.h>
static unsigned step,failstep,cancelled;
static int next(void){return ++step==failstep?-1:0;}
int fake_challenge(struct euicc_ctx *ctx){(void)ctx;return next();}
int fake_initiate(struct euicc_ctx *ctx){ctx->http._internal.transaction_id_bin=(uint8_t *)"x";return next();}
int fake_authserver(struct euicc_ctx *ctx,const char *matching,const char *imei){(void)ctx;assert(!strcmp(matching,"secret"));assert(valid_imei(imei));return next();}
int fake_authclient(struct euicc_ctx *ctx){(void)ctx;return next();}
int fake_prepare(struct euicc_ctx *ctx,const char *code){(void)ctx;assert(!strcmp(code,"confirmation"));return next();}
int fake_package(struct euicc_ctx *ctx){(void)ctx;return next();}
int fake_install(struct euicc_ctx *ctx,struct es10b_load_bound_profile_package_result *r){(void)ctx;r->iccid=NULL;return next();}
int fake_cancel_card(struct euicc_ctx *ctx,enum es10b_cancel_session_reason r){(void)ctx;(void)r;cancelled++;return 0;}
int fake_cancel_server(struct euicc_ctx *ctx){(void)ctx;cancelled++;return 0;}
int main(void){
    assert(hostname("smdp.example.com")&&!hostname("localhost")&&!hostname("a.-b.com")&&!hostname("a.com:443"));
    assert(public_ipv4(0x01010101)&&!public_ipv4(0xc0a8e101)&&!public_ipv4(0x7f000001)&&!public_ipv4(0x64400001)&&!public_ipv4(0xc6120001));
    assert(valid_imei("490154203237518")&&!valid_imei("490154203237519"));
    char h[254],pin[320];assert(!http_destination("https://127.0.0.1/gsma/rsp2/es9plus/initiateAuthentication",h,pin));
    assert(!http_destination("http://example.com/gsma/rsp2/es9plus/initiateAuthentication",h,pin));
    assert(!http_destination("https://example.com/anything",h,pin));
    assert(!http_destination("https://a@example.com/gsma/rsp2/es9plus/initiateAuthentication",h,pin));
    for(unsigned fail=0;fail<=7;fail++){
        EsimOperation op={0};strcpy(op.activation,"LPA:1$smdp.example.com$secret$$1");strcpy(op.confirmation,"confirmation");strcpy(op.imei,"490154203237518");
        struct euicc_ctx ctx={0};step=cancelled=mutation_started=0;failstep=fail;
        reader.deadline=esim_ms()+10000;last_idle=esim_ms();int ok=download(&ctx,&op);
        assert(ok==(fail==0));assert(step==(fail?fail:7));
        assert(mutation_started==(fail==0||fail==7));assert(cancelled==(fail>=2&&fail<=6?2:0));
    }
    struct euicc_derutil_node node;
    const unsigned char indefinite[]={0xbf,0x3e,0x80},overflow[]={0xbf,0x3e,0x85,0,0,0,0,0},truncated[]={0xbf,0x3e,0x82,0,6};
    assert(euicc_derutil_unpack_first(&node,indefinite,sizeof indefinite)<0);
    assert(euicc_derutil_unpack_first(&node,overflow,sizeof overflow)<0);
    assert(euicc_derutil_unpack_first(&node,truncated,sizeof truncated)<0);
    uint32_t random=1;unsigned char bytes[1024];
    for(unsigned k=0;k<15000;k++){
        random=random*1664525+1013904223;unsigned n=random%sizeof bytes;
        for(unsigned i=0;i<n;i++){random=random*1664525+1013904223;bytes[i]=(unsigned char)(random>>24);}
        if(!euicc_derutil_unpack_first(&node,bytes,n)){
            assert(node.self.length<=n&&node.self.length>=2);
            (void)euicc_derutil_unpack_next(&node,&node,bytes,n);
        }
    }
    puts("eSIM worker passed: download stage faults/cancellation, no automatic activation/retry, IMEI, destination restrictions and DER bounds.");return 0;
}
