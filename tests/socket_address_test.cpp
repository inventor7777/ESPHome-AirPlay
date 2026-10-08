#include "../components/airplay/socket_address.h"
#include <cassert>
#include <cstdio>
#include <unistd.h>
#include <sys/poll.h>

using namespace esphome::airplay;

static sockaddr_storage address(int family, const char *text) {
  sockaddr_storage result{};
  result.ss_family = family;
  if (family == AF_INET)
    assert(inet_pton(family, text, &reinterpret_cast<sockaddr_in &>(result).sin_addr) == 1);
  else
    assert(inet_pton(family, text, &reinterpret_cast<sockaddr_in6 &>(result).sin6_addr) == 1);
  return result;
}

int main() {
  const std::array<uint8_t, 6> mac{1, 2, 3, 4, 5, 6};
  for (int family : {AF_INET, AF_INET6}) {
    const char *host = family == AF_INET ? "127.0.0.1" : "::1";
    auto local = address(family, host);
    set_address_port(local, 5000);
    assert(address_port(local) == 5000);
    assert(address_text(local) == host);
    assert(http_host(local) == (family == AF_INET ? "127.0.0.1:5000" : "[::1]:5000"));
    auto other_port = local;
    set_address_port(other_port, 1234);
    assert(same_host(local, other_port));
    assert(!same_host(local, address(family, family == AF_INET ? "127.0.0.2" : "::2")));
    std::vector<uint8_t> challenge(16, 0xab);
    assert(append_challenge_address(challenge, local, mac));
    size_t ip_size;
    const auto *ip = address_bytes(local, ip_size);
    assert(challenge.size() == (family == AF_INET ? 32 : 38));
    assert(std::all_of(challenge.begin(), challenge.begin() + 16, [](auto b) { return b == 0xab; }));
    assert(std::equal(ip, ip + ip_size, challenge.begin() + 16));
    assert(std::equal(mac.begin(), mac.end(), challenge.begin() + 16 + ip_size));
    assert(std::all_of(challenge.begin() + 22 + ip_size, challenge.end(), [](auto b) { return b == 0; }));

    // Exercise the shared sockaddr lengths/ports against native TCP and UDP APIs.
    for (int type : {SOCK_STREAM, SOCK_DGRAM}) {
      int receiver = socket(family, type, 0);
      int sender = socket(family, type, 0);
      assert(receiver >= 0 && sender >= 0);
      set_address_port(local, 0);
      int bound = bind(receiver, reinterpret_cast<sockaddr *>(&local), address_length(local));
      if (bound != 0) std::perror("bind");
      assert(bound == 0);
      socklen_t length = sizeof(local);
      assert(getsockname(receiver, reinterpret_cast<sockaddr *>(&local), &length) == 0);
      assert(address_port(local) != 0);
      sockaddr_storage peer{};
      int connection = receiver;
      if (type == SOCK_STREAM) {
        assert(configure_keepalive(sender));
        int keepalive = 0;
        socklen_t option_length = sizeof(keepalive);
        assert(getsockopt(sender, SOL_SOCKET, SO_KEEPALIVE, &keepalive, &option_length) == 0 && keepalive != 0);
        assert(listen(receiver, 1) == 0);
        assert(connect(sender, reinterpret_cast<sockaddr *>(&local), address_length(local)) == 0);
        pollfd ready{receiver, POLLIN, 0};
        assert(poll(&ready, 1, 1000) == 1);
        length = sizeof(peer);
        connection = accept(receiver, reinterpret_cast<sockaddr *>(&peer), &length);
        assert(connection >= 0 && same_host(peer, local));
        sockaddr_storage accepted_local{};
        length = sizeof(accepted_local);
        assert(getsockname(connection, reinterpret_cast<sockaddr *>(&accepted_local), &length) == 0);
        assert(same_host(accepted_local, local));
        assert(send(sender, "X", 1, 0) == 1);
      } else {
        assert(sendto(sender, "X", 1, 0, reinterpret_cast<sockaddr *>(&local), address_length(local)) == 1);
      }
      pollfd ready{connection, POLLIN, 0};
      assert(poll(&ready, 1, 1000) == 1);
      char data = 0;
      length = sizeof(peer);
      assert(recvfrom(connection, &data, 1, 0, reinterpret_cast<sockaddr *>(&peer), &length) == 1);
      assert(data == 'X');
      if (type == SOCK_DGRAM) {
        assert(same_host(peer, local));
        assert(sendto(receiver, "Y", 1, 0, reinterpret_cast<sockaddr *>(&peer), address_length(peer)) == 1);
        ready = {sender, POLLIN, 0};
        assert(poll(&ready, 1, 1000) == 1);
        assert(recv(sender, &data, 1, 0) == 1 && data == 'Y');
      }
      if (connection != receiver) close(connection);
      close(sender);
      close(receiver);
    }
  }
  auto scoped = address(AF_INET6, "fe80::1");
  reinterpret_cast<sockaddr_in6 &>(scoped).sin6_scope_id = 2;
  auto wrong_interface = scoped;
  reinterpret_cast<sockaddr_in6 &>(wrong_interface).sin6_scope_id = 3;
  assert(!same_host(scoped, wrong_interface));
  auto target = scoped;
  set_address_port(target, 6000);
  assert(same_host(scoped, target));
  assert(reinterpret_cast<sockaddr_in6 &>(target).sin6_scope_id == 2);
  assert(!same_host(address(AF_INET, "127.0.0.1"), address(AF_INET6, "::ffff:127.0.0.1")));
  sockaddr_storage unknown{};
  assert(address_length(unknown) == 0 && !same_host(unknown, unknown));
  std::vector<uint8_t> empty, oversized(17, 0);
  assert(!append_challenge_address(empty, scoped, mac));
  assert(!append_challenge_address(oversized, scoped, mac));
  std::vector<uint8_t> challenge(16, 0);
  assert(!append_challenge_address(challenge, unknown, mac));
}
