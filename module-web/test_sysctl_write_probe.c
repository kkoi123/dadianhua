/* Confirm that the regression fixture models the observed kernel behavior. */
#include <fcntl.h>
#include <unistd.h>
int main(int argc,char **argv){
    if(argc!=2)return 2;
    int fd=open(argv[1],O_WRONLY);if(fd<0)return 2;
    ssize_t n=write(fd,"2\n",2);close(fd);return n==1?0:1;
}
