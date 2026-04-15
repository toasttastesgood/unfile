// SPDX-License-Identifier: GPL-2.0-only

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/namei.h>
#include <linux/string.h>
#include <linux/pagemap.h>

#include "ext4ts.h"
#include "timestomp_kmod_common.h"

#ifndef EXT4_CRTIME_OFFSET
#define EXT4_CRTIME_OFFSET 0
#define EXT4_CRTIME_UNKNOWN 1
#else
#define EXT4_CRTIME_UNKNOWN 0
#endif

static timestomp_kallsyms_lookup_name_t ext4ts_kallsyms_lookup_name;

static unsigned long sym_addr(const char *name)
{
	if (!ext4ts_kallsyms_lookup_name)
		return 0;
	return timestomp_sym_addr(ext4ts_kallsyms_lookup_name, name);
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

static int do_stomp(struct ext4ts_req __user *ureq)
{
	typedef struct inode *(*ext4_iget_t)(struct super_block *, unsigned long, int);

	struct ext4ts_req req;
	struct path path;
	struct super_block *sb;
	struct timestomp_find_sb_ctx ctx;
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
		ctx.match_bdev = false;
		iterate_supers_type(ext4_type, timestomp_find_sb_cb, &ctx);
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
			TIMESTOMP_COPY_DONOR_TS(req, donor, atime, inode_get_atime);
			TIMESTOMP_COPY_DONOR_TS(req, donor, mtime, inode_get_mtime);
			TIMESTOMP_COPY_DONOR_TS(req, donor, ctime, inode_get_ctime);
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

	TIMESTOMP_APPLY_TS(req, target, atime, 'a');
	TIMESTOMP_APPLY_TS(req, target, mtime, 'm');
	TIMESTOMP_APPLY_TS(req, target, ctime, 'c');
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
	timestomp_resolve_kallsyms(&ext4ts_kallsyms_lookup_name);
	return misc_register(&ext4ts_dev);
}

static void __exit ext4ts_exit(void)
{
	misc_deregister(&ext4ts_dev);
}

module_init(ext4ts_init);
module_exit(ext4ts_exit);

MODULE_LICENSE("GPL v2");
