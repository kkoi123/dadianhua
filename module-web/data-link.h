#include <unistd.h>
/* The ordinary internet interface only. Leave USB, parent and IMS untouched. */
static int qdc_data_link(int desired) {
#if defined(DATA_TEST)
 const char *fixture=getenv("QDC_DATA_TEST_LINK");
 if(fixture){FILE *f=fopen(fixture,desired<0?"r":"w");if(!f)return -1;int value=-1;
  if(desired<0){if(fscanf(f,"%d",&value)!=1)value=-1;}else{value=desired;if(fprintf(f,"%d\n",value)<0)value=-1;}fclose(f);return value;}
#endif
 int fd=socket(AF_INET,SOCK_DGRAM,0);if(fd<0)return -1;
 struct ifreq request;memset(&request,0,sizeof request);strcpy(request.ifr_name,"rmnet_data0");
 if(ioctl(fd,SIOCGIFFLAGS,&request)){close(fd);return -1;}
 if(desired>=0){if(desired)request.ifr_flags|=IFF_UP;else request.ifr_flags&=~IFF_UP;
  if(ioctl(fd,SIOCSIFFLAGS,&request)||ioctl(fd,SIOCGIFFLAGS,&request)){close(fd);return -1;}}
 close(fd);return !!(request.ifr_flags&IFF_UP);
}
