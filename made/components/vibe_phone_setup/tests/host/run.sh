#!/usr/bin/env bash
set -euo pipefail
test_dir="$(cd "$(dirname "$0")" && pwd)"
cjson_dir="${1:-${CJSON_DIR:-}}"
if [[ ! -f "$cjson_dir/cJSON.c" || ! -f "$cjson_dir/cJSON.h" ]]; then
  echo "Usage: $0 /path/to/cJSON [preview.html] [en]" >&2
  exit 2
fi
build_dir="$(mktemp -d "${TMPDIR:-/tmp}/made-phone-tests.XXXXXX")"
trap 'rm -rf "$build_dir"' EXIT
"${CC:-cc}" -std=c11 -c "$cjson_dir/cJSON.c" -o "$build_dir/cJSON.o"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -I"$test_dir/stubs" -I"$test_dir/../../include" \
  -I"$test_dir/../.." -I"$cjson_dir" "$test_dir/protocol_test.cpp" \
  "$test_dir/../../phone_setup_protocol.cpp" "$build_dir/cJSON.o" -o "$build_dir/protocol_test"
"$build_dir/protocol_test" "${2:-/dev/null}" "${3:-zh}"
