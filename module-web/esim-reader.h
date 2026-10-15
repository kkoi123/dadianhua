/* Bounded ES10c read-only response parser. No profile management commands.
 * Wire reference: the pinned ESIMCore euicc-go dependency (v2/es10c.go).
 */
#ifndef QDC_ESIM_READER_H
#define QDC_ESIM_READER_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#define ESIM_MAX_PROFILES 32
typedef struct { unsigned tag; const unsigned char *p; size_t n; } EsimTLV;
typedef struct { char iccid[23],name[129],provider[129]; int enabled; } EsimProfile;
typedef struct {
    char eid[33],firmware[32]; EsimProfile profiles[ESIM_MAX_PROFILES];
    unsigned count; uint32_t free_bytes; int has_free;
} EsimCard;
static int esim_next(const unsigned char **p,size_t *left,EsimTLV *out) {
    const unsigned char *b=*p;size_t n=*left,at=0,length=0;
    if(n<2)return 0;
    unsigned tag=b[at++];
    if((tag&31)==31){unsigned bytes=0;do {
        if(at>=n||++bytes>2)return 0;
        unsigned v=b[at++];if(bytes==1&&!(v&127))return 0;
        tag=(tag<<8)|v;if(!(v&128))break;
    }while(1);}
    if(at>=n)return 0;
    length=b[at++];
    if(length&128){unsigned bytes=(unsigned)length&127;
        if(!bytes||bytes>2||bytes>n-at)return 0;
        length=0;for(unsigned i=0;i<bytes;i++)length=(length<<8)|b[at++];
    }
    if(length>n-at)return 0;
    *out=(EsimTLV){tag,b+at,length};*p=b+at+length;*left=n-at-length;return 1;
}
static int esim_root(const unsigned char *b,size_t n,unsigned tag,EsimTLV *out) {
    return esim_next(&b,&n,out)&&out->tag==tag&&!n;
}
static int esim_field(EsimTLV root,unsigned tag,EsimTLV *out) {
    const unsigned char *p=root.p;size_t left=root.n;int found=0;
    while(left){EsimTLV item;if(!esim_next(&p,&left,&item))return -1;
        if(item.tag==tag){if(found)return -1;*out=item;found=1;}}
    return found;
}
static int esim_decimal(const unsigned char *p,size_t n,char *out,size_t cap,int swapped) {
    if(n*2+1>cap)return 0;size_t at=0;int padding=0;
    for(size_t i=0;i<n*2;i++){
        unsigned v=(i%2==(unsigned)swapped)?p[i/2]>>4:p[i/2]&15;
        if(swapped&&v==15&&i==n*2-1){padding=1;continue;}
        if(v>9||padding)return 0;out[at++]=(char)('0'+v);
    }
    out[at]=0;return 1;
}
/* Reject invalid UTF-8, embedded NUL, and overlong names rather than truncate. */
static int esim_text(EsimTLV field,char *out,size_t cap) {
    if(field.n>=cap)return 0;
    size_t i=0;while(i<field.n){unsigned c=field.p[i++];
        if(!c)return 0;if(c<128)continue;
        unsigned count=c>=0xc2&&c<=0xdf?1:c>=0xe0&&c<=0xef?2:c>=0xf0&&c<=0xf4?3:0;
        if(!count||count>field.n-i)return 0;
        unsigned first=field.p[i];
        if((c==0xe0&&first<0xa0)||(c==0xed&&first>=0xa0)||(c==0xf0&&first<0x90)||(c==0xf4&&first>0x8f))return 0;
        for(unsigned j=0;j<count;j++)if((field.p[i++]&0xc0)!=0x80)return 0;
    }
    memcpy(out,field.p,field.n);out[field.n]=0;return 1;
}
static int esim_eid(const unsigned char *b,size_t n,char out[33]) {
    EsimTLV root={0},field={0};
    return esim_root(b,n,0xbf3e,&root)&&esim_field(root,0x5a,&field)==1&&field.n==16&&esim_decimal(field.p,16,out,33,0);
}
static int esim_profiles(const unsigned char *b,size_t n,EsimCard *card) {
    EsimTLV root={0},list={0},field={0};card->count=0;
    if(!esim_root(b,n,0xbf2d,&root)||esim_field(root,0x81,&field)!=0||esim_field(root,0xa0,&list)!=1)return 0;
    const unsigned char *p=list.p;size_t left=list.n;
    while(left){EsimTLV profile={0};EsimProfile item={0};
        if(card->count==ESIM_MAX_PROFILES||!esim_next(&p,&left,&profile)||(profile.tag!=0xe3&&profile.tag!=0xbf25))return 0;
        if(esim_field(profile,0x5a,&field)!=1||field.n!=10||!esim_decimal(field.p,10,item.iccid,sizeof item.iccid,1))return 0;
        for(unsigned i=0;i<card->count;i++)if(!strcmp(card->profiles[i].iccid,item.iccid))return 0;
        if(esim_field(profile,0x9f70,&field)!=1||field.n!=1||field.p[0]>1)return 0;
        item.enabled=field.p[0];
        int present=esim_field(profile,0x90,&field);
        if(present<0||(present&&!esim_text(field,item.name,sizeof item.name)))return 0;
        if(!*item.name){present=esim_field(profile,0x92,&field);if(present<0||(present&&!esim_text(field,item.name,sizeof item.name)))return 0;}
        present=esim_field(profile,0x91,&field);
        if(present<0||(present&&!esim_text(field,item.provider,sizeof item.provider)))return 0;
        card->profiles[card->count++]=item;
    }
    return 1;
}
static int esim_info(const unsigned char *b,size_t n,EsimCard *card) {
    EsimTLV root={0},field={0},memory={0};
    if(!esim_root(b,n,0xbf22,&root))return 0;
    int found=esim_field(root,0x83,&field);if(found<0)return 0;
    if(found){if(field.n!=3)return 0;snprintf(card->firmware,sizeof card->firmware,"%u.%u.%u",field.p[0],field.p[1],field.p[2]);}
    found=esim_field(root,0x84,&memory);if(found<0)return 0;
    if(found){ /* Contents of resourceInfo use context-specific integer tags. */
        found=esim_field(memory,0x82,&field);if(found<0)return 0;
        if(found){if(!field.n||field.n>5||(field.p[0]&128)||(field.n==5&&field.p[0]))return 0;uint32_t free_bytes=0;
            for(size_t i=0;i<field.n;i++)free_bytes=(free_bytes<<8)|field.p[i];
            card->free_bytes=free_bytes;card->has_free=1;}
    }
    return 1;
}
#endif
