/*
 * Stubs for POSIX functions that newlib/Horizon declares but does not implement,
 * yet are referenced by CPython's posixmodule / pthread layer and ffmedia.c.
 * Ren'Py never exercises these on Switch (process/fs syscalls are disabled and
 * video uses SDL allocation), so an ENOSYS stub is safe and just satisfies the linker.
 */
#include <errno.h>
#include <stddef.h>
#include <malloc.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <reent.h>

/* devkitPro newlib's getentropy() delegates to _getentropy_r, which the platform
   must provide. Back it with libnx's hardware CSRNG (randomGet) so os.urandom,
   ecdsa key generation, ssl, uuid, etc. work instead of failing on /dev/urandom. */
extern void randomGet(void *buf, size_t len);   /* libnx, resolved via -lnx */

int _getentropy_r(struct _reent *r, void *buf, size_t len)
{
    (void)r;
    randomGet(buf, len);
    return 0;
}

int fdatasync(int fd) { (void)fd; errno = ENOSYS; return -1; }
int fchdir(int fd) { (void)fd; errno = ENOSYS; return -1; }
int chroot(const char *path) { (void)path; errno = ENOSYS; return -1; }
int setgroups(int ngroups, const gid_t *list) { (void)ngroups; (void)list; errno = ENOSYS; return -1; }
int pause(void) { errno = ENOSYS; return -1; }

/* Referenced by CPython's _posixsubprocess (process spawning). The Switch can't
   fork/exec, so these never run; the stubs just satisfy the linker. */
int execv(const char *path, char *const argv[]) { (void)path; (void)argv; errno = ENOSYS; return -1; }
mode_t umask(mode_t mask) { (void)mask; return 0; }
int dirfd(DIR *dirp) { (void)dirp; errno = ENOTSUP; return -1; }

long sysconf(int name)
{
#ifdef _SC_PAGESIZE
    if (name == _SC_PAGESIZE) return 4096;
#endif
#ifdef _SC_OPEN_MAX
    if (name == _SC_OPEN_MAX) return 1024;
#endif
#ifdef _SC_NPROCESSORS_ONLN
    if (name == _SC_NPROCESSORS_ONLN) return 1;
#endif
    errno = EINVAL;
    return -1;
}

int posix_memalign(void **memptr, size_t alignment, size_t size)
{
    void *p = memalign(alignment, size);
    if (p == NULL) {
        return ENOMEM;
    }
    *memptr = p;
    return 0;
}
