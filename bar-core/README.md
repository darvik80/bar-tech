# bar-core

Rust business service for bar-tech: MQTT external plugin, Opus UDP sessions, STT/TTS (local|cloud), RAG, OpenAI-compatible LLM dialog, pour stub tool.

## Build

Requires CMake (for bundled libopus):

```powershell
$env:OPUS_BUNDLED = "1"
cargo build -p bar-core
```

## Run

```powershell
copy config.example.yaml config.yaml
# edit mqtt / advertise_ip / paths / keys
$env:OPENAI_API_KEY = "sk-..."
$env:OPUS_BUNDLED = "1"
cargo run -p bar-core
```

Config path can be passed as argv: `cargo run -p bar-core -- path\to\config.yaml`

## HTTP

| Method | Path | Purpose |
|--------|------|---------|
| GET | `/health` | Liveness |
| GET | `/v1/devices` | Known devices |
| POST | `/v1/rag/reload` | Re-embed `data/knowledge/*.md` |

## Crates

`bar-config`, `bar-mqtt`, `bar-media`, `bar-voice`, `bar-llm`, `bar-rag`, `bar-dialog`, `bar-storage`, `bar-api`, `bar-core` (binary).
