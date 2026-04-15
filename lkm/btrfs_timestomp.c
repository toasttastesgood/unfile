// SPDX-License-Identifier: GPL-2.0-only

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/namei.h>
#include <linux/string.h>
#include <linux/pagemap.h>
#include <linux/blkdev.h>

#include "btrfsts.h"
#include "timestomp_kmod_common.h"

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

static timestomp_kallsyms_lookup_name_t btrfsts_kallsyms_lookup_name;

static unsigned long sym_addr(const char *name)
{
	if (!btrfsts_kallsyms_lookup_name)
		return 0;
	return timestomp_sym_addr(btrfsts_kallsyms_lookup_name, name);
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
	struct super_block *sb;
	struct inode *target = NULL;
	struct inode *donor = NULL;
	struct timestomp_find_sb_ctx ctx;
	struct file_system_type *btrfs_type;
	int ret;

	if (copy_from_user(&req, ureq, sizeof(req)))
		return -EFAULT;

	req.dev[sizeof(req.dev) - 1] = '\0';
	req.target[sizeof(req.target) - 1] = '\0';
	req.donor_path[sizeof(req.donor_path) - 1] = '\0';

	ret = kern_path(req.dev, LOOKUP_FOLLOW, &path);
	if (ret)
		return ret;

	if (!path.dentry->d_sb) {
		path_put(&path);
		return -ENODEV;
	}

	if (path.dentry->d_inode && S_ISBLK(path.dentry->d_inode->i_mode)) {
		dev_t bdev_dev = path.dentry->d_inode->i_rdev;

		path_put(&path);

		btrfs_type = (struct file_system_type *)sym_addr("btrfs_fs_type");
		if (!btrfs_type)
			return -ENOENT;

		ctx.target_dev = bdev_dev;
		ctx.found = NULL;
		ctx.match_bdev = true;
		iterate_supers_type(btrfs_type, timestomp_find_sb_cb, &ctx);
		sb = ctx.found;
		if (!sb)
			return -ENOENT;
	} else {
		sb = path.dentry->d_sb;
		path_put(&path);
	}

	if (strcmp(sb->s_type->name, "btrfs") != 0)
		return -EINVAL;

	if (require_path_target && !req.target[0])
		return -EINVAL;

	if (req.target[0]) {
		ret = kern_path(req.target, LOOKUP_FOLLOW, &tpath);
		if (ret)
			return ret;
		if (!tpath.dentry->d_sb || tpath.dentry->d_sb != sb ||
		    strcmp(tpath.dentry->d_sb->s_type->name, "btrfs") != 0) {
			path_put(&tpath);
			return -EXDEV;
		}
		target = igrab(d_inode(tpath.dentry));
		path_put(&tpath);
		if (!target)
			return -ENOENT;
		req.ino = target->i_ino;
	} else {
		target = ilookup(sb, (unsigned long)req.ino);
		if (!target)
			return -ENOENT;
	}

	if (req.donor_path[0] || req.donor_ino) {
		if (req.donor_path[0]) {
			ret = kern_path(req.donor_path, LOOKUP_FOLLOW, &dpath);
			if (ret) {
				iput(target);
				return ret;
			}
			if (!dpath.dentry->d_sb || dpath.dentry->d_sb != sb ||
			    strcmp(dpath.dentry->d_sb->s_type->name, "btrfs") != 0) {
				path_put(&dpath);
				iput(target);
				return -EXDEV;
			}
			donor = igrab(d_inode(dpath.dentry));
			path_put(&dpath);
			if (!donor) {
				iput(target);
				return -ENOENT;
			}
			req.donor_ino = donor->i_ino;
		} else {
			donor = ilookup(sb, (unsigned long)req.donor_ino);
		}

		if (donor) {
			TIMESTOMP_COPY_DONOR_TS(req, donor, atime, inode_get_atime);
			TIMESTOMP_COPY_DONOR_TS(req, donor, mtime, inode_get_mtime);
			TIMESTOMP_COPY_DONOR_TS(req, donor, ctime, inode_get_ctime);
			if (!req.btime.valid)
				read_btime(donor, &req.btime);
			iput(donor);
		}
	}

	TIMESTOMP_APPLY_TS(req, target, atime, 'a');
	TIMESTOMP_APPLY_TS(req, target, mtime, 'm');
	TIMESTOMP_APPLY_TS(req, target, ctime, 'c');
	if (req.btime.valid) {
		ret = write_btime(target, &req.btime);
		if (ret) {
			iput(target);
			return ret;
		}
	}

	mark_inode_dirty(target);
	ret = write_inode_now(target, 1);
	if (!ret)
		req.out_ino_gen = target->i_generation;

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

	ret = timestomp_resolve_kallsyms(&btrfsts_kallsyms_lookup_name);
	if (ret)
		return ret;

	return misc_register(&btrfsts_miscdev);
}

static void __exit btrfsts_exit(void)
{
	misc_deregister(&btrfsts_miscdev);
}

module_init(btrfsts_init);
module_exit(btrfsts_exit);

MODULE_LICENSE("GPL");
