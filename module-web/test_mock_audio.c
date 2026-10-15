/* Native-only synthetic audio; stores operation names/counts, never recordings. */
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
static void logop(const char *s){const char *name=getenv("QDC_AUDIO_TEST_LOG");if(!name)return;FILE *f=fopen(name,"a");if(f){fprintf(f,"%s\n",s);fclose(f);}}
void *quec_pcm_open(const char *name,unsigned flags,unsigned rate,unsigned channels,unsigned format,unsigned hostless){
    /* Model the real firmware: reject ASM D0, allow only the existing
       hostless voice D4 and correctly directed AFE proxy D5/D6. */
    if(rate!=8000||channels!=1||format!=2)return NULL;
    if(!strcmp(name,"hw:0,4")){if(hostless!=1||(flags!=0x01000001U&&flags!=0x11000001U))return NULL;}
    else if(!strcmp(name,"hw:0,5")){if(hostless||flags!=0x01000000U)return NULL;}
    else if(!strcmp(name,"hw:0,6")){if(hostless||flags!=0x11000000U)return NULL;}
    else return NULL;
    logop(name);return malloc(1);
}
int quec_pcm_close(void *p){free(p);logop("close");return 0;}
unsigned quec_get_pem_buffer_len(void *p){(void)p;return 256;}
int quec_read_pcm(void *p,void *data,unsigned n){(void)p;int16_t *s=data;for(unsigned i=0;i<n/2;i++)s[i]=(i&1)?2000:-2000;return 0;}
int quec_write_pcm(void *p,void *data,unsigned n){(void)p;unsigned nz=0;unsigned char *b=data;for(unsigned i=0;i<n;i++)if(b[i])nz++;if(nz)logop("uplink-nonzero");if(n!=256){logop("unaligned-period");return -1;}return 0;}
int quectel_clt_set_mixer_value(const char *name,int index,const char *value){(void)index;char buf[256];snprintf(buf,sizeof buf,"%s=%s",name,value);logop(buf);return 1;}
