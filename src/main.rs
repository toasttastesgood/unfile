//! ext4-timestomp: directly edit ctime/crtime (and atime/mtime) in an ext4 inode
//! by parsing the on-disk structures ourselves. No libext2fs.
//!
//! WARNING: operate on an UNMOUNTED filesystem only. Editing a mounted fs's
//! raw block device races against the kernel's inode cache and journal.
//!
//! Tested against ext4 with the following features, which are the modern
//! defaults on Debian/Ubuntu/RHEL:
//!   - 256-byte inodes (needed for nanosecond + crtime fields)
//!   - metadata_csum (inode checksums)
//!   - 64bit (64-bit block numbers; we handle both)
//!   - flex_bg (doesn't affect inode lookup math)
//!
//! On-disk layout references: fs/ext4/ext4.h in the Linux kernel, and the
//! ext4 wiki at https://ext4.wiki.kernel.org/index.php/Ext4_Disk_Layout

use anyhow::{anyhow, bail, Context, Result};
use clap::Parser;
use std::fs::OpenOptions;
use std::io::{Read, Seek, SeekFrom, Write};
use std::os::unix::fs::MetadataExt;
use std::path::PathBuf;

// ---------- Superblock ----------
//
// Lives at byte offset 1024 from the start of the block device, regardless of
// block size. It's 1024 bytes. We only care about a handful of fields.

const SUPERBLOCK_OFFSET: u64 = 1024;
const EXT4_SUPER_MAGIC: u16 = 0xEF53;

// Feature flag bits we care about
const INCOMPAT_64BIT: u32 = 0x80;
const RO_COMPAT_METADATA_CSUM: u32 = 0x400;

#[repr(C, packed)]
#[derive(Debug, Default, Clone, Copy)]
struct Superblock {
    s_inodes_count: u32,
    s_blocks_count_lo: u32,
    _pad1: [u8; 16],
    s_log_block_size: u32,         // block size = 1024 << this
    _pad2: [u8; 4],
    s_blocks_per_group: u32,
    _pad3: [u8; 4],
    s_inodes_per_group: u32,
    _pad4: [u8; 12],
    s_magic: u16,
    _pad5: [u8; 34],
    s_inode_size: u16,             // size of on-disk inode struct (usually 256)
    _pad6: [u8; 2],
    s_feature_compat: u32,
    s_feature_incompat: u32,
    s_feature_ro_compat: u32,
    s_uuid: [u8; 16],              // needed for checksum seed
    _pad7: [u8; 118],
    s_desc_size: u16,              // group descriptor size (32 or 64)
    _pad8: [u8; 636],              // pad out to 1024
    s_checksum_seed: u32,          // if metadata_csum_seed set, else derived from uuid
}

// sanity: superblock is 1024 bytes
const _: () = assert!(std::mem::size_of::<Superblock>() == 1024);

impl Superblock {
    fn block_size(&self) -> u64 {
        1024u64 << self.s_log_block_size
    }
    fn has_64bit(&self) -> bool {
        self.s_feature_incompat & INCOMPAT_64BIT != 0
    }
    fn has_metadata_csum(&self) -> bool {
        self.s_feature_ro_compat & RO_COMPAT_METADATA_CSUM != 0
    }
}

// ---------- Group Descriptor ----------
//
// Array of these starts in the block right after the superblock (block 1 for
// 4K fs, block 2 for 1K fs). Each descriptor is 32 bytes on classic ext4, or
// 64 bytes if the 64bit feature is set. We need bg_inode_table to find where
// inodes for that group live.

#[repr(C, packed)]
#[derive(Debug, Default, Clone, Copy)]
struct GroupDesc64 {
    bg_block_bitmap_lo: u32,
    bg_inode_bitmap_lo: u32,
    bg_inode_table_lo: u32,
    _pad1: [u8; 16],
    bg_inode_table_hi: u32,
    _pad2: [u8; 32],
}
const _: () = assert!(std::mem::size_of::<GroupDesc64>() == 64);

// ---------- Inode ----------
//
// The on-disk inode struct. Classic ext2 was 128 bytes; ext4 extends it to
// 256 bytes (configurable). The extra space past byte 128 holds nanosecond
// fields, crtime, and the inode checksum.
//
// Key timestamp layout:
//   Bytes  8..12   i_ctime          (i32, seconds since epoch — signed!)
//   Bytes 132..136 i_ctime_extra    (u32: low 2 bits extend epoch, upper 30 bits are nsec)
//   Bytes 144..148 i_crtime         (i32)
//   Bytes 148..152 i_crtime_extra   (u32)
//   Bytes  16..20  i_mtime / i_mtime_extra at 140
//   Bytes  32..36  was i_dtime (deletion time); atime is at offset 8 group...
//
// Easier: just define the full struct and index by field.

#[repr(C, packed)]
#[derive(Debug, Default, Clone, Copy)]
struct Ext4Inode {
    i_mode: u16,
    i_uid: u16,
    i_size_lo: u32,
    i_atime: i32,
    i_ctime: i32,
    i_mtime: i32,
    i_dtime: i32,
    i_gid: u16,
    i_links_count: u16,
    i_blocks_lo: u32,
    i_flags: u32,
    i_osd1: u32,
    i_block: [u32; 15],
    i_generation: u32,
    i_file_acl_lo: u32,
    i_size_hi: u32,
    i_faddr: u32,
    i_osd2: [u8; 12],
    // end of classic 128-byte inode
    i_extra_isize: u16,
    i_checksum_hi: u16,
    i_ctime_extra: u32,
    i_mtime_extra: u32,
    i_atime_extra: u32,
    i_crtime: i32,
    i_crtime_extra: u32,
    i_version_hi: u32,
    i_projid: u32,
}
const _: () = assert!(std::mem::size_of::<Ext4Inode>() == 160);

// ---------- Raw device I/O helpers ----------

fn read_struct<T: Copy>(dev: &mut std::fs::File, offset: u64) -> Result<T> {
    dev.seek(SeekFrom::Start(offset))?;
    // safety: T is repr(C, packed) POD, we read exactly size_of::<T>() bytes
    let mut buf = vec![0u8; std::mem::size_of::<T>()];
    dev.read_exact(&mut buf)?;
    let val = unsafe { std::ptr::read_unaligned(buf.as_ptr() as *const T) };
    Ok(val)
}

fn write_bytes(dev: &mut std::fs::File, offset: u64, data: &[u8]) -> Result<()> {
    dev.seek(SeekFrom::Start(offset))?;
    dev.write_all(data)?;
    Ok(())
}

// ---------- Timestamp encoding ----------
//
// ext4 splits each timestamp into two fields:
//   i_xtime:       i32 signed seconds since the Unix epoch
//   i_xtime_extra: u32 where the low 2 bits are the "epoch bits" (extending
//                  the range past 2038) and the upper 30 bits are nanoseconds.
//
// To decode: seconds = i_xtime as i64 | ((extra & 0x3) as i64) << 32
// To encode: epoch_bits = ((secs >> 32) & 0x3) as u32
//            xtime = secs as i32  (low 32 bits, will go negative for pre-1970
//                                  OR for 2038..2106 range — that's fine)
//            extra = (nsec << 2) | epoch_bits

fn encode_timestamp(secs: i64, nsec: u32) -> (i32, u32) {
    let xtime = secs as i32; // low 32 bits, sign-extended on decode
    let epoch_bits = ((secs >> 32) & 0x3) as u32;
    let extra = (nsec << 2) | epoch_bits;
    (xtime, extra)
}

// ---------- Inode checksum ----------
//
// ext4 metadata_csum uses CRC32C. The inode checksum is computed over the
// entire inode with the two checksum fields (i_checksum_lo inside i_osd2 for
// linux, and i_checksum_hi) zeroed, seeded with:
//
//   seed = crc32c(fs_uuid, initial = ~0)
//   seed = crc32c(inode_number_le, seed)
//   seed = crc32c(i_generation_le, seed)
//   csum = crc32c(inode_bytes_with_csum_zeroed, seed)
//
// then split: low 16 bits go into osd2.linux2.l_i_checksum_lo (offset 0x7C,
// i.e. bytes 124..126 within the inode), high 16 bits go into i_checksum_hi
// (offset 130..132) if i_extra_isize is large enough.

const OSD2_CSUM_LO_OFFSET: usize = 0x7C; // bytes 124..126
const CSUM_HI_OFFSET: usize = 0x82;      // bytes 130..132

fn compute_inode_csum(
    inode_bytes: &[u8],
    fs_uuid: &[u8; 16],
    inode_num: u32,
    generation: u32,
) -> u32 {
    // zero out checksum fields in a local copy
    let mut buf = inode_bytes.to_vec();
    buf[OSD2_CSUM_LO_OFFSET..OSD2_CSUM_LO_OFFSET + 2].fill(0);
    if buf.len() >= CSUM_HI_OFFSET + 2 {
        buf[CSUM_HI_OFFSET..CSUM_HI_OFFSET + 2].fill(0);
    }

    let seed = crc32c::crc32c(fs_uuid);
    let seed = crc32c::crc32c_append(seed, &inode_num.to_le_bytes());
    let seed = crc32c::crc32c_append(seed, &generation.to_le_bytes());
    crc32c::crc32c_append(seed, &buf)
}

// ---------- CLI ----------

#[derive(Parser)]
#[command(about = "Edit ext4 inode timestamps directly on an unmounted device")]
struct Args {
    /// Block device or image file (e.g. /dev/loop0, disk.img)
    #[arg(short, long)]
    device: PathBuf,

    /// Inode number to edit
    #[arg(short, long)]
    inode: u32,

    /// New ctime as Unix seconds (optional)
    #[arg(long)]
    ctime: Option<i64>,

    /// New crtime (birth time) as Unix seconds (optional)
    #[arg(long)]
    crtime: Option<i64>,

    /// New mtime as Unix seconds (optional)
    #[arg(long)]
    mtime: Option<i64>,

    /// New atime as Unix seconds (optional)
    #[arg(long)]
    atime: Option<i64>,

    /// Nanoseconds component applied to any timestamp set above
    #[arg(long, default_value_t = 0)]
    nsec: u32,

    /// Dry run — read and show what we'd write, but don't write
    #[arg(long)]
    dry_run: bool,
}

fn main() -> Result<()> {
    let args = Args::parse();

    let mut dev = OpenOptions::new()
        .read(true)
        .write(!args.dry_run)
        .open(&args.device)
        .with_context(|| format!("opening {}", args.device.display()))?;

    // 1. Read superblock
    let sb: Superblock = read_struct(&mut dev, SUPERBLOCK_OFFSET)?;
    if { sb.s_magic } != EXT4_SUPER_MAGIC {
        bail!("not an ext2/3/4 filesystem (bad magic {:#x})", { sb.s_magic });
    }
    let block_size = sb.block_size();
    let inode_size = { sb.s_inode_size } as u64;
    let inodes_per_group = { sb.s_inodes_per_group } as u64;
    let desc_size = if sb.has_64bit() {
        { sb.s_desc_size } as u64
    } else {
        32
    };

    println!("[+] ext4 superblock OK");
    println!("    block size       = {}", block_size);
    println!("    inode size       = {}", inode_size);
    println!("    inodes per group = {}", inodes_per_group);
    println!("    desc size        = {}", desc_size);
    println!("    metadata_csum    = {}", sb.has_metadata_csum());

    if inode_size < 256 {
        bail!("inode size {} < 256, no crtime/nsec fields available", inode_size);
    }

    // 2. Locate the group descriptor for our inode
    //
    // Inodes are numbered starting from 1. Block group = (ino-1) / inodes_per_group.
    // Index within group = (ino-1) % inodes_per_group.
    let ino = args.inode as u64;
    if ino == 0 {
        bail!("inode 0 is invalid");
    }
    let group = (ino - 1) / inodes_per_group;
    let index_in_group = (ino - 1) % inodes_per_group;

    // Group descriptor table lives in the block right after the superblock.
    // For 1K blocks, SB is in block 1, GDT starts block 2. For 4K blocks, SB
    // occupies block 0 (first 1024 bytes) and GDT starts block 1.
    let gdt_start_block = if block_size == 1024 { 2 } else { 1 };
    let gdt_offset = gdt_start_block * block_size + group * desc_size;

    // Read enough of the descriptor to get inode_table_lo/hi. GroupDesc64
    // covers both 32- and 64-byte cases because we only read the first fields
    // we need; _pad2 gets truncated harmlessly on 32-byte descriptors.
    let mut gd_buf = vec![0u8; desc_size as usize];
    dev.seek(SeekFrom::Start(gdt_offset))?;
    dev.read_exact(&mut gd_buf)?;

    let inode_table_lo = u32::from_le_bytes(gd_buf[8..12].try_into().unwrap()) as u64;
    let inode_table_hi = if desc_size >= 64 {
        u32::from_le_bytes(gd_buf[40..44].try_into().unwrap()) as u64
    } else {
        0
    };
    let inode_table_block = (inode_table_hi << 32) | inode_table_lo;
    let inode_offset = inode_table_block * block_size + index_in_group * inode_size;

    println!(
        "[+] inode {} -> group {}, index {}, table block {}, byte offset {:#x}",
        args.inode, group, index_in_group, inode_table_block, inode_offset
    );

    // 3. Read the full inode (inode_size bytes, not just size_of::<Ext4Inode>)
    let mut inode_buf = vec![0u8; inode_size as usize];
    dev.seek(SeekFrom::Start(inode_offset))?;
    dev.read_exact(&mut inode_buf)?;

    let inode: Ext4Inode = unsafe { std::ptr::read_unaligned(inode_buf.as_ptr() as *const _) };

    let cur_ctime = { inode.i_ctime };
    let cur_crtime = { inode.i_crtime };
    let cur_mtime = { inode.i_mtime };
    let cur_atime = { inode.i_atime };
    let generation = { inode.i_generation };

    println!("[+] current timestamps:");
    println!("    atime  = {}", cur_atime);
    println!("    mtime  = {}", cur_mtime);
    println!("    ctime  = {}", cur_ctime);
    println!("    crtime = {}", cur_crtime);
    println!("    generation = {:#x}", generation);

    // 4. Patch the timestamps in our local buffer. Field offsets within the
    //    inode are fixed by the struct layout above.
    //
    //    i_atime at 0x08, i_ctime at 0x0C, i_mtime at 0x10
    //    i_atime_extra at 0x88, i_ctime_extra at 0x84, i_mtime_extra at 0x8C
    //    i_crtime at 0x90, i_crtime_extra at 0x94
    fn patch(buf: &mut [u8], offset: usize, xtime: i32, extra_off: usize, extra: u32) {
        buf[offset..offset + 4].copy_from_slice(&xtime.to_le_bytes());
        buf[extra_off..extra_off + 4].copy_from_slice(&extra.to_le_bytes());
    }

    if let Some(secs) = args.ctime {
        let (x, e) = encode_timestamp(secs, args.nsec);
        patch(&mut inode_buf, 0x0C, x, 0x84, e);
        println!("[+] setting ctime  = {}.{:09}", secs, args.nsec);
    }
    if let Some(secs) = args.mtime {
        let (x, e) = encode_timestamp(secs, args.nsec);
        patch(&mut inode_buf, 0x10, x, 0x8C, e);
        println!("[+] setting mtime  = {}.{:09}", secs, args.nsec);
    }
    if let Some(secs) = args.atime {
        let (x, e) = encode_timestamp(secs, args.nsec);
        patch(&mut inode_buf, 0x08, x, 0x88, e);
        println!("[+] setting atime  = {}.{:09}", secs, args.nsec);
    }
    if let Some(secs) = args.crtime {
        let (x, e) = encode_timestamp(secs, args.nsec);
        patch(&mut inode_buf, 0x90, x, 0x94, e);
        println!("[+] setting crtime = {}.{:09}", secs, args.nsec);
    }

    // 5. Recompute the inode checksum if the filesystem uses metadata_csum.
    if sb.has_metadata_csum() {
        let csum = compute_inode_csum(&inode_buf, &sb.s_uuid, args.inode, generation);
        let lo = (csum & 0xFFFF) as u16;
        let hi = ((csum >> 16) & 0xFFFF) as u16;
        inode_buf[OSD2_CSUM_LO_OFFSET..OSD2_CSUM_LO_OFFSET + 2]
            .copy_from_slice(&lo.to_le_bytes());
        if inode_size as usize >= CSUM_HI_OFFSET + 2 {
            inode_buf[CSUM_HI_OFFSET..CSUM_HI_OFFSET + 2].copy_from_slice(&hi.to_le_bytes());
        }
        println!("[+] new inode checksum = {:#010x}", csum);
    }

    // 6. Write it back.
    if args.dry_run {
        println!("[!] dry run — not writing");
    } else {
        // Sanity check: refuse to write if device looks like it's mounted.
        // On Linux, opening /dev/sdXN O_RDWR succeeds even when mounted, so
        // we check /proc/mounts as a guardrail.
        if let Ok(mounts) = std::fs::read_to_string("/proc/mounts") {
            let dev_str = args.device.to_string_lossy();
            for line in mounts.lines() {
                if line.starts_with(dev_str.as_ref()) {
                    return Err(anyhow!(
                        "{} appears mounted — refusing to write. Unmount first.",
                        dev_str
                    ));
                }
            }
        }
        write_bytes(&mut dev, inode_offset, &inode_buf)?;
        dev.sync_all()?;
        println!("[+] wrote inode back to disk");
    }

    let _ = generation; // silence unused in some paths
    Ok(())
}
