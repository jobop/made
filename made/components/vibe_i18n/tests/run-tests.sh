#!/usr/bin/env bash
set -euo pipefail
test_dir=$(cd "$(dirname "$0")" && pwd)
out=$(mktemp -d "${TMPDIR:-/tmp}/made-i18n.XXXXXX")
trap 'rm -rf "$out"' EXIT
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -pthread -I"$test_dir/stubs" -I"$test_dir/../include" \
  "$test_dir/../vibe_i18n.cpp" "$test_dir/i18n_test.cpp" -o "$out/test"
"$out/test"
"$out/test" en
"$out/test" zh-CN
"$out/test" garbage
"$out/test" too-long-invalid-value
"$out/test" en 0 zh-CN
"$out/test" zh-CN 0 en
"$out/test" en 1 en
"$out/test" zh-CN garbage
