/* Does the guest see its own writes to an OpenCL buffer, and does the GPU?
 * Splits "CPU/GPU views diverge" from "the CPU's own writes are lost".
 *   argv: <churn> [N_log2]
 */
#define CL_TARGET_OPENCL_VERSION 120
#include <CL/cl.h>
#include <stdio.h>
#include <stdlib.h>
static const char *SRC =
"__kernel void k(__global float*o,__global const float*a){size_t i=get_global_id(0);o[i]=a[i];}\n";
int main(int argc,char**argv){
    int CHURN=argc>1?atoi(argv[1]):3;
    int RO=argc>3?atoi(argv[3]):0;   /* 1 = CL_MEM_READ_ONLY/WRITE_ONLY like clchurn */
    size_t N=(size_t)1<<(argc>2?atoi(argv[2]):20), B=N*sizeof(float);
    cl_platform_id p; cl_device_id d; cl_int e;
    clGetPlatformIDs(1,&p,0); clGetDeviceIDs(p,CL_DEVICE_TYPE_GPU,1,&d,0);
    cl_context c=clCreateContext(0,1,&d,0,0,&e);
    cl_command_queue q=clCreateCommandQueue(c,d,0,&e);
    cl_program pr=clCreateProgramWithSource(c,1,&SRC,0,&e);
    clBuildProgram(pr,1,&d,0,0,0);
    cl_kernel k=clCreateKernel(pr,"k",&e);

    /* Churn exactly as opencl_map_churn.c does: allocate a pair, map and fill
     * the input, run the kernel over it, then release both -- so the device
     * memory is actually used before it goes back to the driver's pool.
     * CHURN_MODE (argv[4]): 0 = alloc/release only, 1 = +map/fill, 2 = +kernel. */
    int CHURN_MODE = argc>4?atoi(argv[4]):2;
    for(int i=0;i<CHURN;i++){
        cl_mem a=clCreateBuffer(c,CL_MEM_READ_ONLY |CL_MEM_ALLOC_HOST_PTR,B,0,&e);
        cl_mem b=clCreateBuffer(c,CL_MEM_WRITE_ONLY|CL_MEM_ALLOC_HOST_PTR,B,0,&e);
        if(CHURN_MODE>=1){
            float*t=clEnqueueMapBuffer(q,a,CL_TRUE,CL_MAP_WRITE,0,B,0,0,0,&e);
            if(t){ for(size_t j=0;j<N;j++) t[j]=-999.0f;
                   clEnqueueUnmapMemObject(q,a,t,0,0,0); }
        }
        if(CHURN_MODE>=2){
            clSetKernelArg(k,0,sizeof(cl_mem),&b); clSetKernelArg(k,1,sizeof(cl_mem),&a);
            clEnqueueNDRangeKernel(q,k,1,0,&N,0,0,0,0); clFinish(q);
        }
        /* mode 3: map and write the churn buffer but NEVER release it, to
         * separate "a mapping existed" from "the RM object was freed while
         * nvkvm still held its window mapping". */
        if(CHURN_MODE!=3){ clReleaseMemObject(a); clReleaseMemObject(b); }
    }
    printf("churn=%d N=%zu\n",CHURN,N);

    cl_mem_flags fin = (RO?CL_MEM_READ_ONLY:CL_MEM_READ_WRITE)|CL_MEM_ALLOC_HOST_PTR;
    cl_mem_flags fout= (RO?CL_MEM_WRITE_ONLY:CL_MEM_READ_WRITE)|CL_MEM_ALLOC_HOST_PTR;
    printf("flags: %s\n", RO?"READ_ONLY/WRITE_ONLY":"READ_WRITE");
    cl_mem in =clCreateBuffer(c,fin,B,0,&e);
    cl_mem out=clCreateBuffer(c,fout,B,0,&e);

    /* 1. CPU writes a pattern */
    float*w=clEnqueueMapBuffer(q,in,CL_TRUE,CL_MAP_WRITE,0,B,0,0,0,&e);
    for(size_t i=0;i<N;i++) w[i]=(float)(i%1000)+1.0f;
    printf("wrote pattern via %p\n",(void*)w);
    clEnqueueUnmapMemObject(q,in,w,0,0,0); clFinish(q);

    /* 2. CPU reads it back -- does the guest see its OWN writes? */
    float*r=clEnqueueMapBuffer(q,in,CL_TRUE,CL_MAP_READ,0,B,0,0,0,&e);
    size_t bad=0; float first=-1; size_t fi=0;
    for(size_t i=0;i<N;i++){ float want=(float)(i%1000)+1.0f;
        if(r[i]!=want){ if(!bad){first=r[i];fi=i;} bad++; } }
    printf("CPU readback: %zu/%zu wrong%s", bad,N, bad?"":"  -> guest sees its own writes\n");
    if(bad) printf("  first idx %zu got %.1f want %.1f\n",fi,first,(float)(fi%1000)+1.0f);
    clEnqueueUnmapMemObject(q,in,r,0,0,0); clFinish(q);

    /* 3. GPU copies in -> out; does the GPU see the CPU's writes? */
    clSetKernelArg(k,0,sizeof(cl_mem),&out); clSetKernelArg(k,1,sizeof(cl_mem),&in);
    clEnqueueNDRangeKernel(q,k,1,0,&N,0,0,0,0); clFinish(q);
    float*o=clEnqueueMapBuffer(q,out,CL_TRUE,CL_MAP_READ,0,B,0,0,0,&e);
    size_t gbad=0; float gfirst=-1; size_t gfi=0;
    for(size_t i=0;i<N;i++){ float want=(float)(i%1000)+1.0f;
        if(o[i]!=want){ if(!gbad){gfirst=o[i];gfi=i;} gbad++; } }
    printf("GPU view    : %zu/%zu wrong%s", gbad,N, gbad?"":"  -> GPU sees the CPU's writes\n");
    if(gbad) printf("  first idx %zu got %.1f want %.1f\n",gfi,gfirst,(float)(gfi%1000)+1.0f);
    clEnqueueUnmapMemObject(q,out,o,0,0,0); clFinish(q);
    return (bad||gbad)?1:0;
}
