# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

# Board definitions live in firmware/boards.
list(APPEND BOARD_ROOT ${CMAKE_CURRENT_LIST_DIR}/../..)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ir_head LANGUAGES C CXX)

target_sources(app PRIVATE src/main.cpp)
target_include_directories(app PRIVATE ../../lib/irlink/include)

# Zephyr's Kconfig stops at C++23; the application itself is C++26.
target_compile_options(app PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-std=c++26>)
