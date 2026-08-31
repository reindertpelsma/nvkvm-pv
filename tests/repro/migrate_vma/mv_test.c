/* Try migrate_vma_setup on the two shapes that matter. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#define MV_IOC_TRY _IOW('M', 1, unsigned long)
#define MV_IOC_DST _IOW('M', 2, unsigned long)
#define LEN (64u*1024u)

static void try_one(int fd, const char *label, int flags)
{
    void *m = mmap(NULL, LEN, PROT_READ|PROT_WRITE, flags, -1, 0);
    if (m == MAP_FAILED) { printf("%-34s mmap failed\n", label); return; }
    memset(m, 0xAB, LEN);              /* fault it in */
    unsigned long ua = (unsigned long)m;
    int rc = ioctl(fd, MV_IOC_TRY, &ua);
    printf("%-34s migrate_vma_setup -> %s\n", label,
           rc == 0 ? "ACCEPTED" : strerror(errno));
    munmap(m, LEN);
}

int main(void)
{
    int fd = open("/dev/mv_probe", O_RDWR);
    if (fd < 0) { perror("open /dev/mv_probe"); return 2; }
    try_one(fd, "MAP_PRIVATE|MAP_ANONYMOUS",  MAP_PRIVATE|MAP_ANONYMOUS);
    try_one(fd, "MAP_SHARED|MAP_ANONYMOUS",   MAP_SHARED|MAP_ANONYMOUS);
    printf("\n(the second is what cuMemHostAlloc returns)\n\n");

    /* The discriminator: does an actual MOVE succeed for private-anon but not
     * for shmem? If so the blocker is the source's address_space, not the
     * destination page type. */
    int shapes[2] = { MAP_PRIVATE|MAP_ANONYMOUS, MAP_SHARED|MAP_ANONYMOUS };
    const char *names[2] = { "MAP_PRIVATE|MAP_ANONYMOUS", "MAP_SHARED|MAP_ANONYMOUS" };
    for (int i = 0; i < 2; i++) {
        void *m = mmap(NULL, LEN, PROT_READ|PROT_WRITE, shapes[i], -1, 0);
        if (m == MAP_FAILED) continue;
        memset(m, 0xCD, LEN);
        unsigned long ua = (unsigned long)m;
        int rc = ioctl(fd, MV_IOC_DST, &ua);
        printf("%-34s actual move -> %s\n", names[i],
               rc == 0 ? "ran (see dmesg for migrated=YES/NO)" : strerror(errno));
        munmap(m, LEN);
    }
    return 0;
}
