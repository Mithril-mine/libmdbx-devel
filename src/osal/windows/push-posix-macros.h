/// \copyright SPDX-License-Identifier: Apache-2.0
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2015-2026

/* Protection against leakage/use of POSIX identifiers during build for Windows,
 * which may also be defined as unstable, arbitrary values dependent on the build environment.
 *   git grep -h -w -o 'E[A-Z]\+' | sort | uniq
 */

#if MDBX_PRAGMA_PUSHPOP_MACRO

#define xMDBX_POSIX_MACROS_PUSHED
#pragma push_macro("errno")
#pragma push_macro("EACCES")
#pragma push_macro("EAFNOSUPPORT")
#pragma push_macro("EAGAIN")
#pragma push_macro("EBADF")
#pragma push_macro("EBADFD")
#pragma push_macro("EBUSY")
#pragma push_macro("ECHILD")
#pragma push_macro("EDEADLK")
#pragma push_macro("EDQUOT")
#pragma push_macro("EEXIST")
#pragma push_macro("EFAULT")
#pragma push_macro("EIDRM")
#pragma push_macro("EINTR")
#pragma push_macro("EINVAL")
#pragma push_macro("EIO")
#pragma push_macro("ENODATA")
#pragma push_macro("ENOENT")
#pragma push_macro("ENOFILE")
#pragma push_macro("ENOIMPL")
#pragma push_macro("ENOLCK")
#pragma push_macro("ENOMEM")
#pragma push_macro("ENOPROTOOPT")
#pragma push_macro("ENOSPC")
#pragma push_macro("ENOSUPP")
#pragma push_macro("ENOSYS")
#pragma push_macro("ENOTBLK")
#pragma push_macro("ENOTDIR")
#pragma push_macro("ENOTSUP")
#pragma push_macro("ENOTTY")
#pragma push_macro("EOPNOTSUPP")
#pragma push_macro("EOWNERDEAD")
#pragma push_macro("EPERM")
#pragma push_macro("EPIPE")
#pragma push_macro("EREMOTEIO")
#pragma push_macro("EROFS")
#pragma push_macro("ESRCH")
#pragma push_macro("EWOULDBLOCK")
#pragma push_macro("EXDEV")

#undef EACCES
#undef EAFNOSUPPORT
#undef EAGAIN
#undef EBADF
#undef EBADFD
#undef EBUSY
#undef ECHILD
#undef EDEADLK
#undef EDQUOT
#undef EEXIST
#undef EFAULT
#undef EIDRM
#undef EINTR
#undef EINVAL
#undef EIO
#undef ENODATA
#undef ENOENT
#undef ENOFILE
#undef ENOIMPL
#undef ENOLCK
#undef ENOMEM
#undef ENOPROTOOPT
#undef ENOSPC
#undef ENOSUPP
#undef ENOSYS
#undef ENOTBLK
#undef ENOTDIR
#undef ENOTSUP
#undef ENOTTY
#undef EOPNOTSUPP
#undef EOWNERDEAD
#undef EPERM
#undef EPIPE
#undef EREMOTEIO
#undef EROFS
#undef ESRCH
#undef EWOULDBLOCK
#undef EXDEV
#define POSIX_FUSE POSIX_errno_codes_should_not_be_used_in_nonOSAL_libmdbx_code_on_Windows
/* EINVAL keeps the declared-extern-function trap form: the MSVC STL itself
 * (<string>, <xlocnum>, <xloctime>) uses EINVAL internally, so the bare
 * identifier would break the parsing of these headers in every consumer TU
 * (a NO_PCH test, for instance) that has the poison active at its include
 * time. The call form compiles wherever a real E*-code does, and the trap
 * remains: the call reaches a function that is never defined, so any actual
 * use in the library code fails at the link stage. */
extern int mdbx_POSIX_errno_codes_should_not_be_used_in_nonOSAL_libmdbx_code_on_Windows(void);
#define EINVAL mdbx_POSIX_errno_codes_should_not_be_used_in_nonOSAL_libmdbx_code_on_Windows()
#define EACCES POSIX_FUSE
#define EAFNOSUPPORT POSIX_FUSE
#define EAGAIN POSIX_FUSE
#define EBADF POSIX_FUSE
#define EBADFD POSIX_FUSE
#define EBUSY POSIX_FUSE
#define ECHILD POSIX_FUSE
#define EDEADLK POSIX_FUSE
#define EDQUOT POSIX_FUSE
#define EEXIST POSIX_FUSE
#define EFAULT POSIX_FUSE
#define EIDRM POSIX_FUSE
#define EINTR POSIX_FUSE
#define EINVAL mdbx_POSIX_errno_codes_should_not_be_used_in_nonOSAL_libmdbx_code_on_Windows()
#define EIO POSIX_FUSE
#define ENODATA POSIX_FUSE
#define ENOENT POSIX_FUSE
#define ENOFILE POSIX_FUSE
#define ENOIMPL POSIX_FUSE
#define ENOLCK POSIX_FUSE
#define ENOMEM POSIX_FUSE
#define ENOPROTOOPT POSIX_FUSE
#define ENOSPC POSIX_FUSE
#define ENOSUPP POSIX_FUSE
#define ENOSYS POSIX_FUSE
#define ENOTBLK POSIX_FUSE
#define ENOTDIR POSIX_FUSE
#define ENOTSUP POSIX_FUSE
#define ENOTTY POSIX_FUSE
#define EOPNOTSUPP POSIX_FUSE
#define EOWNERDEAD POSIX_FUSE
#define EPERM POSIX_FUSE
#define EPIPE POSIX_FUSE
#define EREMOTEIO POSIX_FUSE
#define EROFS POSIX_FUSE
#define ESRCH POSIX_FUSE
#define EWOULDBLOCK POSIX_FUSE
#define EXDEV POSIX_FUSE

#ifndef xMDBX_TOOLS
#undef errno
#define errno POSIX_FUSE
#endif /* xMDBX_TOOLS */

#endif /* MDBX_PRAGMA_PUSHPOP_MACRO */
