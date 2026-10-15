/* Bounds and malformed-card checks under address/undefined-behavior sanitizers. */
#include "esim-reader.h"
#include <assert.h>
int main(void) {
    char eid[33];EsimCard card={0};
    const unsigned char empty_profiles[]={0xbf,0x2d,2,0xa0,0};
    assert(esim_profiles(empty_profiles,sizeof empty_profiles,&card)&&card.count==0);
    const unsigned char info[]={0xbf,0x22,10,0x83,3,1,2,3,0x84,3,0x82,1,0};
    assert(esim_info(info,sizeof info,&card)&&card.has_free&&card.free_bytes==0);
    const unsigned char invalid_utf8[]={0xe0,0x80,0x80};char text[129];
    assert(!esim_text((EsimTLV){0x90,invalid_utf8,sizeof invalid_utf8},text,sizeof text));
    const unsigned char embedded_nul[]={'a',0,'b'};
    assert(!esim_text((EsimTLV){0x90,embedded_nul,sizeof embedded_nul},text,sizeof text));
    uint32_t random=1;unsigned char buffer[1024];
    for(unsigned iteration=0;iteration<12000;iteration++){
        random=random*1664525u+1013904223u;size_t n=random%sizeof buffer;
        for(size_t i=0;i<n;i++){random=random*1664525u+1013904223u;buffer[i]=(unsigned char)(random>>24);}
        (void)esim_eid(buffer,n,eid);(void)esim_profiles(buffer,n,&card);(void)esim_info(buffer,n,&card);
    }
    puts("eSIM parser bounds checks passed.");return 0;
}
