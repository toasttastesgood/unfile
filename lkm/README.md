# ext4_timestomp LKM

Loadable kernel module for **live** ext4 inode timestamp modification — no unmounting required.

## Files

| File | Purpose |
|---|---|
| `ext4ts.h` | Shared ioctl structure + constants (kernel + userspace) |
| `ext4_timestomp.c` | Kernel module source |
| `ext4ts_ctl.c` | Userspace ioctl client |
| `Makefile` | Builds both; auto-detects `i_crtime` offset via BTF/pahole |

## Requirements

- Linux **6.1+** (uses `inode_set_{a,m,c}time_to_ts()` accessors)
- Kernel headers matching the running kernel (`linux-headers-$(uname -r)`)
- `gcc`, `make`
- `pahole` (optional, for crtime/birth-time support)
- Root to load the module and use `/dev/ext4_timestomp`

## Build

```bash
# Install kernel headers (Debian/Ubuntu)
sudo apt install linux-headers-$(uname -r) pahole

cd lkm/
make          # builds ext4_timestomp.ko + ext4ts_ctl
```

## Load / Unload

```bash
sudo insmod ext4_timestomp.ko   # creates /dev/ext4_timestomp
sudo rmmod  ext4_timestomp
```

## Usage

### Explicit timestamps

```bash
# Set ctime + mtime on inode 42, leaving other timestamps unchanged
sudo ./ext4ts_ctl --device /dev/sda1 --inode 42 \
    --ctime 1000000000 --mtime 1000000000 --nsec 0
```

### Donor mode

Copy all four timestamps from inode 17 onto inode 42 (explicit args override):

```bash
sudo ./ext4ts_ctl --device /dev/sda1 --inode 42 --donor 17
```

Override just ctime while copying the rest from donor:

```bash
sudo ./ext4ts_ctl --device /dev/sda1 --inode 42 --donor 17 \
    --ctime 1609459200
```

### Dry run

```bash
sudo ./ext4ts_ctl --device /dev/sda1 --inode 42 --donor 17 --dry-run
```

### Loop-mounted images

```bash
sudo losetup -fP test.img
LODEV=$(losetup -j test.img | cut -d: -f1)
sudo ./ext4ts_ctl --device $LODEV --inode 12 --donor 13
sudo losetup -d $LODEV
```

## How it works

1. The module registers a misc device at `/dev/ext4_timestomp` (mode 0600).
2. The userspace tool opens the device and issues `EXT4TS_STOMP` ioctl with a
   `struct ext4ts_req` containing the block device path, inode number, and
   desired timestamps.
3. The module:
   - Resolves `kallsyms_lookup_name` via a kprobe (works on 5.7+).
   - Calls `iterate_supers()` to find the superblock matching the device.
   - Looks up the inode via `ilookup()` (cache hit) or `ext4_iget()` (via
     kallsyms, for inodes not yet in the page cache).
   - In donor mode, looks up the donor inode and fills any unset timestamp
     fields from it.
   - Applies timestamps using `inode_set_{atime,mtime,ctime}_to_ts()`.
   - Patches `i_crtime` in `struct ext4_inode_info` directly if the byte
     offset is known (provided at compile time via BTF/pahole).
   - Calls `mark_inode_dirty()` + `write_inode_now(sync=1)` for durable
     writeback.

## crtime / birth time

`i_crtime` lives in `struct ext4_inode_info` (private to fs/ext4/) and is not
accessible through the VFS `struct inode`.  The Makefile detects its byte
offset using `pahole` + BTF:

- If `/sys/kernel/btf/vmlinux` exists → uses it directly (most accurate).
- If `/boot/vmlinux-$(uname -r)` exists → uses its embedded BTF.
- Otherwise → crtime patching is disabled at compile time and a warning is
  printed at module load.

## Precedence

For each timestamp field:

| Explicit arg | Donor given | Result |
|---|---|---|
| yes | yes/no | explicit value + `--nsec` |
| no | yes | donor value (including donor nsec) |
| no | no | field unchanged |

## Comparison with off-line tool

| | `ext4-timestomp` (Rust) | `ext4_timestomp` (LKM) |
|---|---|---|
| Filesystem state | **Must be unmounted** | **Live, mounted** |
| Checksum update | Manual CRC32C | Kernel handles it |
| crtime support | Yes (direct disk write) | Yes (BTF offset required) |
| Donor mode | Yes | Yes |
| Risk | Direct block device I/O | Standard VFS + writeback |
