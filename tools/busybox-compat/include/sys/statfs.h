#ifndef _SYS_STATFS_H
#define _SYS_STATFS_H

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

struct statfs {
	unsigned long f_type;
	unsigned long f_bsize;
	unsigned long f_blocks;
	unsigned long f_bfree;
	unsigned long f_bavail;
	unsigned long f_files;
	unsigned long f_ffree;
	int f_fsid[2];
	unsigned long f_namelen;
	unsigned long f_frsize;
	unsigned long f_flags;
	unsigned long f_spare[4];
};

int statfs(const char *__path, struct statfs *__buf);
int fstatfs(int __fd, struct statfs *__buf);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_STATFS_H */
