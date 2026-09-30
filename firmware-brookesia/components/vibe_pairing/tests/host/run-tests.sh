#!/usr/bin/env bash
set -euo pipefail
test_dir="$(cd "$(dirname "$0")" && pwd)"
cjson_dir="${1:-${CJSON_DIR:-}}"
if [[ ! -f "$cjson_dir/cJSON.c" || ! -f "$cjson_dir/cJSON.h" ]]; then
  echo "Usage: $0 /path/to/espressif__cjson/cJSON" >&2
  exit 2
fi
build_dir="$(mktemp -d "${TMPDIR:-/tmp}/vibe-pairing-tests.XXXXXX")"
trap 'rm -rf "$build_dir"' EXIT
"${CC:-cc}" -std=c11 -c "$cjson_dir/cJSON.c" -o "$build_dir/cJSON.o"
for test in usb_regression_test chooser_test; do
  compile_command=("${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -Wno-deprecated-declarations -pthread
    -I"$test_dir/stubs" -I"$test_dir/../../include" -I"$cjson_dir" -I"$test_dir/../../../vibe_i18n/include"
    "$test_dir/$test.cpp" "$test_dir/../../../vibe_i18n/vibe_i18n.cpp" "$build_dir/cJSON.o" -o "$build_dir/$test")
  if [[ "$(uname -s)" != Darwin ]]; then compile_command+=(-lcrypto); fi
  "${compile_command[@]}"
  "$build_dir/$test"
done
