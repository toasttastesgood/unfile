/* xfsts.h — shared header for xfs_timestomp LKM and userspace clients */
#ifndef XFSTS_H
#define XFSTS_H

#include <linux/types.h>
#include <linux/ioctl.h>

#define XFSTS_DEV_NAME "xfs_timestomp"
#define XFSTS_DEV_PATH "/dev/xfs_timestomp"

struct xfsts_ts {
	__s64 secs;
	__u32 nsec;
	__u8 valid;
	__u8 _pad[3];
};

struct xfsts_req {
	char dev[256];
	__u64 ino;          /* legacy target selection (deprecated for xfs) */
	char target[256];

	struct xfsts_ts atime;
	struct xfsts_ts mtime;
	struct xfsts_ts ctime;
	struct xfsts_ts btime;

	/* Optional donor inode/path; explicit fields still override donor values. */
	__u64 donor_ino;
	char donor_path[256];

	/* Output */
	__u64 out_ino_gen;
};

#define XFSTS_MAGIC 'X'
#define XFSTS_STOMP _IOWR(XFSTS_MAGIC, 1, struct xfsts_req)
#define XFSTS_STOMP_PATH _IOWR(XFSTS_MAGIC, 2, struct xfsts_req)

#endif /* XFSTS_H */
