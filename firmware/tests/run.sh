#!/usr/bin/env bash
#
# Build and run the host tests: the decoders and the link framing, compiled
# for the PC. Needs a C++26 compiler; set CXX to choose one (default: clang++).
#
#   firmware/tests/run.sh

set -euo pipefail

tests="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
firmware="$tests/.."
build="${BUILD_ROOT:-$firmware/../../build}/tests"
cxx="${CXX:-clang++}"

mkdir -p "$build"

"$cxx" -std=c++26 -O1 -g -Wall -Wextra \
	-I "$firmware/apps/s0_master/src" \
	-I "$firmware/lib/irlink/include" \
	"$tests/decoders_test.cpp" \
	"$firmware/apps/s0_master/src/meter/sml.cpp" \
	"$firmware/apps/s0_master/src/meter/iec62056.cpp" \
	-o "$build/decoders_test"

"$build/decoders_test"
