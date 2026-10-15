/* Emulate stock proc_dointvec returning only the consumed digit. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef __APPLE__
static ssize_t sysctl_write(int fd,const void *buffer,size_t n)
#else
ssize_t write(int fd,const void *buffer,size_t n)
#endif
{
    static ssize_t(*next)(int,const void*,size_t);
#ifdef __APPLE__
    /* Calls from the interposing image itself keep the original binding. */
    if(!next)next=write;
#else
    if(!next)next=dlsym(RTLD_NEXT,"write");
#endif
    const char *root=getenv("QDC_DATA_TEST_SYSCTL");char path[PATH_MAX]={0},target[PATH_MAX],canonical[PATH_MAX];
    if(root&&realpath(root,canonical))root=canonical;
    int known=0;
#ifdef __APPLE__
    known=fcntl(fd,F_GETPATH,path)==0;
#else
    char link[64];snprintf(link,sizeof link,"/proc/self/fd/%d",fd);ssize_t length=readlink(link,path,sizeof path-1);known=length>=0;
#endif
    if(root&&known&&snprintf(target,sizeof target,"%s/ipv6",root)<(int)sizeof target&&!strcmp(path,target)&&n==2&&((const char*)buffer)[1]=='\n')n=1;
    return next(fd,buffer,n);
}

#ifdef __APPLE__
__attribute__((used)) static struct { const void *replacement; const void *original; } redirect_write __attribute__((section("__DATA,__interpose"))) = { (const void *)sysctl_write, (const void *)write };
#endif
