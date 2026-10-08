#pragma once

#include "protocol.h"
#include "socket_address.h"
#include "esphome/core/helpers.h"

#include <atomic>
#include <array>
#include <netinet/in.h>
#include <sys/socket.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <freertos/queue.h>

namespace esphome::airplay {

class AirplayMediaSource;
class SocketReader;

class RaopServer {
 public:
  bool start(AirplayMediaSource *source, const std::string &name, size_t buffer_frames);
  void abort_session() { this->abort_.store(true); }
  void shutdown() { this->running_.store(false); this->abort_session(); }
  bool is_running() const { return this->running_.load(); }
  void set_session_timeout(uint32_t ms) { this->session_timeout_ms_ = ms; }
  void set_remote_control(bool enabled) { this->remote_control_ = enabled; }
  void request_remote_play(bool play) {
    if (this->remote_running_.load()) this->remote_command_.store(play ? 2 : 1);
  }
  void request_remote_track(RemoteTrackCommand command);
  uint32_t concealed_packets() const { return this->concealed_frames_.load(); }
  uint32_t late_frames() const { return this->late_frames_.load(); }
  uint32_t output_drops() const { return this->output_drops_.load(); }
  uint32_t decode_errors() const { return this->decode_errors_.load(); }
  bool dacp_available() const {
    return this->session_active_.load() && this->remote_running_.load() && this->remote_available_.load();
  }
  bool is_session_active() const { return this->session_active_.load(); }

 private:
  struct Frame {
    std::array<uint8_t, FRAME_BYTES> pcm;
    uint32_t timestamp;
    uint16_t sequence;
    uint16_t length;
    bool ready;
  };

  static void server_task(void *arg);
  static void audio_task(void *arg);
  static void playback_task(void *arg);
  static void remote_task(void *arg);
  uint16_t discover_remote_();
  bool send_remote_command_(uint16_t port, const char *command);
  void serve_();
  void receive_audio_();
  bool handle_request_(int socket, SocketReader &reader);
  bool setup_audio_(const std::string &transport);
  void cleanup_audio_();
  void reset_audio_(const std::string &rtp_info);
  void request_timing_();
  void request_resend_(uint16_t first, uint16_t count);
  bool receive_packet_(size_t index);
  bool play_frame_();

  AirplayMediaSource *source_{nullptr};
  std::atomic<bool> running_{false};
  std::atomic<bool> abort_{false};
  std::atomic<bool> audio_running_{false};
  std::atomic<bool> session_active_{false};
  TaskHandle_t audio_task_{nullptr};
  TaskHandle_t playback_task_{nullptr};
  TaskHandle_t remote_task_{nullptr};
  SemaphoreHandle_t audio_done_{nullptr};
  SemaphoreHandle_t playback_done_{nullptr};
  SemaphoreHandle_t remote_done_{nullptr};
  bool remote_control_{false};
  std::atomic<bool> remote_running_{false};
  std::atomic<bool> remote_available_{false};
  std::atomic<bool> remote_paused_{false};
  std::atomic<int> remote_command_{0};  // Latest play/pause intent wins.
  // Fixed native FIFO preserves repeated skips and next/previous ordering.
  static constexpr size_t TRACK_QUEUE_LENGTH = 16;
  StaticQueue_t remote_tracks_control_{};
  std::array<uint8_t, TRACK_QUEUE_LENGTH * sizeof(RemoteTrackCommand)> remote_tracks_storage_{};
  QueueHandle_t remote_tracks_{nullptr};
  std::string dacp_id_;
  std::string active_remote_;
  SemaphoreHandle_t audio_mutex_{nullptr};
  std::array<int, 2> listeners_{-1, -1};
  sockaddr_storage peer_{};
  sockaddr_storage local_{};
  std::array<int, 3> sockets_{-1, -1, -1};
  std::array<uint16_t, 3> ports_{};
  uint16_t control_port_{0};
  uint16_t timing_port_{0};
  std::array<uint8_t, 6> mac_{};
  std::array<uint8_t, 24> cookie_{};
  std::array<uint8_t, 16> aes_key_{};
  std::array<uint8_t, 16> aes_iv_{};
  bool has_format_{false};
  bool encrypted_{false};
  void *decoder_{nullptr};
  Frame *frames_{nullptr};
  size_t buffer_frames_{512};
  uint32_t session_timeout_ms_{15000};
  RAMAllocator<Frame> allocator_{RAMAllocator<Frame>::ALLOC_EXTERNAL};
  bool recording_{false};
  std::atomic<uint32_t> audio_generation_{0};
  bool have_sequence_{false};
  bool have_timing_{false};
  bool have_sync_{false};
  uint16_t read_sequence_{0};
  uint16_t latest_sequence_{0};
  uint32_t sync_rtp_{0};
  uint32_t sync_ms_{0};
  uint64_t remote_ntp_{0};
  uint32_t local_ms_{0};
  uint32_t timing_request_ms_{0};
  uint32_t resend_request_ms_{0};
  std::array<uint32_t, 3> received_packets_{};
  uint32_t decoded_frames_{0};
  std::atomic<uint32_t> decode_errors_{0};
  std::atomic<uint32_t> late_frames_{0};
  std::atomic<uint32_t> output_drops_{0};
  std::atomic<uint32_t> concealed_frames_{0};
  std::atomic<uint32_t> written_bytes_{0};
};

}  // namespace esphome::airplay
