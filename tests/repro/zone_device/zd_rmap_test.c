/*
 * zd_rmap_test.c -- does the kernel TRACK mappings of a window page?
 *
 * The whole question: VM_PFNMAP has no rmap, so relocating a range can only
 * rewrite the VMA in front of it. If a ZONE_DEVICE window page is rmap-tracked
 * then page_mapcount() rises when a second process maps it, and the kernel is
 * doing the find-every-view bookkeeping for us.
 *
 * PASS: mapcount goes 0 -> 1 (mmap) -> 2 (fork).
 * FAIL: mapcount stays 0 -- the mapping exists but the kernel does not account
 *       it, i.e. a pte_special with no rmap, and struct page bought us nothing.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <errno.h>

#define ZD_IOC_GUP  _IOW('Z', 1, unsigned long)
#define ZD_IOC_PIN  _IOW('Z', 2, unsigned long)

#define LEN (64u * 1024u)

static void dump(const char *when)
{
    char buf[1024];
    int fd = open("/proc/zd_probe", O_RDONLY);
    ssize_t n;
    if (fd < 0) { perror("open /proc/zd_probe"); return; }
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return;
    buf[n] = 0;
    printf("--- %s ---\n%s", when, buf);
}

static int mapcount(void)
{
    char buf[1024]; int fd = open("/proc/zd_probe", O_RDONLY); ssize_t n; char *p;
    if (fd < 0) return -1;
    n = read(fd, buf, sizeof(buf) - 1); close(fd);
    if (n <= 0) return -1;
    buf[n] = 0;
    p = strstr(buf, "page_mapcount");
    return p ? atoi(p + strlen("page_mapcount")) : -1;
}

int main(void)
{
    int fd = open("/dev/zd_probe", O_RDWR);
    if (fd < 0) { perror("open /dev/zd_probe"); return 2; }

    dump("before mmap");
    int before = mapcount();

    void *m = mmap(NULL, LEN, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (m == MAP_FAILED) {
        perror("mmap");
        printf("\nRESULT: window pages could NOT be inserted into a user VMA\n");
        return 1;
    }
    printf("mmap OK at %p\n", m);
    dump("after mmap (one mapping)");
    int one = mapcount();

    /* A second process mapping the same pages. MAP_SHARED survives fork as a
     * shared mapping, so if rmap accounts these pages the count must rise. */
    int to_parent[2], to_child[2];
    if (pipe(to_parent) || pipe(to_child)) { perror("pipe"); return 2; }

    pid_t pid = fork();
    if (pid == 0) {
        char c;
        if (write(to_parent[1], "r", 1) != 1) _exit(3);
        if (read(to_child[0], &c, 1) != 1) _exit(3);
        _exit(0);
    }
    char c;
    if (read(to_parent[0], &c, 1) != 1) return 2;
    dump("after fork (two mappings)");
    int two = mapcount();
    if (write(to_child[1], "g", 1) != 1) return 2;
    waitpid(pid, NULL, 0);

    /* ---- Q3/Q4: can the kernel GUP and PIN these pages, and can the CPU
     * actually touch them? GUP is what nvkvm does to the buffer and PIN is
     * what a DMA driver does; either failing sinks the design. ---- */
    unsigned long ua = (unsigned long)m;
    printf("\n--- GUP / PIN on the mapped window page ---\n");
    printf("  get_user_pages_fast: %s\n",
           ioctl(fd, ZD_IOC_GUP, &ua) == 0 ? "OK" : strerror(errno));
    printf("  pin_user_pages_fast: %s\n",
           ioctl(fd, ZD_IOC_PIN, &ua) == 0 ? "OK" : strerror(errno));

    printf("\n--- CPU access through the mapping ---\n");
    printf("  (this window sub-range may be unbacked; a read that returns all\n"
           "   ones means the mapping works and the GPA has no backing)\n");
    volatile unsigned int *w = (volatile unsigned int *)m;
    unsigned int r0 = w[0];
    printf("  initial read       = 0x%08x\n", r0);
    w[0] = 0x5A5A5A5A;
    unsigned int r1 = w[0];
    printf("  write 0x5A5A5A5A -> readback = 0x%08x  %s\n", r1,
           r1 == 0x5A5A5A5A ? "PERSISTS (backed)" : "does not persist (unbacked GPA)");

    printf("\nmapcount: before=%d  after mmap=%d  after fork=%d\n", before, one, two);
    if (one > before && two > one) {
        printf("RESULT: PASS -- the kernel tracks these mappings (rmap works)\n");
        return 0;
    }
    if (one == before && before == 0) {
        printf("RESULT: FAIL -- mapping exists but is unaccounted (pte_special,\n"
               "        no rmap). struct page alone does not buy the tracking.\n");
        return 1;
    }
    printf("RESULT: PARTIAL -- counted on mmap but not across fork\n");
    return 1;
}
