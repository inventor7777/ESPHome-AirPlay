#pragma once

#include "raop_server.h"
#include "esphome/core/component.h"
#include "esphome/components/media_source/media_source.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/speaker/speaker.h"

namespace esphome::airplay {

class AirplayMediaSource final : public Component, public media_source::MediaSource {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  void on_shutdown() override;
  float get_setup_priority() const override { return setup_priority::LATE; }
  void set_name(const std::string &name) { this->name_ = name; }
  void set_speaker(speaker::Speaker *speaker) { this->speaker_ = speaker; }
  void set_buffer_frames(size_t frames) { this->buffer_frames_ = frames; }
  void set_prefeed_duration(uint32_t ms) { this->prefeed_duration_ms_ = ms; }
  void set_output_latency(uint32_t ms) { this->output_latency_ms_ = ms; }
  void set_volume_update_interval(uint32_t ms) { this->volume_update_interval_ms_ = ms; }
  void set_session_timeout(uint32_t ms) { this->server_.set_session_timeout(ms); }
  void set_remote_control(bool enabled) { this->server_.set_remote_control(enabled); }
  void set_active_sensor(binary_sensor::BinarySensor *sensor) { this->active_sensor_ = sensor; }
  void set_client_sensor(text_sensor::TextSensor *sensor) { this->client_sensor_ = sensor; }

  void set_dacp_available_sensor(binary_sensor::BinarySensor *sensor) { this->dacp_available_sensor_ = sensor; }
  void set_concealed_packets_sensor(sensor::Sensor *sensor) { this->concealed_packets_sensor_ = sensor; }
  void set_late_frames_sensor(sensor::Sensor *sensor) { this->late_frames_sensor_ = sensor; }
  void set_output_drops_sensor(sensor::Sensor *sensor) { this->output_drops_sensor_ = sensor; }
  void set_decode_errors_sensor(sensor::Sensor *sensor) { this->decode_errors_sensor_ = sensor; }

  void set_title_sensor(text_sensor::TextSensor *sensor) { this->title_sensor_ = sensor; }
  void set_artist_sensor(text_sensor::TextSensor *sensor) { this->artist_sensor_ = sensor; }
  void set_album_sensor(text_sensor::TextSensor *sensor) { this->album_sensor_ = sensor; }
  bool metadata_enabled() const { return this->title_sensor_ || this->artist_sensor_ || this->album_sensor_; }
  void sender_metadata(TrackMetadata metadata);

  bool can_handle(const std::string &uri) const override { return uri == "airplay://current"; }
  bool play_uri(const std::string &uri) override;
  bool has_internal_playlist() const override { return true; }  // Track selection belongs to the sender.
  void handle_command(media_source::MediaSourceCommand command) override;
  void notify_volume_changed(float volume) override { this->reported_volume_.store(volume); }
  void notify_mute_changed(bool muted) override { this->reported_mute_.store(muted); }

  // Transport callbacks run on background tasks; ESPHome state changes run in loop().
  void client_connected(const sockaddr_storage &client);
  void client_disconnected() { this->client_connected_.store(false); }
  void stream_started();
  void stream_flushed();
  void stream_ended();
  void sender_volume(float volume, bool muted);
  void stream_paused() { this->sender_paused_.store(true); }
  size_t write_pcm(const uint8_t *data, size_t length);
  uint32_t output_delay_ms() const;

 private:
  RaopServer server_;
  speaker::Speaker *speaker_{nullptr};
  Mutex output_mutex_;
  std::atomic<bool> flush_requested_{false};
  std::atomic<bool> output_flushing_{false};
  binary_sensor::BinarySensor *active_sensor_{nullptr};
  text_sensor::TextSensor *client_sensor_{nullptr};
  binary_sensor::BinarySensor *dacp_available_sensor_{nullptr};
  sensor::Sensor *concealed_packets_sensor_{nullptr};
  sensor::Sensor *late_frames_sensor_{nullptr};
  sensor::Sensor *output_drops_sensor_{nullptr};
  sensor::Sensor *decode_errors_sensor_{nullptr};
  text_sensor::TextSensor *title_sensor_{nullptr};
  text_sensor::TextSensor *artist_sensor_{nullptr};
  text_sensor::TextSensor *album_sensor_{nullptr};
  Mutex metadata_mutex_;
  TrackMetadata metadata_{"Inactive", "Inactive", "Inactive"};
  std::atomic<bool> metadata_dirty_{true};
  Mutex client_mutex_;
  std::string client_address_;
  std::string name_;
  size_t buffer_frames_{512};
  uint32_t prefeed_duration_ms_{100};
  uint32_t output_latency_ms_{20};
  uint32_t volume_update_interval_ms_{100};
  uint32_t last_start_attempt_{0};
  uint32_t last_volume_update_{0};
  uint32_t last_diagnostic_update_{0};
  bool client_was_active_{false};
  uint32_t client_generation_{0};
  uint32_t seen_generation_{0};
  std::atomic<uint32_t> stream_generation_{0};
  std::atomic<bool> client_connected_{false};
  std::atomic<uint32_t> connection_generation_{0};
  std::atomic<bool> session_active_{false};
  std::atomic<bool> output_enabled_{false};
  std::atomic<bool> sender_paused_{false};
  std::atomic<float> volume_request_{-1.0f};
  std::atomic<float> reported_volume_{-1.0f};
  std::atomic<bool> reported_mute_{false};
  std::atomic<bool> logged_audio_{false};
};

}  // namespace esphome::airplay
