// SPDX-License-Identifier: GPL-2.0-only
/*
 * ext4_timestomp.c — Loadable kernel module for live ext4 timestamp patching.
 *
 * Exposes /dev/ext4_timestomp as a misc device.  A single ioctl (EXT4TS_STOMP)
 * accepts a device path + inode number, optional per-field timestamp overrides,
 * and an optional donor inode number.  It modifies the in-memory inode and
 * forces a writeback so the change is durable without unmounting.
 *
 * Kernel compatibility: Linux 6.1+ (uses inode_set_{a,m,c}time() accessors).
 * Tested against Linux 6.18.
 *
 * BUILD (on a machine with matching kernel headers):
 *   make -C /lib/modules/$(uname -r)/build M=$(pwd) modules
 *
 * LOAD:
 *   insmod ext4_timestomp.ko
 *   # /dev/ext4_timestomp appears automatically via misc_register
 *
 * UNLOAD:
 *   rmmod ext4_timestomp
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/namei.h>
#include <linux/mount.h>
#include <linux/dcache.h>
#include <linux/time64.h>
#include <linux/kprobes.h>
#include <linux/string.h>

/* Pull in the shared ioctl header (also used by userspace clients). */
#include "ext4ts.h"

/* --------------------------------------------------------------------------
 * ext4 private types we need
 * --------------------------------------------------------------------------
 * We need struct ext4_inode_info to access i_crtime (birth time), which is
 * not exposed via the VFS inode.  Rather than duplicating the full struct
 * (which would break across kernel versions), we detect the field offset at
 * module load time using one of two methods:
 *
 *   1. pahole/BTF — if /sys/kernel/btf/vmlinux exists, the Makefile extracts
 *      the offset and passes -DEXT4_CRTIME_OFFSET=<n> at compile time.
 *   2. Fallback heuristic — scan the ext4_inode_info memory for a timespec64
 *      that matches i_crtime as seen from disk (requires a reference inode).
 *
 * For the common case we just use the BTF-derived compile-time constant.
 * If neither is available we skip crtime modification and warn the user.
 */

/* Opaque pointer to ext4_inode_info — we only need the offset into it. */
struct ext4_inode_info; /* forward decl only */

/* Access the VFS inode embedded in ext4_inode_info.
 * In ext4, ext4_inode_info starts with a struct inode (vfs_inode is the
 * last field but the struct inode itself is at a known offset).
 * container_of works because the kernel exposes EXT4_I(inode) which does:
 *   container_of(inode, struct ext4_inode_info, vfs_inode)
 * We replicate that without including fs/ext4/ext4.h (which is not exported).
 */
#ifndef EXT4_CRTIME_OFFSET
/* Will be set by Makefile via pahole/BTF.  0 means "unknown, skip crtime". */
# define EXT4_CRTIME_OFFSET  0
# define EXT4_CRTIME_UNKNOWN 1
#else
# define EXT4_CRTIME_UNKNOWN 0
#endif

/* struct timespec64 is two 64-bit fields: tv_sec, tv_nsec */
#define TIMESPEC64_SIZE  16

/* --------------------------------------------------------------------------
 * kallsyms lookup via kprobe trick (works on 5.7+ where kallsyms_lookup_name
 * is no longer exported).
 * -------------------------------------------------------------------------- */
typedef unsigned long (*kallsyms_lookup_name_t)(const char *name);
static kallsyms_lookup_name_t ext4ts_kallsyms_lookup_name;

static int __init resolve_kallsyms(void)
{
	struct kprobe kp = {
		.symbol_name = "kallsyms_lookup_name",
	};
	int ret;

	ret = register_kprobe(&kp);
	if (ret < 0)
		return ret;
	ext4ts_kallsyms_lookup_name = (kallsyms_lookup_name_t)kp.addr;
	unregister_kprobe(&kp);
	return 0;
}

/* Convenience wrapper */
static unsigned long sym_addr(const char *name)
{
	if (!ext4ts_kallsyms_lookup_name)
		return 0;
	return ext4ts_kallsyms_lookup_name(name);
}

/* --------------------------------------------------------------------------
 * Superblock iteration helpers
 * --------------------------------------------------------------------------
 * iterate_supers() is exported.  We use it to find the superblock that
 * corresponds to the requested block device.
 */
struct find_sb_ctx {
	dev_t        target_dev;
	struct super_block *found;
};

static void find_sb_cb(struct super_block *sb, void *arg)
{
	struct find_sb_ctx *ctx = arg;

	if (ctx->found)
		return; /* already found */

	/* Match by block device number */
	if (sb->s_bdev && sb->s_bdev->bd_dev == ctx->target_dev)
		ctx->found = sb;
}

/* --------------------------------------------------------------------------
 * crtime write helpers
 * -------------------------------------------------------------------------- */
static void write_crtime(struct inode *inode, const struct timespec64 *ts)
{
#if EXT4_CRTIME_UNKNOWN
	pr_warn("ext4_timestomp: crtime offset unknown — skipping crtime patch\n");
	(void)inode; (void)ts;
#else
	/* ext4_inode_info layout:
	 *   vfs_inode is at some offset WITHIN ext4_inode_info.
	 * EXT4_CRTIME_OFFSET is the byte offset of i_crtime from the START of
	 * ext4_inode_info.  To get to ext4_inode_info from vfs inode we need
	 * the offset of vfs_inode within ext4_inode_info — stored at compile
	 * time as EXT4_VFS_INODE_OFFSET (also from pahole/BTF).
	 */
# ifndef EXT4_VFS_INODE_OFFSET
#  error "EXT4_VFS_INODE_OFFSET must be set together with EXT4_CRTIME_OFFSET"
# endif
	char *ei = (char *)inode - EXT4_VFS_INODE_OFFSET;
	struct timespec64 *crtime_ptr = (struct timespec64 *)(ei + EXT4_CRTIME_OFFSET);
	*crtime_ptr = *ts;
	pr_info("ext4_timestomp: wrote crtime %lld.%09ld\n",
		(long long)ts->tv_sec, ts->tv_nsec);
#endif
}

/* --------------------------------------------------------------------------
 * Core timestamp stomp logic
 * -------------------------------------------------------------------------- */

/* Apply a single ext4ts_ts to a struct inode's atime/mtime/ctime using the
 * 6.1+ accessor API.  "which" is 'a', 'm', 'c'. */
static void apply_vfs_ts(struct inode *inode, const struct ext4ts_ts *ts, char which)
{
	struct timespec64 t = {
		.tv_sec  = ts->secs,
		.tv_nsec = ts->nsec,
	};

	switch (which) {
	case 'a': inode_set_atime_to_ts(inode, t); break;
	case 'm': inode_set_mtime_to_ts(inode, t); break;
	case 'c': inode_set_ctime_to_ts(inode, t); break;
	}
}

static int do_stomp(struct ext4ts_req __user *ureq)
{
	struct ext4ts_req req;
	struct path path;
	struct super_block *sb = NULL;
	struct find_sb_ctx ctx;
	struct inode *target = NULL, *donor = NULL;
	int ret = 0;

	if (copy_from_user(&req, ureq, sizeof(req)))
		return -EFAULT;

	/* Ensure strings are NUL-terminated */
	req.dev[sizeof(req.dev) - 1] = '\0';

	/* ---------- locate the superblock ---------- */
	ret = kern_path(req.dev, LOOKUP_FOLLOW, &path);
	if (ret) {
		pr_err("ext4_timestomp: kern_path(%s) failed: %d\n", req.dev, ret);
		return ret;
	}

	if (!path.dentry->d_sb) {
		path_put(&path);
		return -ENODEV;
	}

	/* We want the sb of the block device, not the sb of the path itself
	 * (req.dev might be a block device node, so path.dentry->d_sb is the
	 * sb of the filesystem containing /dev — use bd_dev to match). */
	if (path.dentry->d_inode &&
	    S_ISBLK(path.dentry->d_inode->i_mode)) {
		dev_t bdev_dev = path.dentry->d_inode->i_rdev;
		path_put(&path);

		ctx.target_dev = bdev_dev;
		ctx.found = NULL;
		iterate_supers(find_sb_cb, &ctx);
		if (!ctx.found) {
			pr_err("ext4_timestomp: no mounted fs found for dev %u:%u\n",
			       MAJOR(bdev_dev), MINOR(bdev_dev));
			return -ENOENT;
		}
		sb = ctx.found;
	} else {
		/* req.dev is a mount point or regular path — use that sb */
		sb = path.dentry->d_sb;
		path_put(&path);
	}

	/* Verify it's ext4 */
	if (strcmp(sb->s_type->name, "ext4") != 0) {
		pr_err("ext4_timestomp: filesystem is %s, not ext4\n",
		       sb->s_type->name);
		return -EINVAL;
	}

	/* ---------- look up target inode ---------- */
	target = ilookup(sb, (unsigned long)req.ino);
	if (!target) {
		/* ilookup only succeeds if the inode is already in cache.
		 * Force it into cache by looking it up through ext4_iget via
		 * kallsyms. */
		typedef struct inode *(*ext4_iget_t)(struct super_block *, unsigned long, int);
		ext4_iget_t ext4_iget_fn = (ext4_iget_t)sym_addr("ext4_iget");
		if (ext4_iget_fn) {
			/* ext4_iget flags: EXT4_IGET_NORMAL = 0 */
			target = ext4_iget_fn(sb, (unsigned long)req.ino, 0);
			if (IS_ERR(target)) {
				ret = PTR_ERR(target);
				pr_err("ext4_timestomp: ext4_iget(%llu) failed: %d\n",
				       req.ino, ret);
				return ret;
			}
		} else {
			pr_err("ext4_timestomp: inode %llu not in cache and "
			       "ext4_iget not found via kallsyms\n", req.ino);
			return -ENOENT;
		}
	}

	/* ---------- donor mode: read timestamps from donor inode ---------- */
	if (req.donor_ino != 0) {
		typedef struct inode *(*ext4_iget_t)(struct super_block *, unsigned long, int);
		ext4_iget_t ext4_iget_fn = (ext4_iget_t)sym_addr("ext4_iget");

		donor = ilookup(sb, (unsigned long)req.donor_ino);
		if (!donor && ext4_iget_fn) {
			donor = ext4_iget_fn(sb, (unsigned long)req.donor_ino, 0);
			if (IS_ERR(donor)) {
				pr_warn("ext4_timestomp: donor iget(%llu) failed: %ld — ignoring donor\n",
					req.donor_ino, PTR_ERR(donor));
				donor = NULL;
			}
		}

		if (donor) {
			/* Copy donor timestamps into fields that aren't explicitly set */
			if (!req.atime.valid) {
				struct timespec64 t = inode_get_atime(donor);
				req.atime.secs  = t.tv_sec;
				req.atime.nsec  = t.tv_nsec;
				req.atime.valid = 1;
			}
			if (!req.mtime.valid) {
				struct timespec64 t = inode_get_mtime(donor);
				req.mtime.secs  = t.tv_sec;
				req.mtime.nsec  = t.tv_nsec;
				req.mtime.valid = 1;
			}
			if (!req.ctime.valid) {
				struct timespec64 t = inode_get_ctime(donor);
				req.ctime.secs  = t.tv_sec;
				req.ctime.nsec  = t.tv_nsec;
				req.ctime.valid = 1;
			}
			/* crtime from donor: read from ext4_inode_info if offset known */
			if (!req.crtime.valid && !EXT4_CRTIME_UNKNOWN) {
#ifndef EXT4_VFS_INODE_OFFSET
				/* can't read crtime without offset */
#else
				char *ei = (char *)donor - EXT4_VFS_INODE_OFFSET;
				struct timespec64 *d_crtime =
					(struct timespec64 *)(ei + EXT4_CRTIME_OFFSET);
				req.crtime.secs  = d_crtime->tv_sec;
				req.crtime.nsec  = d_crtime->tv_nsec;
				req.crtime.valid = 1;
#endif
			}
			iput(donor);
			donor = NULL;
		}
	}

	/* ---------- apply timestamps ---------- */
	if (req.atime.valid)
		apply_vfs_ts(target, &req.atime, 'a');
	if (req.mtime.valid)
		apply_vfs_ts(target, &req.mtime, 'm');
	if (req.ctime.valid)
		apply_vfs_ts(target, &req.ctime, 'c');
	if (req.crtime.valid) {
		struct timespec64 t = { .tv_sec = req.crtime.secs, .tv_nsec = req.crtime.nsec };
		write_crtime(target, &t);
	}

	/* Mark inode dirty and force writeback */
	mark_inode_dirty(target);
	ret = write_inode_now(target, 1 /* sync */);
	if (ret)
		pr_warn("ext4_timestomp: write_inode_now failed: %d\n", ret);

	/* Return generation to caller for diagnostics */
	req.out_ino_gen = target->i_generation;
	if (copy_to_user(ureq, &req, sizeof(req)))
		ret = -EFAULT;

	iput(target);
	return ret;
}

/* --------------------------------------------------------------------------
 * Misc device file operations
 * -------------------------------------------------------------------------- */
static long ext4ts_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	switch (cmd) {
	case EXT4TS_STOMP:
		return do_stomp((struct ext4ts_req __user *)arg);
	default:
		return -ENOTTY;
	}
}

static const struct file_operations ext4ts_fops = {
	.owner          = THIS_MODULE,
	.unlocked_ioctl = ext4ts_ioctl,
	.compat_ioctl   = ext4ts_ioctl,
};

static struct miscdevice ext4ts_dev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name  = EXT4TS_DEV_NAME,
	.fops  = &ext4ts_fops,
	.mode  = 0600,
};

/* --------------------------------------------------------------------------
 * Module init / exit
 * -------------------------------------------------------------------------- */
static int __init ext4ts_init(void)
{
	int ret;

	ret = resolve_kallsyms();
	if (ret) {
		pr_warn("ext4_timestomp: kprobe kallsyms lookup failed (%d) — "
			"iget for uncached inodes will be unavailable\n", ret);
		/* Non-fatal: ilookup still works for cached inodes */
	}

	ret = misc_register(&ext4ts_dev);
	if (ret) {
		pr_err("ext4_timestomp: misc_register failed: %d\n", ret);
		return ret;
	}

	if (EXT4_CRTIME_UNKNOWN)
		pr_warn("ext4_timestomp: EXT4_CRTIME_OFFSET not set — "
			"crtime (birth time) patching disabled\n");
	else
		pr_info("ext4_timestomp: crtime offset = %d, vfs_inode offset = %d\n",
			EXT4_CRTIME_OFFSET,
#ifdef EXT4_VFS_INODE_OFFSET
			EXT4_VFS_INODE_OFFSET
#else
			-1
#endif
			);

	pr_info("ext4_timestomp: loaded, device %s\n", EXT4TS_DEV_PATH);
	return 0;
}

static void __exit ext4ts_exit(void)
{
	misc_deregister(&ext4ts_dev);
	pr_info("ext4_timestomp: unloaded\n");
}

module_init(ext4ts_init);
module_exit(ext4ts_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("ext4-timestomp");
MODULE_DESCRIPTION("Live ext4 inode timestamp modification via ioctl");
MODULE_VERSION("1.0");
