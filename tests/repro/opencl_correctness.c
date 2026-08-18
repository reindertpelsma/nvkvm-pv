// Probe host-shared memory paths: ALLOC_HOST_PTR, USE_HOST_PTR, map/unmap,
// non-float image formats, and repeated map cycles. Each validates vs CPU ref.
#define CL_TARGET_OPENCL_VERSION 200
#include <CL/cl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *SRC =
"__kernel void k(__global float*o,__global const float*a){size_t i=get_global_id(0);o[i]=a[i]*3.0f+7.0f;}\n"
"__kernel void kb(__write_only image2d_t out,__read_only image2d_t in,sampler_t s){\n"
"  int2 p=(int2)(get_global_id(0),get_global_id(1));\n"
"  float4 v=read_imagef(in,s,p); write_imagef(out,p,v);}\n";

static int fails=0;
static void rep(const char*n,int bad,int tot){
    if(bad){printf("  %-18s FAIL  %d/%d wrong\n",n,bad,tot);fails++;}
    else printf("  %-18s ok    (%d checked)\n",n,tot);
}
#define CHK(x,m) do{cl_int _e=(x); if(_e!=CL_SUCCESS){printf("  %-18s ERROR rc=%d\n",m,_e);fails++;}}while(0)

int main(void){
    cl_platform_id pl[8]; cl_uint np=0; clGetPlatformIDs(8,pl,&np);
    cl_device_id dev=0;
    for(cl_uint i=0;i<np;i++){char nm[128]={0};clGetPlatformInfo(pl[i],CL_PLATFORM_NAME,sizeof nm,nm,0);
        if(strstr(nm,"NVIDIA")&&clGetDeviceIDs(pl[i],CL_DEVICE_TYPE_GPU,1,&dev,0)==CL_SUCCESS)break;}
    if(!dev){printf("no NVIDIA device\n");return 2;}
    char dn[128]={0};clGetDeviceInfo(dev,CL_DEVICE_NAME,sizeof dn,dn,0);printf("device: %s\n",dn);
    cl_int e; cl_context ctx=clCreateContext(0,1,&dev,0,0,&e);
    cl_command_queue q=clCreateCommandQueueWithProperties(ctx,dev,0,&e);
    cl_program pr=clCreateProgramWithSource(ctx,1,&SRC,0,&e);
    if(clBuildProgram(pr,1,&dev,"",0,0)!=CL_SUCCESS){char l[4096]={0};
        clGetProgramBuildInfo(pr,dev,CL_PROGRAM_BUILD_LOG,sizeof l,l,0);printf("build:%s\n",l);return 1;}
    cl_kernel k=clCreateKernel(pr,"k",&e);
    const int N=1<<20; size_t gs=N;
    float *ref=malloc(N*4); for(int i=0;i<N;i++) ref[i]=(float)(i%997)*0.25f;

    /* 1. ALLOC_HOST_PTR (pinned) + map to fill, map to read */
    {
        cl_mem A=clCreateBuffer(ctx,CL_MEM_READ_ONLY|CL_MEM_ALLOC_HOST_PTR,N*4,0,&e); CHK(e,"alloc_host in");
        cl_mem O=clCreateBuffer(ctx,CL_MEM_WRITE_ONLY|CL_MEM_ALLOC_HOST_PTR,N*4,0,&e); CHK(e,"alloc_host out");
        float*p=clEnqueueMapBuffer(q,A,CL_TRUE,CL_MAP_WRITE,0,N*4,0,0,0,&e); CHK(e,"map write");
        if(p){memcpy(p,ref,N*4); CHK(clEnqueueUnmapMemObject(q,A,p,0,0,0),"unmap write");}
        clSetKernelArg(k,0,sizeof(cl_mem),&O); clSetKernelArg(k,1,sizeof(cl_mem),&A);
        CHK(clEnqueueNDRangeKernel(q,k,1,0,&gs,0,0,0,0),"run");
        float*r=clEnqueueMapBuffer(q,O,CL_TRUE,CL_MAP_READ,0,N*4,0,0,0,&e); CHK(e,"map read");
        int bad=0; if(r){for(int i=0;i<N;i++){float w=ref[i]*3.0f+7.0f; if(r[i]!=w)bad++;}
            clEnqueueUnmapMemObject(q,O,r,0,0,0);} else bad=N;
        rep("ALLOC_HOST_PTR",bad,N);
        clReleaseMemObject(A);clReleaseMemObject(O);
    }
    /* 2. USE_HOST_PTR over app memory */
    {
        float*ha=aligned_alloc(4096,N*4),*ho=aligned_alloc(4096,N*4);
        memcpy(ha,ref,N*4); memset(ho,0,N*4);
        cl_mem A=clCreateBuffer(ctx,CL_MEM_READ_ONLY|CL_MEM_USE_HOST_PTR,N*4,ha,&e); CHK(e,"use_host in");
        cl_mem O=clCreateBuffer(ctx,CL_MEM_WRITE_ONLY|CL_MEM_USE_HOST_PTR,N*4,ho,&e); CHK(e,"use_host out");
        clSetKernelArg(k,0,sizeof(cl_mem),&O); clSetKernelArg(k,1,sizeof(cl_mem),&A);
        CHK(clEnqueueNDRangeKernel(q,k,1,0,&gs,0,0,0,0),"run");
        float*r=clEnqueueMapBuffer(q,O,CL_TRUE,CL_MAP_READ,0,N*4,0,0,0,&e);
        int bad=0; if(r){for(int i=0;i<N;i++){float w=ref[i]*3.0f+7.0f; if(r[i]!=w)bad++;}
            clEnqueueUnmapMemObject(q,O,r,0,0,0);} else bad=N;
        rep("USE_HOST_PTR",bad,N);
        clReleaseMemObject(A);clReleaseMemObject(O);
    }
    /* 3. repeated map/unmap cycles on the same buffer */
    {
        cl_mem A=clCreateBuffer(ctx,CL_MEM_READ_ONLY|CL_MEM_ALLOC_HOST_PTR,N*4,0,&e);
        cl_mem O=clCreateBuffer(ctx,CL_MEM_WRITE_ONLY|CL_MEM_ALLOC_HOST_PTR,N*4,0,&e);
        clSetKernelArg(k,0,sizeof(cl_mem),&O); clSetKernelArg(k,1,sizeof(cl_mem),&A);
        int bad=0;
        for(int it=0; it<20; it++){
            float*p=clEnqueueMapBuffer(q,A,CL_TRUE,CL_MAP_WRITE,0,N*4,0,0,0,&e);
            if(!p){bad=N;break;}
            for(int i=0;i<N;i++) p[i]=ref[i]+(float)it;
            clEnqueueUnmapMemObject(q,A,p,0,0,0);
            clEnqueueNDRangeKernel(q,k,1,0,&gs,0,0,0,0);
            float*r=clEnqueueMapBuffer(q,O,CL_TRUE,CL_MAP_READ,0,N*4,0,0,0,&e);
            if(!r){bad=N;break;}
            for(int i=0;i<N;i+=997){ float w=(ref[i]+(float)it)*3.0f+7.0f; if(r[i]!=w) bad++; }
            clEnqueueUnmapMemObject(q,O,r,0,0,0);
        }
        rep("map/unmap x20",bad,20*(N/997));
        clReleaseMemObject(A);clReleaseMemObject(O);
    }
    /* 4. image with CL_UNORM_INT8 (what real image filters use) */
    {
        cl_bool img=0; clGetDeviceInfo(dev,CL_DEVICE_IMAGE_SUPPORT,sizeof img,&img,0);
        if(!img) printf("  %-18s SKIP\n","image UNORM_INT8");
        else{
            const int W=1024,H=1024; unsigned char*ib=malloc(W*H*4),*ob=malloc(W*H*4);
            for(int i=0;i<W*H*4;i++) ib[i]=(unsigned char)(i%251);
            cl_image_format f={CL_RGBA,CL_UNORM_INT8}; cl_image_desc d; memset(&d,0,sizeof d);
            d.image_type=CL_MEM_OBJECT_IMAGE2D; d.image_width=W; d.image_height=H;
            cl_mem IM=clCreateImage(ctx,CL_MEM_READ_ONLY|CL_MEM_COPY_HOST_PTR,&f,&d,ib,&e); CHK(e,"img8 in");
            cl_mem OM=clCreateImage(ctx,CL_MEM_WRITE_ONLY,&f,&d,0,&e); CHK(e,"img8 out");
            cl_sampler_properties sp[]={CL_SAMPLER_NORMALIZED_COORDS,CL_FALSE,
                CL_SAMPLER_ADDRESSING_MODE,CL_ADDRESS_CLAMP_TO_EDGE,
                CL_SAMPLER_FILTER_MODE,CL_FILTER_NEAREST,0};
            cl_sampler s=clCreateSamplerWithProperties(ctx,sp,&e);
            cl_kernel kb=clCreateKernel(pr,"kb",&e);
            clSetKernelArg(kb,0,sizeof(cl_mem),&OM); clSetKernelArg(kb,1,sizeof(cl_mem),&IM);
            clSetKernelArg(kb,2,sizeof(cl_sampler),&s);
            size_t g2[2]={W,H};
            CHK(clEnqueueNDRangeKernel(q,kb,2,0,g2,0,0,0,0),"img8 run");
            size_t o3[3]={0,0,0},r3[3]={W,H,1};
            CHK(clEnqueueReadImage(q,OM,CL_TRUE,o3,r3,0,0,ob,0,0,0),"img8 read");
            int bad=0; for(int i=0;i<W*H*4;i++){int df=(int)ob[i]-(int)ib[i]; if(df<0)df=-df; if(df>1) bad++;}
            rep("image UNORM_INT8",bad,W*H*4);
        }
    }
    printf("RESULT: %s (%d failed)\n", fails?"FAIL":"PASS", fails);
    return fails?1:0;
}
