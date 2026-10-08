import esphome.codegen as cg

DEPENDENCIES = ["network", "psram"]
AUTO_LOAD = ["mdns", "binary_sensor", "text_sensor", "sensor"]

airplay_ns = cg.esphome_ns.namespace("airplay")
