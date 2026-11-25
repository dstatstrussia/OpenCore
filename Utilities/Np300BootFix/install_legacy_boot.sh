#!/bin/bash

#
# Install the OpenDuet legacy boot chain on a GPT disk so that a BIOS/CSM
# boot entry can start OpenCore in addition to the regular UEFI entry.
#
# The disk keeps its GPT layout: boot0 is GPT aware and locates the EFI
# system partition by its type GUID, therefore no hybrid MBR is created.
#
# Usage: sudo ./install_legacy_boot.sh disk6
#

set -euo pipefail

if [ "$(uname)" != "Darwin" ]; then
  echo "This script supports macOS only." >&2
  exit 1
fi

if [ "$EUID" -ne 0 ]; then
  echo "Please run this script as root." >&2
  exit 1
fi

if [ $# -ne 1 ]; then
  echo "Usage: $0 diskN" >&2
  echo "Example: $0 disk6" >&2
  exit 1
fi

DISK="${1#/dev/}"
DISK="${DISK#r}"

case "${DISK}" in
  disk[0-9]*) ;;
  *)
    echo "Invalid disk name: $1" >&2
    exit 1
    ;;
esac

SELF_DIR="$(cd "$(dirname "$0")" && pwd)"
LEGACY_DIR="${SELF_DIR}/../LegacyBoot"

BOOT0="${LEGACY_DIR}/boot0"
BOOT1="${LEGACY_DIR}/boot1f32"
BOOTX="${LEGACY_DIR}/bootX64"

for f in "${BOOT0}" "${BOOT1}" "${BOOTX}"; do
  if [ ! -f "${f}" ]; then
    echo "Missing ${f}, build the legacy boot files first." >&2
    exit 1
  fi
done

if ! diskutil info "${DISK}" >/dev/null 2>&1; then
  echo "Disk ${DISK} not found." >&2
  exit 1
fi

ESP_ID="${DISK}s1"
if ! diskutil info "${ESP_ID}" 2>/dev/null | grep -q -e 'EFI' -e 'FAT_32'; then
  echo "${ESP_ID} is not an EFI system partition." >&2
  exit 1
fi

RDEV="/dev/r${DISK}"
RESP="/dev/r${ESP_ID}"

echo "Installing legacy boot chain on ${DISK} (ESP ${ESP_ID})"

TMP_DIR="$(mktemp -d)"
trap 'rm -rf "${TMP_DIR}"' EXIT

diskutil unmountDisk "${DISK}" >/dev/null

#
# Keep the original MBR and VBR around for manual recovery.
#
dd if="${RDEV}" bs=512 count=1 of="${TMP_DIR}/mbr.orig" 2>/dev/null
dd if="${RESP}" bs=512 count=1 of="${TMP_DIR}/pbr.orig" 2>/dev/null
echo "Originals saved to ${TMP_DIR}"

#
# boot0 goes into the MBR boot code area only, the partition table stays.
#
dd if="${BOOT0}" of="${RDEV}" bs=1 count=446 conv=notrunc

#
# boot1f32 goes into the ESP VBR, keeping the FAT32 BPB (bytes 3..89) and
# the 0xAA55 signature, randomising the remaining reserved bytes.
#
cp "${BOOT1}" "${TMP_DIR}/pbr.new"
dd if="${TMP_DIR}/pbr.orig" of="${TMP_DIR}/pbr.new" bs=1 skip=3 seek=3 count=87 conv=notrunc
dd if=/dev/urandom of="${TMP_DIR}/pbr.new" bs=1 skip=496 seek=496 count=14 conv=notrunc
dd if="${TMP_DIR}/pbr.new" of="${RESP}" bs=512 count=1

sync

diskutil mount "${ESP_ID}" >/dev/null
MOUNT_POINT="$(diskutil info "${ESP_ID}" | sed -n 's/.*Mount Point: *//p')"

if [ -z "${MOUNT_POINT}" ] || [ ! -d "${MOUNT_POINT}" ]; then
  echo "Failed to mount ${ESP_ID}." >&2
  exit 1
fi

cp "${BOOTX}" "${MOUNT_POINT}/boot"

echo "Done: ${MOUNT_POINT}/boot"
echo "Legacy and UEFI boot entries should now both reach OpenCore."
