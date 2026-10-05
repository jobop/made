#!/usr/bin/env bash
set -euo pipefail
test_dir="$(cd "$(dirname "$0")" && pwd)"
cjson_dir="${1:-${CJSON_DIR:-}}"
if [[ ! -f "$cjson_dir/cJSON.c" || ! -f "$cjson_dir/cJSON.h" ]]; then
  echo "Usage: $0 /path/to/espressif__cjson/cJSON" >&2
  exit 2
fi
build_dir="$(mktemp -d "${TMPDIR:-/tmp}/vibe-usb-tests.XXXXXX")"
trap 'rm -rf "$build_dir"' EXIT
"${CC:-cc}" -std=c11 -c "$cjson_dir/cJSON.c" -o "$build_dir/cJSON.o"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -pthread \
  -I"$test_dir/stubs" -I"$test_dir/../../include" -I"$cjson_dir" \
  "$test_dir/transport_test.cpp" "$build_dir/cJSON.o" -o "$build_dir/transport_test"
"$build_dir/transport_test"
