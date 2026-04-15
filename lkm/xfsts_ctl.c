/*
 * xfsts_ctl.c - Userspace control tool for the xfs_timestomp LKM.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <getopt.h>
#include <time.h>
#include <inttypes.h>

#include "xfsts.h"

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage: %s --device DEV (--inode INO | --path FILE) [options]\n"
		"\n"
		"Options:\n"
		"  --device  PATH   block device or mount point of xfs fs\n"
		"  --inode   NUM    target inode number\n"
		"  --path    FILE   target file path (preferred for xfs)\n"
		"  --donor   NUM    copy timestamps from donor inode (explicit args override)\n"
		"  --donor-path FILE copy timestamps from donor file path\n"
		"  --atime   SECS   set atime (Unix seconds)\n"
		"  --mtime   SECS   set mtime\n"
		"  --ctime   SECS   set ctime\n"
		"  --btime   SECS   set birth/creation time\n"
		"  --nsec    NSEC   nanoseconds for all explicit timestamps (default 0)\n"
		"  --dry-run        print what would be done, do not call ioctl\n"
		"  --help\n"
		"\n"
		"Requires the xfs_timestomp kernel module to be loaded.\n",
		prog);
}

static void print_ts(const char *label, const struct xfsts_ts *ts, const char *src)
{
	if (!ts->valid) {
		printf("  %-8s  (unchanged)\n", label);
		return;
	}
	time_t t = (time_t)ts->secs;
	struct tm *tm = localtime(&t);
	char buf[64];

	if (!tm) {
		snprintf(buf, sizeof(buf), "invalid-time");
	} else {
		strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", tm);
	}
	printf("  %-8s  %lld.%09u  (%s)  [%s]\n",
	       label, (long long)ts->secs, ts->nsec, buf, src);
}

int main(int argc, char *argv[])
{
	struct xfsts_req req;
	const char *device = NULL;
	const char *target_path = NULL;
	const char *donor_path = NULL;
	uint64_t ino = 0;
	uint32_t nsec = 0;
	int dry_run = 0;
	int explicit_atime = 0, explicit_mtime = 0, explicit_ctime = 0, explicit_btime = 0;
	unsigned int ioctl_cmd = XFSTS_STOMP;
	const char *donor_src = "skip";
	int fd, opt;

	memset(&req, 0, sizeof(req));

	static const struct option opts[] = {
		{"device", required_argument, NULL, 'd'},
		{"inode", required_argument, NULL, 'i'},
		{"path", required_argument, NULL, 'p'},
		{"donor", required_argument, NULL, 'D'},
		{"donor-path", required_argument, NULL, 'P'},
		{"atime", required_argument, NULL, 'a'},
		{"mtime", required_argument, NULL, 'm'},
		{"ctime", required_argument, NULL, 'c'},
		{"btime", required_argument, NULL, 'b'},
		{"nsec", required_argument, NULL, 'n'},
		{"dry-run", no_argument, NULL, 'r'},
		{"help", no_argument, NULL, 'h'},
		{NULL, 0, NULL, 0},
	};

	while ((opt = getopt_long(argc, argv, "", opts, NULL)) != -1) {
		switch (opt) {
		case 'd':
			device = optarg;
			break;
		case 'i':
			ino = (uint64_t)strtoull(optarg, NULL, 10);
			break;
		case 'p':
			target_path = optarg;
			break;
		case 'D':
			req.donor_ino = (uint64_t)strtoull(optarg, NULL, 10);
			break;
		case 'P':
			donor_path = optarg;
			break;
		case 'a':
			req.atime.secs = (int64_t)strtoll(optarg, NULL, 10);
			req.atime.valid = 1;
			explicit_atime = 1;
			break;
		case 'm':
			req.mtime.secs = (int64_t)strtoll(optarg, NULL, 10);
			req.mtime.valid = 1;
			explicit_mtime = 1;
			break;
		case 'c':
			req.ctime.secs = (int64_t)strtoll(optarg, NULL, 10);
			req.ctime.valid = 1;
			explicit_ctime = 1;
			break;
		case 'b':
			req.btime.secs = (int64_t)strtoll(optarg, NULL, 10);
			req.btime.valid = 1;
			explicit_btime = 1;
			break;
		case 'n':
			nsec = (uint32_t)strtoul(optarg, NULL, 10);
			break;
		case 'r':
			dry_run = 1;
			break;
		case 'h':
			usage(argv[0]);
			return 0;
		default:
			usage(argv[0]);
			return 1;
		}
	}

	if (!device || (!ino && !target_path)) {
		fprintf(stderr, "Error: --device and one of (--inode, --path) are required.\n\n");
		usage(argv[0]);
		return 1;
	}

	if (explicit_atime)
		req.atime.nsec = nsec;
	if (explicit_mtime)
		req.mtime.nsec = nsec;
	if (explicit_ctime)
		req.ctime.nsec = nsec;
	if (explicit_btime)
		req.btime.nsec = nsec;

	strncpy(req.dev, device, sizeof(req.dev) - 1);
	req.ino = ino;
	if (target_path)
		strncpy(req.target, target_path, sizeof(req.target) - 1);
	if (donor_path)
		strncpy(req.donor_path, donor_path, sizeof(req.donor_path) - 1);
	if (target_path)
		ioctl_cmd = XFSTS_STOMP_PATH;
	if (donor_path)
		donor_src = "from donor path";
	else if (req.donor_ino)
		donor_src = "from donor inode";

	printf("xfsts_ctl: target device=%s", device);
	if (target_path)
		printf(" path=%s", target_path);
	if (ino)
		printf(" inode=%" PRIu64, ino);
	printf("\n");
	if (req.donor_ino)
		printf("  donor inode: %" PRIu64 "\n", (uint64_t)req.donor_ino);
	if (donor_path)
		printf("  donor path: %s\n", donor_path);

	if (dry_run) {
		printf("[dry-run] would apply:\n");
		print_ts("atime", &req.atime, explicit_atime ? "explicit" : donor_src);
		print_ts("mtime", &req.mtime, explicit_mtime ? "explicit" : donor_src);
		print_ts("ctime", &req.ctime, explicit_ctime ? "explicit" : donor_src);
		print_ts("btime", &req.btime, explicit_btime ? "explicit" : donor_src);
		return 0;
	}

	fd = open(XFSTS_DEV_PATH, O_RDWR);
	if (fd < 0) {
		perror("open " XFSTS_DEV_PATH);
		fprintf(stderr, "Is the xfs_timestomp module loaded? "
			"(sudo insmod xfs_timestomp.ko)\n");
		return 1;
	}

	if (ioctl(fd, ioctl_cmd, &req) < 0) {
		perror(ioctl_cmd == XFSTS_STOMP_PATH ? "ioctl XFSTS_STOMP_PATH" : "ioctl XFSTS_STOMP");
		close(fd);
		return 1;
	}
	close(fd);

	printf("[+] success inode_gen=%u\n", (unsigned)req.out_ino_gen);
	printf("Applied timestamps:\n");
	print_ts("atime", &req.atime, explicit_atime ? "explicit" : donor_src);
	print_ts("mtime", &req.mtime, explicit_mtime ? "explicit" : donor_src);
	print_ts("ctime", &req.ctime, explicit_ctime ? "explicit" : donor_src);
	print_ts("btime", &req.btime, explicit_btime ? "explicit" : donor_src);

	return 0;
}
