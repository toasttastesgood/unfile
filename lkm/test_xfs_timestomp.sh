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

for c in make lsmod grep awk mknod chmod mktemp dd mkfs.xfs mkfs.ext4 losetup mount umount stat sync; do
  need_cmd "${c}"
done

if [[ "${SKIP_BUILD}" -eq 0 ]]; then
  echo "[1/14] Building modules + userspace tools"
  make -j"$(nproc)"
else
  echo "[1/14] Skipping build"
fi

if [[ ! -x ./xfsts_ctl ]]; then
  echo "Missing ./xfsts_ctl (build first or remove --skip-build)" >&2
  exit 1
fi

if lsmod | grep -q '^xfs_timestomp'; then
  if [[ "${SKIP_LOAD}" -eq 1 ]]; then
    echo "[2/14] Module already loaded (skip-load set)"
  else
    echo "[2/14] Reloading xfs_timestomp module"
    rmmod xfs_timestomp
    insmod ./xfs_timestomp.ko
  fi
else
  if [[ "${SKIP_LOAD}" -eq 1 ]]; then
    echo "Module not loaded and --skip-load was set." >&2
    exit 1
  fi
  echo "[2/14] Loading xfs_timestomp module"
  insmod ./xfs_timestomp.ko
fi

minor="$(awk '$2 == "xfs_timestomp" { print $1 }' /proc/misc | head -n1)"
if [[ -z "${minor}" ]]; then
  echo "Could not find xfs_timestomp in /proc/misc." >&2
  exit 1
fi

if [[ ! -e /dev/xfs_timestomp ]]; then
  echo "[3/14] Creating /dev/xfs_timestomp (major 10 minor ${minor})"
  mknod /dev/xfs_timestomp c 10 "${minor}"
fi
chmod 600 /dev/xfs_timestomp

TMPDIR="$(mktemp -d /tmp/xfsts-test.XXXXXX)"
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

echo "[4/14] Creating loopback xfs test filesystem"
dd if=/dev/zero of="${IMG}" bs=1M count=512 status=none
LOOPDEV="$(losetup --find --show "${IMG}")"
mkfs.xfs -f "${LOOPDEV}" >/dev/null
mkdir -p "${MNT}"
mount -t xfs -o noatime,nodiratime "${LOOPDEV}" "${MNT}"
MOUNTED=1

echo "[5/14] Creating target and donor files"
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

echo "[6/14] Individual atime test (only atime changes)"
ATIME_SET=1609459601
read -r a0 m0 c0 < <(stat -c '%X %Y %Z' "${MNT}/atime.txt")
./xfsts_ctl --device "${MNT}" --path "${MNT}/atime.txt" --atime "${ATIME_SET}" --nsec 555555555
sync
read -r a1 m1 c1 < <(stat -c '%X %Y %Z' "${MNT}/atime.txt")
assert_eq "atime target value" "${ATIME_SET}" "${a1}"
assert_eq "atime test mtime unchanged" "${m0}" "${m1}"
assert_eq "atime test ctime unchanged" "${c0}" "${c1}"

echo "[7/14] Individual mtime test (only mtime changes)"
MTIME_SET=1609459602
read -r a0 m0 c0 < <(stat -c '%X %Y %Z' "${MNT}/mtime.txt")
./xfsts_ctl --device "${MNT}" --path "${MNT}/mtime.txt" --mtime "${MTIME_SET}" --nsec 555555555
sync
read -r a1 m1 c1 < <(stat -c '%X %Y %Z' "${MNT}/mtime.txt")
assert_eq "mtime test atime unchanged" "${a0}" "${a1}"
assert_eq "mtime target value" "${MTIME_SET}" "${m1}"
assert_eq "mtime test ctime unchanged" "${c0}" "${c1}"

echo "[8/14] Individual ctime test (only ctime changes)"
CTIME_SET=1609459603
read -r a0 m0 c0 < <(stat -c '%X %Y %Z' "${MNT}/ctime.txt")
./xfsts_ctl --device "${MNT}" --path "${MNT}/ctime.txt" --ctime "${CTIME_SET}" --nsec 555555555
sync
read -r a1 m1 c1 < <(stat -c '%X %Y %Z' "${MNT}/ctime.txt")
assert_eq "ctime test atime unchanged" "${a0}" "${a1}"
assert_eq "ctime test mtime unchanged" "${m0}" "${m1}"
assert_eq "ctime target value" "${CTIME_SET}" "${c1}"

echo "[9/14] Individual btime test (only btime changes)"
BTIME_SET=1609459604
read -r a0 m0 c0 < <(stat -c '%X %Y %Z' "${MNT}/btime.txt")
b0="$(stat -c '%W' "${MNT}/btime.txt")"
./xfsts_ctl --device "${MNT}" --path "${MNT}/btime.txt" --btime "${BTIME_SET}" --nsec 444444444
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

echo "[10/14] Donor mode test (copy all timestamps)"
DONOR_A=1609459701
DONOR_M=1609459702
DONOR_C=1609459703
DONOR_B=1609459704
./xfsts_ctl --device "${MNT}" --path "${MNT}/donor.txt" --atime "${DONOR_A}" --nsec 111111111
./xfsts_ctl --device "${MNT}" --path "${MNT}/donor.txt" --mtime "${DONOR_M}" --nsec 222222222
./xfsts_ctl --device "${MNT}" --path "${MNT}/donor.txt" --ctime "${DONOR_C}" --nsec 333333333
./xfsts_ctl --device "${MNT}" --path "${MNT}/donor.txt" --btime "${DONOR_B}" --nsec 444444444
sync

read -r da dm dc < <(stat -c '%X %Y %Z' "${MNT}/donor.txt")
db="$(stat -c '%W' "${MNT}/donor.txt")"
./xfsts_ctl --device "${MNT}" --path "${MNT}/receiver.txt" --donor-path "${MNT}/donor.txt"
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

echo "[11/14] Inode mode smoke test"
INODE_SET=1609459801
read -r a0 m0 c0 < <(stat -c '%X %Y %Z' "${MNT}/receiver.txt")
./xfsts_ctl --device "${MNT}" --inode "${recv_ino}" --atime "${INODE_SET}" --nsec 777777777
sync
read -r a1 m1 c1 < <(stat -c '%X %Y %Z' "${MNT}/receiver.txt")
assert_eq "inode-mode atime value" "${INODE_SET}" "${a1}"
assert_eq "inode-mode mtime unchanged" "${m0}" "${m1}"
assert_eq "inode-mode ctime unchanged" "${c0}" "${c1}"

echo "[12/14] Negative test: missing target path should fail"
assert_fail "missing target path" \
  ./xfsts_ctl --device "${MNT}" --path "${MNT}/does-not-exist.txt" --atime 1609459901 --nsec 1

echo "[13/14] Negative test: non-xfs target path should fail"
assert_fail "non-xfs target path" \
  ./xfsts_ctl --device "${MNT}" --path "/tmp" --atime 1609459902 --nsec 2

echo "[14/14] Negative test: cross-filesystem donor should fail"
dd if=/dev/zero of="${OTHER_IMG}" bs=1M count=64 status=none
OTHER_LOOPDEV="$(losetup --find --show "${OTHER_IMG}")"
mkfs.ext4 -F -q "${OTHER_LOOPDEV}"
mkdir -p "${OTHER_MNT}"
mount -t ext4 "${OTHER_LOOPDEV}" "${OTHER_MNT}"
OTHER_MOUNTED=1
echo "otherfs donor" > "${OTHER_MNT}/donor.txt"
sync
assert_fail "cross-fs donor path" \
  ./xfsts_ctl --device "${MNT}" --path "${MNT}/receiver.txt" --donor-path "${OTHER_MNT}/donor.txt"

echo "All xfs timestamp tests passed."
