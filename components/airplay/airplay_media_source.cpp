#include "airplay_media_source.h"

#include "esphome/components/network/util.h"
#include "esphome/core/log.h"
#include "esphome/core/hal.h"

namespace esphome::airplay {

static const char *const TAG = "airplay";
static constexpr float MUTE_REQUEST = -2.0f;  // Preserve volume when the sender requests -144 dB.
using media_source::MediaSourceCommand;
using media_source::MediaSourceState;

static void publish_counter(sensor::Sensor *sensor, uint32_t count) {
  if (sensor && (!sensor->has_state() || sensor->state != count)) sensor->publish_state(count);
}

static void publish_metadata(text_sensor::TextSensor *sensor, const std::string &value) {
  if (sensor && (!sensor->has_state() || sensor->state != value)) sensor->publish_state(value);
}

void AirplayMediaSource::setup() {
  if (!this->has_listener()) {
    ESP_LOGE(TAG, "Assign this source to a speaker_source media player");
    this->mark_failed();
  }
}

void AirplayMediaSource::loop() {
  bool connected = network::is_connected();
  if (!connected && this->client_connected_.load()) this->server_.abort_session();
  if (connected && !this->server_.is_running() && millis() - this->last_start_attempt_ >= 5000) {
    this->last_start_attempt_ = millis();
    if (!this->server_.start(this, this->name_, this->buffer_frames_))
      ESP_LOGW(TAG, "Receiver startup failed; retrying in five seconds");
  }
  auto generation = this->stream_generation_.load();
  if (this->session_active_.load()) {
    if (generation != this->seen_generation_) {
      this->seen_generation_ = generation;
      this->pending_start_ = true;
      this->request_play_uri_("airplay://current");
    } else if (this->sender_paused_.load()) {
      this->output_enabled_.store(false);
      this->set_state_(MediaSourceState::PAUSED);
    }
  } else {
    this->pending_start_ = false;
    this->output_enabled_.store(false);
    this->set_state_(MediaSourceState::IDLE);
  }
  // Publish only from ESPHome's main loop, never the RTSP/audio workers.
  bool active = this->client_connected_.load();
  uint32_t connection_generation = this->connection_generation_.load();
  if (this->active_sensor_ && (!this->active_sensor_->has_state() || this->active_sensor_->state != active))
    this->active_sensor_->publish_state(active);
  if (this->client_sensor_ && (!this->client_sensor_->has_state() || active != this->client_was_active_ ||
                              (active && connection_generation != this->client_generation_))) {
    std::string client = "Inactive";
    if (active) {
      LockGuard lock(this->client_mutex_);
      client = this->client_address_;
    }
    if (!this->client_sensor_->has_state() || this->client_sensor_->state != client)
      this->client_sensor_->publish_state(client);
    this->client_was_active_ = active;
    this->client_generation_ = connection_generation;
  }
  if (this->metadata_enabled() && this->metadata_dirty_.exchange(false)) {
    TrackMetadata metadata;
    {
      LockGuard lock(this->metadata_mutex_);
      metadata = this->metadata_;
    }
    publish_metadata(this->title_sensor_, metadata.title);
    publish_metadata(this->artist_sensor_, metadata.artist);
    publish_metadata(this->album_sensor_, metadata.album);
  }
  bool dacp_available = this->server_.dacp_available();
  if (this->dacp_available_sensor_ &&
      (!this->dacp_available_sensor_->has_state() || this->dacp_available_sensor_->state != dacp_available))
    this->dacp_available_sensor_->publish_state(dacp_available);
  // Bound diagnostic traffic while retaining the final counters after disconnect.
  if (millis() - this->last_diagnostic_update_ >= 1000) {
    this->last_diagnostic_update_ = millis();
    publish_counter(this->concealed_packets_sensor_, this->server_.concealed_packets());
    publish_counter(this->late_frames_sensor_, this->server_.late_frames());
    publish_counter(this->output_drops_sensor_, this->server_.output_drops());
    publish_counter(this->decode_errors_sensor_, this->server_.decode_errors());
  }
  if (this->session_active_.load() && millis() - this->last_volume_update_ >= this->volume_update_interval_ms_) {
    float volume = this->volume_request_.exchange(-1.0f);
    if (volume >= 0.0f || volume == MUTE_REQUEST) {
      this->last_volume_update_ = millis();
      bool muted = volume == MUTE_REQUEST;
      if (!muted && std::abs(volume - this->reported_volume_.load()) > 0.0001f)
        this->request_volume_(volume);
      if (muted != this->reported_mute_.load()) this->request_mute_(muted);
    }
  }
}

bool AirplayMediaSource::play_uri(const std::string &uri) {
  this->pending_start_ = false;
  if (!this->is_ready() || !this->has_listener() || !this->can_handle(uri) ||
      !this->session_active_.load() || this->get_state() != MediaSourceState::IDLE)
    return false;
  this->logged_audio_.store(false);
  this->set_state_(MediaSourceState::PLAYING);
  this->output_enabled_.store(true);
  return true;
}

void AirplayMediaSource::handle_command(MediaSourceCommand command) {
  switch (command) {
    case MediaSourceCommand::STOP:
      this->output_enabled_.store(false);
      // The orchestrator may stop this source while delivering our incoming-stream URI.
      if (!this->pending_start_) this->server_.abort_session();
      this->set_state_(MediaSourceState::IDLE);
      break;
    case MediaSourceCommand::PAUSE:
      this->output_enabled_.store(false);
      if (this->session_active_.load()) {
        this->server_.request_remote_play(false);
        this->set_state_(MediaSourceState::PAUSED);
      }
      break;
    case MediaSourceCommand::PLAY:
      if (this->session_active_.load()) {
        this->server_.request_remote_play(true);
        this->sender_paused_.store(false);
        this->set_state_(MediaSourceState::PLAYING);
        this->output_enabled_.store(true);
      }
      break;
    case MediaSourceCommand::NEXT:
      this->server_.request_remote_track(RemoteTrackCommand::NEXT);
      break;
    case MediaSourceCommand::PREVIOUS:
      this->server_.request_remote_track(RemoteTrackCommand::PREVIOUS);
      break;
    default:
      break;
  }
}

void AirplayMediaSource::client_connected(const sockaddr_storage &client) {
  {
    LockGuard lock(this->client_mutex_);
    this->client_address_ = address_text(client);
  }
  this->connection_generation_.fetch_add(1);
  this->client_connected_.store(true);
}
void AirplayMediaSource::stream_started() {
  this->sender_paused_.store(false);
  this->session_active_.store(true);
  this->stream_generation_.fetch_add(1);
}
void AirplayMediaSource::stream_ended() {
  this->output_enabled_.store(false);
  this->session_active_.store(false);
  this->volume_request_.store(-1.0f);
  if (this->metadata_enabled()) this->sender_metadata({"Inactive", "Inactive", "Inactive"});
}
void AirplayMediaSource::sender_metadata(TrackMetadata metadata) {
  {
    LockGuard lock(this->metadata_mutex_);
    this->metadata_ = std::move(metadata);
  }
  this->metadata_dirty_.store(true);
}
void AirplayMediaSource::sender_volume(float volume, bool muted) {
  // One atomic request keeps rapid mute/volume updates ordered: latest wins.
  this->volume_request_.store(muted ? MUTE_REQUEST : volume);
}

size_t AirplayMediaSource::write_pcm(const uint8_t *data, size_t length) {
  if (!this->output_enabled_.load()) return length;  // Paused/inactive streams advance their live timeline.
  audio::AudioStreamInfo info(16, 2, SAMPLE_RATE);
  size_t written = this->write_output(data, length, 5, info);
  if (written && !this->logged_audio_.exchange(true))
    ESP_LOGD(TAG, "First PCM audio accepted by native speaker pipeline");
  return written;
}

uint32_t AirplayMediaSource::output_delay_ms() const {
  // ponytail: bounded prefeed absorbs task jitter; sample-level speaker
  // clock scheduling is needed for accurate synchronization with other receivers.
  return this->output_latency_ms_ + this->prefeed_duration_ms_;
}

void AirplayMediaSource::dump_config() {
  ESP_LOGCONFIG(TAG, "AirPlay source '%s': buffer=%u frames, prefeed=%u ms, output latency=%u ms",
                this->name_.c_str(), unsigned(this->buffer_frames_), unsigned(this->prefeed_duration_ms_),
                unsigned(this->output_latency_ms_));
}
void AirplayMediaSource::on_shutdown() {
  this->output_enabled_.store(false);
  this->server_.shutdown();
}

}  // namespace esphome::airplay
