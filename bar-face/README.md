# bar-face

ESP32-S3 firmware for the virtual bartender face: wake word (esp-sr), ST7789 display assets, Opus-over-UDP voice, MQTT via Cogitor IoT.

Forked from `embed-ai-assistant`. Protocol contracts (`cogitor.session.control`, UDP framing, `ui_state`) are unchanged. Phase-1 pour is RPC stub `bar.pour` only.

## Build

```bash
idf.py set-target esp32s3
idf.py build flash monitor
```

Requires access to private `embed-framework` git deps listed in `main/idf_component.yml`.
