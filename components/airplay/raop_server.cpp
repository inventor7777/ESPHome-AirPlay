// RAOP protocol adapted from Philippe's MIT-licensed implementation and HairTunes.
// Original notices are retained in THIRD-PARTY-NOTICES.md.
#include "raop_server.h"
#include "airplay_media_source.h"
#include "raop_key.h"
#include "socket_reader.h"
#include "esphome/core/log.h"
#include "esphome/core/hal.h"

#include <esp_mac.h>
#include <mdns.h>
#include <esp_alac_dec.h>
#include <mbedtls/aes.h>
#include <mbedtls/base64.h>
#include <mbedtls/pk.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <arpa/inet.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <cstdio>
#include <cctype>
#include <cerrno>
#include <map>
#include <vector>

namespace esphome::airplay {

static const char *const TAG = "airplay.raop";

static bool send_all(int sock, const std::string &data) {
  size_t offset = 0;
  uint32_t deadline = millis() + 2000;
  while (offset < data.size()) {
    if (time_delta(deadline, millis()) <= 0) return false;
    pollfd fd{sock, POLLOUT, 0};
    if (poll(&fd, 1, 100) <= 0) continue;
    int n = send(sock, data.data() + offset, data.size() - offset, MSG_DONTWAIT);
    if (n <= 0) return false;
    offset += n;
  }
  return true;
}

static bool decode_base64(std::string input, std::vector<uint8_t> &output) {
  while (input.size() % 4) input.push_back('=');
  output.resize(input.size() / 4 * 3);
  size_t size = 0;
  if (output.empty() || mbedtls_base64_decode(output.data(), output.size(), &size,
      reinterpret_cast<const uint8_t *>(input.data()), input.size()) != 0) return false;
  output.resize(size);
  return true;
}

static std::string encode_base64(const uint8_t *data, size_t size) {
  std::string output((size + 2) / 3 * 4 + 1, '\0');
  size_t written = 0;
  if (mbedtls_base64_encode(reinterpret_cast<uint8_t *>(output.data()), output.size(), &written, data, size) != 0)
    return {};
  output.resize(written);
  while (!output.empty() && output.back() == '=') output.pop_back();
  return output;
}

// AirPlay's Apple-Challenge uses the private RSA operation with type-1 padding,
// not public-key encryption. The ANNOUNCE key uses RSA-OAEP/SHA-1.
static bool rsa_operation(const std::vector<uint8_t> &input, bool challenge, std::vector<uint8_t> &output) {
  mbedtls_pk_context key;
  mbedtls_entropy_context entropy;
  mbedtls_ctr_drbg_context rng;
  mbedtls_pk_init(&key);
  mbedtls_entropy_init(&entropy);
  mbedtls_ctr_drbg_init(&rng);
  const uint8_t personalization[] = "esphome-airplay";
  int result = mbedtls_ctr_drbg_seed(&rng, mbedtls_entropy_func, &entropy, personalization,
                                    sizeof(personalization) - 1);
  if (result == 0)
    result = mbedtls_pk_parse_key(&key, reinterpret_cast<const uint8_t *>(RAOP_PRIVATE_KEY),
                                 sizeof(RAOP_PRIVATE_KEY), nullptr, 0, mbedtls_ctr_drbg_random, &rng);
  if (result == 0) {
    auto *rsa = mbedtls_pk_rsa(key);
    size_t modulus = mbedtls_rsa_get_len(rsa);
    output.resize(modulus);
    if (challenge && input.size() <= modulus - 11) {
      std::vector<uint8_t> padded(modulus, 0xff);
      padded[0] = 0;
      padded[1] = 1;
      padded[modulus - input.size() - 1] = 0;
      std::copy(input.begin(), input.end(), padded.end() - input.size());
      result = mbedtls_rsa_private(rsa, mbedtls_ctr_drbg_random, &rng, padded.data(), output.data());
    } else if (!challenge && input.size() == modulus) {
      size_t written = 0;
      mbedtls_rsa_set_padding(rsa, MBEDTLS_RSA_PKCS_V21, MBEDTLS_MD_SHA1);
      result = mbedtls_rsa_rsaes_oaep_decrypt(rsa, mbedtls_ctr_drbg_random, &rng, nullptr, 0, &written,
                                            input.data(), output.data(), output.size());
      output.resize(written);
    } else {
      result = -1;
    }
  }
  mbedtls_pk_free(&key);
  mbedtls_ctr_drbg_free(&rng);
  mbedtls_entropy_free(&entropy);
  if (result != 0) output.clear();
  return result == 0;
}

static std::string sdp_attribute(const std::string &body, const char *key) {
  std::string prefix = std::string("a=") + key + ":";
  size_t pos = body.find(prefix);
  if (pos == std::string::npos) return {};
  pos += prefix.size();
  return body.substr(pos, body.find_first_of("\r\n", pos) - pos);
}

static int open_listener(int family) {
  int sock = socket(family, SOCK_STREAM, 0);
  if (sock < 0) return -1;
  int enabled = 1;
  sockaddr_storage address{};
  address.ss_family = family;
  set_address_port(address, 5000);
  bool configured = setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)) == 0;
#if LWIP_IPV6
  if (family == AF_INET6)
    configured = configured && setsockopt(sock, IPPROTO_IPV6, IPV6_V6ONLY, &enabled, sizeof(enabled)) == 0;
#endif
  if (!configured || bind(sock, reinterpret_cast<sockaddr *>(&address), address_length(address)) != 0 ||
      listen(sock, 1) != 0) {
    ESP_LOGW(TAG, "Listener setup failed for family %d: errno=%d", family, errno);
    close(sock);
    return -1;
  }
  return sock;
}

bool RaopServer::start(AirplayMediaSource *source, const std::string &name, size_t buffer_frames) {
  if (this->remote_control_ && !this->remote_tracks_)
    this->remote_tracks_ = xQueueCreateStatic(TRACK_QUEUE_LENGTH, sizeof(RemoteTrackCommand),
                                             this->remote_tracks_storage_.data(), &this->remote_tracks_control_);
  this->source_ = source;
  this->buffer_frames_ = buffer_frames;
  this->listeners_[0] = open_listener(AF_INET);
#if LWIP_IPV6
  this->listeners_[1] = open_listener(AF_INET6);
  bool listening = this->listeners_[0] >= 0 && this->listeners_[1] >= 0;
#else
  bool listening = this->listeners_[0] >= 0;
#endif
  if (!listening) {
    for (auto &listener : this->listeners_) {
      if (listener >= 0) close(listener);
      listener = -1;
    }
    return false;
  }
  esp_efuse_mac_get_default(this->mac_.data());
  char instance[64];
  snprintf(instance, sizeof(instance), "%02X%02X%02X%02X%02X%02X@%s", this->mac_[0], this->mac_[1],
           this->mac_[2], this->mac_[3], this->mac_[4], this->mac_[5], name.c_str());
  mdns_txt_item_t txt[] = {{"am", "ESPHome"}, {"tp", "UDP"}, {"sm", "false"}, {"sv", "false"},
    {"et", "0,1"}, {"cn", "1"}, {"ch", "2"}, {"ss", "16"}, {"sr", "44100"}, {"vn", "3"},
    {"txtvers", "1"}, {"pw", "false"}, {"md", "0"}};
  if (mdns_service_add(instance, "_raop", "_tcp", 5000, txt,
                       sizeof(txt) / sizeof(txt[0]) - (source->metadata_enabled() ? 0 : 1)) != ESP_OK) {
    for (auto &listener : this->listeners_) {
      if (listener >= 0) close(listener);
      listener = -1;
    }
    return false;
  }
  this->running_.store(true);
  if (xTaskCreate(server_task, "airplay_rtsp", 12288, this, 3, nullptr) != pdPASS) {
    this->running_.store(false);
    mdns_service_remove("_raop", "_tcp");
    for (auto &listener : this->listeners_) {
      if (listener >= 0) close(listener);
      listener = -1;
    }
    return false;
  }
  ESP_LOGI(TAG, "AirPlay receiver listening on port 5000 (%s)",
           this->listeners_[1] >= 0 ? "IPv4 + IPv6" : "IPv4");
  return true;
}

void RaopServer::server_task(void *arg) {
  static_cast<RaopServer *>(arg)->serve_();
  vTaskDelete(nullptr);
}

void RaopServer::serve_() {
  while (this->running_.load()) {
    std::array<pollfd, 2> fds{};
    for (size_t i = 0; i < fds.size(); ++i) fds[i] = {this->listeners_[i], POLLIN, 0};
    if (poll(fds.data(), fds.size(), 100) <= 0) continue;
    int listener = -1;
    for (const auto &fd : fds) {
      if (fd.revents & POLLIN) { listener = fd.fd; break; }
    }
    if (listener < 0) continue;
    this->peer_ = {};
    socklen_t length = sizeof(this->peer_);
    int sock = accept(listener, reinterpret_cast<sockaddr *>(&this->peer_), &length);
    if (sock < 0) continue;
    if (!configure_keepalive(sock)) {
      ESP_LOGW(TAG, "Could not enable RTSP TCP keepalive: errno=%d", errno);
      close(sock);
      continue;
    }
    this->local_ = {};
    length = sizeof(this->local_);
    if (getsockname(sock, reinterpret_cast<sockaddr *>(&this->local_), &length) != 0 ||
        !address_length(this->peer_) || this->local_.ss_family != this->peer_.ss_family) {
      close(sock);
      continue;
    }
    ESP_LOGI(TAG, "Sender connected (%s): %s", this->peer_.ss_family == AF_INET ? "IPv4" : "IPv6",
             address_text(this->peer_).c_str());
    this->abort_.store(false);
    this->has_format_ = false;
    this->encrypted_ = false;
    this->dacp_id_.clear();
    this->active_remote_.clear();
    this->source_->client_connected(this->peer_);
    SocketReader reader(sock);
    while (this->running_.load() && !this->abort_.load()) {
      pollfd session{sock, POLLIN, 0};
      int result = reader.buffered() ? 1 : poll(&session, 1, 100);
      if (reader.buffered()) session.revents = POLLIN;
      if (result < 0 || (result > 0 && !(session.revents & POLLIN))) break;
      // Selecting an output can leave RTSP idle before ANNOUNCE/SETUP.
      // Native TCP keepalive detects vanished clients without expiring a live selection.
      if (result == 0) continue;
      if (!this->handle_request_(sock, reader)) {
        ESP_LOGD(TAG, "RTSP connection ended while reading or responding to a request");
        break;
      }
    }
    this->cleanup_audio_();
    close(sock);
    this->source_->client_disconnected();
    ESP_LOGD(TAG, "Sender disconnected");
  }
  for (auto &listener : this->listeners_) {
    if (listener >= 0) close(listener);
    listener = -1;
  }
  mdns_service_remove("_raop", "_tcp");
  this->running_.store(false);
}

bool RaopServer::handle_request_(int sock, SocketReader &reader) {
  auto deadline = SocketReader::Clock::now() + std::chrono::seconds(5);
  std::string line, method, body;
  if (!reader.read_line(line, deadline)) return false;
  method = line.substr(0, line.find(' '));
  std::map<std::string, std::string> headers;
  size_t total = line.size();
  while (true) {
    if (!reader.read_line(line, deadline)) return false;
    if (line.empty()) break;
    total += line.size();
    size_t colon = line.find(':');
    if (total > 8192 || headers.size() >= 31 || colon == std::string::npos) return false;
    std::string key = line.substr(0, colon);
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return std::tolower(c); });
    size_t start = line.find_first_not_of(" \t", colon + 1);
    headers[key] = start == std::string::npos ? "" : line.substr(start);
  }
  if (!headers.count("cseq")) return false;
  if (this->remote_control_ && !this->audio_running_.load()) {
    if (headers.count("dacp-id")) this->dacp_id_ = headers["dacp-id"];
    if (headers.count("active-remote")) this->active_remote_ = headers["active-remote"];
  }
  if (headers.count("content-length")) {
    const auto &text = headers["content-length"];
    char *end;
    unsigned long size = std::strtoul(text.c_str(), &end, 10);
    if (text.empty() || text[0] == '-' || *end || size > 16384) return false;
    body.resize(size);
    if (!reader.read_exact(body.data(), size, deadline)) return false;
  }
  int status = 200;
  std::string extra;
  if (method != "SET_PARAMETER" && method != "GET_PARAMETER")
    ESP_LOGD(TAG, "RTSP %s", method.c_str());
  if (headers.count("apple-challenge")) {
    std::vector<uint8_t> challenge, response;
    if (!decode_base64(headers["apple-challenge"], challenge) ||
        !append_challenge_address(challenge, this->local_, this->mac_)) return false;
    if (!rsa_operation(challenge, true, response)) return false;
    extra += "Apple-Response: " + encode_base64(response.data(), response.size()) + "\r\n";
  }
  if (method == "OPTIONS") {
    extra += "Public: ANNOUNCE, SETUP, RECORD, PAUSE, FLUSH, TEARDOWN, OPTIONS, GET_PARAMETER, SET_PARAMETER\r\n";
  } else if (method == "ANNOUNCE") {
    this->cleanup_audio_();
    this->has_format_ = parse_fmtp(sdp_attribute(body, "fmtp"), this->cookie_);
    auto key_text = sdp_attribute(body, "rsaaeskey");
    auto iv_text = sdp_attribute(body, "aesiv");
    this->encrypted_ = !key_text.empty() || !iv_text.empty();
    if (this->encrypted_) {
      std::vector<uint8_t> wrapped, key, iv;
      if (!decode_base64(key_text, wrapped) || !rsa_operation(wrapped, false, key) || key.size() != 16 ||
          !decode_base64(iv_text, iv) || iv.size() != 16) this->has_format_ = false;
      else {
        std::copy(key.begin(), key.end(), this->aes_key_.begin());
        std::copy(iv.begin(), iv.end(), this->aes_iv_.begin());
      }
    }
    if (!this->has_format_) status = 415;
  } else if (method == "SETUP") {
    if (!this->has_format_ || this->audio_running_.load() || !this->setup_audio_(headers["transport"])) status = 503;
    else {
      extra += "Transport: RTP/AVP/UDP;unicast;mode=record;control_port=" + std::to_string(this->ports_[1]) +
        ";timing_port=" + std::to_string(this->ports_[2]) + ";server_port=" + std::to_string(this->ports_[0]) + "\r\n";
      extra += "Session: 1\r\n";
    }
  } else if (method == "RECORD") {
    if (!this->audio_running_.load()) status = 455;
    else {
      this->reset_audio_(headers["rtp-info"]);
      xSemaphoreTake(this->audio_mutex_, portMAX_DELAY);
      this->recording_ = true;
      this->remote_paused_.store(false);
      xSemaphoreGive(this->audio_mutex_);
      this->source_->stream_started();
      extra += "Audio-Latency: 88200\r\n";
    }
  } else if (method == "FLUSH" || method == "PAUSE") {
    if (this->audio_running_.load()) this->reset_audio_(headers["rtp-info"]);
    if (method == "PAUSE") {
      if (this->audio_mutex_) {
        xSemaphoreTake(this->audio_mutex_, portMAX_DELAY);
        this->recording_ = false;
        xSemaphoreGive(this->audio_mutex_);
      }
      this->remote_paused_.store(true);
      this->source_->stream_paused();
    }
  } else if (method == "TEARDOWN") {
    this->cleanup_audio_();
    this->abort_.store(true);
    extra += "Connection: close\r\n";
  } else if (method == "SET_PARAMETER") {
    if (headers["content-type"].starts_with("application/x-dmap-tagged")) {
      if (this->source_->metadata_enabled()) {
        TrackMetadata metadata;
        if (parse_track_metadata(body, metadata)) {
          uint32_t root_tag = body.size() >= 4 ? read_be32(reinterpret_cast<const uint8_t *>(body.data())) : 0;
          ESP_LOGD(TAG, "Metadata: %u bytes, root=0x%08x, fields=0x%02x, text lengths=%u/%u/%u (title/artist/album)",
                   unsigned(body.size()), unsigned(root_tag), unsigned(metadata.fields),
                   unsigned(metadata.title.size()), unsigned(metadata.artist.size()), unsigned(metadata.album.size()));
          if (metadata.fields) {
            if (metadata.title.empty()) metadata.title = "Unknown";
            if (metadata.artist.empty()) metadata.artist = "Unknown";
            if (metadata.album.empty()) metadata.album = "Unknown";
            this->source_->sender_metadata(std::move(metadata));
          }  // Metadata-only/status packets must not erase the current track.
        } else {
          ESP_LOGW(TAG, "Rejected malformed track metadata (%u bytes)", unsigned(body.size()));
          status = 400;
        }
      }
    } else if (body.starts_with("volume:")) {
      float volume;
      bool muted;
      if (parse_volume(body.substr(7), volume, muted)) this->source_->sender_volume(volume, muted);
      else status = 400;
    }
  } else if (method != "GET_PARAMETER") {
    status = 405;
  }
  return send_all(sock, "RTSP/1.0 " + std::to_string(status) + (status == 200 ? " OK\r\n" : " Error\r\n") +
      "CSeq: " + headers["cseq"] + "\r\nAudio-Jack-Status: connected; type=analog\r\n" + extra + "\r\n");
}

bool RaopServer::setup_audio_(const std::string &transport) {
  if (transport.find("RTP/AVP/UDP") == std::string::npos ||
      !parse_port(transport, "control_port", this->control_port_) ||
      !parse_port(transport, "timing_port", this->timing_port_)) return false;
  this->frames_ = this->allocator_.allocate(this->buffer_frames_);
  this->audio_done_ = xSemaphoreCreateBinary();
  this->playback_done_ = xSemaphoreCreateBinary();
  this->audio_mutex_ = xSemaphoreCreateMutex();
  esp_alac_dec_cfg_t config{this->cookie_.data(), uint32_t(this->cookie_.size())};
  if (!this->frames_ || !this->audio_done_ || !this->playback_done_ || !this->audio_mutex_ ||
      esp_alac_dec_open(&config, sizeof(config), &this->decoder_) != ESP_AUDIO_ERR_OK) {
    this->cleanup_audio_();
    return false;
  }
  for (size_t i = 0; i < this->buffer_frames_; ++i) this->frames_[i].ready = false;
  for (size_t i = 0; i < this->sockets_.size(); ++i) {
    this->sockets_[i] = socket(this->peer_.ss_family, SOCK_DGRAM, 0);
    auto address = this->local_;
    set_address_port(address, 0);
    socklen_t length = address_length(address);
    if (this->sockets_[i] < 0 || bind(this->sockets_[i], reinterpret_cast<sockaddr *>(&address), length) != 0 ||
        getsockname(this->sockets_[i], reinterpret_cast<sockaddr *>(&address), &length) != 0) {
      this->cleanup_audio_();
      return false;
    }
    this->ports_[i] = address_port(address);
  }
  this->recording_ = this->have_sequence_ = this->have_timing_ = this->have_sync_ = false;
  this->received_packets_.fill(0);
  this->decoded_frames_ = 0;
  this->decode_errors_.store(0);
  this->late_frames_.store(0);
  this->output_drops_.store(0);
  this->concealed_frames_.store(0);
  this->written_bytes_.store(0);
  ESP_LOGI(TAG, "UDP receiver ports: audio=%u control=%u timing=%u; sender control=%u timing=%u",
           this->ports_[0], this->ports_[1], this->ports_[2], this->control_port_, this->timing_port_);
  this->timing_request_ms_ = millis() - 3000;
  this->resend_request_ms_ = millis() - 100;
  this->audio_running_.store(true);
  if (xTaskCreate(audio_task, "airplay_rtp", 8192, this, 4, &this->audio_task_) != pdPASS) {
    this->audio_running_.store(false);
    this->cleanup_audio_();
    return false;
  }
  if (xTaskCreate(playback_task, "airplay_pcm", 6144, this, 4, &this->playback_task_) != pdPASS) {
    this->cleanup_audio_();
    return false;
  }
  this->session_active_.store(true);
  if (this->remote_control_) {
    if (!valid_dacp_credentials(this->dacp_id_, this->active_remote_)) {
      ESP_LOGI(TAG, "DACP unavailable: sender did not provide valid remote-control headers; local controls only");
    } else {
      this->remote_done_ = xSemaphoreCreateBinary();
      this->remote_command_.store(0);
      xQueueReset(this->remote_tracks_);
      this->remote_paused_.store(false);
      this->remote_running_.store(true);
      if (!this->remote_done_ ||
          xTaskCreate(remote_task, "airplay_dacp", 4096, this, 2, &this->remote_task_) != pdPASS) {
        this->remote_running_.store(false);
        if (this->remote_done_) vSemaphoreDelete(this->remote_done_);
        this->remote_done_ = nullptr;
        ESP_LOGW(TAG, "DACP worker could not start; local controls only");
      }
    }
  }
  return true;
}

void RaopServer::cleanup_audio_() {
  this->audio_running_.store(false);
  this->remote_running_.store(false);
  if (this->audio_task_) {
    xSemaphoreTake(this->audio_done_, portMAX_DELAY);
    vTaskDelete(this->audio_task_);
    this->audio_task_ = nullptr;
  }
  if (this->playback_task_) {
    xSemaphoreTake(this->playback_done_, portMAX_DELAY);
    vTaskDelete(this->playback_task_);
    this->playback_task_ = nullptr;
  }
  if (this->remote_task_) {
    xSemaphoreTake(this->remote_done_, portMAX_DELAY);
    vTaskDelete(this->remote_task_);
    this->remote_task_ = nullptr;
  }
  if (this->remote_done_) vSemaphoreDelete(this->remote_done_);
  this->remote_done_ = nullptr;
  this->remote_available_.store(false);
  this->remote_paused_.store(false);
  this->remote_command_.store(0);
  if (this->remote_tracks_) xQueueReset(this->remote_tracks_);
  for (auto &socket : this->sockets_) {
    if (socket >= 0) close(socket);
    socket = -1;
  }
  if (this->decoder_) esp_alac_dec_close(this->decoder_);
  this->decoder_ = nullptr;
  if (this->frames_) this->allocator_.deallocate(this->frames_, this->buffer_frames_);
  this->frames_ = nullptr;
  if (this->audio_done_) vSemaphoreDelete(this->audio_done_);
  if (this->playback_done_) vSemaphoreDelete(this->playback_done_);
  if (this->audio_mutex_) vSemaphoreDelete(this->audio_mutex_);
  this->audio_done_ = this->playback_done_ = this->audio_mutex_ = nullptr;
  this->session_active_.store(false);
  this->source_->stream_ended();
}

void RaopServer::reset_audio_(const std::string &rtp_info) {
  xSemaphoreTake(this->audio_mutex_, portMAX_DELAY);
  esp_alac_dec_reset(this->decoder_);
  this->audio_generation_.fetch_add(1);
  for (size_t i = 0; i < this->buffer_frames_; ++i) this->frames_[i].ready = false;
  this->have_sequence_ = false;
  uint16_t sequence;
  if (parse_parameter(rtp_info, "seq", sequence)) {
    this->read_sequence_ = sequence;
    this->latest_sequence_ = uint16_t(sequence - 1);
    this->have_sequence_ = true;
  }
  xSemaphoreGive(this->audio_mutex_);
}

void RaopServer::audio_task(void *arg) {
  auto *server = static_cast<RaopServer *>(arg);
  server->receive_audio_();
  xSemaphoreGive(server->audio_done_);
  vTaskSuspend(nullptr);  // RTSP task joins and deletes this task before releasing session memory.
}

void RaopServer::playback_task(void *arg) {
  auto *server = static_cast<RaopServer *>(arg);
  while (server->audio_running_.load() && !server->abort_.load()) {
    // Drain all due frames after a scheduling delay. Speaker backpressure must
    // never prevent the separate RTP task from receiving UDP packets.
    if (!server->play_frame_()) vTaskDelay(pdMS_TO_TICKS(1));
  }
  xSemaphoreGive(server->playback_done_);
  vTaskSuspend(nullptr);
}

void RaopServer::receive_audio_() {
  uint32_t last_packet = millis();
  uint32_t last_report = last_packet;
  while (this->audio_running_.load() && !this->abort_.load()) {
    std::array<pollfd, 3> fds{};
    for (size_t i = 0; i < fds.size(); ++i) fds[i] = {this->sockets_[i], POLLIN, 0};
    int result = poll(fds.data(), fds.size(), 5);
    if (result < 0) {
      ESP_LOGW(TAG, "UDP poll failed: errno=%d", errno);
      break;
    }
    for (size_t i = 0; i < fds.size(); ++i) {
      if (fds[i].revents & POLLIN) {
        if (this->receive_packet_(i)) last_packet = millis();
      }
    }
    if (millis() - this->timing_request_ms_ >= 3000) this->request_timing_();
    if (millis() - last_report >= 5000) {
      ESP_LOGD(TAG, "Audio progress: UDP=%u/%u/%u timing=%s sync=%s decoded=%u errors=%u late=%u concealed=%u output_drops=%u consumed=%u bytes",
               unsigned(this->received_packets_[0]), unsigned(this->received_packets_[1]),
               unsigned(this->received_packets_[2]), this->have_timing_ ? "yes" : "no",
               this->have_sync_ ? "yes" : "no", unsigned(this->decoded_frames_),
               unsigned(this->decode_errors_.load()), unsigned(this->late_frames_.load()),
               unsigned(this->concealed_frames_.load()), unsigned(this->output_drops_.load()),
               unsigned(this->written_bytes_.load()));
      last_report = millis();
    }
    if (this->remote_paused_.load()) last_packet = millis();
    // An established sender can remain selected without producing audio yet.
    if (udp_session_expired(this->decoded_frames_ != 0, this->remote_paused_.load(), millis(), last_packet,
                            this->session_timeout_ms_)) {
      ESP_LOGW(TAG, "Ending session: no UDP packets received for %u ms", unsigned(this->session_timeout_ms_));
      this->abort_.store(true);
      break;
    }
  }
  // A failed UDP task must release the session, even if RTSP keepalives continue.
  if (this->audio_running_.load()) this->abort_.store(true);
}

uint16_t RaopServer::discover_remote_() {
  mdns_result_t *results = nullptr;
  uint16_t port = 0;
  if (mdns_query_ptr("_dacp", "_tcp", 1000, 32, &results) == ESP_OK) {
    std::string id = this->dacp_id_;
    std::transform(id.begin(), id.end(), id.begin(), [](unsigned char c) { return std::tolower(c); });
    for (auto *result = results; result && !port; result = result->next) {
      if (!result->instance_name || !result->port) continue;
      std::string instance = result->instance_name;
      std::transform(instance.begin(), instance.end(), instance.begin(), [](unsigned char c) { return std::tolower(c); });
      if (!instance.ends_with(id)) continue;
      for (auto *address = result->addr; address; address = address->next) {
        // Only control the device that established this RAOP session.
        sockaddr_storage candidate{};
        if (address->addr.type == ESP_IPADDR_TYPE_V4) {
          auto &v4 = reinterpret_cast<sockaddr_in &>(candidate);
          v4.sin_family = AF_INET;
          v4.sin_addr.s_addr = address->addr.u_addr.ip4.addr;
        }
#if LWIP_IPV6
        else if (address->addr.type == ESP_IPADDR_TYPE_V6) {
          auto &v6 = reinterpret_cast<sockaddr_in6 &>(candidate);
          v6.sin6_family = AF_INET6;
          std::memcpy(&v6.sin6_addr, address->addr.u_addr.ip6.addr, sizeof(v6.sin6_addr));
          if (IN6_IS_ADDR_LINKLOCAL(&v6.sin6_addr) && result->esp_netif) {
            int index = esp_netif_get_netif_impl_index(result->esp_netif);
            if (index > 0) v6.sin6_scope_id = index;
          }
        }
#endif
        if (same_host(candidate, this->peer_)) {
          port = result->port;
          break;
        }
      }
    }
  }
  mdns_query_results_free(results);
  this->remote_available_.store(port != 0);
  if (port) ESP_LOGI(TAG, "DACP endpoint found on sender port %u", port);
  else ESP_LOGI(TAG, "DACP endpoint not found; local controls remain available");
  return port;
}

bool RaopServer::send_remote_command_(uint16_t port, const char *command) {
  int sock = socket(this->peer_.ss_family, SOCK_STREAM, 0);
  if (sock < 0) return false;
  auto target = this->peer_;
  set_address_port(target, port);
  bool connected = false;
  if (fcntl(sock, F_SETFL, O_NONBLOCK) == 0) {
    int result = connect(sock, reinterpret_cast<sockaddr *>(&target), address_length(target));
    connected = result == 0;
    if (result < 0 && errno == EINPROGRESS) {
      pollfd fd{sock, POLLOUT, 0};
      int error = 0;
      socklen_t size = sizeof(error);
      connected = poll(&fd, 1, 1500) > 0 &&
                  getsockopt(sock, SOL_SOCKET, SO_ERROR, &error, &size) == 0 && error == 0;
    }
  }
  unsigned status = 0;
  if (connected && this->remote_running_.load()) {
    std::string request = std::string("GET /ctrl-int/1/") + command + " HTTP/1.0\r\nHost: " +
        http_host(target) + "\r\nActive-Remote: " +
        this->active_remote_ + "\r\nConnection: close\r\n\r\n";
    std::string response;
    SocketReader reader(sock);
    if (send_all(sock, request) && reader.read_line(response, SocketReader::Clock::now() + std::chrono::seconds(1)))
      status = dacp_status_code(response);
  }
  close(sock);
  bool success = status >= 200 && status < 300;
  if (success) ESP_LOGI(TAG, "DACP %s accepted (HTTP %u)", command, status);
  else ESP_LOGW(TAG, "DACP %s failed (HTTP %u); local control retained", command, status);
  return success;
}

void RaopServer::request_remote_track(RemoteTrackCommand command) {
  if (!this->remote_running_.load() || !this->session_active_.load()) return;
  if (xQueueSend(this->remote_tracks_, &command, 0) != pdTRUE)
    ESP_LOGW(TAG, "DACP track queue full; skip rejected");
}

void RaopServer::remote_task(void *arg) {
  auto *server = static_cast<RaopServer *>(arg);
  uint16_t port = server->discover_remote_();
  while (server->remote_running_.load() && !server->abort_.load()) {
    int command = server->remote_command_.exchange(0);
    const char *path = command ? (command == 2 ? "play" : "pause") : nullptr;
    RemoteTrackCommand track;
    if (!path && xQueueReceive(server->remote_tracks_, &track, 0) == pdTRUE)
      path = dacp_track_command(track);
    if (path) {
      if (!port) port = server->discover_remote_();
      if (port && server->send_remote_command_(port, path)) {
        if (command) server->remote_paused_.store(command == 1);
      } else {
        server->remote_available_.store(false);
        port = 0;  // Retry discovery on the next explicit user command.
      }
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
  xSemaphoreGive(server->remote_done_);
  vTaskSuspend(nullptr);
}

void RaopServer::request_timing_() {
  std::array<uint8_t, 32> request{};
  request[0] = 0x80;
  request[1] = 0xd2;
  request[3] = 7;
  this->timing_request_ms_ = millis();
  write_be32(request.data() + 28, this->timing_request_ms_);
  auto target = this->peer_;
  set_address_port(target, this->timing_port_);
  sendto(this->sockets_[2], request.data(), request.size(), MSG_DONTWAIT,
         reinterpret_cast<sockaddr *>(&target), address_length(target));
}

void RaopServer::request_resend_(uint16_t first, uint16_t count) {
  if (!count || count > this->buffer_frames_) return;
  std::array<uint8_t, 8> request{0x80, 0xd5, 0, 1, uint8_t(first >> 8), uint8_t(first),
                              uint8_t(count >> 8), uint8_t(count)};
  auto target = this->peer_;
  set_address_port(target, this->control_port_);
  sendto(this->sockets_[1], request.data(), request.size(), MSG_DONTWAIT,
         reinterpret_cast<sockaddr *>(&target), address_length(target));
}

bool RaopServer::receive_packet_(size_t index) {
  std::array<uint8_t, 2048> packet{};
  sockaddr_storage from{};
  socklen_t length = sizeof(from);
  int received = recvfrom(this->sockets_[index], packet.data(), packet.size(), MSG_DONTWAIT,
                          reinterpret_cast<sockaddr *>(&from), &length);
  if (received <= 0 || !same_host(from, this->peer_) ||
      !valid_raop_packet(packet.data(), size_t(received), index)) return false;
  if (this->received_packets_[index]++ == 0)
    ESP_LOGD(TAG, "First UDP packet on socket %u: %d bytes from %s", unsigned(index), received,
             address_text(from).c_str());
  uint8_t type = packet[1] & 0x7f;
  auto *data = packet.data();
  if (type == 0x53 && received >= 32 && index == 2) {
    uint32_t reference = read_be32(data + 12);
    if (millis() - reference > 100) return false;
    xSemaphoreTake(this->audio_mutex_, portMAX_DELAY);
    this->remote_ntp_ = (uint64_t(read_be32(data + 16)) << 32) | read_be32(data + 20);
    this->local_ms_ = reference + (millis() - reference) / 2;
    this->have_timing_ = true;
    xSemaphoreGive(this->audio_mutex_);
    return true;
  }
  if (type == 0x54 && received >= 20 && index == 1 && this->have_timing_) {
    uint64_t ntp = (uint64_t(read_be32(data + 8)) << 32) | read_be32(data + 12);
    int64_t delta = int64_t(ntp - this->remote_ntp_);
    int64_t gap_ms = (delta / 65536) * 1000 / 65536;
    if (gap_ms < -10000 || gap_ms > 10000) return false;
    uint32_t latency = read_be32(data + 16) - read_be32(data + 4);
    uint16_t flags = read_be16(data + 2);
    if (flags == 7 || flags == 4) latency += 11025;
    latency = std::clamp(latency, uint32_t(11025), uint32_t(105840));
    xSemaphoreTake(this->audio_mutex_, portMAX_DELAY);
    this->sync_rtp_ = read_be32(data + 16) - latency;
    this->sync_ms_ = this->local_ms_ + int32_t(gap_ms);
    this->have_sync_ = true;
    xSemaphoreGive(this->audio_mutex_);
    return true;
  }
  // A valid sync packet can precede the first timing reply.
  if (type == 0x54) return true;
  if (type == 0x56) {
    if (received < 16) return false;
    data += 4;
    received -= 4;
  } else if (type != 0x60 || index != 0) return false;
  if (received <= 12 || received == int(packet.size())) return false;
  uint16_t sequence = read_be16(data + 2);
  uint32_t timestamp = read_be32(data + 4);
  data += 12;
  received -= 12;
  xSemaphoreTake(this->audio_mutex_, portMAX_DELAY);
  if (!this->recording_) {
    xSemaphoreGive(this->audio_mutex_);
    return true;
  }
  if (!this->have_sequence_) {
    this->read_sequence_ = sequence;
    this->latest_sequence_ = uint16_t(sequence - 1);
    this->have_sequence_ = true;
  }
  int16_t distance = sequence_delta(sequence, this->read_sequence_);
  if (distance < 0) {
    xSemaphoreGive(this->audio_mutex_);
    return true;
  }
  if (size_t(distance) >= this->buffer_frames_) {
    for (size_t i = 0; i < this->buffer_frames_; ++i) this->frames_[i].ready = false;
    this->read_sequence_ = sequence;
    this->latest_sequence_ = uint16_t(sequence - 1);
  }
  if (this->encrypted_) {
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    auto iv = this->aes_iv_;
    int result = mbedtls_aes_setkey_dec(&aes, this->aes_key_.data(), 128);
    if (result == 0)
      result = mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_DECRYPT, received & ~15, iv.data(), data, data);
    mbedtls_aes_free(&aes);
    if (result != 0) {
      xSemaphoreGive(this->audio_mutex_);
      return false;
    }
  }
  auto &frame = this->frames_[sequence % this->buffer_frames_];
  esp_audio_dec_in_raw_t raw{};
  raw.buffer = data;
  raw.len = received;
  esp_audio_dec_out_frame_t output{};
  output.buffer = frame.pcm.data();
  output.len = frame.pcm.size();
  esp_audio_dec_info_t info{};
  frame.ready = false;
  auto decode_result = esp_alac_dec_decode(this->decoder_, &raw, &output, &info);
  bool decoded = decode_result == ESP_AUDIO_ERR_OK &&
                 valid_pcm_frame(output.decoded_size, info.sample_rate, info.bits_per_sample, info.channel);
  if (decoded) {
    frame.sequence = sequence;
    frame.timestamp = timestamp;
    frame.length = output.decoded_size;
    frame.ready = true;
    this->decoded_frames_++;
  } else if (this->decode_errors_++ == 0) {
    ESP_LOGW(TAG, "ALAC decode rejected: result=%d bytes=%u rate=%u bits=%u channels=%u",
             int(decode_result), unsigned(output.decoded_size), unsigned(info.sample_rate),
             unsigned(info.bits_per_sample), unsigned(info.channel));
  }
  int16_t gap = sequence_delta(sequence, uint16_t(this->latest_sequence_ + 1));
  if (gap > 0 && size_t(gap) < this->buffer_frames_)
    this->request_resend_(uint16_t(this->latest_sequence_ + 1), gap);
  if (sequence_delta(sequence, this->latest_sequence_) > 0) this->latest_sequence_ = sequence;
  xSemaphoreGive(this->audio_mutex_);
  return decoded;
}

bool RaopServer::play_frame_() {
  Frame frame{};
  xSemaphoreTake(this->audio_mutex_, portMAX_DELAY);
  uint32_t generation = this->audio_generation_.load();
  if (!this->have_sync_ || !this->recording_ || !this->have_sequence_ ||
      sequence_delta(this->latest_sequence_, this->read_sequence_) < 0) {
    xSemaphoreGive(this->audio_mutex_);
    return false;
  }
  auto &head = this->frames_[this->read_sequence_ % this->buffer_frames_];
  if (head.ready && head.sequence == this->read_sequence_) frame = head;
  else {
    // Conceal a missing packet on its timeline, using a later packet as the timestamp anchor.
    bool found = false;
    for (uint16_t offset = 1; offset < this->buffer_frames_; ++offset) {
      auto sequence = uint16_t(this->read_sequence_ + offset);
      auto &next = this->frames_[sequence % this->buffer_frames_];
      if (next.ready && next.sequence == sequence) {
        frame.timestamp = next.timestamp - uint32_t(offset) * FRAME_SAMPLES;
        frame.length = FRAME_BYTES;
        found = true;
        break;
      }
    }
    if (!found) {
      xSemaphoreGive(this->audio_mutex_);
      return false;
    }
  }
  int32_t wait = playback_wait_ms(frame.timestamp, this->sync_rtp_, this->sync_ms_, millis(),
                                 this->source_->output_delay_ms());
  if (wait > 5) {
    if (!frame.ready && millis() - this->resend_request_ms_ > 100) {
      this->request_resend_(this->read_sequence_, 1);
      this->resend_request_ms_ = millis();
    }
    xSemaphoreGive(this->audio_mutex_);
    return false;
  }
  head.ready = false;
  this->read_sequence_++;
  xSemaphoreGive(this->audio_mutex_);
  // ponytail: millisecond scheduling; sample-level clock discipline is needed for tighter group sync.
  if (wait < -100) {
    this->late_frames_++;
    return true;
  }
  if (!frame.ready) this->concealed_frames_++;
  size_t offset = 0;
  uint32_t deadline = millis() + 100;
  while (offset < frame.length && this->audio_running_.load() && !this->abort_.load() &&
         generation == this->audio_generation_.load() &&
         time_delta(deadline, millis()) > 0) {
    size_t written = this->source_->write_pcm(frame.pcm.data() + offset, frame.length - offset);
    this->written_bytes_ += written;
    offset += written;
  }
  // Count backpressure loss, excluding cancellation during FLUSH or disconnect.
  if (offset < frame.length && this->audio_running_.load() && !this->abort_.load() &&
      generation == this->audio_generation_.load() && time_delta(deadline, millis()) <= 0)
    this->output_drops_++;
  return true;
}

}  // namespace esphome::airplay
