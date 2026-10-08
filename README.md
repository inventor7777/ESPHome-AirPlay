# ESPHome AirPlay media source

Seamlessly and easily allow your ESPHome ESP32s to become Classic AirPlay (AirPlay 1 / RAOP) receivers using native ESPHome APIs.

This custom component implements AirPlay inside ESPHome's native `media_source::MediaSource`, with decoded PCM going through a `speaker_source` media player, just like official components such as Sendspin. It integrates seamlessly with your existing ESPHome config and audio outputs with no need to modify hardware. It also implements basic DACP support, which allows basic playback control of the sender.

AirPlay 2 is *not* supported, as I did not feel comfortable trying to implement the advanced features, which are the only reason to choose AirPlay 2 anyway. If you need synchronized AirPlay on ESP32s, I'd probably use a bare metal firmware such as [this one](https://github.com/rbouteiller/airplay-esp32), or if you would prefer to keep ESPHome, try the [native Sendspin component](https://esphome.io/components/sendspin/), as it's built for use cases like this.

## Requirements

- ESPHome 2026.9.1 or newer, with support for the `media_source` and `speaker_source` APIs.
- ESP32 or ESP32-S3 using ESP-IDF with PSRAM enabled.
- An ESPHome speaker that accepts 44.1 kHz, 16-bit stereo. Use a native resampler speaker if the output requires another rate.
- Network supporting mDNS.

## Configuration example
This isn't a standalone config, just an example. You'll need a working speaker first, then you can simply set AirPlay as one of the native `media_source`s.

```yaml
external_components:
  - source: github://inventor7777/ESPHome-AirPlay
    components: [airplay]

sendspin:

media_source:
  - platform: airplay
    id: airplay_audio
    name: "Living Room AirPlay"
    active:
      name: "AirPlay Active"
    client:
      name: "AirPlay Client"
    title:
      name: "AirPlay Title"
    artist:
      name: "AirPlay Artist"
    album:
      name: "AirPlay Album"
    concealed_packets:
      name: "AirPlay Concealed Packets"
    late_frames:
      name: "AirPlay Late Frames"
    decode_errors:
      name: "AirPlay Decode Errors"
    output_drops:
      name: "AirPlay Output Drops"
    dacp_available:
      name: "AirPlay DACP Available"
    buffer_frames: 512
    udp_receive_buffer_packets: 32
    prefeed_duration: 100ms
    output_latency: 20ms
    volume_update_interval: 100ms
    session_timeout: 15s
    remote_control: true
  - platform: sendspin
    id: sendspin_audio

media_player:
  - platform: speaker_source
    name: "Living Room Speakers"
    media_pipeline:
      speaker: dac_speaker
      sources: [sendspin_audio, airplay_audio]
      format: FLAC
      sample_rate: 44100
      num_channels: 2

i2s_audio:
  id: dac_bus
  i2s_lrclk_pin: GPIOX
  i2s_bclk_pin: GPIOX

speaker:
  - platform: i2s_audio
    id: dac_speaker
    i2s_audio_id: dac_bus
    i2s_dout_pin: GPIOX
    dac_type: external
    sample_rate: 44100
    bits_per_sample: 16bit
    channel: stereo
    num_channels: 2
```

## Tuning

- `buffer_frames` accepts 512, 1024 or 2048. Larger values use more PSRAM but provide more buffering against network or scheduling jitter. The default 512-frame buffer uses about 710 KiB of PSRAM per session.
- `udp_receive_buffer_packets` accepts 6–64, default 32 (about 250 ms of audio). It controls ESP-IDF's per-socket UDP mailbox, independently of the decoded PCM buffer. This is a firmware-wide setting affecting all UDP sockets; larger queues can retain more packet memory during bursts. Changing it requires rebuilding firmware. The ESP-IDF default of six packets proved too small for my ESP32-S3.
- `prefeed_duration` accepts 0–500 ms, default 100 ms. It controls how early PCM is offered to the native speaker. Larger values trade timing accuracy for jitter headroom; keep the total of this value and `output_latency` below the native speaker's `buffer_duration` to avoid routinely blocking on a full speaker buffer. The speaker's own buffer remains configured on the native `speaker`, not the AirPlay source.
- `volume_update_interval` accepts 10 ms–1 s, default 100 ms. It sets the minimum interval between native updates from sender volume requests; the most recent pending value wins, and unchanged values are ignored. It does not throttle volume changes made directly in Home Assistant.
- `session_timeout` accepts 5 s–5 min, default 15 s. A session is disconnected after this long without UDP traffic after the first audio frame has decoded. Live RTSP selections can stay idle before setup; native TCP keepalive detects vanished senders. Timing/control packets count as UDP activity. It is an inactivity timeout, not a maximum playback duration.
- `output_latency` (0–500 ms, default 20 ms) adds to that prefeed window. Larger values feed audio earlier; they do not increase the RTP buffer. Scheduling is in milliseconds and has no sample-level drift correction.

# Limitations
- Minimal metadata support. While the component itself supports it, ESPHome's code currently does not allow media sources to provide the media player with it.
- No password support.
- No AirPlay 2; proper multiroom syncing is not supported.

## Details

- Stop disconnects the sender's session and releases decoder, sockets and PSRAM.
- Pause/resume in Home Assistant suppresses/resumes local output. With `remote_control: true`, those commands also request sender pause/play through DACP. Support depends on valid `DACP-ID`/`Active-Remote` headers and a matching `_dacp._tcp` service on the sender's session address.
- Optional `dacp_available` turns on when valid remote-control credentials and a matching sender endpoint have been discovered. It turns off when remote control is disabled, discovery or a command fails, or the session ends. Discovery does not guarantee the sender supports every command.
- One receiver per device; only UDP ALAC at 44.1 kHz / 16-bit stereo / 352 samples per packet is accepted.
- The AirPlay source's `name` sets the advertised receiver name. Its mDNS service instance is `<MAC-without-colons>@<name>._raop._tcp.local`, on port 5000. For the above example, senders would display **Living Room AirPlay**. This name is independent of the native media player's name and ESPHome's device hostname/friendly name.
- Optional `active` and `client` entities use native ESPHome binary/text sensors. `active` is on while a sender has an established RTSP connection, including idle selection and paused playback. `client` reports that sender’s IPv4 or IPv6 address for the same connection lifetime; it reports `Inactive` when disconnected. Playback state remains independent. Sensor names and other standard entity options are configurable.
- With `remote_control: true`, Home Assistant next/previous commands use DACP to select tracks on the sender, when the sender supports DACP.

- Optional `title`, `artist`, and `album` text sensors expose sender-provided track metadata, using the same field names as Sendspin. They preserve UTF-8 text, retain the track while paused, report `Unknown` for missing fields and `Inactive` after disconnect. Metadata publishes only when changed, from the main loop. Configuring any of these sensors advertises text metadata support (`md=0`); no DACP connection is required. Sender apps may omit metadata. These are separate HA sensors, not media-player title attributes; artwork and progress are not advertised.
## Technical Details

- Incoming AirPlay RECORD requests select `airplay://current` through the native orchestrator. ESPHome handles stopping the previous source and routing PCM; there is no custom source-switching or mixing code. Other source inputs remain available on the same entity. A single configured pipeline also accepts announcement requests through ESPHome's normal fallback.
- Sender volume requests update the native player's volume. AirPlay’s `-144 dB` mute requests use the native mute callback and preserve the previous volume; a later nonzero sender volume request unmutes. The existing slider mapping for non-mute volume remains unchanged. Home Assistant volume and mute use the native speaker, with no second software gain stage.
- Session connection messages are logged at INFO. Five-second audio progress summaries and first-packet/output messages use DEBUG; RTSP session methods and metadata field-presence summaries use DEBUG.
- UDP reception/decoding and native speaker writes run in separate tasks, so speaker backpressure does not block packet reception. Playback drains due frames within a bounded prefeed window to absorb task jitter.
- The optional `concealed_packets`, `late_frames`, `decode_errors`, and `output_drops` numeric sensors are diagnostic session counters. They reset at the next audio setup and retain the final values after disconnect. Changed values publish at most once per second. Concealed packets are missing audio packets replaced with silence; late frames are discarded for missing their playback deadline; decode errors count rejected ALAC frames. Output drops count audio packets with an unwritten tail after the native speaker write deadline (100 ms); intentional cancellation is excluded. RTSP reads use a 512-byte connection buffer, retaining read-ahead without waiting for the buffer to fill.
- A sender-issued RTSP pause or an accepted DACP pause suspends the UDP inactivity timeout until playback resumes. A sender closing its RTSP connection still ends the session; HA cannot resume a disconnected sender with this basic implementation.
- RTSP connections use native TCP keepalive: 30 seconds idle, 10 seconds between probes, and three failed probes. This releases a vanished sender even while paused; a reachable paused sender can stay connected. Only valid packets from the connected sender refresh UDP inactivity.

## Credits, disclaimer and license

Forked from [jptrsn/esphome-raop](https://github.com/jptrsn/esphome-raop). The RAOP implementation derives from Philippe's work and HairTunes; see [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md). The project retains [GPL-3.0](LICENSE). Espressif's managed decoder carries its own license.
Component code and some of this README was written by GPT-6 Sol, but I defined the purpose from the fork up and I tested this *exhaustively* on real hardware before releasing.
