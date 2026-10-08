#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <cerrno>
#include <string>
#include <sys/poll.h>
#include <sys/socket.h>

namespace esphome::airplay {

// One reader per TCP connection: retain read-ahead across headers, bodies and requests.
class SocketReader {
 public:
  using Clock = std::chrono::steady_clock;
  explicit SocketReader(int socket) : socket_(socket) {}
  bool buffered() const { return this->offset_ < this->length_; }

  bool read_exact(char *data, size_t length, Clock::time_point deadline) {
    while (length) {
      if (Clock::now() >= deadline) return false;
      if (!this->buffered() && !this->refill_(deadline)) return false;
      size_t count = std::min(length, this->length_ - this->offset_);
      std::memcpy(data, this->buffer_.data() + this->offset_, count);
      this->offset_ += count;
      data += count;
      length -= count;
    }
    return true;
  }

  bool read_line(std::string &line, Clock::time_point deadline) {
    line.clear();
    while (line.size() < 1024) {
      if (Clock::now() >= deadline) return false;
      if (!this->buffered() && !this->refill_(deadline)) return false;
      while (this->buffered() && line.size() < 1024) {
        char c = this->buffer_[this->offset_++];
        if (c == '\n') return true;
        if (c != '\r') line.push_back(c);
      }
    }
    return false;
  }

 private:
  bool refill_(Clock::time_point deadline) {
    while (Clock::now() < deadline) {
      auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
      pollfd fd{this->socket_, POLLIN, 0};
      int ready = poll(&fd, 1, int(std::max<int64_t>(1, std::min<int64_t>(100, remaining))));
      if (ready < 0) {
        if (errno == EINTR) continue;
        return false;
      }
      if (ready == 0) continue;
      int count = recv(this->socket_, this->buffer_.data(), this->buffer_.size(), MSG_DONTWAIT);
      if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
      if (count <= 0) return false;
      this->offset_ = 0;
      this->length_ = count;
      return true;
    }
    return false;
  }

  int socket_;
  std::array<char, 512> buffer_{};
  size_t offset_{0};
  size_t length_{0};
};

}  // namespace esphome::airplay
