/*
 * ext4ts_ctl.c — Userspace control tool for the ext4_timestomp LKM.
 *
 * Usage:
 *   ext4ts_ctl --device /dev/sda1 --inode 42 [options]
 *
 * Options:
 *   --device <path>   Block device or mount point of the ext4 filesystem
 *   --inode  <num>    Target inode number
 *   --donor  <num>    Copy timestamps from this inode (overridden by explicit args)
 *   --atime  <secs>   Set access time (Unix seconds)
 *   --mtime  <secs>   Set modify time
 *   --ctime  <secs>   Set change time
 *   --crtime <secs>   Set creation/birth time (ext4-specific)
 *   --nsec   <nsec>   Nanoseconds applied to all explicit timestamps (default 0)
 *
 * Build:
 *   gcc -O2 -Wall -o ext4ts_ctl ext4ts_ctl.c
 *   (or use: make ctl)
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <getopt.h>
#include <time.h>
#include <inttypes.h>

/* Include shared header — mirrors the kernel-side definition */
#include "ext4ts.h"

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage: %s --device DEV --inode INO [options]\n"
		"\n"
		"Options:\n"
		"  --device  PATH   block device or mount point of ext4 fs\n"
		"  --inode   NUM    target inode number\n"
		"  --donor   NUM    copy timestamps from donor inode (explicit args override)\n"
		"  --atime   SECS   set atime (Unix seconds)\n"
		"  --mtime   SECS   set mtime\n"
		"  --ctime   SECS   set ctime\n"
		"  --crtime  SECS   set crtime/birth time\n"
		"  --nsec    NSEC   nanoseconds for all explicit timestamps (default 0)\n"
		"  --dry-run        print what would be done, do not call ioctl\n"
		"  --help\n"
		"\n"
		"Requires the ext4_timestomp kernel module to be loaded.\n",
		prog);
}

static void print_ts(const char *label, const struct ext4ts_ts *ts, const char *src)
{
	if (!ts->valid) {
		printf("  %-8s  (unchanged)\n", label);
		return;
	}
	struct tm *tm;
	time_t t = (time_t)ts->secs;
	char buf[64];
	tm = localtime(&t);
	strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", tm);
	printf("  %-8s  %lld.%09u  (%s)  [%s]\n",
	       label, (long long)ts->secs, ts->nsec, buf, src);
}

int main(int argc, char *argv[])
{
	struct ext4ts_req req;
	memset(&req, 0, sizeof(req));

	const char *device  = NULL;
	uint64_t    ino     = 0;
	uint32_t    nsec    = 0;
	int         dry_run = 0;

	/* Track which timestamps were set explicitly (for annotation) */
	int explicit_atime = 0, explicit_mtime = 0, explicit_ctime = 0, explicit_crtime = 0;

	static const struct option opts[] = {
		{"device",  required_argument, NULL, 'd'},
		{"inode",   required_argument, NULL, 'i'},
		{"donor",   required_argument, NULL, 'D'},
		{"atime",   required_argument, NULL, 'a'},
		{"mtime",   required_argument, NULL, 'm'},
		{"ctime",   required_argument, NULL, 'c'},
		{"crtime",  required_argument, NULL, 'b'},
		{"nsec",    required_argument, NULL, 'n'},
		{"dry-run", no_argument,       NULL, 'r'},
		{"help",    no_argument,       NULL, 'h'},
		{NULL, 0, NULL, 0},
	};

	int opt;
	while ((opt = getopt_long(argc, argv, "", opts, NULL)) != -1) {
		switch (opt) {
		case 'd': device = optarg; break;
		case 'i': ino    = (uint64_t)strtoull(optarg, NULL, 10); break;
		case 'D':
			req.donor_ino = (uint64_t)strtoull(optarg, NULL, 10);
			break;
		case 'a':
			req.atime.secs  = (int64_t)strtoll(optarg, NULL, 10);
			req.atime.valid = 1;
			explicit_atime  = 1;
			break;
		case 'm':
			req.mtime.secs  = (int64_t)strtoll(optarg, NULL, 10);
			req.mtime.valid = 1;
			explicit_mtime  = 1;
			break;
		case 'c':
			req.ctime.secs  = (int64_t)strtoll(optarg, NULL, 10);
			req.ctime.valid = 1;
			explicit_ctime  = 1;
			break;
		case 'b':
			req.crtime.secs  = (int64_t)strtoll(optarg, NULL, 10);
			req.crtime.valid = 1;
			explicit_crtime  = 1;
			break;
		case 'n':
			nsec = (uint32_t)strtoul(optarg, NULL, 10);
			break;
		case 'r': dry_run = 1; break;
		case 'h': usage(argv[0]); return 0;
		default:  usage(argv[0]); return 1;
		}
	}

	if (!device || !ino) {
		fprintf(stderr, "Error: --device and --inode are required.\n\n");
		usage(argv[0]);
		return 1;
	}

	/* Apply nsec to all explicitly-set timestamps */
	if (explicit_atime)  req.atime.nsec  = nsec;
	if (explicit_mtime)  req.mtime.nsec  = nsec;
	if (explicit_ctime)  req.ctime.nsec  = nsec;
	if (explicit_crtime) req.crtime.nsec = nsec;

	/* Fill request */
	strncpy(req.dev, device, sizeof(req.dev) - 1);
	req.ino = ino;

	printf("ext4ts_ctl: target device=%s inode=%"PRIu64"\n", device, ino);
	if (req.donor_ino)
		printf("  donor inode: %" PRIu64 "\n", (uint64_t)req.donor_ino);

	if (dry_run) {
		printf("[dry-run] would apply:\n");
		print_ts("atime",  &req.atime,  explicit_atime  ? "explicit" : req.donor_ino ? "from donor" : "skip");
		print_ts("mtime",  &req.mtime,  explicit_mtime  ? "explicit" : req.donor_ino ? "from donor" : "skip");
		print_ts("ctime",  &req.ctime,  explicit_ctime  ? "explicit" : req.donor_ino ? "from donor" : "skip");
		print_ts("crtime", &req.crtime, explicit_crtime ? "explicit" : req.donor_ino ? "from donor" : "skip");
		return 0;
	}

	/* Open misc device */
	int fd = open(EXT4TS_DEV_PATH, O_RDWR);
	if (fd < 0) {
		perror("open " EXT4TS_DEV_PATH);
		fprintf(stderr, "Is the ext4_timestomp module loaded? "
			"(sudo insmod ext4_timestomp.ko)\n");
		return 1;
	}

	if (ioctl(fd, EXT4TS_STOMP, &req) < 0) {
		perror("ioctl EXT4TS_STOMP");
		close(fd);
		return 1;
	}
	close(fd);

	printf("[+] success  inode_gen=%u\n", (unsigned)req.out_ino_gen);
	printf("Applied timestamps:\n");
	print_ts("atime",  &req.atime,  explicit_atime  ? "explicit" : "from donor");
	print_ts("mtime",  &req.mtime,  explicit_mtime  ? "explicit" : "from donor");
	print_ts("ctime",  &req.ctime,  explicit_ctime  ? "explicit" : "from donor");
	print_ts("crtime", &req.crtime, explicit_crtime ? "explicit" : "from donor");

	return 0;
}
