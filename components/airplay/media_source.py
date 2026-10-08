import esphome.codegen as cg
import esphome.config_validation as cv
import esphome.final_validate as fv
from esphome.components import binary_sensor, esp32, media_source, network, sensor, socket, text_sensor
from esphome.const import CONF_ID, CONF_NAME, ENTITY_CATEGORY_DIAGNOSTIC, STATE_CLASS_TOTAL_INCREASING
from esphome.core import CORE

from . import airplay_ns

AirplayMediaSource = airplay_ns.class_(
    "AirplayMediaSource", cg.Component, media_source.MediaSource
)


def _validate_framework(config):
    if CORE.using_arduino:
        raise cv.Invalid("AirPlay requires the ESP-IDF framework")
    return config


def _request_networking(config):
    network.require_high_performance_networking()
    return config


def _remote_socket_usage(config):
    if config["remote_control"]:
        socket.consume_sockets(1, "airplay_dacp", socket.SocketType.TCP)(config)
    return config


CONFIG_SCHEMA = cv.All(
    media_source.media_source_schema(AirplayMediaSource).extend(
        {
            cv.Required(CONF_NAME): cv.All(cv.string_strict, cv.Length(min=1, max=40)),
            cv.Optional("remote_control", default=False): cv.boolean,
            cv.Optional("active"):
                binary_sensor.binary_sensor_schema(icon="mdi:cast-audio-variant"),
            cv.Optional("client"):
                text_sensor.text_sensor_schema(icon="mdi:lan-connect"),
            cv.Optional("title"): text_sensor.text_sensor_schema(icon="mdi:text-short"),
            cv.Optional("artist"): text_sensor.text_sensor_schema(icon="mdi:account-music"),
            cv.Optional("album"): text_sensor.text_sensor_schema(icon="mdi:album"),
            cv.Optional("dacp_available"): binary_sensor.binary_sensor_schema(
                icon="mdi:remote", entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            cv.Optional("concealed_packets"): sensor.sensor_schema(
                accuracy_decimals=0, icon="mdi:counter",
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
                state_class=STATE_CLASS_TOTAL_INCREASING,
            ),
            cv.Optional("late_frames"): sensor.sensor_schema(
                accuracy_decimals=0, icon="mdi:counter",
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
                state_class=STATE_CLASS_TOTAL_INCREASING,
            ),
            cv.Optional("decode_errors"): sensor.sensor_schema(
                accuracy_decimals=0, icon="mdi:counter",
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
                state_class=STATE_CLASS_TOTAL_INCREASING,
            ),
            cv.Optional("output_drops"): sensor.sensor_schema(
                accuracy_decimals=0, icon="mdi:counter",
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
                state_class=STATE_CLASS_TOTAL_INCREASING,
            ),
            cv.Optional("buffer_frames", default=512): cv.one_of(512, 1024, 2048, int=True),
            cv.Optional("udp_receive_buffer_packets", default=32): cv.int_range(min=6, max=64),
            cv.Optional("prefeed_duration", default="100ms"): cv.All(
                cv.positive_time_period_milliseconds,
                cv.Range(max=cv.TimePeriod(milliseconds=500)),
            ),
            cv.Optional("output_latency", default="20ms"): cv.All(
                cv.positive_time_period_milliseconds,
                cv.Range(max=cv.TimePeriod(milliseconds=500)),
            ),
            cv.Optional("volume_update_interval", default="100ms"): cv.All(
                cv.positive_time_period_milliseconds,
                cv.Range(min=cv.TimePeriod(milliseconds=10), max=cv.TimePeriod(seconds=1)),
            ),
            cv.Optional("session_timeout", default="15s"): cv.All(
                cv.positive_time_period_milliseconds,
                cv.Range(min=cv.TimePeriod(seconds=5), max=cv.TimePeriod(minutes=5)),
            ),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    cv.only_on_esp32,
    esp32.only_on_variant(supported=["ESP32", "ESP32S3"]),
    _validate_framework,
    _request_networking,
    _remote_socket_usage,
    socket.consume_sockets(2, "airplay", socket.SocketType.TCP_LISTEN),
    socket.consume_sockets(1, "airplay", socket.SocketType.TCP),
    socket.consume_sockets(3, "airplay", socket.SocketType.UDP),
)


def _validate_receiver(config):
    full = fv.full_config.get()
    if sum(source["platform"] == "airplay" for source in full.get("media_source", [])) > 1:
        raise cv.Invalid("Only one AirPlay receiver can advertise and listen on port 5000")
    if full.get("mdns", {}).get("disabled", False):
        raise cv.Invalid("AirPlay requires mDNS discovery")
    return config


FINAL_VALIDATE_SCHEMA = _validate_receiver


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await media_source.register_media_source(var, config)
    cg.add(var.set_name(config[CONF_NAME]))
    cg.add(var.set_buffer_frames(config["buffer_frames"]))
    cg.add(var.set_prefeed_duration(config["prefeed_duration"].total_milliseconds))
    cg.add(var.set_output_latency(config["output_latency"].total_milliseconds))
    cg.add(var.set_volume_update_interval(config["volume_update_interval"].total_milliseconds))
    cg.add(var.set_session_timeout(config["session_timeout"].total_milliseconds))
    cg.add(var.set_remote_control(config["remote_control"]))
    if "active" in config:
        entity = await binary_sensor.new_binary_sensor(config["active"])
        cg.add(var.set_active_sensor(entity))
    if "client" in config:
        entity = await text_sensor.new_text_sensor(config["client"])
        cg.add(var.set_client_sensor(entity))
    for field in ("title", "artist", "album"):
        if field in config:
            entity = await text_sensor.new_text_sensor(config[field])
            cg.add(getattr(var, f"set_{field}_sensor")(entity))
    if "dacp_available" in config:
        entity = await binary_sensor.new_binary_sensor(config["dacp_available"])
        cg.add(var.set_dacp_available_sensor(entity))
    if "concealed_packets" in config:
        entity = await sensor.new_sensor(config["concealed_packets"])
        cg.add(var.set_concealed_packets_sensor(entity))
    if "late_frames" in config:
        entity = await sensor.new_sensor(config["late_frames"])
        cg.add(var.set_late_frames_sensor(entity))
    if "decode_errors" in config:
        entity = await sensor.new_sensor(config["decode_errors"])
        cg.add(var.set_decode_errors_sensor(entity))
    if "output_drops" in config:
        entity = await sensor.new_sensor(config["output_drops"])
        cg.add(var.set_output_drops_sensor(entity))
    esp32.add_idf_component(name="espressif/esp_audio_codec", ref="2.5.0")
    esp32.add_idf_sdkconfig_option("CONFIG_AUDIO_DECODER_ALAC_SUPPORT", True)
    # TCP high-performance settings do not enlarge the UDP socket mailbox.
    esp32.add_idf_sdkconfig_option(
        "CONFIG_LWIP_UDP_RECVMBOX_SIZE", config["udp_receive_buffer_packets"]
    )
