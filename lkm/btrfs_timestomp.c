// SPDX-License-Identifier: GPL-2.0-only
/*
 * btrfs_timestomp.c - Loadable kernel module for live btrfs timestamp updates.
 *
 * Exposes /dev/btrfs_timestomp and accepts BTRFSTS_STOMP ioctls that target a
 * mounted btrfs filesystem plus inode number. Supports atime/mtime/ctime/btime.
 *
 * btime support:
 * - btrfs stores birth time in struct btrfs_inode as i_otime_sec/nsec.
 * - We patch these fields via BTF-derived offsets passed from Makefile.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/namei.h>
#include <linux/mount.h>
#include <linux/string.h>
#include <linux/pagemap.h>
#include <linux/time64.h>
#include <linux/kprobes.h>
#include <linux/blkdev.h>

#include "btrfsts.h"

#if defined(BTRFS_OTIME_SEC_OFFSET) && defined(BTRFS_OTIME_NSEC_OFFSET) && \
	defined(BTRFS_VFS_INODE_OFFSET)
#define BTRFS_HAVE_BTIME_OFFSETS 1
#else
#define BTRFS_HAVE_BTIME_OFFSETS 0
#ifndef BTRFS_OTIME_SEC_OFFSET
#define BTRFS_OTIME_SEC_OFFSET 0
#endif
#ifndef BTRFS_OTIME_NSEC_OFFSET
#define BTRFS_OTIME_NSEC_OFFSET 0
#endif
#ifndef BTRFS_VFS_INODE_OFFSET
#define BTRFS_VFS_INODE_OFFSET 0
#endif
#endif

struct find_sb_ctx {
	dev_t target_dev;
	struct super_block *found;
};

static void find_sb_cb(struct super_block *sb, void *arg)
{
	struct find_sb_ctx *ctx = arg;

	if (ctx->found)
		return;
	/* btrfs may not expose the backing block dev in sb->s_dev. */
	if ((sb->s_bdev && sb->s_bdev->bd_dev == ctx->target_dev) ||
	    sb->s_dev == ctx->target_dev)
		ctx->found = sb;
}

typedef unsigned long (*kallsyms_lookup_name_t)(const char *name);
static kallsyms_lookup_name_t btrfsts_kallsyms_lookup_name;

static int __init resolve_kallsyms(void)
{
	struct kprobe kp = {
		.symbol_name = "kallsyms_lookup_name",
	};
	int ret;

	ret = register_kprobe(&kp);
	if (ret < 0)
		return ret;

	btrfsts_kallsyms_lookup_name = (kallsyms_lookup_name_t)kp.addr;
	unregister_kprobe(&kp);
	return 0;
}

static unsigned long sym_addr(const char *name)
{
	if (!btrfsts_kallsyms_lookup_name)
		return 0;
	return btrfsts_kallsyms_lookup_name(name);
}

static void apply_vfs_ts(struct inode *inode, const struct btrfsts_ts *ts, char which)
{
	struct timespec64 t = {
		.tv_sec = ts->secs,
		.tv_nsec = ts->nsec,
	};

	switch (which) {
	case 'a':
		inode_set_atime_to_ts(inode, t);
		break;
	case 'm':
		inode_set_mtime_to_ts(inode, t);
		break;
	case 'c':
		inode_set_ctime_to_ts(inode, t);
		break;
	default:
		break;
	}
}

static int read_btime(struct inode *inode, struct btrfsts_ts *out)
{
#if !BTRFS_HAVE_BTIME_OFFSETS
	(void)inode;
	(void)out;
	return -EOPNOTSUPP;
#else
	char *bi = (char *)inode - BTRFS_VFS_INODE_OFFSET;
	__s64 *sec_ptr = (__s64 *)(bi + BTRFS_OTIME_SEC_OFFSET);
	__u32 *nsec_ptr = (__u32 *)(bi + BTRFS_OTIME_NSEC_OFFSET);

	out->secs = *sec_ptr;
	out->nsec = *nsec_ptr;
	out->valid = 1;
	return 0;
#endif
}

static int write_btime(struct inode *inode, const struct btrfsts_ts *ts)
{
#if !BTRFS_HAVE_BTIME_OFFSETS
	(void)inode;
	(void)ts;
	pr_err("btrfs_timestomp: btime offsets unknown; rebuild with pahole+BTF\n");
	return -EOPNOTSUPP;
#else
	char *bi = (char *)inode - BTRFS_VFS_INODE_OFFSET;
	__s64 *sec_ptr = (__s64 *)(bi + BTRFS_OTIME_SEC_OFFSET);
	__u32 *nsec_ptr = (__u32 *)(bi + BTRFS_OTIME_NSEC_OFFSET);

	if (ts->nsec > 999999999U)
		return -EINVAL;

	*sec_ptr = ts->secs;
	*nsec_ptr = ts->nsec;
	return 0;
#endif
}

static int do_stomp(struct btrfsts_req __user *ureq, bool require_path_target)
{
	struct btrfsts_req req;
	struct path path;
	struct path tpath;
	struct path dpath;
	struct super_block *sb = NULL;
	struct inode *target = NULL, *donor = NULL;
	struct find_sb_ctx ctx;
	struct file_system_type *btrfs_type;
	int ret;

	if (copy_from_user(&req, ureq, sizeof(req)))
		return -EFAULT;

	req.dev[sizeof(req.dev) - 1] = '\0';
	req.target[sizeof(req.target) - 1] = '\0';
	req.donor_path[sizeof(req.donor_path) - 1] = '\0';

	ret = kern_path(req.dev, LOOKUP_FOLLOW, &path);
	if (ret) {
		pr_err("btrfs_timestomp: kern_path(%s) failed: %d\n", req.dev, ret);
		return ret;
	}

	if (!path.dentry->d_sb) {
		pr_err("btrfs_timestomp: no superblock for path %s\n", req.dev);
		path_put(&path);
		return -ENODEV;
	}

	if (path.dentry->d_inode && S_ISBLK(path.dentry->d_inode->i_mode)) {
		dev_t bdev_dev = path.dentry->d_inode->i_rdev;
		path_put(&path);

		btrfs_type = (struct file_system_type *)sym_addr("btrfs_fs_type");
		if (!btrfs_type) {
			pr_err("btrfs_timestomp: btrfs_fs_type not found via kallsyms\n");
			return -ENOENT;
		}

		ctx.target_dev = bdev_dev;
		ctx.found = NULL;
		iterate_supers_type(btrfs_type, find_sb_cb, &ctx);
		sb = ctx.found;
		if (!sb) {
			pr_err("btrfs_timestomp: no mounted btrfs sb for dev %u:%u\n",
			       MAJOR(bdev_dev), MINOR(bdev_dev));
			return -ENOENT;
		}
	} else {
		sb = path.dentry->d_sb;
		path_put(&path);
	}

	if (!sb || strcmp(sb->s_type->name, "btrfs") != 0) {
		pr_err("btrfs_timestomp: filesystem is %s, not btrfs\n",
		       sb ? sb->s_type->name : "<null>");
		return -EINVAL;
	}

	if (require_path_target && !req.target[0]) {
		pr_err("btrfs_timestomp: path ioctl requires req.target\n");
		return -EINVAL;
	}

	if (req.target[0]) {
		ret = kern_path(req.target, LOOKUP_FOLLOW, &tpath);
		if (ret) {
			pr_err("btrfs_timestomp: kern_path(target=%s) failed: %d\n", req.target, ret);
			return ret;
		}
		if (!tpath.dentry->d_sb || tpath.dentry->d_sb != sb ||
		    strcmp(tpath.dentry->d_sb->s_type->name, "btrfs") != 0) {
			pr_err("btrfs_timestomp: target path %s not on selected btrfs mount\n", req.target);
			path_put(&tpath);
			return -EXDEV;
		}
		target = igrab(d_inode(tpath.dentry));
		path_put(&tpath);
		if (!target) {
			pr_err("btrfs_timestomp: failed to pin target inode from %s\n", req.target);
			return -ENOENT;
		}
		req.ino = target->i_ino;
	} else {
		target = ilookup(sb, (unsigned long)req.ino);
		if (!target) {
			pr_err("btrfs_timestomp: inode %llu not found in cache\n", req.ino);
			return -ENOENT;
		}
	}

	if (req.donor_path[0] || req.donor_ino) {
		if (req.donor_path[0]) {
			ret = kern_path(req.donor_path, LOOKUP_FOLLOW, &dpath);
			if (ret) {
				pr_err("btrfs_timestomp: kern_path(donor=%s) failed: %d\n", req.donor_path, ret);
				iput(target);
				return ret;
			}
			if (!dpath.dentry->d_sb || dpath.dentry->d_sb != sb ||
			    strcmp(dpath.dentry->d_sb->s_type->name, "btrfs") != 0) {
				pr_err("btrfs_timestomp: donor path %s not on selected btrfs mount\n",
				       req.donor_path);
				path_put(&dpath);
				iput(target);
				return -EXDEV;
			}
			donor = igrab(d_inode(dpath.dentry));
			path_put(&dpath);
			if (!donor) {
				pr_err("btrfs_timestomp: failed to pin donor inode from %s\n", req.donor_path);
				iput(target);
				return -ENOENT;
			}
			req.donor_ino = donor->i_ino;
		} else {
			donor = ilookup(sb, (unsigned long)req.donor_ino);
		}

		if (donor) {
			if (!req.atime.valid) {
				struct timespec64 t = inode_get_atime(donor);
				req.atime.secs = t.tv_sec;
				req.atime.nsec = t.tv_nsec;
				req.atime.valid = 1;
			}
			if (!req.mtime.valid) {
				struct timespec64 t = inode_get_mtime(donor);
				req.mtime.secs = t.tv_sec;
				req.mtime.nsec = t.tv_nsec;
				req.mtime.valid = 1;
			}
			if (!req.ctime.valid) {
				struct timespec64 t = inode_get_ctime(donor);
				req.ctime.secs = t.tv_sec;
				req.ctime.nsec = t.tv_nsec;
				req.ctime.valid = 1;
			}
			if (!req.btime.valid)
				read_btime(donor, &req.btime);
			iput(donor);
		}
	}

	if (req.atime.valid)
		apply_vfs_ts(target, &req.atime, 'a');
	if (req.mtime.valid)
		apply_vfs_ts(target, &req.mtime, 'm');
	if (req.ctime.valid)
		apply_vfs_ts(target, &req.ctime, 'c');
	if (req.btime.valid) {
		ret = write_btime(target, &req.btime);
		if (ret) {
			pr_err("btrfs_timestomp: write_btime(%llu) failed: %d\n", req.ino, ret);
			iput(target);
			return ret;
		}
	}

	mark_inode_dirty(target);
	ret = write_inode_now(target, 1);
	if (!ret)
		req.out_ino_gen = target->i_generation;
	else
		pr_err("btrfs_timestomp: write_inode_now(%llu) failed: %d\n",
		       req.ino, ret);

	iput(target);

	if (ret)
		return ret;

	if (copy_to_user(ureq, &req, sizeof(req)))
		return -EFAULT;

	return 0;
}

static long btrfsts_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	switch (cmd) {
	case BTRFSTS_STOMP:
		return do_stomp((struct btrfsts_req __user *)arg, false);
	case BTRFSTS_STOMP_PATH:
		return do_stomp((struct btrfsts_req __user *)arg, true);
	default:
		return -ENOTTY;
	}
}

static const struct file_operations btrfsts_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = btrfsts_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = btrfsts_ioctl,
#endif
};

static struct miscdevice btrfsts_miscdev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = BTRFSTS_DEV_NAME,
	.fops = &btrfsts_fops,
	.mode = 0600,
};

static int __init btrfsts_init(void)
{
	int ret;

	ret = resolve_kallsyms();
	if (ret)
		return ret;

	ret = misc_register(&btrfsts_miscdev);

	if (ret)
		return ret;

	pr_info("btrfs_timestomp: loaded (%s)\n", BTRFSTS_DEV_PATH);
	return 0;
}

static void __exit btrfsts_exit(void)
{
	misc_deregister(&btrfsts_miscdev);
	pr_info("btrfs_timestomp: unloaded\n");
}

module_init(btrfsts_init);
module_exit(btrfsts_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Codex");
MODULE_DESCRIPTION("Live btrfs inode timestamp patching module (atime/mtime/ctime/btime)");
