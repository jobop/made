// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <string>
#include "esp_err.h"

namespace vibe_phone_setup {

enum class Phase { Idle, Ready, Submitted, Error, Expired };

struct InitialConfig {
    std::string mode = "automatic", wifi_ssid, receiver_ssid, host;
    uint16_t port = 443;
    bool https = true;
};

struct Submission {
    std::string mode, wifi_ssid, wifi_password, receiver_ssid, receiver_password, host;
    uint16_t port = 443;
    bool https = true;
};

struct Snapshot {
    Phase phase = Phase::Idle;
    std::string ssid, password, url, qr_payload, message;
    uint32_t revision = 0;
};

// Lifecycle/consumption run on the application's worker, never the GUI thread.
// HTTP only validates and queues; the worker stops this portal before applying.
esp_err_t start(bool english, const InitialConfig &initial);
void stop();
Snapshot snapshot();
bool take_submission(Submission &out);
void tick();

} // namespace vibe_phone_setup
