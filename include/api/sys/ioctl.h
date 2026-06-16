#ifndef	_SYS_IOCTL_H
#define	_SYS_IOCTL_H
#ifdef __cplusplus
extern "C" {
#endif

/* OSv's ioctl request numbers (was added by include/glibc-compat/sys/ioctl.h,
 * removed in Phase 9.6). */
#include <osv/ioctl.h>
#include <bits/ioctl.h>

int ioctl (int, unsigned long, ...);

#ifdef __cplusplus
}
#endif
#endif
