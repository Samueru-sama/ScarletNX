/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Linux asm-generic errno values; never expose the host's errno numbering. */
#ifndef SNX_GNU_ERRNO_H
#define SNX_GNU_ERRNO_H
enum {
    GNU_EPERM = 1,
    GNU_ENOENT = 2,
    GNU_ESRCH = 3,
    GNU_EINTR = 4,
    GNU_EIO = 5,
    GNU_ENXIO = 6,
    GNU_E2BIG = 7,
    GNU_EBADF = 9,
    GNU_EAGAIN = 11,
    GNU_ENOMEM = 12,
    GNU_EACCES = 13,
    GNU_EFAULT = 14,
    GNU_EBUSY = 16,
    GNU_EEXIST = 17,
    GNU_ENODEV = 19,
    GNU_ENOTDIR = 20,
    GNU_EISDIR = 21,
    GNU_EINVAL = 22,
    GNU_ENFILE = 23,
    GNU_EMFILE = 24,
    GNU_ENOTTY = 25,
    GNU_EFBIG = 27,
    GNU_ENOSPC = 28,
    GNU_ESPIPE = 29,
    GNU_EROFS = 30,
    GNU_EPIPE = 32,
    GNU_ERANGE = 34,
    GNU_ENAMETOOLONG = 36,
    GNU_ENOSYS = 38,
    GNU_ENOTEMPTY = 39,
    GNU_ELOOP = 40,
    GNU_EOVERFLOW = 75,
    GNU_ENOTSUP = 95,
    GNU_ETIMEDOUT = 110
};
#endif
