#include "../components/airplay/socket_reader.h"

#include <cassert>
#include <fcntl.h>
#include <unistd.h>

using esphome::airplay::SocketReader;

int main() {
  int sockets[2];
  assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
  assert(fcntl(sockets[0], F_SETFL, O_NONBLOCK) == 0);
  SocketReader reader(sockets[0]);
  auto deadline = SocketReader::Clock::now() + std::chrono::seconds(1);
  // Cross the refill boundary and preserve a binary body plus a pipelined request.
  std::string input = std::string(600, 'x') + "\r\n\r\n";
  input.append("a\0b\n", 4);
  input += "OPTIONS * RTSP/1.0\r\n\r\n";
  assert(write(sockets[1], input.data(), input.size()) == static_cast<ssize_t>(input.size()));
  std::string line;
  assert(reader.read_line(line, deadline) && line == std::string(600, 'x'));
  assert(reader.read_line(line, deadline) && line.empty());
  char body[4];
  assert(reader.read_exact(body, sizeof(body), deadline));
  assert(std::memcmp(body, "a\0b\n", 4) == 0);
  assert(reader.buffered());
  assert(reader.read_line(line, deadline) && line == "OPTIONS * RTSP/1.0");
  assert(reader.read_line(line, deadline) && line.empty());
  assert(!reader.buffered());
  assert(!reader.read_line(line, SocketReader::Clock::now() + std::chrono::milliseconds(5)));
  // Preserve the original maximum line length and close-on-truncation behavior.
  input = std::string(1024, 'x') + "\n";
  assert(write(sockets[1], input.data(), input.size()) == static_cast<ssize_t>(input.size()));
  assert(!reader.read_line(line, deadline));
  close(sockets[0]);
  close(sockets[1]);

  assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
  SocketReader truncated(sockets[0]);
  assert(write(sockets[1], "abc", 3) == 3);
  close(sockets[1]);
  assert(!truncated.read_line(line, deadline) && line == "abc");
  close(sockets[0]);
}
