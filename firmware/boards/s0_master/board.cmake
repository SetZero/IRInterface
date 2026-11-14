# SPDX-License-Identifier: Apache-2.0

# The USB-C connector is power only and there is no JTAG header, so the
# board is flashed with esptool over UART0 on the 4-pin DEBUG header.
# EN and BOOT are not wired to the header: hold BOOT, tap EN, release
# BOOT before flashing.
include(${ZEPHYR_BASE}/boards/common/esp32.board.cmake)
