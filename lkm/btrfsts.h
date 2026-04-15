/* btrfsts.h — shared header for btrfs_timestomp LKM and userspace clients */
#ifndef BTRFSTS_H
#define BTRFSTS_H

#include <linux/types.h>
#include <linux/ioctl.h>

#define BTRFSTS_DEV_NAME "btrfs_timestomp"
#define BTRFSTS_DEV_PATH "/dev/btrfs_timestomp"

struct btrfsts_ts {
	__s64 secs;
	__u32 nsec;
	__u8 valid;
	__u8 _pad[3];
};

struct btrfsts_req {
	char dev[256];
	__u64 ino;          /* legacy target selection (deprecated for btrfs) */
	char target[256];

	struct btrfsts_ts atime;
	struct btrfsts_ts mtime;
	struct btrfsts_ts ctime;
	struct btrfsts_ts btime;

	/* Optional donor inode/path; explicit fields still override donor values. */
	__u64 donor_ino;
	char donor_path[256];

	/* Output */
	__u64 out_ino_gen;
};

#define BTRFSTS_MAGIC 'B'
#define BTRFSTS_STOMP _IOWR(BTRFSTS_MAGIC, 1, struct btrfsts_req)
#define BTRFSTS_STOMP_PATH _IOWR(BTRFSTS_MAGIC, 2, struct btrfsts_req)

#endif /* BTRFSTS_H */
