#!/usr/bin/env bash
set -euo pipefail

KEEP_ARTIFACTS=0
SKIP_BUILD=0
SKIP_LOAD=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --keep-artifacts) KEEP_ARTIFACTS=1; shift ;;
    --skip-build) SKIP_BUILD=1; shift ;;
    --skip-load) SKIP_LOAD=1; shift ;;
    *)
      echo "Unknown option: $1" >&2
      echo "Usage: sudo $0 [--keep-artifacts] [--skip-build] [--skip-load]" >&2
      exit 2
      ;;
  esac
done

if [[ "${EUID}" -ne 0 ]]; then
  echo "Run as root: sudo $0" >&2
  exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "${SCRIPT_DIR}"

need_cmd() {
  command -v "$1" >/dev/null 2>&1 || {
    echo "Missing required command: $1" >&2
    exit 1
  }
}

for c in make lsmod grep awk mknod chmod mktemp dd mkfs.btrfs mkfs.ext4 losetup mount umount stat touch sync; do
  need_cmd "${c}"
done

if [[ "${SKIP_BUILD}" -eq 0 ]]; then
  echo "[1/13] Building modules + userspace tools"
  make -j"$(nproc)"
else
  echo "[1/13] Skipping build"
fi

if [[ ! -x ./btrfsts_ctl ]]; then
  echo "Missing ./btrfsts_ctl (build first or remove --skip-build)" >&2
  exit 1
fi

if lsmod | grep -q '^btrfs_timestomp'; then
  if [[ "${SKIP_LOAD}" -eq 1 ]]; then
    echo "[2/13] Module already loaded (skip-load set)"
  else
    echo "[2/13] Reloading btrfs_timestomp module"
    rmmod btrfs_timestomp
    insmod ./btrfs_timestomp.ko
  fi
else
  if [[ "${SKIP_LOAD}" -eq 1 ]]; then
    echo "Module not loaded and --skip-load was set." >&2
    exit 1
  fi
  echo "[2/13] Loading btrfs_timestomp module"
  insmod ./btrfs_timestomp.ko
fi

minor="$(awk '$2 == "btrfs_timestomp" { print $1 }' /proc/misc | head -n1)"
if [[ -z "${minor}" ]]; then
  echo "Could not find btrfs_timestomp in /proc/misc." >&2
  exit 1
fi

if [[ ! -e /dev/btrfs_timestomp ]]; then
  echo "[3/13] Creating /dev/btrfs_timestomp (major 10 minor ${minor})"
  mknod /dev/btrfs_timestomp c 10 "${minor}"
fi
chmod 600 /dev/btrfs_timestomp

TMPDIR="$(mktemp -d /tmp/btrfsts-test.XXXXXX)"
IMG="${TMPDIR}/fs.img"
MNT="${TMPDIR}/mnt"
OTHER_MNT="${TMPDIR}/other_mnt"
LOOPDEV=""
OTHER_IMG="${TMPDIR}/other_ext4.img"
OTHER_LOOPDEV=""
MOUNTED=0
OTHER_MOUNTED=0

cleanup() {
  if [[ "${OTHER_MOUNTED}" -eq 1 ]]; then
    umount "${OTHER_MNT}" || true
  fi
  if [[ "${MOUNTED}" -eq 1 ]]; then
    umount "${MNT}" || true
  fi
  if [[ -n "${OTHER_LOOPDEV}" ]]; then
    losetup -d "${OTHER_LOOPDEV}" || true
  fi
  if [[ -n "${LOOPDEV}" ]]; then
    losetup -d "${LOOPDEV}" || true
  fi
  if [[ "${KEEP_ARTIFACTS}" -eq 0 ]]; then
    rm -rf "${TMPDIR}" || true
  else
    echo "Artifacts kept at: ${TMPDIR}"
  fi
}
trap cleanup EXIT

echo "[4/13] Creating loopback btrfs test filesystem"
dd if=/dev/zero of="${IMG}" bs=1M count=512 status=none
LOOPDEV="$(losetup --find --show "${IMG}")"
mkfs.btrfs -f -q "${LOOPDEV}"
mkdir -p "${MNT}"
mount -t btrfs -o noatime "${LOOPDEV}" "${MNT}"
MOUNTED=1

echo "[5/13] Creating target and donor files"
echo "atime-target" > "${MNT}/atime.txt"
echo "mtime-target" > "${MNT}/mtime.txt"
echo "ctime-target" > "${MNT}/ctime.txt"
echo "btime-target" > "${MNT}/btime.txt"
echo "donor-file"   > "${MNT}/donor.txt"
echo "recv-file"    > "${MNT}/receiver.txt"
sync

atime_ino="$(stat -c '%i' "${MNT}/atime.txt")"
mtime_ino="$(stat -c '%i' "${MNT}/mtime.txt")"
ctime_ino="$(stat -c '%i' "${MNT}/ctime.txt")"
btime_ino="$(stat -c '%i' "${MNT}/btime.txt")"
donor_ino="$(stat -c '%i' "${MNT}/donor.txt")"
recv_ino="$(stat -c '%i' "${MNT}/receiver.txt")"

echo "    atime inode:    ${atime_ino}"
echo "    mtime inode:    ${mtime_ino}"
echo "    ctime inode:    ${ctime_ino}"
echo "    btime inode:    ${btime_ino}"
echo "    donor inode:    ${donor_ino}"
echo "    receiver inode: ${recv_ino}"

assert_eq() {
  local label="$1"
  local expected="$2"
  local actual="$3"
  if [[ "${expected}" != "${actual}" ]]; then
    echo "FAIL: ${label} expected=${expected} actual=${actual}" >&2
    exit 1
  fi
  echo "PASS: ${label} = ${actual}"
}

assert_fail() {
  local label="$1"
  shift
  if "$@"; then
    echo "FAIL: ${label} (unexpected success)" >&2
    exit 1
  fi
  echo "PASS: ${label} failed as expected"
}

echo "[6/13] Individual atime test (only atime changes)"
ATIME_SET=1609459301
read -r a0 m0 c0 < <(stat -c '%X %Y %Z' "${MNT}/atime.txt")
./btrfsts_ctl --device "${MNT}" --path "${MNT}/atime.txt" --atime "${ATIME_SET}" --nsec 555555555
sync
read -r a1 m1 c1 < <(stat -c '%X %Y %Z' "${MNT}/atime.txt")
assert_eq "atime target value" "${ATIME_SET}" "${a1}"
assert_eq "atime test mtime unchanged" "${m0}" "${m1}"
assert_eq "atime test ctime unchanged" "${c0}" "${c1}"

echo "[7/13] Individual mtime test (only mtime changes)"
MTIME_SET=1609459302
read -r a0 m0 c0 < <(stat -c '%X %Y %Z' "${MNT}/mtime.txt")
./btrfsts_ctl --device "${MNT}" --path "${MNT}/mtime.txt" --mtime "${MTIME_SET}" --nsec 555555555
sync
read -r a1 m1 c1 < <(stat -c '%X %Y %Z' "${MNT}/mtime.txt")
assert_eq "mtime test atime unchanged" "${a0}" "${a1}"
assert_eq "mtime target value" "${MTIME_SET}" "${m1}"
assert_eq "mtime test ctime unchanged" "${c0}" "${c1}"

echo "[8/13] Individual ctime test (only ctime changes)"
CTIME_SET=1609459303
read -r a0 m0 c0 < <(stat -c '%X %Y %Z' "${MNT}/ctime.txt")
./btrfsts_ctl --device "${MNT}" --path "${MNT}/ctime.txt" --ctime "${CTIME_SET}" --nsec 555555555
sync
read -r a1 m1 c1 < <(stat -c '%X %Y %Z' "${MNT}/ctime.txt")
assert_eq "ctime test atime unchanged" "${a0}" "${a1}"
assert_eq "ctime test mtime unchanged" "${m0}" "${m1}"
assert_eq "ctime target value" "${CTIME_SET}" "${c1}"

echo "[9/13] Individual btime test (only btime changes)"
BTIME_SET=1609459304
read -r a0 m0 c0 < <(stat -c '%X %Y %Z' "${MNT}/btime.txt")
b0="$(stat -c '%W' "${MNT}/btime.txt")"
./btrfsts_ctl --device "${MNT}" --path "${MNT}/btime.txt" --btime "${BTIME_SET}" --nsec 444444444
sync
read -r a1 m1 c1 < <(stat -c '%X %Y %Z' "${MNT}/btime.txt")
b1="$(stat -c '%W' "${MNT}/btime.txt")"
assert_eq "btime test atime unchanged" "${a0}" "${a1}"
assert_eq "btime test mtime unchanged" "${m0}" "${m1}"
assert_eq "btime test ctime unchanged" "${c0}" "${c1}"
if [[ "${b1}" != "-1" ]]; then
  assert_eq "btime target value (%W)" "${BTIME_SET}" "${b1}"
else
  echo "WARN: stat %W unsupported; skipping direct btime value check"
fi

echo "[10/13] Donor mode test (copy all timestamps)"
DONOR_A=1609459401
DONOR_M=1609459402
DONOR_C=1609459403
DONOR_B=1609459404
./btrfsts_ctl --device "${MNT}" --path "${MNT}/donor.txt" --atime "${DONOR_A}" --nsec 111111111
./btrfsts_ctl --device "${MNT}" --path "${MNT}/donor.txt" --mtime "${DONOR_M}" --nsec 222222222
./btrfsts_ctl --device "${MNT}" --path "${MNT}/donor.txt" --ctime "${DONOR_C}" --nsec 333333333
./btrfsts_ctl --device "${MNT}" --path "${MNT}/donor.txt" --btime "${DONOR_B}" --nsec 444444444
sync

read -r da dm dc < <(stat -c '%X %Y %Z' "${MNT}/donor.txt")
db="$(stat -c '%W' "${MNT}/donor.txt")"
./btrfsts_ctl --device "${MNT}" --path "${MNT}/receiver.txt" --donor-path "${MNT}/donor.txt"
sync
read -r ra rm rc < <(stat -c '%X %Y %Z' "${MNT}/receiver.txt")
rb="$(stat -c '%W' "${MNT}/receiver.txt")"
assert_eq "donor copy atime" "${da}" "${ra}"
assert_eq "donor copy mtime" "${dm}" "${rm}"
assert_eq "donor copy ctime" "${dc}" "${rc}"
if [[ "${db}" != "-1" && "${rb}" != "-1" ]]; then
  assert_eq "donor copy btime" "${db}" "${rb}"
else
  echo "WARN: stat %W unsupported; skipping donor btime comparison"
fi

echo "[11/13] Negative test: missing target path should fail"
assert_fail "missing target path" \
  ./btrfsts_ctl --device "${MNT}" --path "${MNT}/does-not-exist.txt" --atime 1609459501 --nsec 1

echo "[12/13] Negative test: non-btrfs target path should fail"
assert_fail "non-btrfs target path" \
  ./btrfsts_ctl --device "${MNT}" --path "/tmp" --atime 1609459502 --nsec 2

echo "[13/13] Negative test: cross-filesystem donor should fail"
dd if=/dev/zero of="${OTHER_IMG}" bs=1M count=32 status=none
OTHER_LOOPDEV="$(losetup --find --show "${OTHER_IMG}")"
mkfs.ext4 -F -q "${OTHER_LOOPDEV}"
mkdir -p "${OTHER_MNT}"
mount -t ext4 "${OTHER_LOOPDEV}" "${OTHER_MNT}"
OTHER_MOUNTED=1
echo "otherfs donor" > "${OTHER_MNT}/donor.txt"
sync
assert_fail "cross-fs donor path" \
  ./btrfsts_ctl --device "${MNT}" --path "${MNT}/receiver.txt" --donor-path "${OTHER_MNT}/donor.txt"

echo "All btrfs timestamp tests passed."
