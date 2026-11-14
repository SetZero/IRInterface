#!/usr/bin/env bash
#
# Check the firmware sources against .clang-format and .clang-tidy.
#
#   firmware/tools/lint.sh          report layout and naming violations
#   firmware/tools/lint.sh --fix    reformat in place, then report naming violations
#
# The naming check reads the compile commands of the two application builds,
# so run build.sh for ir_head and s0_master first. Needs clang-format and
# clang-tidy (LLVM 18 or newer) on the PATH or in CLANG_BIN.

set -uo pipefail

firmware="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_root="${BUILD_ROOT:-$firmware/../../build}"
clang_bin="${CLANG_BIN:+$CLANG_BIN/}"
failed=0

mapfile -t sources < <(find "$firmware/apps" "$firmware/lib" "$firmware/tests" \
	-type f \( -name '*.cpp' -o -name '*.hpp' -o -name '*.c' -o -name '*.h' \) 2>/dev/null | sort)

if [ "${1:-}" = "--fix" ]; then
	"${clang_bin}clang-format" -i "${sources[@]}"
else
	"${clang_bin}clang-format" --dry-run --Werror "${sources[@]}" || failed=1
fi

# clang needs to be told the target the compile commands were written for
declare -A targets=([ir_head]=arm-none-eabi [s0_master]=xtensa-esp-elf)

for app in ir_head s0_master; do
	database="$build_root/$app"

	if [ ! -f "$database/compile_commands.json" ]; then
		echo "lint: no build in $database, run build.sh $app first" >&2
		failed=1
		continue
	fi

	mapfile -t units < <(find "$firmware/apps/$app/src" -type f \( -name '*.cpp' -o -name '*.c' \) | sort)

	# GCC knows where its own standard library lives; clang has to be told.
	compiler="$(grep -o -m1 -E '[^ "]*-g\+\+(\.exe)?' "$database/compile_commands.json" |
		head -1 | tr -s '\\' '/')"
	system_includes=()
	while IFS= read -r directory; do
		system_includes+=("--extra-arg=-isystem${directory# }")
	done < <("$compiler" -xc++ -E -Wp,-v - </dev/null 2>&1 | tr -d '\015' |
		sed -n '/#include <...> search starts here:/,/End of search list/p' | sed '1d;$d')

	# The cross compilers are GCC, so clang rejects a few of their flags; those
	# complaints are dropped by keeping only the two checks' own findings.
	# MSYS2_ARG_CONV_EXCL stops Git Bash on Windows from rewriting the paths
	# inside the -isystem arguments; it has no effect elsewhere.
	report="$(MSYS2_ARG_CONV_EXCL='--extra-arg=' "${clang_bin}clang-tidy" -p "$database" --quiet \
		--config-file="$firmware/.clang-tidy" \
		--extra-arg=--target="${targets[$app]}" \
		--extra-arg=-Wno-everything "${system_includes[@]}" "${units[@]}" 2>/dev/null |
		grep -E '\[readability-identifier-(naming|length)\]' | sort -u)"

	if [ -n "$report" ]; then
		echo "$report"
		failed=1
	fi
done

[ "$failed" -eq 0 ] && echo "lint: clean"
exit "$failed"
