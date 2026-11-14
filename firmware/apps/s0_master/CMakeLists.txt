# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

# Board definitions live in firmware/boards.
list(APPEND BOARD_ROOT ${CMAKE_CURRENT_LIST_DIR}/../..)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(s0_master LANGUAGES C CXX)

target_sources(app PRIVATE
  src/link/link.cpp
  src/main.cpp
  src/meter/hub.cpp
  src/meter/iec62056.cpp
  src/meter/sml.cpp
  src/network/captive_portal.cpp
  src/network/mqtt.cpp
  src/network/wifi.cpp
  src/system/config.cpp
  src/system/glue.c
  src/web/api.cpp
  src/web/web.cpp
)
# Sources include each other relative to src/, e.g. "meter/hub.hpp".
target_include_directories(app PRIVATE src ../../lib/irlink/include)

# Zephyr's Kconfig stops at C++23; the application itself is C++26.
target_compile_options(app PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-std=c++26>)

# The HTTP server finds its resources in an iterable linker section that is
# named after the service declared in src/system/glue.c.
zephyr_linker_sources(SECTIONS sections-rom.ld)
zephyr_linker_section(NAME http_resource_desc_s0_http KVMA RAM_REGION GROUP RODATA_REGION)

# The dashboard is a single page, gzip-compressed into the firmware image.
generate_inc_file_for_target(app
  dashboard/index.html
  ${ZEPHYR_BINARY_DIR}/include/generated/index.html.gz.inc
  --gzip
)
