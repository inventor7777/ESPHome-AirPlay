#include "../components/airplay/protocol.h"
#include <cassert>

using namespace esphome::airplay;

static std::string dmap(const char *tag, const std::string &value) {
  std::string item(tag, 4);
  std::array<uint8_t, 4> length{};
  write_be32(length.data(), value.size());
  item.append(reinterpret_cast<const char *>(length.data()), length.size());
  return item + value;
}

int main() {
  // FLUSH retains its timestamp sample, including a packet spanning the boundary and RTP wraparound.
  uint32_t timestamp = 0;
  assert(parse_parameter("rtptime=4294967295;seq=65535", "rtptime", timestamp) && timestamp == UINT32_MAX);
  assert(parse_parameter("rtptime=0", "rtptime", timestamp) && timestamp == 0);
  assert(!parse_parameter("rtptime=4294967296", "rtptime", timestamp));
  assert(!parse_parameter("rtptime=-1", "rtptime", timestamp));
  assert(!parse_parameter("rtptime=123junk", "rtptime", timestamp));
  assert(flush_samples(1000, FRAME_SAMPLES, 1000) == 0);
  assert(flush_samples(1001, FRAME_SAMPLES, 1000) == 0);
  assert(flush_samples(1000, FRAME_SAMPLES, 1100) == 100);
  assert(flush_samples(1000, FRAME_SAMPLES, 1352) == FRAME_SAMPLES);
  assert(flush_samples(1000, FRAME_SAMPLES, 2000) == FRAME_SAMPLES);
  assert(flush_samples(UINT32_MAX - 99, FRAME_SAMPLES, 0) == 100);
  assert(flush_samples(0, FRAME_SAMPLES, UINT32_MAX - 99) == 0);

  // Idle output selection and paused senders are governed by TCP keepalive.
  assert(!udp_session_expired(false, false, 60000, 0, 15000));
  assert(!udp_session_expired(true, true, 60000, 0, 15000));
  assert(!udp_session_expired(true, false, 15000, 0, 15000));
  assert(udp_session_expired(true, false, 15001, 0, 15000));
  assert(udp_session_expired(true, false, 15000, UINT32_MAX - 1, 15000));

  TrackMetadata metadata;
  assert(parse_track_metadata(dmap("mlit", dmap("minm", "") + dmap("asar", "") + dmap("asal", "")), metadata));
  assert(metadata.fields == 7 && metadata.title.empty() && metadata.artist.empty() && metadata.album.empty());
  std::string track = dmap("mlit", dmap("minm", "Café 🎵") + dmap("asar", "Artist") +
                          dmap("asal", "Album") + dmap("asdk", "ignored"));
  assert(parse_track_metadata(track, metadata));
  assert(metadata.title == "Café 🎵" && metadata.artist == "Artist" && metadata.album == "Album");
  assert(metadata.fields == 7);
  for (const auto &container : {"mdcl", "mlcl", "cmst"}) {
    assert(parse_track_metadata(dmap(container, track), metadata));
    assert(metadata.title == "Café 🎵" && metadata.album == "Album" && metadata.fields == 7);
  }
  assert(parse_track_metadata(dmap("cmst", dmap("cann", "Now playing") + dmap("cana", "Artist") +
                                      dmap("canl", "Album")), metadata));
  assert(metadata.title == "Now playing" && metadata.artist == "Artist" && metadata.fields == 7);
  assert(parse_track_metadata(dmap("mdcl", dmap("mstt", "status")), metadata) && metadata.fields == 0);
  assert(parse_track_metadata(track, metadata));
  assert(!parse_track_metadata(track.substr(0, track.size() - 1), metadata));
  assert(metadata.title == "Café 🎵" && metadata.album == "Album");
  assert(!parse_track_metadata(track + "short", metadata));
  assert(metadata.album == "Album");
  assert(!parse_track_metadata(dmap("minm", std::string(1025, 'a')), metadata));
  assert(!parse_track_metadata(dmap("minm", std::string("a\0b", 3)), metadata));
  assert(parse_track_metadata(dmap("mlit", dmap("minm", "Next track")), metadata));
  assert(metadata.title == "Next track" && metadata.artist.empty() && metadata.album.empty() && metadata.fields == 1);
  assert(parse_track_metadata(dmap("minm", ""), metadata) && metadata.title.empty() && metadata.fields == 1);
  std::string nested = dmap("minm", "Title");
  for (unsigned i = 0; i < 5; ++i) nested = dmap("mlit", nested);
  assert(!parse_track_metadata(nested, metadata));
  std::string bad_length = dmap("minm", "a");
  std::fill(bad_length.begin() + 4, bad_length.begin() + 8, char(0xff));
  assert(!parse_track_metadata(bad_length, metadata));

  assert(std::string(dacp_track_command(RemoteTrackCommand::NEXT)) == "nextitem");
  assert(std::string(dacp_track_command(RemoteTrackCommand::PREVIOUS)) == "previtem");
  assert(valid_dacp_credentials("ABC0123456789DEF", "4294967295"));
  assert(valid_dacp_credentials("abc0123456789def", "0"));
  assert(!valid_dacp_credentials("", "123"));
  assert(!valid_dacp_credentials("not-hex", "123"));
  assert(!valid_dacp_credentials("ABC", "4294967296"));
  assert(!valid_dacp_credentials("ABC", "123\r\nInjected: yes"));
  assert(!valid_dacp_credentials("ABC", "-1"));
  assert(dacp_status_code("HTTP/1.0 204 No Content") == 204);
  assert(dacp_status_code("HTTP/1.1 200 OK") == 200);
  assert(dacp_status_code("HTTP/1.1 403 Forbidden") == 403);
  for (const auto &bad : {"", "HTTP/1.1 20", "HTTP/1.1 2000 OK", "HTTP/1.1 abc", "RTSP/1.0 200 OK"})
    assert(dacp_status_code(bad) == 0);
  // Regression: successful ALAC frames may omit format metadata after setup.
  assert(valid_pcm_frame(1408, 0, 0, 0));
  assert(valid_pcm_frame(1408, SAMPLE_RATE, 16, 2));
  assert(valid_pcm_frame(1408, SAMPLE_RATE, 0, 0));
  assert(!valid_pcm_frame(0, 0, 0, 0));
  assert(!valid_pcm_frame(1412, 0, 0, 0));
  assert(!valid_pcm_frame(1407, 0, 0, 0));
  assert(!valid_pcm_frame(1408, 48000, 0, 0));
  assert(!valid_pcm_frame(1408, 0, 24, 0));
  assert(!valid_pcm_frame(1408, 0, 0, 1));
  // A delayed worker must drain multiple due packets, then stop at the bounded
  // prefeed window. One packet per 20 ms wakeup cannot sustain 44.1 kHz audio.
  unsigned delivered = 0;
  for (uint32_t now = 0; now <= 5000; now += 20) {
    while (playback_wait_ms(delivered * FRAME_SAMPLES, 0, 0, now, 100) <= 5) {
      assert(++delivered < 1000);
    }
    assert(playback_wait_ms(delivered * FRAME_SAMPLES, 0, 0, now, 100) <= 13);
  }
  assert(delivered >= 625 && delivered <= 641);
  assert(playback_wait_ms(0, 0, UINT32_MAX - 4, 5, 0) == -10);
  std::array<uint8_t, 24> cookie{};
  assert(parse_fmtp("96 352 0 16 40 10 14 2 255 0 0 44100", cookie));
  assert(read_be32(cookie.data()) == 352);
  assert(cookie[5] == 16 && cookie[9] == 2);
  assert(read_be32(cookie.data() + 20) == 44100);
  for (const auto &bad : {"", "96 352", "96 352 0 24 40 10 14 2 255 0 0 44100",
      "96 352 0 16 40 10 14 2 255 0 0 48000", "96 4096 0 16 40 10 14 2 255 0 0 44100",
      "96 352 0 16 40 10 14 2 255 0 0 44100 1", "96 352 0 16 999 10 14 2 255 0 0 44100",
      "96 352 0 16 40 10 14 2 255 4294967296 0 44100",
      "96 352 0 16 40 10 14 2 255 999999999999999999999999999 0 44100"})
    assert(!parse_fmtp(bad, cookie));
  uint16_t port = 0;
  assert(parse_port("RTP/AVP/UDP;control_port=6001;timing_port=6002", "control_port", port) && port == 6001);
  assert(!parse_port("control_port=0", "control_port", port));
  assert(!parse_port("control_port=65536", "control_port", port));
  assert(!parse_port("control_port=-1", "control_port", port));
  assert(!parse_port("control_port=2oops", "control_port", port));
  assert(!parse_port("fake_control_port=6001", "control_port", port));
  assert(!parse_port("control_port=+6001", "control_port", port));
  assert(!parse_port("control_port=999999999999999999999", "control_port", port));
  assert(parse_port("fake_control_port=2;control_port=6001", "control_port", port) && port == 6001);
  uint16_t sequence = 7;
  assert(parse_parameter("seq=0;rtptime=123", "seq", sequence) && sequence == 0);
  assert(parse_parameter("rtptime=123; seq=65535", "seq", sequence) && sequence == 65535);
  assert(!parse_parameter("seq=123oops", "seq", sequence));
  assert(!parse_parameter("seq=-1", "seq", sequence));
  assert(!parse_parameter("seq=65536", "seq", sequence));
  assert(!parse_parameter("notseq=123", "seq", sequence));
  float volume = -1;
  bool muted = false;
  assert(parse_volume("-144\r\n", volume, muted) && muted);
  assert(parse_volume("-30", volume, muted) && volume == 0 && !muted);
  assert(parse_volume("-15", volume, muted) && volume == 0.5f && !muted);
  assert(parse_volume("0", volume, muted) && volume == 1 && !muted);
  for (const auto &bad : {"", "nan", "inf", "1", "-31", "-15junk"}) assert(!parse_volume(bad, volume, muted));
  std::array<uint8_t, 32> packet{};
  packet[0] = 0x80;
  packet[1] = 0xd3;
  assert(valid_raop_packet(packet.data(), 32, 2));
  assert(!valid_raop_packet(packet.data(), 31, 2));
  assert(!valid_raop_packet(packet.data(), 32, 0));
  packet[1] = 0xd4;
  assert(valid_raop_packet(packet.data(), 20, 1));
  packet[0] = 0x90;
  assert(valid_raop_packet(packet.data(), 20, 1));
  packet[1] = 0x54;
  assert(valid_raop_packet(packet.data(), 20, 1));
  assert(!valid_raop_packet(packet.data(), 20, 0));
  assert(!valid_raop_packet(packet.data(), 20, 2));
  packet[1] = 0xd3;
  assert(!valid_raop_packet(packet.data(), 32, 2));
  packet[1] = 0x60;
  assert(!valid_raop_packet(packet.data(), 20, 0));
  packet[0] = 0x80;
  packet[1] = 0xd4;
  assert(!valid_raop_packet(packet.data(), 19, 1));
  packet[1] = 0x60;
  assert(valid_raop_packet(packet.data(), 13, 0));
  assert(!valid_raop_packet(packet.data(), 12, 0));
  assert(!valid_raop_packet(packet.data(), 2048, 0));
  packet[0] = 0x40;
  assert(!valid_raop_packet(packet.data(), 13, 0));
  packet[0] = 0x80;
  packet[1] = 0xd6;
  packet[4] = 0x80;
  packet[5] = 0x60;
  assert(valid_raop_packet(packet.data(), 17, 1));
  assert(!valid_raop_packet(packet.data(), 16, 1));
  assert(!valid_raop_packet(packet.data(), 17, 2));
  packet[5] = 0xd3;
  assert(!valid_raop_packet(packet.data(), 17, 1));
  assert(sequence_delta(0, 65535) == 1);
  assert(sequence_delta(65535, 0) == -1);
  assert(time_delta(5, UINT32_MAX - 4) == 10);
  assert(time_delta(UINT32_MAX - 4, 5) == -10);
}
