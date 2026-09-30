// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#ifdef ESP_PLATFORM
#include "lwip/sockets.h"
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#endif

namespace vibe_phone_setup::detail {
// esp_http_server listens on an IPv6 dual-stack socket when LWIP_IPV6 is on.
// lwIP getsockname then returns IPv4 destinations as ::ffff:a.b.c.d. Accept
// only the temporary AP destination in either representation, never native
// IPv6 or another STA address. Check lengths before inspecting either shape.
inline bool is_setup_address(const sockaddr *address, socklen_t length) {
    if (!address || length < offsetof(sockaddr, sa_family) + sizeof(address->sa_family)) return false;
    if (address->sa_family == AF_INET) {
        if (length < sizeof(sockaddr_in)) return false;
        sockaddr_in ipv4{};
        std::memcpy(&ipv4, address, sizeof(ipv4));
        return ntohl(ipv4.sin_addr.s_addr) == 0xc0a80801UL;
    }
#if !defined(ESP_PLATFORM) || CONFIG_LWIP_IPV6
    if (address->sa_family == AF_INET6) {
        if (length < sizeof(sockaddr_in6)) return false;
        sockaddr_in6 ipv6{};
        std::memcpy(&ipv6, address, sizeof(ipv6));
        static constexpr uint8_t mapped_ap[16] = {
            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 192, 168, 8, 1
        };
        return std::memcmp(&ipv6.sin6_addr, mapped_ap, sizeof(mapped_ap)) == 0;
    }
#endif
    return false;
}
} // namespace vibe_phone_setup::detail
