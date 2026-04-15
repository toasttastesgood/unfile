// SPDX-License-Identifier: GPL-2.0-only

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/namei.h>
#include <linux/string.h>
#include <linux/pagemap.h>
#include <linux/kprobes.h>

#include "ext4ts.h"

#ifndef EXT4_CRTIME_OFFSET
#define EXT4_CRTIME_OFFSET 0
#define EXT4_CRTIME_UNKNOWN 1
#else
#define EXT4_CRTIME_UNKNOWN 0
#endif

typedef unsigned long (*kallsyms_lookup_name_t)(const char *name);
static kallsyms_lookup_name_t ext4ts_kallsyms_lookup_name;

struct find_sb_ctx {
	dev_t target_dev;
	struct super_block *found;
};

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

static unsigned long sym_addr(const char *name)
{
	if (!ext4ts_kallsyms_lookup_name)
		return 0;
	return ext4ts_kallsyms_lookup_name(name);
}

static void find_sb_cb(struct super_block *sb, void *arg)
{
	struct find_sb_ctx *ctx = arg;

	if (ctx->found)
		return;
	if (sb->s_dev == ctx->target_dev)
		ctx->found = sb;
}

static void write_crtime(struct inode *inode, const struct timespec64 *ts)
{
#if EXT4_CRTIME_UNKNOWN
	(void)inode;
	(void)ts;
#else
#ifndef EXT4_VFS_INODE_OFFSET
#error "EXT4_VFS_INODE_OFFSET must be set together with EXT4_CRTIME_OFFSET"
#endif
	char *ei = (char *)inode - EXT4_VFS_INODE_OFFSET;
	struct timespec64 *crtime_ptr = (struct timespec64 *)(ei + EXT4_CRTIME_OFFSET);

	*crtime_ptr = *ts;
#endif
}

static void apply_vfs_ts(struct inode *inode, const struct ext4ts_ts *ts, char which)
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
	}
}

static int do_stomp(struct ext4ts_req __user *ureq)
{
	typedef struct inode *(*ext4_iget_t)(struct super_block *, unsigned long, int);

	struct ext4ts_req req;
	struct path path;
	struct super_block *sb;
	struct find_sb_ctx ctx;
	struct inode *target = NULL;
	struct inode *donor = NULL;
	ext4_iget_t ext4_iget_fn;
	int ret = 0;

	if (copy_from_user(&req, ureq, sizeof(req)))
		return -EFAULT;

	req.dev[sizeof(req.dev) - 1] = '\0';

	ret = kern_path(req.dev, LOOKUP_FOLLOW, &path);
	if (ret)
		return ret;

	if (!path.dentry->d_sb) {
		path_put(&path);
		return -ENODEV;
	}

	if (path.dentry->d_inode && S_ISBLK(path.dentry->d_inode->i_mode)) {
		dev_t bdev_dev = path.dentry->d_inode->i_rdev;
		struct file_system_type *ext4_type;

		path_put(&path);

		ext4_type = (struct file_system_type *)sym_addr("ext4_fs_type");
		if (!ext4_type)
			return -ENOENT;

		ctx.target_dev = bdev_dev;
		ctx.found = NULL;
		iterate_supers_type(ext4_type, find_sb_cb, &ctx);
		if (!ctx.found)
			return -ENOENT;

		sb = ctx.found;
	} else {
		sb = path.dentry->d_sb;
		path_put(&path);
	}

	if (strcmp(sb->s_type->name, "ext4") != 0)
		return -EINVAL;

	target = ilookup(sb, (unsigned long)req.ino);
	if (!target) {
		ext4_iget_fn = (ext4_iget_t)sym_addr("ext4_iget");
		if (!ext4_iget_fn)
			return -ENOENT;

		target = ext4_iget_fn(sb, (unsigned long)req.ino, 0);
		if (IS_ERR(target))
			return PTR_ERR(target);
	}

	if (req.donor_ino) {
		ext4_iget_fn = (ext4_iget_t)sym_addr("ext4_iget");
		donor = ilookup(sb, (unsigned long)req.donor_ino);
		if (!donor && ext4_iget_fn) {
			donor = ext4_iget_fn(sb, (unsigned long)req.donor_ino, 0);
			if (IS_ERR(donor))
				donor = NULL;
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
			if (!req.crtime.valid && !EXT4_CRTIME_UNKNOWN) {
#ifndef EXT4_VFS_INODE_OFFSET
#else
				char *ei = (char *)donor - EXT4_VFS_INODE_OFFSET;
				struct timespec64 *d_crtime =
					(struct timespec64 *)(ei + EXT4_CRTIME_OFFSET);
				req.crtime.secs = d_crtime->tv_sec;
				req.crtime.nsec = d_crtime->tv_nsec;
				req.crtime.valid = 1;
#endif
			}
			iput(donor);
		}
	}

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

	mark_inode_dirty(target);
	ret = write_inode_now(target, 1);
	req.out_ino_gen = target->i_generation;
	if (copy_to_user(ureq, &req, sizeof(req)))
		ret = -EFAULT;

	iput(target);
	return ret;
}

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
	.owner = THIS_MODULE,
	.unlocked_ioctl = ext4ts_ioctl,
	.compat_ioctl = ext4ts_ioctl,
};

static struct miscdevice ext4ts_dev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = EXT4TS_DEV_NAME,
	.fops = &ext4ts_fops,
	.mode = 0600,
};

static int __init ext4ts_init(void)
{
	resolve_kallsyms();
	return misc_register(&ext4ts_dev);
}

static void __exit ext4ts_exit(void)
{
	misc_deregister(&ext4ts_dev);
}

module_init(ext4ts_init);
module_exit(ext4ts_exit);

MODULE_LICENSE("GPL v2");
