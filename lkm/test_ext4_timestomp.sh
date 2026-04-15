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

for c in make lsmod grep awk mknod chmod mktemp dd mkfs.ext4 losetup mount umount stat sync debugfs; do
  need_cmd "${c}"
done

if [[ "${SKIP_BUILD}" -eq 0 ]]; then
  echo "[1/11] Building module + userspace tool"
  make -j"$(nproc)"
else
  echo "[1/11] Skipping build"
fi

if [[ ! -x ./ext4ts_ctl ]]; then
  echo "Missing ./ext4ts_ctl (build first or remove --skip-build)" >&2
  exit 1
fi

if lsmod | grep -q '^ext4_timestomp'; then
  if [[ "${SKIP_LOAD}" -eq 1 ]]; then
    echo "[2/11] Module already loaded (skip-load set)"
  else
    echo "[2/11] Reloading ext4_timestomp module"
    rmmod ext4_timestomp
    insmod ./ext4_timestomp.ko
  fi
else
  if [[ "${SKIP_LOAD}" -eq 1 ]]; then
    echo "Module not loaded and --skip-load was set." >&2
    exit 1
  fi
  echo "[2/11] Loading ext4_timestomp module"
  insmod ./ext4_timestomp.ko
fi

minor="$(awk '$2 == "ext4_timestomp" { print $1 }' /proc/misc | head -n1)"
if [[ -z "${minor}" ]]; then
  echo "Could not find ext4_timestomp in /proc/misc." >&2
  exit 1
fi

if [[ ! -e /dev/ext4_timestomp ]]; then
  echo "[3/11] Creating /dev/ext4_timestomp (major 10 minor ${minor})"
  mknod /dev/ext4_timestomp c 10 "${minor}"
fi
chmod 600 /dev/ext4_timestomp

TMPDIR="$(mktemp -d /tmp/ext4ts-test.XXXXXX)"
IMG="${TMPDIR}/fs.img"
MNT="${TMPDIR}/mnt"
OTHER_MNT="${TMPDIR}/other_mnt"
LOOPDEV=""
MOUNTED=0
OTHER_MOUNTED=0

cleanup() {
  if [[ "${OTHER_MOUNTED}" -eq 1 ]]; then
    umount "${OTHER_MNT}" || true
  fi
  if [[ "${MOUNTED}" -eq 1 ]]; then
    umount "${MNT}" || true
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

assert_ne() {
  local label="$1"
  local before="$2"
  local after="$3"
  if [[ "${before}" == "${after}" ]]; then
    echo "FAIL: ${label} should change, but did not" >&2
    exit 1
  fi
  echo "PASS: ${label} changed"
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

get_crtime_line() {
  local ino="$1"
  debugfs -R "stat <${ino}>" "${LOOPDEV}" 2>/dev/null | awk '/crtime:/ {print; exit}'
}

echo "[4/11] Creating loopback ext4 test filesystem"
dd if=/dev/zero of="${IMG}" bs=1M count=64 status=none
mkfs.ext4 -F -q "${IMG}"
LOOPDEV="$(losetup --find --show "${IMG}")"
mkdir -p "${MNT}"
mount -t ext4 -o noatime,nodiratime "${LOOPDEV}" "${MNT}"
MOUNTED=1

echo "[5/11] Creating target and donor files"
echo "atime-target"  > "${MNT}/atime.txt"
echo "mtime-target"  > "${MNT}/mtime.txt"
echo "ctime-target"  > "${MNT}/ctime.txt"
echo "crtime-target" > "${MNT}/crtime.txt"
echo "donor-file"    > "${MNT}/donor.txt"
echo "recv-file"     > "${MNT}/receiver.txt"
sync

atime_ino="$(stat -c '%i' "${MNT}/atime.txt")"
mtime_ino="$(stat -c '%i' "${MNT}/mtime.txt")"
ctime_ino="$(stat -c '%i' "${MNT}/ctime.txt")"
crtime_ino="$(stat -c '%i' "${MNT}/crtime.txt")"
donor_ino="$(stat -c '%i' "${MNT}/donor.txt")"
recv_ino="$(stat -c '%i' "${MNT}/receiver.txt")"

echo "    atime inode:    ${atime_ino}"
echo "    mtime inode:    ${mtime_ino}"
echo "    ctime inode:    ${ctime_ino}"
echo "    crtime inode:   ${crtime_ino}"
echo "    donor inode:    ${donor_ino}"
echo "    receiver inode: ${recv_ino}"

echo "[6/11] Individual atime test (only atime changes)"
ATIME_SET=1609459201
read -r a0 m0 c0 < <(stat -c '%X %Y %Z' "${MNT}/atime.txt")
cr0="$(get_crtime_line "${atime_ino}")"
./ext4ts_ctl --device "${LOOPDEV}" --inode "${atime_ino}" --atime "${ATIME_SET}" --nsec 696969696
sync
read -r a1 m1 c1 < <(stat -c '%X %Y %Z' "${MNT}/atime.txt")
cr1="$(get_crtime_line "${atime_ino}")"
assert_eq "atime target value" "${ATIME_SET}" "${a1}"
assert_eq "atime test mtime unchanged" "${m0}" "${m1}"
assert_eq "atime test ctime unchanged" "${c0}" "${c1}"
assert_eq "atime test crtime unchanged" "${cr0}" "${cr1}"

echo "[7/11] Individual mtime test (only mtime changes)"
MTIME_SET=1609459202
read -r a0 m0 c0 < <(stat -c '%X %Y %Z' "${MNT}/mtime.txt")
cr0="$(get_crtime_line "${mtime_ino}")"
./ext4ts_ctl --device "${LOOPDEV}" --inode "${mtime_ino}" --mtime "${MTIME_SET}" --nsec 696969696
sync
read -r a1 m1 c1 < <(stat -c '%X %Y %Z' "${MNT}/mtime.txt")
cr1="$(get_crtime_line "${mtime_ino}")"
assert_eq "mtime test atime unchanged" "${a0}" "${a1}"
assert_eq "mtime target value" "${MTIME_SET}" "${m1}"
assert_eq "mtime test ctime unchanged" "${c0}" "${c1}"
assert_eq "mtime test crtime unchanged" "${cr0}" "${cr1}"

echo "[8/11] Individual ctime/crtime tests"
CTIME_SET=1609459203
read -r a0 m0 c0 < <(stat -c '%X %Y %Z' "${MNT}/ctime.txt")
cr0="$(get_crtime_line "${ctime_ino}")"
./ext4ts_ctl --device "${LOOPDEV}" --inode "${ctime_ino}" --ctime "${CTIME_SET}" --nsec 696969696
sync
read -r a1 m1 c1 < <(stat -c '%X %Y %Z' "${MNT}/ctime.txt")
cr1="$(get_crtime_line "${ctime_ino}")"
assert_eq "ctime test atime unchanged" "${a0}" "${a1}"
assert_eq "ctime test mtime unchanged" "${m0}" "${m1}"
assert_eq "ctime target value" "${CTIME_SET}" "${c1}"
assert_eq "ctime test crtime unchanged" "${cr0}" "${cr1}"

CRTIME_SET=1609459204
read -r a0 m0 c0 < <(stat -c '%X %Y %Z' "${MNT}/crtime.txt")
cr0="$(get_crtime_line "${crtime_ino}")"
b0="$(stat -c '%W' "${MNT}/crtime.txt")"
./ext4ts_ctl --device "${LOOPDEV}" --inode "${crtime_ino}" --crtime "${CRTIME_SET}" --nsec 696969696
sync
read -r a1 m1 c1 < <(stat -c '%X %Y %Z' "${MNT}/crtime.txt")
cr1="$(get_crtime_line "${crtime_ino}")"
b1="$(stat -c '%W' "${MNT}/crtime.txt")"
assert_eq "crtime test atime unchanged" "${a0}" "${a1}"
assert_eq "crtime test mtime unchanged" "${m0}" "${m1}"
assert_eq "crtime test ctime unchanged" "${c0}" "${c1}"
if [[ -n "${cr0}" && -n "${cr1}" ]]; then
  assert_ne "crtime line" "${cr0}" "${cr1}"
else
  echo "WARN: debugfs did not return crtime line; skipping direct crtime line comparison"
fi
if [[ "${b1}" != "-1" ]]; then
  assert_eq "crtime target value (stat %W)" "${CRTIME_SET}" "${b1}"
else
  echo "WARN: stat %W unsupported on this mount/kernel; relied on debugfs crtime change check"
fi

echo "[9/11] Donor mode test (copy all timestamps)"
DONOR_A=1609459301
DONOR_M=1609459302
DONOR_C=1609459303
DONOR_CR=1609459304
./ext4ts_ctl --device "${LOOPDEV}" --inode "${donor_ino}" --atime "${DONOR_A}" --nsec 111111111
./ext4ts_ctl --device "${LOOPDEV}" --inode "${donor_ino}" --mtime "${DONOR_M}" --nsec 222222222
./ext4ts_ctl --device "${LOOPDEV}" --inode "${donor_ino}" --ctime "${DONOR_C}" --nsec 333333333
./ext4ts_ctl --device "${LOOPDEV}" --inode "${donor_ino}" --crtime "${DONOR_CR}" --nsec 444444444
sync

read -r da dm dc < <(stat -c '%X %Y %Z' "${MNT}/donor.txt")
dcr="$(get_crtime_line "${donor_ino}")"
db="$(stat -c '%W' "${MNT}/donor.txt")"
./ext4ts_ctl --device "${LOOPDEV}" --inode "${recv_ino}" --donor "${donor_ino}"
sync
read -r ra rm rc < <(stat -c '%X %Y %Z' "${MNT}/receiver.txt")
rcr="$(get_crtime_line "${recv_ino}")"
rb="$(stat -c '%W' "${MNT}/receiver.txt")"
assert_eq "donor copy atime" "${da}" "${ra}"
assert_eq "donor copy mtime" "${dm}" "${rm}"
assert_eq "donor copy ctime" "${dc}" "${rc}"
if [[ -n "${dcr}" && -n "${rcr}" ]]; then
  assert_eq "donor copy crtime (debugfs)" "${dcr}" "${rcr}"
else
  echo "WARN: debugfs crtime unavailable; skipping donor crtime comparison"
fi
if [[ "${db}" != "-1" && "${rb}" != "-1" ]]; then
  assert_eq "donor copy crtime (stat %W)" "${db}" "${rb}"
else
  echo "WARN: stat %W unsupported; relied on debugfs donor crtime comparison"
fi

echo "[10/11] Negative test: invalid inode should fail"
assert_fail "invalid inode" \
  ./ext4ts_ctl --device "${LOOPDEV}" --inode 999999999 --atime 1609459401 --nsec 1

echo "[11/11] Negative test: non-ext4 device should fail"
mkdir -p "${OTHER_MNT}"
mount -t tmpfs -o size=8M tmpfs "${OTHER_MNT}"
OTHER_MOUNTED=1
assert_fail "non-ext4 mountpoint device" \
  ./ext4ts_ctl --device "${OTHER_MNT}" --inode "${recv_ino}" --atime 1609459402 --nsec 2

echo "All ext4 timestamp tests passed."
