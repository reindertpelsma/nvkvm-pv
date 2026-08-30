/*
 * zd_lifetime_test.c -- is "refcount says nobody references it" a reliable
 * release gate?
 *
 * MEMORY_DEVICE_GENERIC gives no page_free() callback (the pgmap holds a
 * permanent reference, so the count never reaches 0) and COHERENT is not
 * compiled into stock kernels. So a window GPA can only be safely released by
 * CHECKING that nothing references its pages. This asks whether that check
 * actually sees a pin that outlives the mapping -- the exact case that would
 * otherwise let a GPA be re-pointed underneath a live reference.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/ioctl.h>

#define ZD_IOC_HOLD _IOW('Z', 3, unsigned long)
#define ZD_IOC_DROP _IO('Z', 4)
#define LEN (64u * 1024u)

static int refcount(void)
{
    char b[1024]; int fd = open("/proc/zd_probe", O_RDONLY); ssize_t n; char *p;
    if (fd < 0) return -1;
    n = read(fd, b, sizeof(b) - 1); close(fd);
    if (n <= 0) return -1;
    b[n] = 0; p = strstr(b, "page_ref_count");
    return p ? atoi(p + strlen("page_ref_count")) : -1;
}

int main(void)
{
    int fd = open("/dev/zd_probe", O_RDWR);
    if (fd < 0) { perror("open"); return 2; }

    printf("baseline (nothing mapped)          refcount=%d\n", refcount());

    void *m = mmap(NULL, LEN, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (m == MAP_FAILED) { perror("mmap"); return 2; }
    int mapped = refcount();
    printf("after mmap                          refcount=%d\n", mapped);

    unsigned long ua = (unsigned long)m;
    if (ioctl(fd, ZD_IOC_HOLD, &ua)) { perror("HOLD"); return 2; }
    int pinned = refcount();
    printf("after a pin is taken and HELD       refcount=%d\n", pinned);

    /* The dangerous moment: the mapping goes away while the pin remains.
     * If the refcount does not still show the pin here, a release gate built
     * on it is blind exactly when it matters. */
    munmap(m, LEN);
    int after_unmap = refcount();
    printf("after munmap, pin still held        refcount=%d  <-- the gate\n", after_unmap);

    if (ioctl(fd, ZD_IOC_DROP)) { perror("DROP"); return 2; }   /* check it! */
    int after_drop = refcount();
    printf("after the pin is dropped            refcount=%d\n", after_drop);

    printf("\n");
    if (after_unmap > after_drop && after_drop == 1) {
        printf("RESULT: PASS -- a pin outliving the mapping is still visible in\n"
               "        the refcount, and it returns to the 1 baseline once\n"
               "        dropped. A release gate on refcount==1 is sound.\n");
        return 0;
    }
    printf("RESULT: FAIL -- refcount does not distinguish held from released\n"
           "        (unmap=%d drop=%d). A gate on it would be blind.\n",
           after_unmap, after_drop);
    return 1;
}
