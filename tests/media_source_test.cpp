#include "../components/airplay/airplay_media_source.h"
#include "esphome/core/hal.h"
#include <cassert>
#include <functional>
#include <future>

using namespace esphome::airplay;
using namespace esphome::media_source;

// Transport is deliberately stubbed; all source lifecycle/output code is compiled unchanged.
bool RaopServer::start(AirplayMediaSource *, const std::string &, size_t) { return true; }
void RaopServer::request_remote_track(RemoteTrackCommand) {}

struct Speaker : esphome::speaker::Speaker {
  unsigned stops{0};
  size_t play(const uint8_t *, size_t length) override { this->start(); return length; }
  void start() override { this->state_ = esphome::speaker::STATE_RUNNING; }
  void stop() override { ++this->stops; this->state_ = esphome::speaker::STATE_STOPPING; }
  bool has_buffered_data() const override { return !this->is_stopped(); }
  void complete_stop() { this->state_ = esphome::speaker::STATE_STOPPED; }
};

struct Listener : MediaSourceListener {
  Speaker speaker;
  unsigned starts{0}, writes{0}, volumes{0}, mutes{0};
  std::function<void()> on_write;
  size_t write_audio(const uint8_t *data, size_t length, uint32_t,
                     const esphome::audio::AudioStreamInfo &) override {
    if (this->on_write) this->on_write();
    ++this->writes;
    return this->speaker.play(data, length);
  }
  void report_state(MediaSourceState) override {}
  void request_play_uri(const std::string &uri) override { assert(uri == "airplay://current"); ++this->starts; }
  void request_volume(float) override { ++this->volumes; }
  void request_mute(bool) override { ++this->mutes; }
  void bind(AirplayMediaSource &source) { source.set_listener(this); source.set_speaker(&this->speaker); }
};

int main() {
  const uint8_t pcm[4]{};
  Listener listener;
  AirplayMediaSource source;
  listener.bind(source);
  source.setup();
  source.sender_volume(0.25f, false);  // Initial volume may precede RECORD.
  source.stream_flushed();
  source.stream_started();
  source.loop();
  assert(listener.starts == 1 && listener.speaker.stops == 0 && listener.volumes == 1);
  assert(source.play_uri("airplay://current"));
  assert(source.write_pcm(pcm, sizeof(pcm)) == sizeof(pcm) && listener.writes == 1);

  // A repeated RECORD flushes the owned speaker without a new URI/implicit STOP.
  source.stream_flushed();
  source.stream_started();
  assert(source.write_pcm(pcm, sizeof(pcm)) == 0);
  source.loop();
  assert(listener.starts == 1 && listener.speaker.stops == 1);
  assert(source.write_pcm(pcm, sizeof(pcm)) == 0);
  source.loop();
  assert(listener.speaker.stops == 1);  // Asynchronous stop is issued exactly once.
  source.stream_flushed();
  source.loop();
  assert(listener.speaker.stops == 1);  // Coalesce another FLUSH during the same stop.
  listener.speaker.complete_stop();
  source.loop();
  assert(source.write_pcm(pcm, sizeof(pcm)) == sizeof(pcm) && listener.writes == 2);

  // An already-stopped speaker needs no queued STOP that could cancel the next start.
  listener.speaker.complete_stop();
  source.stream_flushed();
  source.loop();
  assert(listener.speaker.stops == 1);
  assert(source.write_pcm(pcm, sizeof(pcm)) == sizeof(pcm) && listener.writes == 3);

  // FLUSH preserves local pause; RECORD subsequently resumes the existing pipeline.
  source.handle_command(MediaSourceCommand::PAUSE);
  source.stream_flushed();
  source.loop();
  listener.speaker.complete_stop();
  source.loop();
  assert(source.get_state() == MediaSourceState::PAUSED);
  assert(source.write_pcm(pcm, sizeof(pcm)) == sizeof(pcm) && listener.writes == 3);
  source.stream_flushed();
  source.stream_started();
  source.loop();
  listener.speaker.complete_stop();
  source.loop();
  assert(source.get_state() == MediaSourceState::PLAYING && listener.starts == 1);
  assert(source.write_pcm(pcm, sizeof(pcm)) == sizeof(pcm) && listener.writes == 4);

  // A flush waits for an in-flight native write before requesting speaker stop.
  std::promise<void> writing, release_write;
  auto released = release_write.get_future().share();
  listener.on_write = [&] { writing.set_value(); released.wait(); };
  auto write = std::async(std::launch::async, [&] { source.write_pcm(pcm, sizeof(pcm)); });
  writing.get_future().wait();
  auto flush = std::async(std::launch::async, [&] { source.stream_flushed(); });
  assert(flush.wait_for(std::chrono::milliseconds(5)) == std::future_status::timeout);
  release_write.set_value();
  write.get(); flush.get();
  listener.on_write = {};
  source.loop();

  // STOP during flushing revokes control; delayed transport callbacks cannot revive the source
  // or stop the speaker once another source owns it.
  source.handle_command(MediaSourceCommand::STOP);
  listener.speaker.start();
  auto stops = listener.speaker.stops;
  auto writes = listener.writes;
  source.stream_flushed();
  source.stream_started();
  source.sender_volume(0.8f, false);
  source.sender_volume(0, true);
  esphome::test_ms += 1000;
  source.loop();
  source.handle_command(MediaSourceCommand::PLAY);
  assert(source.get_state() == MediaSourceState::IDLE);
  assert(!source.play_uri("airplay://current"));
  assert(source.write_pcm(pcm, sizeof(pcm)) == sizeof(pcm) && listener.writes == writes);
  assert(listener.volumes == 1 && listener.mutes == 0 && listener.speaker.stops == stops);

  // STOP before the deferred initial URI executes must cancel startup as well.
  Listener pending_listener;
  AirplayMediaSource pending;
  pending_listener.bind(pending);
  pending.stream_started();
  pending.loop();
  assert(pending_listener.starts == 1);
  pending.handle_command(MediaSourceCommand::STOP);
  pending.stream_started();
  pending.loop();
  assert(!pending.play_uri("airplay://current"));
  assert(pending.get_state() == MediaSourceState::IDLE && pending_listener.starts == 1);
}
