#pragma once

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>

namespace esphome::airplay {

constexpr uint32_t SAMPLE_RATE = 44100;
constexpr size_t FRAME_SAMPLES = 352;
constexpr size_t FRAME_BYTES = FRAME_SAMPLES * 4;

inline uint16_t read_be16(const uint8_t *p) { return (uint16_t(p[0]) << 8) | p[1]; }
inline uint32_t read_be32(const uint8_t *p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
inline void write_be32(uint8_t *p, uint32_t value) {
  p[0] = value >> 24;
  p[1] = value >> 16;
  p[2] = value >> 8;
  p[3] = value;
}
inline int16_t sequence_delta(uint16_t a, uint16_t b) { return int16_t(uint16_t(a - b)); }
inline int32_t time_delta(uint32_t a, uint32_t b) { return int32_t(a - b); }

// RTP timestamps count samples; retain the sample at the FLUSH boundary.
inline size_t flush_samples(uint32_t timestamp, size_t samples, uint32_t boundary) {
  int32_t delta = time_delta(boundary, timestamp);
  return delta > 0 ? std::min(size_t(delta), samples) : 0;
}

inline bool udp_session_expired(bool audio_started, bool paused, uint32_t now, uint32_t last_packet,
                                uint32_t timeout) {
  return audio_started && !paused && now - last_packet > timeout;
}

inline int32_t playback_wait_ms(uint32_t timestamp, uint32_t sync_rtp, uint32_t sync_ms,
                               uint32_t now, uint32_t prefeed_ms) {
  uint32_t playtime = sync_ms + int64_t(time_delta(timestamp, sync_rtp)) * 1000 / SAMPLE_RATE;
  return time_delta(playtime, now + prefeed_ms);
}

// The negotiated ALAC cookie fixes the format. The decoder may omit unchanged
// format fields on later frames; reject explicit mismatches, not absent metadata.
inline bool valid_pcm_frame(size_t bytes, uint32_t rate, uint8_t bits, uint8_t channels) {
  return bytes > 0 && bytes <= FRAME_BYTES && bytes % 4 == 0 &&
         (rate == 0 || rate == SAMPLE_RATE) && (bits == 0 || bits == 16) &&
         (channels == 0 || channels == 2);
}

// AirPlay 1's ALAC magic cookie is 24 bytes in network order.
inline bool parse_fmtp(const std::string &text, std::array<uint8_t, 24> &cookie) {
  std::array<uint32_t, 12> values{};
  const char *p = text.c_str();
  const char *limit = p + text.size();
  for (auto &value : values) {
    while (*p == ' ' || *p == '\t') ++p;
    if (*p < '0' || *p > '9') return false;
    auto result = std::from_chars(p, limit, value);
    if (result.ec != std::errc{}) return false;
    p = result.ptr;
  }
  while (*p == ' ' || *p == '\t') ++p;
  if (*p || values[1] != FRAME_SAMPLES || values[2] != 0 || values[3] != 16 || values[7] != 2 ||
      values[11] != SAMPLE_RATE || values[4] > 255 || values[5] > 255 || values[6] > 255 || values[8] > 65535)
    return false;
  write_be32(cookie.data(), values[1]);
  for (size_t i = 0; i < 6; ++i) cookie[4 + i] = values[2 + i];
  cookie[10] = values[8] >> 8;
  cookie[11] = values[8];
  write_be32(cookie.data() + 12, values[9]);
  write_be32(cookie.data() + 16, values[10]);
  write_be32(cookie.data() + 20, values[11]);
  return true;
}

inline bool parse_volume(const std::string &text, float &volume, bool &muted) {
  char *end;
  float db = std::strtof(text.c_str(), &end);
  if (end == text.c_str() || !std::isfinite(db)) return false;
  while (*end == ' ' || *end == '\r' || *end == '\n') ++end;
  if (*end || (db != -144.0f && (db < -30.0f || db > 0.0f))) return false;
  muted = db == -144.0f;
  volume = muted ? 0.0f : std::clamp(1.0f + db / 30.0f, 0.0f, 1.0f);
  return true;
}

// The first RAOP sync packet uses 0x90; audio still uses a fixed RTP header.
inline bool valid_raop_packet(const uint8_t *packet, size_t size, size_t socket_index) {
  if (size < 2 || size >= 2048) return false;
  if (packet[0] == 0x90) return socket_index == 1 && (packet[1] & 0x7f) == 0x54 && size == 20;
  if (packet[0] != 0x80) return false;
  switch (packet[1] & 0x7f) {
    case 0x53: return socket_index == 2 && size == 32;  // Timing reply.
    case 0x54: return socket_index == 1 && size == 20;  // Clock synchronization.
    case 0x60: return socket_index == 0 && size > 12;  // Audio.
    case 0x56:  // Retransmission contains a complete audio RTP packet after its four-byte header.
      return socket_index <= 1 && size > 16 && packet[4] == 0x80 && (packet[5] & 0x7f) == 0x60;
    default: return false;
  }
}

template<typename T> inline bool parse_parameter(const std::string &text, const char *key, T &value) {
  const std::string prefix = std::string(key) + "=";
  size_t pos = 0;
  while ((pos = text.find(prefix, pos)) != std::string::npos) {
    if (pos == 0 || text[pos - 1] == ';' || text[pos - 1] == ' ' || text[pos - 1] == '\t') {
      const char *start = text.data() + pos + prefix.size();
      const char *limit = text.data() + text.size();
      T parsed;
      auto result = std::from_chars(start, limit, parsed);
      if (result.ec != std::errc{} ||
          (result.ptr != limit && *result.ptr != ';' && *result.ptr != ' ' && *result.ptr != '\t'))
        return false;
      value = parsed;
      return true;
    }
    pos += prefix.size();
  }
  return false;
}

inline bool parse_port(const std::string &transport, const char *key, uint16_t &port) {
  return parse_parameter(transport, key, port) && port != 0;
}

inline bool valid_dacp_credentials(const std::string &id, const std::string &token) {
  if (id.empty() || id.size() > 32 || token.empty() || token.size() > 10) return false;
  for (char c : id)
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
  uint32_t number;
  auto result = std::from_chars(token.data(), token.data() + token.size(), number);
  return result.ec == std::errc{} && result.ptr == token.data() + token.size();
}

struct TrackMetadata {
  std::string title;
  std::string artist;
  std::string album;
  uint8_t fields{0};  // Presence distinguishes omitted metadata from explicitly empty fields.
};

// DMAP is a sequence of four-byte tags, big-endian lengths, and payloads.
// Standard listing/dictionary/status containers can carry either track or now-playing tags.
inline bool parse_dmap_fields(std::string_view data, TrackMetadata &metadata, unsigned depth = 0) {
  if (depth > 4) return false;
  while (!data.empty()) {
    if (data.size() < 8) return false;
    auto tag = data.substr(0, 4);
    size_t length = read_be32(reinterpret_cast<const uint8_t *>(data.data() + 4));
    data.remove_prefix(8);
    if (length > data.size()) return false;
    auto value = data.substr(0, length);
    if (tag == "mlit" || tag == "mlcl" || tag == "mdcl" || tag == "cmst") {
      if (!parse_dmap_fields(value, metadata, depth + 1)) return false;
    } else {
      uint8_t field_mask = (tag == "minm" || tag == "cann") ? 1 :
                           (tag == "asar" || tag == "cana") ? 2 :
                           (tag == "asal" || tag == "canl") ? 4 : 0;
      std::string *field = field_mask == 1 ? &metadata.title : field_mask == 2 ? &metadata.artist :
                           field_mask == 4 ? &metadata.album : nullptr;
      if (field) {
        if (length > 1024 || value.find('\0') != std::string_view::npos) return false;
        field->assign(value.data(), value.size());
        metadata.fields |= field_mask;
      }
    }
    data.remove_prefix(length);
  }
  return true;
}

inline bool parse_track_metadata(std::string_view data, TrackMetadata &metadata) {
  TrackMetadata next;
  if (!parse_dmap_fields(data, next)) return false;
  metadata = std::move(next);  // Malformed requests never replace the previous track.
  return true;
}

enum class RemoteTrackCommand : uint8_t { NEXT, PREVIOUS };

inline const char *dacp_track_command(RemoteTrackCommand command) {
  return command == RemoteTrackCommand::NEXT ? "nextitem" : "previtem";
}

inline unsigned dacp_status_code(const std::string &line) {
  if (line.size() < 12 || (line.compare(0, 9, "HTTP/1.0 ") && line.compare(0, 9, "HTTP/1.1 ")) ||
      (line.size() > 12 && line[12] != ' ')) return 0;
  unsigned status = 0;
  auto result = std::from_chars(line.data() + 9, line.data() + 12, status);
  return result.ec == std::errc{} && result.ptr == line.data() + 12 && status >= 100 && status <= 599 ? status : 0;
}

}  // namespace esphome::airplay
