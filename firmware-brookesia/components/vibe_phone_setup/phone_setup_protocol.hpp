// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "vibe_phone_setup.hpp"
#include <cstddef>

namespace vibe_phone_setup::detail {
constexpr size_t kMaxBody = 2048;
constexpr int64_t kSessionUs = 10LL * 60 * 1000000;
constexpr int64_t kResponseGraceUs = 1000000;

enum class ValidationError { None, InvalidJson, UnknownField, InvalidNonce, InvalidMode,
    InvalidWifi, InvalidReceiver, InvalidHost, InvalidPort, PublicHttp };

// Never include credentials in errors. Only these whitelisted fields are parsed.
ValidationError parse_submission(const std::string &body, const std::string &nonce,
                                const InitialConfig &initial, Submission &out);
const char *error_message(ValidationError error, bool english);
bool json_content_type(const std::string &value);
std::string json_for_script(const std::string &value);
std::string render_page(bool english, const InitialConfig &initial, const std::string &nonce);
std::string qr_escape(const std::string &value);
// Bounded DNS query parser: only single, uncompressed IN questions are answered.
size_t dns_reply(const uint8_t *query, size_t length, uint8_t *out, size_t capacity);
} // namespace vibe_phone_setup::detail
