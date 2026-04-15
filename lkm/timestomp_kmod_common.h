#ifndef TIMESTOMP_KMOD_COMMON_H
#define TIMESTOMP_KMOD_COMMON_H

#include <linux/fs.h>
#include <linux/blkdev.h>
#include <linux/kprobes.h>

typedef unsigned long (*timestomp_kallsyms_lookup_name_t)(const char *name);

struct timestomp_find_sb_ctx {
	dev_t target_dev;
	struct super_block *found;
	bool match_bdev;
};

static inline int timestomp_resolve_kallsyms(timestomp_kallsyms_lookup_name_t *lookup)
{
	struct kprobe kp = {
		.symbol_name = "kallsyms_lookup_name",
	};
	int ret;

	ret = register_kprobe(&kp);
	if (ret < 0)
		return ret;

	*lookup = (timestomp_kallsyms_lookup_name_t)kp.addr;
	unregister_kprobe(&kp);
	return 0;
}

static inline unsigned long timestomp_sym_addr(timestomp_kallsyms_lookup_name_t lookup,
					       const char *name)
{
	if (!lookup)
		return 0;
	return lookup(name);
}

static inline void timestomp_find_sb_cb(struct super_block *sb, void *arg)
{
	struct timestomp_find_sb_ctx *ctx = arg;

	if (ctx->found)
		return;
	if ((ctx->match_bdev && sb->s_bdev && sb->s_bdev->bd_dev == ctx->target_dev) ||
	    sb->s_dev == ctx->target_dev)
		ctx->found = sb;
}

static inline void timestomp_apply_vfs_ts(struct inode *inode, __s64 secs, __u32 nsec,
					  char which)
{
	struct timespec64 t = {
		.tv_sec = secs,
		.tv_nsec = nsec,
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

#define TIMESTOMP_COPY_DONOR_TS(req, donor, field, getter) \
	do { \
		if (!(req).field.valid) { \
			struct timespec64 __t = getter(donor); \
			(req).field.secs = __t.tv_sec; \
			(req).field.nsec = __t.tv_nsec; \
			(req).field.valid = 1; \
		} \
	} while (0)

#define TIMESTOMP_APPLY_TS(req, target, field, which) \
	do { \
		if ((req).field.valid) \
			timestomp_apply_vfs_ts((target), (req).field.secs, (req).field.nsec, (which)); \
	} while (0)

#endif
