/* ext4ts.h — shared header for ext4_timestomp LKM and userspace tool
 *
 * Include from both kernel module and the userspace ioctl client.
 */
#ifndef EXT4TS_H
#define EXT4TS_H

#include <linux/types.h>
#include <linux/ioctl.h>

#define EXT4TS_DEV_NAME  "ext4_timestomp"
#define EXT4TS_DEV_PATH  "/dev/ext4_timestomp"

/* Timestamp value: seconds + nanoseconds.
 * Use -1 in nsec to mean "leave nanoseconds unchanged when using donor".
 * Leave secs == 0 and nsec == (u32)-1 to mean "don't touch this field". */
struct ext4ts_ts {
    __s64 secs;   /* Unix seconds (signed, supports post-2038) */
    __u32 nsec;   /* nanoseconds 0..999999999 */
    __u8  valid;  /* 1 = apply this field, 0 = leave unchanged    */
    __u8  _pad[3];
};

/* ioctl request structure */
struct ext4ts_req {
    /* Target: device path + inode number */
    char        dev[256];       /* e.g. "/dev/sda1" or "/dev/loop0"       */
    __u64       ino;            /* inode number to stomp                   */

    /* Timestamps to write (valid=1) or skip (valid=0) */
    struct ext4ts_ts  atime;
    struct ext4ts_ts  mtime;
    struct ext4ts_ts  ctime;
    struct ext4ts_ts  crtime;   /* birth time (ext4-specific)              */

    /* Donor mode: if donor_ino != 0, read timestamps from this inode first.
     * Explicit fields with valid=1 above still override donor values.    */
    __u64       donor_ino;

    /* Output: filled by the kernel on success */
    __u64       out_ino_gen;    /* inode generation (for diagnostics)      */
};

#define EXT4TS_MAGIC  'T'
#define EXT4TS_STOMP  _IOWR(EXT4TS_MAGIC, 1, struct ext4ts_req)

#endif /* EXT4TS_H */
