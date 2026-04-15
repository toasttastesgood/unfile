/*
 * btrfsts_ctl.c - Userspace control tool for the btrfs_timestomp LKM.
 */

#include "btrfsts.h"

#define TS_TOOL_NAME "btrfsts_ctl"
#define TS_FS_NAME "btrfs"
#define TS_REQ_TYPE struct btrfsts_req
#define TS_TS_TYPE struct btrfsts_ts
#define TS_DEV_PATH BTRFSTS_DEV_PATH
#define TS_IOCTL_STOMP BTRFSTS_STOMP
#define TS_IOCTL_STOMP_PATH BTRFSTS_STOMP_PATH

#include "path_timestomp_ctl_template.h"
