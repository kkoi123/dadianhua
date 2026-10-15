"""Exercise the actual PCM pacer with a deterministic monotonic clock."""
import pathlib,subprocess,tempfile
root=pathlib.Path(__file__).resolve().parent
program=r'''
#define _GNU_SOURCE
#include <stdint.h>
#include <time.h>
#include <assert.h>
static uint64_t fake_ns;
static int test_clock_gettime(clockid_t id,struct timespec *t){(void)id;t->tv_sec=fake_ns/1000000000ULL;t->tv_nsec=fake_ns%1000000000ULL;return 0;}
static int test_nanosleep(const struct timespec *t,struct timespec *rem){(void)rem;fake_ns+=(uint64_t)t->tv_sec*1000000000ULL+t->tv_nsec;return 0;}
#define clock_gettime test_clock_gettime
#define nanosleep test_nanosleep
#define main bridge_unused_main
#include "SOURCE"
#undef main
int main(void){
 const uint64_t period=16000000ULL;
 for(unsigned work=0;work<=6;work+=2){
  fake_ns=1000000000ULL;uint64_t start=fake_ns,deadline=start;
  for(unsigned i=0;i<1000;i++){pace_period(&deadline,256);fake_ns+=work*1000000ULL;}
  assert(deadline-start==1000*period);
  assert(fake_ns-start==1000*period+work*1000000ULL);
 }
 // Short scheduler oversleep is repaid without adding permanent drift.
 fake_ns=1000000000ULL;uint64_t deadline=fake_ns,start=fake_ns;
 pace_period(&deadline,256);fake_ns+=25000000ULL;
 for(unsigned i=1;i<10;i++)pace_period(&deadline,256);
 assert(deadline-start==10*period&&fake_ns==deadline);
 // A long pause resets the timeline and waits one period, avoiding a flood.
 fake_ns+=1000000000ULL;start=fake_ns;pace_period(&deadline,256);
 assert(deadline==start+period&&fake_ns==deadline);
 // Both a quick driver and a blocking real-time driver stay at 8 kHz.
 for(unsigned blocking=0;blocking<2;blocking++){
  fake_ns=1000000000ULL;deadline=fake_ns;start=fake_ns;
  for(unsigned i=0;i<100;i++){pace_period(&deadline,256);if(blocking)fake_ns+=period;}
  assert(fake_ns-start==(100+blocking)*period);
 }
 puts("PCM pacing passed: 8 kHz despite processing cost, short-delay catch-up, bounded recovery after stalls, quick/blocking drivers.");
 return 0;
}
'''.replace('"SOURCE"','"'+str(root/'audio-pcm.c')+'"')
with tempfile.TemporaryDirectory(prefix='qdc-pacing-') as tmp:
    binary=pathlib.Path(tmp)/'test'
    subprocess.run(['clang','-std=c11','-O2','-Wall','-Wextra','-Werror','-x','c','-','-lpthread','-o',str(binary)],input=program.encode(),check=True)
    subprocess.run([str(binary)],check=True)
