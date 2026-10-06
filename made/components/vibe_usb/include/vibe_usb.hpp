#pragma once

#include <cstddef>
#include <cstdint>
#include <atomic>
#include <string>

#include "esp_err.h"

namespace vibe_usb {

// A local transport selector. This hostname must never be resolved by DNS.
inline constexpr const char* kBaseUrl = "http://usb.vibe.local:8788";
// Receiver hotspot. The board speaks the same frames as USB direct; the receiver only moves bytes.
inline constexpr const char* kReceiverUrl = "http://192.168.4.1:8788";
bool is_direct_url(const std::string& url);
bool is_receiver_url(const std::string& url);

esp_err_t initialize();
void set_active(bool enabled);
bool active();
bool connected();
uint32_t connection_epoch();
// Called only after the desktop's HMAC proof or explicit six-digit approval.
// Bind that verified Bearer and epoch; request() checks both under its serial lock.
bool authorize_connection(uint32_t expected_epoch, const std::string& authorization);
void revoke_authorization();

// Nonblocking: the current operation notices cancellation within one I/O slice,
// sends a cancel frame and releases the serial request lock. Future requests work.
void cancel_current();

// Caller must revalidate pairing when connection_epoch() changes. Every request
// also handshakes and refuses to emit Authorization if its connection changes.
esp_err_t request(const std::string& path, bool post,
                  const std::string& authorization, const char* content_type,
                  const uint8_t* body, size_t length, std::string& response,
                  int& status, size_t response_limit = 8192, int timeout_ms = 30000,
                  const std::atomic<bool>* cancellation_flag = nullptr);

// Same frames as request(), over TCP to the receiver. Does not send a direct-mode hello:
// the receiver already introduced this USB link to the computer.
esp_err_t receiver_request(const std::string& path, bool post,
                           const std::string& authorization, const char* content_type,
                           const uint8_t* body, size_t length, std::string& response,
                           int& status, size_t response_limit = 8192, int timeout_ms = 30000,
                           const std::atomic<bool>* cancellation_flag = nullptr);

}  // namespace vibe_usb
