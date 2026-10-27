/* The A30 codec loses its setup on suspend without suspending the PCM, so a stream
 * that was open across a sleep plays silence until the device is reopened. Preloaded
 * into an app, this reopens its playback PCM underneath alsa-lib on the first PCM
 * ioctl after a suspend (replaying hw/sw params and the buffer mapping on the same fd
 * and address), then plays a little silence and lets it run dry so the kernel puts it
 * in XRUN. Whatever alsa-lib asks next, it sees a real underrun, and the app's recovery
 * prepares and restarts the stream. Raw syscalls only: it needs no libc to build. */
#include <stdarg.h>
#include <stddef.h>

#define NR_write 4
#define NR_close 6
#define NR_ioctl 54
#define NR_dup2 63
#define NR_munmap 91
#define NR_nanosleep 162
#define NR_mmap2 192
#define NR_fcntl64 221
#define NR_clock_gettime 263
#define NR_openat 322
#define NR_readlinkat 332

#define PCM_HWSYNC 0x4122
#define PCM_PREPARE 0x4140
#define PCM_START 0x4142
#define PCM_SYNC_PTR 0xc0844123
#define AT_FDCWD -100
#define F_GETFD 1
#define F_SETFD 2
#define F_GETFL 3
#define MAX_MAPS 4
#define MAX_ARG 1024

extern int *__errno_location(void);

static long sys6(long nr, long a, long b, long c, long d, long e, long f)
{
    register long r7 __asm__("r7") = nr;
    register long r0 __asm__("r0") = a;
    register long r1 __asm__("r1") = b;
    register long r2 __asm__("r2") = c;
    register long r3 __asm__("r3") = d;
    register long r4 __asm__("r4") = e;
    register long r5 __asm__("r5") = f;
    __asm__ volatile("svc #0" : "+r"(r0) : "r"(r7), "r"(r1), "r"(r2), "r"(r3), "r"(r4), "r"(r5) : "memory");
    return r0;
}

static long ret_errno(long r)
{
    if (r < 0 && r > -4096) {
        *__errno_location() = (int)-r;
        return -1;
    }
    return r;
}

struct ts { long sec, nsec; };

static long long now_ns(int clk)
{
    struct ts t;
    sys6(NR_clock_gettime, clk, (long)&t, 0, 0, 0, 0);
    return (long long)t.sec * 1000000000LL + t.nsec;
}

/* CLOCK_BOOTTIME counts suspended time, CLOCK_MONOTONIC does not. */
static long long sleep_ns(void)
{
    return now_ns(7) - now_ns(1);
}

static void say(const char *s)
{
    size_t n = 0;
    while (s[n])
        n++;
    sys6(NR_write, 2, (long)s, n, 0, 0, 0);
}

struct map { void *addr; size_t len; int prot, flags; long pgoff; };

static int pcm_fd = -1;
static char hw_params[MAX_ARG], sw_params[MAX_ARG];
static unsigned long hw_cmd, sw_cmd;
static struct map maps[MAX_MAPS];
static int nmaps;
static long long slept;

static int is_playback_pcm(int fd)
{
    char link[32], path[64];
    int n = 0, len;
    const char *p = "/proc/self/fd/";
    while (p[n]) { link[n] = p[n]; n++; }
    if (fd >= 10) link[n++] = '0' + fd / 10 % 10;
    link[n++] = '0' + fd % 10;
    link[n] = 0;
    len = (int)sys6(NR_readlinkat, AT_FDCWD, (long)link, (long)path, sizeof(path) - 1, 0, 0);
    if (len < 12)
        return 0;
    path[len] = 0;
    return path[len - 1] == 'p' && path[5] == 's' && path[9] == 'p' && path[10] == 'c' && path[11] == 'm';
}

static int reopen(int fd)
{
    char link[32], path[64];
    int n = 0, len, fl, fdfl, nfd, i;
    const char *p = "/proc/self/fd/";
    while (p[n]) { link[n] = p[n]; n++; }
    if (fd >= 10) link[n++] = '0' + fd / 10 % 10;
    link[n++] = '0' + fd % 10;
    link[n] = 0;
    len = (int)sys6(NR_readlinkat, AT_FDCWD, (long)link, (long)path, sizeof(path) - 1, 0, 0);
    if (len <= 0)
        return -1;
    path[len] = 0;
    fl = (int)sys6(NR_fcntl64, fd, F_GETFL, 0, 0, 0, 0);
    fdfl = (int)sys6(NR_fcntl64, fd, F_GETFD, 0, 0, 0, 0);
    for (i = 0; i < nmaps; i++)
        sys6(NR_munmap, (long)maps[i].addr, maps[i].len, 0, 0, 0, 0);
    sys6(NR_close, fd, 0, 0, 0, 0, 0);
    nfd = (int)sys6(NR_openat, AT_FDCWD, (long)path, fl & ~0100, 0, 0, 0);
    if (nfd < 0)
        return -1;
    if (nfd != fd) {
        sys6(NR_dup2, nfd, fd, 0, 0, 0, 0);
        sys6(NR_close, nfd, 0, 0, 0, 0, 0);
    }
    sys6(NR_fcntl64, fd, F_SETFD, fdfl, 0, 0, 0);
    if (hw_cmd && sys6(NR_ioctl, fd, hw_cmd, (long)hw_params, 0, 0, 0) < 0)
        return -1;
    if (sw_cmd)
        sys6(NR_ioctl, fd, sw_cmd, (long)sw_params, 0, 0, 0);
    for (i = 0; i < nmaps; i++)
        sys6(NR_mmap2, (long)maps[i].addr, maps[i].len, maps[i].prot, maps[i].flags | 0x10 /* MAP_FIXED */, fd, maps[i].pgoff);
    for (i = 0; i < nmaps; i++)
        if (maps[i].pgoff == 0) {
            char *d = maps[i].addr;
            size_t k;
            for (k = 0; k < maps[i].len; k++)
                d[k] = 0;
        }
    sys6(NR_ioctl, fd, PCM_PREPARE, 0, 0, 0, 0);
    /* struct snd_pcm_sync_ptr: flags, 64-byte status, then control { appl_ptr, avail_min } */
    unsigned int sp[33] = { 0 };
    sp[17] = 256;
    sp[18] = 1;
    sys6(NR_ioctl, fd, PCM_SYNC_PTR, (long)sp, 0, 0, 0);
    sys6(NR_ioctl, fd, PCM_START, 0, 0, 0, 0);
    struct ts nap = { 0, 150000000 };
    sys6(NR_nanosleep, (long)&nap, 0, 0, 0, 0, 0);
    sys6(NR_ioctl, fd, PCM_HWSYNC, 0, 0, 0, 0);
    return 0;
}

int ioctl(int fd, unsigned long cmd, ...)
{
    va_list ap;
    void *arg;
    unsigned size = (cmd >> 16) & 0x3fff;
    va_start(ap, cmd);
    arg = va_arg(ap, void *);
    va_end(ap);

    if (((cmd >> 8) & 0xff) == 'A') {
        if (pcm_fd != fd && is_playback_pcm(fd)) {
            pcm_fd = fd;
            nmaps = 0;
            hw_cmd = sw_cmd = 0;
            slept = sleep_ns();
        }
        if (fd == pcm_fd) {
            long long s = sleep_ns();
            if (s - slept > 500000000LL && hw_cmd) {
                slept = s;
                if (reopen(fd) == 0)
                    say("pcmreopen: reopened the playback PCM after a suspend\n");
                else
                    say("pcmreopen: reopen failed\n");
            }
            slept = s;
        }
    }

    long r = sys6(NR_ioctl, fd, cmd, (long)arg, 0, 0, 0);
    if (r >= 0 && fd == pcm_fd && size <= MAX_ARG) {
        unsigned nr = cmd & 0xff, i;
        if (nr == 0x11) { hw_cmd = cmd; for (i = 0; i < size; i++) hw_params[i] = ((char *)arg)[i]; }
        if (nr == 0x13) { sw_cmd = cmd; for (i = 0; i < size; i++) sw_params[i] = ((char *)arg)[i]; }
        if (nr == 0x12) { hw_cmd = 0; nmaps = 0; } /* HW_FREE */
    }
    return (int)ret_errno(r);
}

static void *do_mmap(void *addr, size_t len, int prot, int flags, int fd, long pgoff)
{
    long r = sys6(NR_mmap2, (long)addr, len, prot, flags, fd, pgoff);
    if (r < 0 && r > -4096) {
        *__errno_location() = (int)-r;
        return (void *)-1;
    }
    if (fd == pcm_fd && fd >= 0 && nmaps < MAX_MAPS) {
        maps[nmaps].addr = (void *)r;
        maps[nmaps].len = len;
        maps[nmaps].prot = prot;
        maps[nmaps].flags = flags;
        maps[nmaps].pgoff = pgoff;
        nmaps++;
    }
    return (void *)r;
}

void *mmap(void *addr, size_t len, int prot, int flags, int fd, long off)
{
    return do_mmap(addr, len, prot, flags, fd, (unsigned long)off >> 12);
}

void *mmap64(void *addr, size_t len, int prot, int flags, int fd, long long off)
{
    return do_mmap(addr, len, prot, flags, fd, (long)(off >> 12));
}

int munmap(void *addr, size_t len)
{
    int i;
    for (i = 0; i < nmaps; i++)
        if (maps[i].addr == addr) {
            maps[i] = maps[--nmaps];
            break;
        }
    return (int)ret_errno(sys6(NR_munmap, (long)addr, len, 0, 0, 0, 0));
}

int close(int fd)
{
    if (fd == pcm_fd) {
        pcm_fd = -1;
        nmaps = 0;
        hw_cmd = sw_cmd = 0;
    }
    return (int)ret_errno(sys6(NR_close, fd, 0, 0, 0, 0, 0));
}
