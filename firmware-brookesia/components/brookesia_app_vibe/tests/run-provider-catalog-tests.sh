#!/usr/bin/env bash
set -euo pipefail
# Pass the directory containing the real cJSON.c and cJSON.h files.
cjson_dir=${1:?Usage: run-provider-catalog-tests.sh /path/to/cJSON}
test_dir=$(cd "$(dirname "$0")" && pwd)
output_dir=$(mktemp -d "${TMPDIR:-/tmp}/vibe-provider-test.XXXXXX")
trap 'rm -rf "$output_dir"' EXIT
"${CC:-cc}" -std=c11 -I"$cjson_dir" -c "$cjson_dir/cJSON.c" -o "$output_dir/cJSON.o"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -I"$test_dir/.." -I"$cjson_dir" \
    "$test_dir/provider_catalog_test.cpp" "$output_dir/cJSON.o" -o "$output_dir/provider-test"
"$output_dir/provider-test"

"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -I"$test_dir/.." -I"$cjson_dir" \
    "$test_dir/provider_icon_test.cpp" "$output_dir/cJSON.o" -o "$output_dir/icon-test"
if [[ $# -ge 2 ]]; then "$output_dir/icon-test" "$2"; else "$output_dir/icon-test"; fi
