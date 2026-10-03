#!/usr/bin/env bash
set -euo pipefail

# Pinned 32-bit Raspberry Pi OS Lite image for the original Pi Zero.
# Run only on an ephemeral Linux CI runner with sudo, loop devices, and QEMU.
image_url="https://downloads.raspberrypi.com/raspios_lite_armhf/images/raspios_lite_armhf-2026-09-15/2026-09-15-raspios-trixie-armhf-lite.img.xz"
image_sha256="c766b3fb279b95c12cb4dd22d06f8eab31972c372675d05bd0ca95b060523a7f"
work_dir="$(mktemp -d)"
root_fs="$work_dir/root"
loop_device=""

cleanup() {
  for mount_point in "$root_fs/work" "$root_fs/proc" "$root_fs/dev" "$root_fs"; do
    if mountpoint -q "$mount_point"; then sudo umount "$mount_point"; fi
  done
  if [[ -n "$loop_device" ]]; then sudo losetup -d "$loop_device"; fi
  sudo rm -rf "$work_dir"
}
trap cleanup EXIT

mkdir -p "$root_fs"
curl --fail --location --retry 3 "$image_url" -o "$work_dir/pi-os.img.xz"
printf '%s  %s\n' "$image_sha256" "$work_dir/pi-os.img.xz" | sha256sum --check -
xz -dc "$work_dir/pi-os.img.xz" > "$work_dir/pi-os.img"
rm "$work_dir/pi-os.img.xz"

# Allow package installation without depending on spare space in the stock image.
truncate -s +2G "$work_dir/pi-os.img"
sudo parted -s "$work_dir/pi-os.img" resizepart 2 100%
loop_device="$(sudo losetup --find --partscan --show "$work_dir/pi-os.img")"
sudo udevadm settle
sudo e2fsck -fy "${loop_device}p2" || [[ $? -eq 1 ]]
sudo resize2fs "${loop_device}p2"
sudo mount "${loop_device}p2" "$root_fs"

# This is an ephemeral copy of a verified OS image. Bind only the checkout and
# essential pseudo-filesystems; no production secret enters the guest.
sudo mkdir -p "$root_fs/work"
sudo mount --bind "$PWD" "$root_fs/work"
sudo mount --bind /dev "$root_fs/dev"
sudo mount -t proc proc "$root_fs/proc"
sudo rm -f "$root_fs/etc/resolv.conf"
sudo cp /etc/resolv.conf "$root_fs/etc/resolv.conf"

sudo chroot "$root_fs" /bin/sh -ec '
  export DEBIAN_FRONTEND=noninteractive
  apt-get update
  apt-get install -y --no-install-recommends build-essential pkg-config file libsdl2-dev libsdl2-ttf-dev fonts-dejavu-core
  make -C /work/pi test CXX=g++
  make -C /work/pi demo CXX=g++
  file /work/pi/build/pi-card
  readelf -l /work/pi/build/pi-card | grep -F "/lib/ld-linux-armhf.so.3"
  SDL_VIDEODRIVER=dummy /work/pi/build/pi-card --width 320 --height 240 --once /work/pi-raspios-320.bmp
  SDL_VIDEODRIVER=dummy /work/pi/build/pi-card --width 1280 --height 720 --once /work/pi-raspios-1280.bmp
'

python3 - <<'PY'
import struct
for name, expected in [('pi-raspios-320.bmp', (320, 240)),
                       ('pi-raspios-1280.bmp', (1280, 720))]:
    with open(name, 'rb') as frame:
        data = frame.read()
    assert data[:2] == b'BM'
    assert struct.unpack_from('<ii', data, 18) == expected
    assert len(set(data[54:])) > 16
    print(f'{name}: {expected[0]}x{expected[1]}, {len(data)} bytes')
PY
