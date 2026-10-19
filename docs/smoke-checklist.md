# Smoke checklist

## Prerequisites

- [ ] iot-platform MQTT broker running
- [ ] External plugin `bar-core` registered; creds in `bar-core/config.yaml`
- [ ] `bar-face` flashed and online
- [ ] `bar-core` running (`cargo run -p bar-core`)
- [ ] STT/TTS backends reachable for selected mode

## Flow

1. [ ] Wake word → device face `barmen-listen`
2. [ ] bar-core logs `wake_word` and `session.control start`
3. [ ] Speak a short phrase → face `thinking`
4. [ ] Spoken reply → face `barmen-speaking` / `speaking` attrs
5. [ ] Ask «налей триста лагера» → tool `pour` → device RPC stub `status: stub`
6. [ ] `GET http://localhost:8091/health` → `{"ok":true}`
7. [ ] `POST http://localhost:8091/v1/rag/reload` after editing knowledge md

## Mode switch

- [ ] `stt.mode: cloud` + `OPENAI_API_KEY` → transcriptions via `/audio/transcriptions`
- [ ] `tts.mode: cloud` → speech via `/audio/speech`
- [ ] Switch back to `local` without code changes
