// Characterise the repeated map/unmap failure: which iteration first goes wrong,
// and whether the wrong value is stale-from-a-previous-iteration, zero, or junk.
#define CL_TARGET_OPENCL_VERSION 200
#include <CL/cl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static const char *SRC =
"__kernel void k(__global float*o,__global const float*a){size_t i=get_global_id(0);o[i]=a[i]*3.0f+7.0f;}\n";
int main(int argc,char**argv){
    int ITER = argc>1?atoi(argv[1]):20;
    int USE_PINNED = argc>2?atoi(argv[2]):1;   /* 1=ALLOC_HOST_PTR, 0=plain device buffer */
    int USE_FINISH = argc>3?atoi(argv[3]):1;   /* 1=clFinish after kernel, 0=rely on blocking map */
    int NBYTES     = argc>4?atoi(argv[4]):16;  /* N = 1<<NBYTES */
    int CHURN      = argc>5?atoi(argv[5]):0;   /* alloc+free this many buffers first */
    cl_platform_id pl[8];cl_uint np=0;clGetPlatformIDs(8,pl,&np);cl_device_id dev=0;
    for(cl_uint i=0;i<np;i++){char nm[128]={0};clGetPlatformInfo(pl[i],CL_PLATFORM_NAME,sizeof nm,nm,0);
        if(strstr(nm,"NVIDIA")&&clGetDeviceIDs(pl[i],CL_DEVICE_TYPE_GPU,1,&dev,0)==CL_SUCCESS)break;}
    if(!dev){printf("no device\n");return 2;}
    cl_int e;cl_context ctx=clCreateContext(0,1,&dev,0,0,&e);
    cl_command_queue q=clCreateCommandQueueWithProperties(ctx,dev,0,&e);
    cl_program pr=clCreateProgramWithSource(ctx,1,&SRC,0,&e);
    clBuildProgram(pr,1,&dev,"",0,0);
    cl_kernel k=clCreateKernel(pr,"k",&e);
    const int N=1<<NBYTES; size_t gs=N;
    cl_mem_flags extra = USE_PINNED?CL_MEM_ALLOC_HOST_PTR:0;
    /* Optional churn: create and release buffers so their GPA window extents go
       back on the free list before the real buffers are allocated. */
    if(CHURN){
        for(int c=0;c<CHURN;c++){
            cl_mem t1=clCreateBuffer(ctx,CL_MEM_READ_ONLY|extra,N*4,0,&e);
            cl_mem t2=clCreateBuffer(ctx,CL_MEM_WRITE_ONLY|extra,N*4,0,&e);
            float*tp=clEnqueueMapBuffer(q,t1,CL_TRUE,CL_MAP_WRITE,0,N*4,0,0,0,&e);
            if(tp){ for(int i=0;i<N;i++) tp[i]=-999.0f; clEnqueueUnmapMemObject(q,t1,tp,0,0,0); }
            clSetKernelArg(k,0,sizeof(cl_mem),&t2);clSetKernelArg(k,1,sizeof(cl_mem),&t1);
            clEnqueueNDRangeKernel(q,k,1,0,&gs,0,0,0,0); clFinish(q);
            clReleaseMemObject(t1); clReleaseMemObject(t2);
        }
        printf("churned %d buffer pairs before test\n",CHURN);
    }
    cl_mem A=clCreateBuffer(ctx,CL_MEM_READ_ONLY|extra,N*4,0,&e);
    cl_mem O=clCreateBuffer(ctx,CL_MEM_WRITE_ONLY|extra,N*4,0,&e);
    clSetKernelArg(k,0,sizeof(cl_mem),&O);clSetKernelArg(k,1,sizeof(cl_mem),&A);
    printf("mode: %s, %d iters, N=%d, clFinish=%s\n", USE_PINNED?"ALLOC_HOST_PTR(pinned)":"device buffer", ITER, N, USE_FINISH?"yes":"NO (blocking map must order)");
    void *prev_in=0,*prev_out=0;
    for(int it=0;it<ITER;it++){
        float*p=clEnqueueMapBuffer(q,A,CL_TRUE,CL_MAP_WRITE,0,N*4,0,0,0,&e);
        if(!p){printf("  iter %2d: map WRITE failed rc=%d\n",it,e);return 1;}
        for(int i=0;i<N;i++) p[i]=(float)it*1000.0f+(float)(i%97);
        clEnqueueUnmapMemObject(q,A,p,0,0,0);
        clEnqueueNDRangeKernel(q,k,1,0,&gs,0,0,0,0);
        if(USE_FINISH) clFinish(q);
        float*r=clEnqueueMapBuffer(q,O,CL_TRUE,CL_MAP_READ,0,N*4,0,0,0,&e);
        if(!r){printf("  iter %2d: map READ failed rc=%d\n",it,e);return 1;}
        int bad=0,first=-1;
        for(int i=0;i<N;i++){ float w=((float)it*1000.0f+(float)(i%97))*3.0f+7.0f;
            if(r[i]!=w){ if(first<0)first=i; bad++; } }
        if(bad){
            float got=r[first], want=((float)it*1000.0f+(float)(first%97))*3.0f+7.0f;
            const char*kind="junk";
            if(got==0.0f) kind="zero";
            else for(int pit=0;pit<it;pit++){
                float sw=((float)pit*1000.0f+(float)(first%97))*3.0f+7.0f;
                if(got==sw){ kind="STALE from earlier iteration"; break; } }
            printf("  iter %2d: %d/%d wrong, first idx %d got %.1f want %.1f  -> %s\n",
                   it,bad,N,first,got,want,kind);
        } else printf("  iter %2d: ok\n",it);
        printf("           map ptrs: in=%p out=%p%s\n", (void*)p, (void*)r,
               (prev_in&&(p!=prev_in||r!=prev_out))?"  (CHANGED since last iter)":"");
        prev_in=p; prev_out=r;
        clEnqueueUnmapMemObject(q,O,r,0,0,0);
    }
    return 0;
}
