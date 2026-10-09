"""Run the host checks, compiling the source against ESPHome's installed interfaces.

Run with the Python environment that has ESPHome installed:
    python tests/run_tests.py
"""
from pathlib import Path
import subprocess
import tempfile

import esphome

ROOT = Path(__file__).resolve().parent.parent
ESPHOME = Path(esphome.__file__).resolve().parent.parent
STUBS = {
    "esphome/core/helpers.h": """
#pragma once
#include <mutex>
namespace esphome {
using Mutex = std::mutex;
using LockGuard = std::lock_guard<Mutex>;
template<class T> struct RAMAllocator { enum { ALLOC_EXTERNAL = 1 }; RAMAllocator(int) {} };
template<class T> struct CallbackManager { template<class F> void add(F&&) {} };
}
""",
    "esphome/core/component.h": """
#pragma once
namespace esphome {
namespace setup_priority { constexpr float LATE = 0; }
struct Component {
  virtual void setup() {} virtual void loop() {} virtual void dump_config() {}
  virtual void on_shutdown() {} virtual float get_setup_priority() const { return 0; }
  bool is_ready() const { return true; } void mark_failed() {}
};
}
""",
    "esphome/core/defines.h": "#pragma once\n",
    "esphome/components/audio/audio.h": """
#pragma once
namespace esphome::audio { struct AudioStreamInfo { AudioStreamInfo(int = 0, int = 0, int = 0) {} }; }
""",
    "esphome/components/network/util.h": """
#pragma once
namespace esphome::network { inline bool is_connected() { return false; } }
""",
    "esphome/core/hal.h": """
#pragma once
#include <cstdint>
namespace esphome { inline uint32_t test_ms = 1000; inline uint32_t millis() { return test_ms; } }
""",
    "esphome/core/log.h": """
#pragma once
#define ESP_LOGD(tag, ...) ((void)(tag))
#define ESP_LOGE(tag, ...) ((void)(tag))
#define ESP_LOGW(tag, ...) ((void)(tag))
#define ESP_LOGCONFIG(tag, ...) ((void)(tag))
""",
    "freertos/FreeRTOS.h": """
#pragma once
using TaskHandle_t = void*; using SemaphoreHandle_t = void*; using QueueHandle_t = void*;
struct StaticQueue_t {};
""",
}
for name in ("task.h", "semphr.h", "queue.h"):
    STUBS[f"freertos/{name}"] = "#pragma once\n"
for component, cls, state in (
    ("binary_sensor", "BinarySensor", "bool"),
    ("text_sensor", "TextSensor", "std::string"),
    ("sensor", "Sensor", "float"),
):
    STUBS[f"esphome/components/{component}/{component}.h"] = f"""
#pragma once
#include <string>
namespace esphome::{component} {{ struct {cls} {{
  {state} state{{}}; bool has_state() const {{ return false; }}
  void publish_state(const {state}&) {{}}
}}; }}
"""

with tempfile.TemporaryDirectory(prefix="airplay-test-") as directory:
    temp = Path(directory)
    for name, contents in STUBS.items():
        path = temp / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(contents)
    for test in sorted((ROOT / "tests").glob("*_test.cpp")):
        sources = [str(test)]
        if test.name == "media_source_test.cpp":
            sources.append(str(ROOT / "components/airplay/airplay_media_source.cpp"))
        binary = temp / test.stem
        subprocess.run([
            "c++", "-std=c++17", "-pthread", "-Wall", "-Wextra", "-Wno-unused-parameter",
            "-fsanitize=address,undefined", f"-I{temp}", f"-I{ESPHOME}",
            *sources, "-o", str(binary),
        ], check=True)
        subprocess.run([str(binary)], check=True)
        print(f"{test.stem}: passed", flush=True)
