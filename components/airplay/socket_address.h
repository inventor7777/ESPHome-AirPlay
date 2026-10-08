#pragma once

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <string>
#include <vector>

namespace esphome::airplay {

inline bool configure_keepalive(int sock) {
  const int enabled = 1, idle = 30, interval = 10, probes = 3;
#if defined(__APPLE__) && !defined(ESP_PLATFORM)
  constexpr int idle_option = TCP_KEEPALIVE;
#else
  constexpr int idle_option = TCP_KEEPIDLE;
#endif
  return setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &enabled, sizeof(enabled)) == 0 &&
         setsockopt(sock, IPPROTO_TCP, idle_option, &idle, sizeof(idle)) == 0 &&
         setsockopt(sock, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof(interval)) == 0 &&
         setsockopt(sock, IPPROTO_TCP, TCP_KEEPCNT, &probes, sizeof(probes)) == 0;
}

inline socklen_t address_length(const sockaddr_storage &address) {
  if (address.ss_family == AF_INET) return sizeof(sockaddr_in);
#if !defined(ESP_PLATFORM) || LWIP_IPV6
  if (address.ss_family == AF_INET6) return sizeof(sockaddr_in6);
#endif
  return 0;
}

inline const uint8_t *address_bytes(const sockaddr_storage &address, size_t &size) {
  if (address.ss_family == AF_INET) {
    size = 4;
    return reinterpret_cast<const uint8_t *>(&reinterpret_cast<const sockaddr_in &>(address).sin_addr);
  }
#if !defined(ESP_PLATFORM) || LWIP_IPV6
  if (address.ss_family == AF_INET6) {
    size = 16;
    return reinterpret_cast<const sockaddr_in6 &>(address).sin6_addr.s6_addr;
  }
#endif
  size = 0;
  return nullptr;
}

inline uint16_t address_port(const sockaddr_storage &address) {
#if !defined(ESP_PLATFORM) || LWIP_IPV6
  if (address.ss_family == AF_INET6) return ntohs(reinterpret_cast<const sockaddr_in6 &>(address).sin6_port);
#endif
  return ntohs(reinterpret_cast<const sockaddr_in &>(address).sin_port);
}

inline void set_address_port(sockaddr_storage &address, uint16_t port) {
#if !defined(ESP_PLATFORM) || LWIP_IPV6
  if (address.ss_family == AF_INET6) {
    reinterpret_cast<sockaddr_in6 &>(address).sin6_port = htons(port);
    return;
  }
#endif
  reinterpret_cast<sockaddr_in &>(address).sin_port = htons(port);
}

inline bool same_host(const sockaddr_storage &a, const sockaddr_storage &b) {
  if (a.ss_family != b.ss_family) return false;
  size_t a_size, b_size;
  const auto *a_bytes = address_bytes(a, a_size);
  const auto *b_bytes = address_bytes(b, b_size);
  if (!a_size || a_size != b_size || std::memcmp(a_bytes, b_bytes, a_size) != 0) return false;
#if !defined(ESP_PLATFORM) || LWIP_IPV6
  if (a.ss_family == AF_INET6) {
    const auto &a6 = reinterpret_cast<const sockaddr_in6 &>(a);
    const auto &b6 = reinterpret_cast<const sockaddr_in6 &>(b);
    if (IN6_IS_ADDR_LINKLOCAL(&a6.sin6_addr)) return a6.sin6_scope_id == b6.sin6_scope_id;
  }
#endif
  return true;
}

inline std::string address_text(const sockaddr_storage &address) {
  size_t size;
  const auto *bytes = address_bytes(address, size);
  char text[46]{};  // Maximum IPv6 presentation length, including terminator.
  if (!size || !inet_ntop(address.ss_family, bytes, text, sizeof(text))) return {};
  return text;
}

inline std::string http_host(const sockaddr_storage &address) {
  auto text = address_text(address);
#if !defined(ESP_PLATFORM) || LWIP_IPV6
  if (address.ss_family == AF_INET6) text = "[" + text + "]";
#endif
  return text + ":" + std::to_string(address_port(address));
}

inline bool append_challenge_address(std::vector<uint8_t> &challenge, const sockaddr_storage &local,
                                     const std::array<uint8_t, 6> &mac) {
  size_t size;
  const auto *bytes = address_bytes(local, size);
  if (challenge.empty() || challenge.size() > 16 || !size) return false;
  challenge.insert(challenge.end(), bytes, bytes + size);
  challenge.insert(challenge.end(), mac.begin(), mac.end());
  challenge.resize(std::max(challenge.size(), size_t(32)), 0);
  return true;
}

}  // namespace esphome::airplay
