# MQTT Protocol — bar-core ↔ iot-platform ↔ bar-face

bar-core registers as an **external plugin** on iot-platform and uses the rules bus.

Typical patterns (align with your platform account slug):

- Subscribe: `platform/v1/events/{account_slug}/{product_id}/#` or `platform/v1/events/{product_id}/#`
- Publish RPC: `platform/v1/commands/{product_id}/{device_id}/rpc`
- Publish attrs: `platform/v1/commands/{product_id}/{device_id}/attributes/desired`

Configure `mqtt.events_prefix` and `mqtt.commands_prefix` in `bar-core` config to match the live broker.

## Events (device → plugin)

### wake_word

```json
{
  "type": "wake_word",
  "device_id": "bar-face-01",
  "product_id": "bar",
  "ts": 1788507773247
}
```

### session_closed

```json
{
  "type": "session_closed",
  "session_id": "sess_3d3d0c84",
  "ts": 1788507773247
}
```

### pour_stub (optional telemetry from firmware stub)

```json
{
  "type": "pour_stub",
  "tap_id": 1,
  "ml": 300,
  "ts": 1788507773247
}
```

## RPC (plugin → device)

### cogitor.session.control — start

```json
{
  "id": 1,
  "method": "cogitor.session.control",
  "params": {
    "action": "start",
    "session_id": "sess_3d3d0c84",
    "udp_session_id": 1031374737,
    "server_ip": "192.168.1.22",
    "udp_port": 50004,
    "opus": {
      "sample_rate": 16000,
      "channels": 1,
      "bitrate": 32000,
      "frame_ms": 60
    }
  }
}
```

### cogitor.session.control — stop

```json
{
  "id": 2,
  "method": "cogitor.session.control",
  "params": {
    "action": "stop",
    "session_id": "sess_3d3d0c84"
  }
}
```

### bar.pour — stub

See [pour-protocol.md](pour-protocol.md).

## Desired attributes

| Payload | When |
|---------|------|
| `session_status: open`, `session_id` | Session created |
| `ui_state: thinking` | After end-of-speech, before reply |
| `ui_state: speaking`, `ui_text` | Before / during TTS |
| `ui_state: idle` | After turn or error |
| `session_status: closed` | Session teardown |

## Message flow

```
Device                          bar-core                        MQTT Broker
  |                                 |                              |
  |--- wake_word event ------------>|                              |
  |                                 |--- session.control start --->|
  |<--------------------------------|------------------------------|
  |=== UDP Opus ===================>|                              |
  |--- END_OF_SPEECH -------------->|                              |
  |                                 |--- ui_state thinking ------->|
  |                                 |  STT → RAG → LLM → TTS       |
  |                                 |--- ui_state speaking ------->|
  |<=== UDP Opus ===================|                              |
  |                                 |--- END_OF_RESPONSE (UDP)     |
```
