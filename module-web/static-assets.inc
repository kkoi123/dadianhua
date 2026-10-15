/* Precompressed immutable-version public assets. Never serves private data. */
#include "build/static-assets.h"
static const char *asset_root="/usrdata/qdc-web/www";
static int asset_gzip(const char *header){
 char copy[512];if(strlen(header)>=sizeof copy)return 0;strcpy(copy,header);char *save=NULL;double wildcard=-1,gzip=-1;
 for(char *part=strtok_r(copy,",",&save);part;part=strtok_r(NULL,",",&save)){
  while(*part==' '||*part=='\t')part++;char *parameter=strchr(part,';');if(parameter)*parameter++=0;
  char *end=part+strlen(part);while(end>part&&(end[-1]==' '||end[-1]=='\t'))*--end=0;
  double q=1;if(parameter){while(*parameter==' '||*parameter=='\t')parameter++;if(strncmp(parameter,"q=",2))q=0;else{char *tail;q=strtod(parameter+2,&tail);while(*tail==' '||*tail=='\t')tail++;if(tail==parameter+2||*tail||q<0||q>1)q=0;}}
  if(!strcasecmp(part,"gzip"))gzip=q;else if(!strcmp(part,"*"))wildcard=q;
 }return (gzip<0?wildcard:gzip)>0;
}
static int asset_etag_matches(const char *header,const char *etag){
 char copy[512];if(strlen(header)>=sizeof copy)return 0;strcpy(copy,header);char *save=NULL;
 for(char *part=strtok_r(copy,",",&save);part;part=strtok_r(NULL,",",&save)){
  while(*part==' '||*part=='\t')part++;char *end=part+strlen(part);while(end>part&&(end[-1]==' '||end[-1]=='\t'))*--end=0;
  if(!strcmp(part,"*"))return 1;if(!strncmp(part,"W/",2))part+=2;if(!strcmp(part,etag))return 1;
 }return 0;
}
static int asset_open(const WebAsset *asset,int packed){
 char file[768];if(snprintf(file,sizeof file,"%s/%s%s",asset_root,asset->name,packed?".gz":"")>=(int)sizeof file)return -1;
 int fd=open(file,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);if(fd<0)return -1;struct stat st;
 if(fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_size<0||(uint64_t)st.st_size!=(packed?asset->gzip_size:asset->raw_size)){close(fd);return -1;}return fd;
}
static void serve_asset(int client,const char *path,const char *target,const char *method,const char *encoding,const char *conditional){
 const char *name=!strcmp(path,"/")?"index.html":path+1;const WebAsset *asset=NULL;
 for(size_t i=0;i<sizeof web_assets/sizeof *web_assets;i++)if(!strcmp(name,web_assets[i].name)){asset=&web_assets[i];break;}
 if(!asset){reject(client,404);return;}
 int packed=asset_gzip(encoding),fd=asset_open(asset,packed);if(fd<0&&packed){packed=0;fd=asset_open(asset,0);}if(fd<0){reject(client,503);return;}
 int html=!strcmp(name,"index.html");char expected[96];snprintf(expected,sizeof expected,"?v=%s",WEB_ASSET_VERSION);const char *query=strchr(target,'?');
 int immutable=!html&&query&&!strcmp(query,expected);
 const char *cache=html?"no-store":immutable?"public, max-age=31536000, immutable":"no-cache";
 const char *type=strstr(name,".js")?"text/javascript; charset=utf-8":strstr(name,".css")?"text/css; charset=utf-8":"text/html; charset=utf-8";
 char etag[96];snprintf(etag,sizeof etag,"\"%s-%s\"",asset->hash,packed?"gz":"id");
 int status=!html&&asset_etag_matches(conditional,etag)?304:200;char header[1024];int n=snprintf(header,sizeof header,
  "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nCache-Control: %s\r\nVary: Accept-Encoding\r\nETag: %s\r\nX-Content-Type-Options: nosniff\r\nConnection: close\r\n%s",
  status,status==304?"Not Modified":"OK",type,cache,etag,packed?"Content-Encoding: gzip\r\n":"");
 if(status==200)n+=snprintf(header+n,sizeof header-(size_t)n,"Content-Length: %zu\r\n",packed?asset->gzip_size:asset->raw_size);
 n+=snprintf(header+n,sizeof header-(size_t)n,"\r\n");
 long long deadline=now_ms()+60000;
 if(n<=0||(size_t)n>=sizeof header||!write_all(client,header,(size_t)n,deadline))goto done;
 if(status==304||!strcmp(method,"HEAD"))goto done;
 char bytes[4096];ssize_t count;while((count=read(fd,bytes,sizeof bytes))>0)if(!write_all(client,bytes,(size_t)count,deadline))break;
done:close(fd);
}
