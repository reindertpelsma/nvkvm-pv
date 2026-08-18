#include <stdio.h>
#include <string.h>
#include <vulkan/vulkan.h>
int main(void){
    VkInstance inst; VkApplicationInfo ai={VK_STRUCTURE_TYPE_APPLICATION_INFO};
    ai.apiVersion=VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici={VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ici.pApplicationInfo=&ai;
    VkResult r=vkCreateInstance(&ici,0,&inst);
    if(r){printf("vkCreateInstance rc=%d\n",r);return 1;}
    uint32_t n=0; vkEnumeratePhysicalDevices(inst,&n,0);
    VkPhysicalDevice pd[8]; if(n>8)n=8; vkEnumeratePhysicalDevices(inst,&n,pd);
    for(uint32_t i=0;i<n;i++){
        VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(pd[i],&p);
        if(p.vendorID!=0x10DE) continue;
        printf("device: %s (vendor 0x%X)\n",p.deviceName,p.vendorID);
        uint32_t qn=0; vkGetPhysicalDeviceQueueFamilyProperties(pd[i],&qn,0);
        VkQueueFamilyProperties qf[16]; if(qn>16)qn=16;
        vkGetPhysicalDeviceQueueFamilyProperties(pd[i],&qn,qf);
        int fam=-1; for(uint32_t q=0;q<qn;q++) if(qf[q].queueFlags&VK_QUEUE_COMPUTE_BIT){fam=q;break;}
        printf("compute queue family: %d (of %u)\n",fam,qn);
        if(fam<0){printf("RESULT: no compute queue\n");return 2;}
        float pri=1.0f;
        VkDeviceQueueCreateInfo qci={VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qci.queueFamilyIndex=fam; qci.queueCount=1; qci.pQueuePriorities=&pri;
        VkDeviceCreateInfo dci={VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        dci.queueCreateInfoCount=1; dci.pQueueCreateInfos=&qci;
        VkDevice dev; VkResult dr=vkCreateDevice(pd[i],&dci,0,&dev);
        printf("RESULT: vkCreateDevice rc=%d %s\n",dr,dr?"FAILED":"OK");
        if(!dr) vkDestroyDevice(dev,0);
        return dr?3:0;
    }
    printf("RESULT: no NVIDIA physical device\n"); return 4;
}
