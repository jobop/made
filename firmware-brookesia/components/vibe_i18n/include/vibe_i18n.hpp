// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>
#include <string>
namespace vibe_i18n {
enum class Locale : uint8_t { Chinese, English };
bool parse_locale(const char *value, Locale &result);
void initialize();
// An explicit on-device choice remains authoritative until follow mode is selected.
bool set_locale(const char *value);
bool set_follow_bridge();
bool follows_bridge();
bool apply_bridge_locale(const char *value);
std::string request_path(const std::string &path);
Locale locale();
const char *locale_name();
uint32_t revision();
bool known(const char *key);
const char *tr(const char *key);
// Only for application-owned notices. Never pass user/plugin/conversation text.
std::string transport_error(const std::string &value);
std::string message(const std::string &value);
}
