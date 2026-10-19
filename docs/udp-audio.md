# UDP audio framing

Direct LAN path between `bar-face` and `bar-core` (not via MQTT).

## Header (big-endian)

```
struct {
  u32 udp_session_id;
  u16 frame_seq;
  u8  flags;
  u8  reserved;
}
```

Format: `>IHBB` (8 bytes). Payload: Opus (or raw PCM in simulators).

## Flags — device → server

| Bit | Name | Value |
|-----|------|-------|
| 0 | START_OF_SPEECH | 0x01 |
| 1 | END_OF_SPEECH | 0x02 |
| 2 | MUTE | 0x04 |

## Flags — server → device

| Bit | Name | Value |
|-----|------|-------|
| 0 | START_OF_RESPONSE | 0x01 |
| 1 | END_OF_RESPONSE | 0x02 |
| 2 | ERROR | 0x04 |

## Audio parameters

| Param | Value |
|-------|-------|
| Sample rate | 16000 Hz |
| Channels | 1 |
| Frame | 960 samples (60 ms) uplink chunking on device |
| Codec | Opus VOIP |

## Session limits (bar-core)

- Max concurrent sessions: 3
- Max recording: ~30 s
- Idle timeout: 120 s
