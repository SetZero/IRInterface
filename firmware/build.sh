#!/usr/bin/env bash
#
# Build one of the firmware images.
#
#   firmware/build.sh ir_head       IR head application   (board: irinterface)
#   firmware/build.sh s0_master     master application    (board: s0_master)
#   firmware/build.sh bringup <board>   hardware bring-up sample
#
# Needs in the environment:
#   ZEPHYR_BASE             the zephyr directory of a west workspace (4.4 or newer)
#   ZEPHYR_SDK_INSTALL_DIR  Zephyr SDK 1.0 or newer with the arm-zephyr-eabi and
#                           xtensa-espressif_esp32s3_zephyr-elf toolchains
# and esptool on the PATH for the ESP32 image.
#
# Output goes to $BUILD_ROOT/<name> (default: build/ next to the repository).

set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
build_root="${BUILD_ROOT:-$here/../../build}"

: "${ZEPHYR_BASE:?set ZEPHYR_BASE to the zephyr directory of your west workspace}"
: "${ZEPHYR_SDK_INSTALL_DIR:?set ZEPHYR_SDK_INSTALL_DIR to a Zephyr SDK 1.0+ installation}"

export ZEPHYR_TOOLCHAIN_VARIANT=zephyr
unset CROSS_COMPILE

case "${1:-}" in
ir_head)
	source_dir="$here/apps/ir_head"
	board=irinterface
	name=ir_head
	;;
s0_master)
	source_dir="$here/apps/s0_master"
	board=s0_master/esp32s3/procpu
	name=s0_master
	;;
bringup)
	source_dir="$here/samples/bringup"
	board="${2:?usage: build.sh bringup <irinterface|s0_master/esp32s3/procpu>}"
	name="bringup_${board%%/*}"
	;;
*)
	echo "usage: $0 <ir_head|s0_master|bringup <board>>" >&2
	exit 2
	;;
esac

cmake -GNinja -B "$build_root/$name" -S "$source_dir" -DBOARD="$board"
ninja -C "$build_root/$name"
