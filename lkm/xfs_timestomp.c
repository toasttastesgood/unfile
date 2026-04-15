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

#include "xfsts.h"
#include "timestomp_kmod_common.h"

#if defined(XFS_VFS_INODE_OFFSET)
#define XFSTS_HAVE_XFS_IGET_OFFSETS 1
#else
#define XFSTS_HAVE_XFS_IGET_OFFSETS 0
#ifndef XFS_VFS_INODE_OFFSET
#define XFS_VFS_INODE_OFFSET 0
#endif
#endif

#if defined(XFS_VFS_INODE_OFFSET) && defined(XFS_CRTIME_OFFSET)
#define XFSTS_HAVE_BTIME_OFFSETS 1
#else
#define XFSTS_HAVE_BTIME_OFFSETS 0
#ifndef XFS_CRTIME_OFFSET
#define XFS_CRTIME_OFFSET 0
#endif
#endif

static timestomp_kallsyms_lookup_name_t xfsts_kallsyms_lookup_name;
typedef int (*xfs_iget_t)(void *mp, void *tp, __u64 ino, unsigned int flags,
			  unsigned int lock_flags, void **ipp);
typedef void (*xfs_irele_t)(void *ip);

static unsigned long sym_addr(const char *name)
{
	if (!xfsts_kallsyms_lookup_name)
		return 0;
	return timestomp_sym_addr(xfsts_kallsyms_lookup_name, name);
}

static int read_btime(struct inode *inode, struct xfsts_ts *out)
{
#if !XFSTS_HAVE_BTIME_OFFSETS
	(void)inode;
	(void)out;
	return -EOPNOTSUPP;
#else
	char *xi = (char *)inode - XFS_VFS_INODE_OFFSET;
	struct timespec64 *crtime_ptr = (struct timespec64 *)(xi + XFS_CRTIME_OFFSET);

	out->secs = crtime_ptr->tv_sec;
	out->nsec = crtime_ptr->tv_nsec;
	out->valid = 1;
	return 0;
#endif
}

static int write_btime(struct inode *inode, const struct xfsts_ts *ts)
{
#if !XFSTS_HAVE_BTIME_OFFSETS
	(void)inode;
	(void)ts;
	return -EOPNOTSUPP;
#else
	char *xi = (char *)inode - XFS_VFS_INODE_OFFSET;
	struct timespec64 *crtime_ptr = (struct timespec64 *)(xi + XFS_CRTIME_OFFSET);

	if (ts->nsec > 999999999U)
		return -EINVAL;

	crtime_ptr->tv_sec = ts->secs;
	crtime_ptr->tv_nsec = ts->nsec;
	return 0;
#endif
}

static void *xfs_mount_from_sb(struct super_block *sb)
{
	return sb->s_fs_info;
}

static struct inode *xfs_inode_to_vfs_inode(void *xip)
{
#if !XFSTS_HAVE_XFS_IGET_OFFSETS
	(void)xip;
	return NULL;
#else
	return (struct inode *)((char *)xip + XFS_VFS_INODE_OFFSET);
#endif
}

static struct inode *xfs_lookup_inode_uncached(struct super_block *sb, __u64 ino)
{
	xfs_iget_t xfs_iget_fn = (xfs_iget_t)sym_addr("xfs_iget");
	xfs_irele_t xfs_irele_fn = (xfs_irele_t)sym_addr("xfs_irele");
	void *xmp;
	void *xip = NULL;
	struct inode *vfs_inode;
	struct inode *held;
	int ret;

	if (!xfs_iget_fn || !xfs_irele_fn)
		return ERR_PTR(-ENOENT);

	xmp = xfs_mount_from_sb(sb);
	if (!xmp)
		return ERR_PTR(-EOPNOTSUPP);

	ret = xfs_iget_fn(xmp, NULL, ino, 0, 0, &xip);
	if (ret)
		return ERR_PTR(ret);
	if (!xip)
		return ERR_PTR(-ENOENT);

	vfs_inode = xfs_inode_to_vfs_inode(xip);
	if (!vfs_inode || vfs_inode->i_sb != sb) {
		xfs_irele_fn(xip);
		return ERR_PTR(-ENOENT);
	}

	held = igrab(vfs_inode);
	xfs_irele_fn(xip);
	if (!held)
		return ERR_PTR(-ENOENT);

	return held;
}

static int do_stomp(struct xfsts_req __user *ureq, bool require_path_target)
{
	struct xfsts_req req;
	struct path path;
	struct path tpath;
	struct path dpath;
	struct super_block *sb;
	struct inode *target = NULL;
	struct inode *donor = NULL;
	struct timestomp_find_sb_ctx ctx;
	struct file_system_type *xfs_type;
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

		xfs_type = (struct file_system_type *)sym_addr("xfs_fs_type");
		if (!xfs_type)
			return -ENOENT;

		ctx.target_dev = bdev_dev;
		ctx.found = NULL;
		ctx.match_bdev = true;
		iterate_supers_type(xfs_type, timestomp_find_sb_cb, &ctx);
		sb = ctx.found;
		if (!sb)
			return -ENOENT;
	} else {
		sb = path.dentry->d_sb;
		path_put(&path);
	}

	if (strcmp(sb->s_type->name, "xfs") != 0)
		return -EINVAL;

	if (require_path_target && !req.target[0])
		return -EINVAL;

	if (req.target[0]) {
		ret = kern_path(req.target, LOOKUP_FOLLOW, &tpath);
		if (ret)
			return ret;
		if (!tpath.dentry->d_sb || tpath.dentry->d_sb != sb ||
		    strcmp(tpath.dentry->d_sb->s_type->name, "xfs") != 0) {
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
		if (!target) {
			target = xfs_lookup_inode_uncached(sb, req.ino);
			if (IS_ERR(target))
				return PTR_ERR(target);
		}
	}

	if (req.donor_path[0] || req.donor_ino) {
		if (req.donor_path[0]) {
			ret = kern_path(req.donor_path, LOOKUP_FOLLOW, &dpath);
			if (ret) {
				iput(target);
				return ret;
			}
			if (!dpath.dentry->d_sb || dpath.dentry->d_sb != sb ||
			    strcmp(dpath.dentry->d_sb->s_type->name, "xfs") != 0) {
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
			if (!donor) {
				donor = xfs_lookup_inode_uncached(sb, req.donor_ino);
				if (IS_ERR(donor))
					donor = NULL;
			}
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

static long xfsts_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	switch (cmd) {
	case XFSTS_STOMP:
		return do_stomp((struct xfsts_req __user *)arg, false);
	case XFSTS_STOMP_PATH:
		return do_stomp((struct xfsts_req __user *)arg, true);
	default:
		return -ENOTTY;
	}
}

static const struct file_operations xfsts_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = xfsts_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = xfsts_ioctl,
#endif
};

static struct miscdevice xfsts_miscdev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = XFSTS_DEV_NAME,
	.fops = &xfsts_fops,
	.mode = 0600,
};

static int __init xfsts_init(void)
{
	int ret;

	ret = timestomp_resolve_kallsyms(&xfsts_kallsyms_lookup_name);
	if (ret)
		return ret;

	return misc_register(&xfsts_miscdev);
}

static void __exit xfsts_exit(void)
{
	misc_deregister(&xfsts_miscdev);
}

module_init(xfsts_init);
module_exit(xfsts_exit);

MODULE_LICENSE("GPL");
