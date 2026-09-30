#!/usr/bin/env bash
set -euo pipefail
task_dir="$(cd "$(dirname "$0")" && pwd)"
task_build="$(mktemp -d)"
trap 'rm -rf "$task_build"' EXIT
for header in esp_err.h esp_event.h esp_log.h esp_netif.h esp_timer.h esp_wifi.h esp_wifi_default.h esp_random.h nvs.h sdkconfig.h; do
  printf '#include "fake_idf.hpp"\n' > "$task_build/$header"
done
for test in receiver_scan_test setup_ap_test; do
  "${CXX:-c++}" -std=c++17 -pthread -fno-exceptions -Wall -Wextra \
    -I"$task_dir/stubs" -I"$task_build" -I"$task_dir/../../include" \
    "$task_dir/../../vibe_wifi.cpp" "$task_dir/$test.cpp" -o "$task_build/$test"
  "$task_build/$test"
done
