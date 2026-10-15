/* Exercise the real watcher guard against private fixtures, no network writes. */
#define main qdc_comm_main
#include "comm.c"
#undef main
#include <assert.h>
static void record(const char *p,const char *value){int fd=open(p,O_WRONLY|O_CREAT|O_TRUNC,0600);assert(fd>=0);assert(write(fd,value,strlen(value))==(ssize_t)strlen(value));close(fd);}
int main(void){
 const char *base=getenv("QDC_WEB_ROOT");assert(base);snprintf(private_dir,sizeof private_dir,"%s/private",base);snprintf(run,sizeof run,"%s/run",base);
 char policy[768],job[768],link[768],lockfile[768];snprintf(policy,sizeof policy,"%s/data-policy",private_dir);snprintf(job,sizeof job,"%s/data-job",run);snprintf(link,sizeof link,"%s/link",base);setenv("QDC_DATA_TEST_LINK",link,1);
 record(link,"1\n");watch_data_gate();assert(qdc_data_link(-1)==1); // Default ON
 record(policy,"1\n");watch_data_gate();assert(qdc_data_link(-1)==1);
 record(policy,"0\n");struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);char timestamp[80];snprintf(timestamp,sizeof timestamp,"%llu cellular\n",(unsigned long long)t.tv_sec);record(job,timestamp);
 watch_data_gate();assert(qdc_data_link(-1)==1); // Delayed worker captures snapshots first
 unlink(job);snprintf(lockfile,sizeof lockfile,"%s/data.lock",run);int lock=safe_open(lockfile,O_RDWR|O_CREAT);assert(lock>=0&&!flock(lock,LOCK_EX|LOCK_NB));watch_data_gate();assert(qdc_data_link(-1)==1);close(lock);
 watch_data_gate();assert(qdc_data_link(-1)==0);record(link,"1\n");watch_data_gate();assert(qdc_data_link(-1)==0); // Vendor restart cannot defeat saved OFF
 record(link,"1\n");record(job,"0 cellular\n");watch_data_gate();assert(qdc_data_link(-1)==0); // Abandoned jobs do not bypass gate
 record(link,"1\n");record(policy,"invalid\n");watch_data_gate();assert(qdc_data_link(-1)==1); // Invalid setting is not interpreted
 unlink(policy);assert(symlink(link,policy)==0);watch_data_gate();assert(qdc_data_link(-1)==1);unlink(policy);
 char roaming[768],gate[768];snprintf(roaming,sizeof roaming,"%s/roaming-policy",private_dir);snprintf(gate,sizeof gate,"%s/roaming-blocked",run);
 unlink(job);record(roaming,"0\n");watch_data_gate();assert(qdc_data_link(-1)==0); // Saved OFF, no boot snapshot: fail closed
 record(link,"1\n");record(gate,"0\n");watch_data_gate();assert(qdc_data_link(-1)==1); // Confirmed home allows data
 record(gate,"1\n");watch_data_gate();assert(qdc_data_link(-1)==0); // Roaming gate survives stock reconnect
 record(link,"1\n");watch_data_gate();assert(qdc_data_link(-1)==0);
 record(link,"1\n");record(roaming,"1\n");watch_data_gate();assert(qdc_data_link(-1)==1); // Explicit ON ignores stale blocked snapshot
 record(roaming,"0\n");record(job,timestamp);watch_data_gate();assert(qdc_data_link(-1)==1); // Pending worker keeps snapshot ownership
 unlink(job);unlink(roaming);watch_data_gate();assert(qdc_data_link(-1)==0); // Inherited native OFF uses runtime gate
 record(link,"1\n");unlink(gate);watch_data_gate();assert(qdc_data_link(-1)==1); // First install keeps existing connectivity
 record(roaming,"bad\n");watch_data_gate();assert(qdc_data_link(-1)==0); // Corrupt roaming policy fails closed
 return 0;
}
