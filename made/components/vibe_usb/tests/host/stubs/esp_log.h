#pragma once
// Keep log arguments type-checked without writing diagnostics during host tests.
#include <cstdio>
#define ESP_LOGW(tag, ...) do { (void)(tag); if (false) std::fprintf(stderr, __VA_ARGS__); } while (0)
#define ESP_LOGD(tag, ...) do { (void)(tag); if (false) std::fprintf(stderr, __VA_ARGS__); } while (0)
