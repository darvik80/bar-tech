# bar-tech

Monorepo for the virtual bartender: ESP32-S3 face/voice device (`bar-face`) and Rust business core (`bar-core`). Device connectivity goes through the external [iot-platform](https://github.com/) (MQTT rules bus). Cloud LLM/STT/TTS use OpenAI-compatible APIs; local STT/TTS use whisper.cpp + Piper.

## Modules

| Path | Role |
|------|------|
| [`bar-face/`](bar-face/) | ESP-IDF firmware: wake word, display, Opus UDP audio, pour stub RPC |
| [`bar-core/`](bar-core/) | Rust service: MQTT plugin, audio sessions, STT/TTS, RAG, LLM dialog |
| [`docs/`](docs/) | Architecture and protocol contracts |
| [`deploy/`](deploy/) | Docker Compose (bar-core + whisper sidecar) |

`iot-platform-go` is **not** vendored here — run it separately and register `bar-core` as an external MQTT plugin.

## Prerequisites

- **ESP-IDF** 5.5+ / 6.x, target `esp32s3`
- **Rust** 1.85+ (edition 2024 optional; workspace uses 2021)
- Running **iot-platform** MQTT broker + external plugin credentials
- Optional local voice: [whisper.cpp](https://github.com/ggerganov/whisper.cpp) server, [Piper](https://github.com/rhasspy/piper)

## Quick start

### 1. Copy / build firmware

```powershell
# If bar-face is empty / needs refresh from embed-ai-assistant:
.\scripts\copy-bar-face.ps1

cd bar-face
idf.py set-target esp32s3
idf.py build flash monitor
```

### 2. Run bar-core

```powershell
cd bar-core
copy config.example.yaml config.yaml
# Edit mqtt.*, audio.advertise_ip, llm/stt/tts keys
$env:OPUS_BUNDLED = "1"   # CMake builds vendored libopus
$env:OPENAI_API_KEY = "sk-..."
cargo run -p bar-core
```

### 3. Docker (optional)

```powershell
cd deploy
docker compose up --build
```

## Voice modes

In `bar-core/config.yaml`:

- `stt.mode`: `local` | `cloud`
- `tts.mode`: `local` | `cloud`
- LLM and RAG embeddings always use OpenAI-compatible HTTP

## Smoke checklist

1. Device provisions Wi‑Fi / MQTT via embed config portal
2. Wake word → display `barmen-listen`
3. Speak → `thinking` face → spoken reply → `barmen-speaking`
4. Ask to pour → LLM calls `pour` tool → device RPC stub returns `status: stub`

See [docs/architecture.md](docs/architecture.md) for diagrams and data flow.  
Full smoke steps: [docs/smoke-checklist.md](docs/smoke-checklist.md).
