/* socketpair round-trip latency: blocking recvmsg vs busy-poll (MSG_DONTWAIT),
 * cross-core vs same-core, idle vs scheduler-loaded. Sizes the QEMU<->isolate
 * transport cost to decide if a shared-mem command buffer is worth building. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sched.h>
#include <pthread.h>
#include <sys/socket.h>
#include <time.h>

static void pin(int cpu){ cpu_set_t s; CPU_ZERO(&s); CPU_SET(cpu,&s); sched_setaffinity(0,sizeof s,&s); }
static double now(){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec+t.tv_nsec/1e9; }
static volatile int load_run=1;
static void* loadfn(void*a){ int c=(long)a; pin(c); volatile unsigned long x=0; while(load_run) x++; return 0; }

#define MSG 64
static int g_busy;
static void echo_child(int fd){
  char b[MSG];
  for(;;){
    ssize_t n;
    if(g_busy){ while((n=recv(fd,b,MSG,MSG_DONTWAIT))<0); } else n=recv(fd,b,MSG,0);
    if(n<=0) _exit(0);
    send(fd,b,MSG,0);
  }
}

int main(int argc,char**argv){
  int busy   = argc>1?atoi(argv[1]):0;   /* 0=blocking 1=busypoll */
  int xcore  = argc>2?atoi(argv[2]):1;   /* 1=cross-core 0=same-core */
  int nload  = argc>3?atoi(argv[3]):0;   /* extra busy threads (oversubscribe) */
  int N=20000;
  int sv[2]; socketpair(AF_UNIX,SOCK_SEQPACKET,0,sv);
  g_busy=busy;
  pid_t pid=fork();
  if(pid==0){ pin(xcore?1:0); close(sv[0]); echo_child(sv[1]); _exit(0); }
  close(sv[1]); pin(0);
  pthread_t lt[64]; for(int i=0;i<nload;i++){ long c=xcore?(2+(i%6)):0; pthread_create(&lt[i],0,loadfn,(void*)c);}
  char b[MSG]; memset(b,0x5a,MSG);
  for(int i=0;i<2000;i++){ send(sv[0],b,MSG,0); ssize_t n; if(busy){while((n=recv(sv[0],b,MSG,MSG_DONTWAIT))<0);} else n=recv(sv[0],b,MSG,0);} /*warmup*/
  double t0=now();
  for(int i=0;i<N;i++){ send(sv[0],b,MSG,0); ssize_t n; if(busy){while((n=recv(sv[0],b,MSG,MSG_DONTWAIT))<0);} else n=recv(sv[0],b,MSG,0); }
  double dt=now()-t0;
  load_run=0;
  printf("mode=%-8s core=%-6s load=%d  RTT=%.2f us  (%.0f rt/s)\n",
         busy?"busypoll":"blocking", xcore?"cross":"same", nload, dt/N*1e6, N/dt);
  kill(pid,9); return 0;
}
