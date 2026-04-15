/*
 * xfsts_ctl.c - Userspace control tool for the xfs_timestomp LKM.
 */

#include "xfsts.h"

#define TS_TOOL_NAME "xfsts_ctl"
#define TS_FS_NAME "xfs"
#define TS_REQ_TYPE struct xfsts_req
#define TS_TS_TYPE struct xfsts_ts
#define TS_DEV_PATH XFSTS_DEV_PATH
#define TS_IOCTL_STOMP XFSTS_STOMP
#define TS_IOCTL_STOMP_PATH XFSTS_STOMP_PATH

#include "path_timestomp_ctl_template.h"
