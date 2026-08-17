// sha256.cu — SHA-256 hash throughput (crypto / mining-style GPU workload).
// Each thread hashes a distinct single-block (<=55B) message (nonce varied).
// Reports MH/s. Correctness: SHA-256("") first word == 0xe3b0c442.
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <ctime>
#define CK(x) do{cudaError_t e=(x); if(e){printf("CUDA %s:%d %s\n",__FILE__,__LINE__,cudaGetErrorString(e));return 1;}}while(0)
static double now(){timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec/1e9;}
__device__ __constant__ uint32_t K[64]={
0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
#define ROR(x,n) (((x)>>(n))|((x)<<(32-(n))))
__device__ void sha256_block(const uint8_t*msg,int len,uint32_t*h){
    uint8_t blk[64]={0};
    for(int i=0;i<len;i++) blk[i]=msg[i];
    blk[len]=0x80; uint64_t bits=(uint64_t)len*8;
    for(int i=0;i<8;i++) blk[63-i]=(bits>>(8*i))&0xff;
    uint32_t w[64];
    for(int i=0;i<16;i++) w[i]=(blk[i*4]<<24)|(blk[i*4+1]<<16)|(blk[i*4+2]<<8)|blk[i*4+3];
    for(int i=16;i<64;i++){ uint32_t s0=ROR(w[i-15],7)^ROR(w[i-15],18)^(w[i-15]>>3);
        uint32_t s1=ROR(w[i-2],17)^ROR(w[i-2],19)^(w[i-2]>>10); w[i]=w[i-16]+s0+w[i-7]+s1; }
    uint32_t a=0x6a09e667,b=0xbb67ae85,c=0x3c6ef372,d=0xa54ff53a,e=0x510e527f,f=0x9b05688c,g=0x1f83d9ab,hh=0x5be0cd19;
    for(int i=0;i<64;i++){
        uint32_t S1=ROR(e,6)^ROR(e,11)^ROR(e,25), ch=(e&f)^((~e)&g), t1=hh+S1+ch+K[i]+w[i];
        uint32_t S0=ROR(a,2)^ROR(a,13)^ROR(a,22), mj=(a&b)^(a&c)^(b&c), t2=S0+mj;
        hh=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;
    }
    h[0]=a+0x6a09e667;h[1]=b+0xbb67ae85;h[2]=c+0x3c6ef372;h[3]=d+0xa54ff53a;
    h[4]=e+0x510e527f;h[5]=f+0x9b05688c;h[6]=g+0x1f83d9ab;h[7]=hh+0x5be0cd19;
}
__global__ void hashk(uint32_t*acc,uint32_t base,int per){
    uint32_t idx=blockIdx.x*blockDim.x+threadIdx.x;
    uint8_t m[16]={'n','v','k','v','m',0,0,0,0,0,0,0,0,0,0,0};
    uint32_t r=0;
    for(int k=0;k<per;k++){ uint32_t nonce=base+idx*per+k;
        m[8]=nonce&0xff;m[9]=(nonce>>8)&0xff;m[10]=(nonce>>16)&0xff;m[11]=(nonce>>24)&0xff;
        uint32_t h[8]; sha256_block(m,12,h); r^=h[0]; }
    if(idx==0) atomicXor(acc,r); // keep result live, prevent dead-code elim
}
__global__ void checkk(uint32_t*out){ uint8_t m[1]; uint32_t h[8]; sha256_block(m,0,h); out[0]=h[0]; }
int main(int argc,char**argv){
    int threads=argc>1?atoi(argv[1]):(1<<20); int per=argc>2?atoi(argv[2]):64; int reps=argc>3?atoi(argv[3]):10;
    uint32_t*acc; CK(cudaMalloc(&acc,4)); CK(cudaMemset(acc,0,4));
    int bs=256, gs=(threads+bs-1)/bs;
    hashk<<<gs,bs>>>(acc,0,per); CK(cudaDeviceSynchronize());
    double t=now();
    for(int r=0;r<reps;r++) hashk<<<gs,bs>>>(acc,r*threads*per,per);
    CK(cudaDeviceSynchronize()); t=now()-t;
    uint32_t*co; CK(cudaMalloc(&co,4)); checkk<<<1,1>>>(co); CK(cudaDeviceSynchronize());
    uint32_t hc; CK(cudaMemcpy(&hc,co,4,cudaMemcpyDeviceToHost));
    printf("METRIC sha256_MHs %.1f\n", (double)threads*per*reps/t/1e6);
    printf("CHECK %s\n", (hc==0xe3b0c442u)?"ok":"FAIL");
    return 0;
}
