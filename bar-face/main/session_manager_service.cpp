#include "session_manager_service.hpp"

#include "embed/registry.hpp"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "cJSON.h"
#include "sdkconfig.h"

#include <cstring>
#include <cstdio>
#include <new>
#include <variant>

static constexpr char TAG[] = "Session";

// ── MQTT attribute JSON keys ──────────────────────────────────────────
static constexpr char kKeySessionStatus[] = "session_status";
static constexpr char kKeyUiState[]       = "ui_state";
static constexpr char kKeyUiText[]        = "ui_text";
static constexpr char kKeySessionId[]     = "session_id";

// ── UI state / session status protocol constants ──────────────────────
static constexpr char kUiStateThinking[]  = "thinking";
static constexpr char kUiStateSpeaking[]  = "speaking";
static constexpr char kUiStateIdle[]      = "idle";
static constexpr char kSessStatusClosed[] = "closed";

namespace embed::audio {

// Message passed through encode queue: pool buffer pointer + UDP flags
struct EncodeFrameMsg {
    uint8_t* buf;
    uint8_t  flags;
};

// ── onStateChanged — display updates via display task ──────────────────

void SessionManagerService::onStateChanged(const embed::TransitionTo<SWaitSession>&) {
    ESP_LOGI(TAG, "state: → WaitSession (waiting for RPC cogitor.session.control, timeout 5s)");
    waitTicks_ = 0;
    requestDisplayAsset("barmen-listen");
}

void SessionManagerService::onStateChanged(const embed::TransitionTo<SRecording>&) {
    // followUpRecording_ is set explicitly by the code paths that trigger
    // the Playback→Recording transition (EvtResumeListening handler below).
    // Do NOT detect it here from sessionId_/udpSessionId_ — those are set
    // by handleSessionStart() BEFORE the state transition, so the first
    // session would also look like follow-up.

    // Reset ALL recording state BEFORE any branching.
    // tick() runs on esp_timer task (core 0) and reads these variables
    // concurrently — they must be reset before the state is visible as SRecording.
    // recordingSessionToken_ is incremented FIRST so that any in-flight tick()
    // that reads the new state but old token will be detected as stale.
    recordingSessionToken_++;
    recordingTicks_    = 0;
    silenceTicks_      = 0;
    isSpeaking_        = false;
    endOfSpeechSent_   = false;
    wasSpeaking_       = false;
    trailingFrames_    = 0;
    micAccumLen_       = 0;

    // Re-enable voice processing (was disabled during Playback for speaker isolation)
    if (voice_) {
        voice_->EnableVoiceProcessing(true);
        voice_->EnableWakeWordDetection(false);  // no wake word during active session
    }

    // Drain stale playback queue — late TTS frames from previous response
    // may still arrive via UDP. If not drained, they block goIdle() in the
    // safety timeout (playback task is stopped, nobody consumes the queue).
    if (playPendQ_ && playFreeQ_) {
        uint8_t* item;
        uint32_t drained = 0;
        while (xQueueReceive(playPendQ_, &item, 0) == pdTRUE) {
            xQueueSend(playFreeQ_, &item, 0);
            drained++;
        }
        if (drained > 0) {
            ESP_LOGI(TAG, "drained %u stale frames from playback queue", drained);
        }
    }

    if (followUpRecording_) {
        // Follow-up: playback task already stopped (drained queue in playbackLoop).
        // Reset flag to ensure clean state.
        playbackRunning_ = false;
        ESP_LOGI(TAG, "state: → Recording (follow-up listening, 30s window)");
        requestDisplayAsset("barmen-listen");
    } else {
        ESP_LOGI(TAG, "state: → Recording");
        micFramesQueued_ = 0;
        micFramesSent_   = 0;
    }
}

void SessionManagerService::onStateChanged(const embed::TransitionTo<SPlayback>&) {
    // Reset ALL playback state FIRST — tick() reads these concurrently on esp_timer task.
    playbackTicks_       = 0;
    responseFrames_      = 0;
    responseAudioStarted_ = false;
    lastAudioTick_       = tickCount_;
    endOfResponseReceived_ = false;
    pendingStop_         = false;
    playbackRunning_     = true;  // ensure playback task starts consuming frames
    decodeCount_         = 0;
    decodeErrors_        = 0;
    playCount_           = 0;
    playErrors_          = 0;
    ringBufDrops_        = 0;
    ringBufWrites_       = 0;

    ESP_LOGI(TAG, "state: → Playback (encode stats: queued=%u sent=%u playQ=%p)",
             micFramesQueued_, micFramesSent_, playPendQ_);

    // Disable voice processing during TTS playback (xiaozhi-style).
    // Prevents speaker → mic feedback from triggering false VAD/speech detection.
    // Feed task keeps reading I2S (hardware stays happy) but no audio callback or VAD events.
    // Wake word detection is also gated by voiceProcessingEnabled_ in fetch task,
    // so no need to enable it separately here.
    if (voice_) {
        voice_->EnableVoiceProcessing(false);
    }

    // Drain leftover encode queue — stop sending mic frames immediately.
    // The encode task may have queued frames from before the state transition;
    // returning them to the free pool prevents stale audio from reaching the server.
    if (encodePendQ_) {
        EncodeFrameMsg drain;
        uint32_t drained = 0;
        while (xQueueReceive(encodePendQ_, &drain, 0) == pdTRUE) {
            if (drain.buf) xQueueSend(encodeFreeQ_, &drain.buf, 0);
            drained++;
        }
        if (drained > 0) {
            ESP_LOGI(TAG, "drained %u leftover frames from encode queue", drained);
        }
    }
    micAccumLen_ = 0;  // discard partial accumulation

    // Drain leftover data from PREVIOUS session's playback queue
    // (goIdle() no longer drains — playback task finishes frames naturally)
    if (playPendQ_ && playFreeQ_) {
        uint8_t* item;
        uint32_t drained = 0;
        while (xQueueReceive(playPendQ_, &item, 0) == pdTRUE) {
            xQueueSend(playFreeQ_, &item, 0);
            drained++;
        }
        if (drained > 0) {
            ESP_LOGI(TAG, "drained %u leftover frames from playback queue", drained);
        }
    }

    requestDisplayAsset("barmen-thinking");
}

void SessionManagerService::onStateChanged(const embed::TransitionTo<SFollowup>&) {
    ESP_LOGI(TAG, "state: → Followup (decode stats: decoded=%u errors=%u played=%u play_errors=%u ring_drops=%u)",
             decodeCount_, decodeErrors_, playCount_, playErrors_, ringBufDrops_);
    followupTicks_ = 0;
    // NOTE: Do NOT set playbackRunning_ = false here!
    // The playback task may still have frames in the queue. Let it finish
    // naturally — it will fire EvtResumeListening to return to Recording.
    requestDisplayAsset("barmen-idle");
}

void SessionManagerService::onStateChanged(const embed::TransitionTo<SIdle>&) {
    ESP_LOGI(TAG, "state: → Idle (decode=%u errors=%u played=%u play_err=%u ring_writes=%u ring_drops=%u rx_frames=%u)",
             decodeCount_, decodeErrors_, playCount_, playErrors_, ringBufWrites_, ringBufDrops_, responseFrames_);
    playbackRunning_ = false;  // ensure playback task goes idle
    followUpRecording_ = false;
    // Re-enable voice processing + wake word for next session
    if (voice_) {
        voice_->EnableVoiceProcessing(true);
        voice_->EnableWakeWordDetection(true);
    }
    requestDisplayAsset("barmen-idle");
}

void SessionManagerService::onStateChanged(const embed::Nothing&) {
    // No transition — nothing to do
}

// ── start / stop ───────────────────────────────────────────────────────

void SessionManagerService::start() {
    auto& reg = embed::ServiceRegistry::instance();
    iot_      = reg.getService<cogitor::iot::IotService>();
    voice_    = reg.getService<VoiceDetectionService>();
    display_  = reg.getService<display::DisplayService>();
    audioBus_ = reg.getService<AudioBusService>();

    if (!iot_) {
        ESP_LOGE(TAG, "IotService not found — session manager disabled");
        // No sessions, but the display must still show the idle face.
        // start() runs in the main task — an ordinary FreeRTOS task, so a
        // direct (synchronous) draw is safe here; the sess_display task
        // below is never created in this path.
        if (display_) {
            display_->showAsset("barmen-idle");
        }
        return;
    }
    if (!voice_) {
        ESP_LOGE(TAG, "VoiceDetectionService not found — session manager disabled");
        if (display_) {
            display_->showAsset("barmen-idle");
        }
        return;
    }

    // Connect signals
    wakeWordSlot_.connect(voice_->onWakeWord);
    voiceActivitySlot_.connect(voice_->onVoiceActivity);
    attrUpdateSlot_.connect(iot_->onAttributeUpdate);

    // Register RPC handler for session control
    iot_->rpc().add("cogitor.session.control",
                    &SessionManagerService::onSessionControlRpc, this,
                    "Voice session start/stop");
    ESP_LOGI(TAG, "listening for RPC cogitor.session.control (start/stop)");

    iot_->rpc().add("bar.pour",
                    &SessionManagerService::onPourRpc, this,
                    "Pour stub (phase 1 — no hardware)");
    ESP_LOGI(TAG, "listening for RPC bar.pour (stub)");

    // Register UDP response callback
    udp_.setResponseCallback(onResponseFrame, this);

    // Create UDP socket + receive task at boot (task runs permanently)
    if (!udp_.init()) {
        ESP_LOGE(TAG, "UDP transport init failed");
        return;
    }
    ESP_LOGI(TAG, "heap after UDP init: %u bytes free",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    // Initialize OPUS codec (16 kHz mono, VOIP profile)
    if (!opus_.init()) {
        ESP_LOGE(TAG, "OPUS codec init failed — audio streaming disabled");
        return;
    }
    ESP_LOGI(TAG, "heap after OPUS init: %u bytes free",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    // Playback queue + buffer pool (replaces ring buffer — FreeRTOS queues
    // are reliable across cores, unlike ring buffers in PSRAM).
    playPool_ = static_cast<uint8_t*>(
        heap_caps_calloc(kPlayPoolSize * kPlayBufSize, 1, MALLOC_CAP_SPIRAM));
    playFreeQ_ = xQueueCreateWithCaps(kPlayPoolSize, sizeof(uint8_t*), MALLOC_CAP_SPIRAM);
    playPendQ_ = xQueueCreateWithCaps(kPlayPoolSize, sizeof(uint8_t*), MALLOC_CAP_SPIRAM);
    if (!playPool_ || !playFreeQ_ || !playPendQ_) {
        ESP_LOGE(TAG, "playback queue alloc failed (pool=%p freeQ=%p pendQ=%p)",
                 playPool_, playFreeQ_, playPendQ_);
        return;
    }
    for (size_t i = 0; i < kPlayPoolSize; ++i) {
        uint8_t* slot = playPool_ + i * kPlayBufSize;
        xQueueSend(playFreeQ_, &slot, 0);
    }
    ESP_LOGI(TAG, "playback queue: %zu buffers × %zu bytes OK", kPlayPoolSize, kPlayBufSize);
    micAccum_ = static_cast<uint8_t*>(
        heap_caps_calloc(1, kUdpFrameBytes, MALLOC_CAP_SPIRAM));
    if (!micAccum_) {
        ESP_LOGE(TAG, "mic buffer alloc failed");
        return;
    }

    // OPUS encode task: pool of frame buffers + two queues (pool in PSRAM)
    encodePool_ = static_cast<uint8_t*>(
        heap_caps_calloc(kEncodePoolSize * kUdpFrameBytes, 1, MALLOC_CAP_SPIRAM));
    if (!encodePool_) {
        ESP_LOGE(TAG, "encode pool alloc failed");
        return;
    }
    encodeFreeQ_ = xQueueCreate(kEncodePoolSize, sizeof(uint8_t*));
    encodePendQ_ = xQueueCreate(2, sizeof(EncodeFrameMsg));
    if (!encodeFreeQ_ || !encodePendQ_) {
        ESP_LOGE(TAG, "encode queue alloc failed");
        return;
    }
    // Pre-populate free queue with all pool slots
    for (size_t i = 0; i < kEncodePoolSize; ++i) {
        uint8_t* slot = encodePool_ + i * kUdpFrameBytes;
        xQueueSend(encodeFreeQ_, &slot, 0);
    }
    ESP_LOGI(TAG, "heap after encode pool: %u bytes free",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    // Register mic audio callback AFTER buffers are ready
    voice_->setAudioFrameCallback(onMicAudioFrame, this);

    // Create tick timer (20ms period = 50 Hz)
    const esp_timer_create_args_t timerArgs = {
        .callback = &SessionManagerService::tickTimerEntry,
        .arg      = this,
        .name     = "session_tick",
    };
    esp_timer_create(&timerArgs, &tickTimer_);
    esp_timer_start_periodic(tickTimer_, 20 * 1000); // 20ms

    ESP_LOGI(TAG, "heap after playback queue: %u bytes free",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    // Display task — SPI transfers require task context (not timer callback)
    displayTaskRunning_ = true;
    xTaskCreate(displayTaskEntry, "sess_display",
                4096, this, 3, &displayTask_);
    ESP_LOGI(TAG, "heap after display task: %u bytes free",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    // Create separate encode and playback tasks at boot.
    // OpusCodec has separate encoder_/decoder_ — safe to call from different tasks.
    // This prevents decode starvation when encode queue is busy.
    startEncodeTask();
    startPlaybackTask();

    ESP_LOGI(TAG, "heap after all tasks: %u bytes free",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    requestDisplayAsset("barmen-idle");

    ESP_LOGI(TAG, "started — Idle");
}

void SessionManagerService::stop() {
    if (tickTimer_) {
        esp_timer_stop(tickTimer_);
        esp_timer_delete(tickTimer_);
        tickTimer_ = nullptr;
    }

    displayTaskRunning_ = false;
    if (displayTask_) {
        xTaskNotifyGive(displayTask_);
        vTaskDelay(pdMS_TO_TICKS(100));
        vTaskDelete(displayTask_);
        displayTask_ = nullptr;
    }

    // Stop playback task FIRST (before draining — task might still be reading)
    playbackRunning_ = false;
    stopPlaybackTask();
    stopEncodeTask();

    // Now safe to delete playback queues (playback task is gone)
    if (playPendQ_) {
        uint8_t* item;
        while (xQueueReceive(playPendQ_, &item, 0) == pdTRUE) {
            xQueueSend(playFreeQ_, &item, 0);
        }
        vQueueDeleteWithCaps(playPendQ_); playPendQ_ = nullptr;
    }
    if (playFreeQ_) { vQueueDeleteWithCaps(playFreeQ_); playFreeQ_ = nullptr; }
    heap_caps_free(playPool_); playPool_ = nullptr;

    // Drain encode queue
    if (encodePendQ_) {
        EncodeFrameMsg msg{};
        while (xQueueReceive(encodePendQ_, &msg, 0) == pdTRUE) {
            if (msg.buf) xQueueSend(encodeFreeQ_, &msg.buf, 0);
        }
    }

    udp_.stop();
    opus_.destroy();

    // Cleanup PSRAM buffers
    heap_caps_free(encodePool_); encodePool_ = nullptr;
    if (encodeFreeQ_) { vQueueDeleteWithCaps(encodeFreeQ_); encodeFreeQ_ = nullptr; }
    if (encodePendQ_) { vQueueDeleteWithCaps(encodePendQ_); encodePendQ_ = nullptr; }

    heap_caps_free(micAccum_); micAccum_ = nullptr;

    if (voice_) {
        voice_->setAudioFrameCallback(nullptr, this);  // clear only this service's callback
    }

    ESP_LOGI(TAG, "stopped");
}

// ── Signal handlers ────────────────────────────────────────────────────

void SessionManagerService::onWakeWord(const WakeWordDetected& msg, void* ctx) {
    auto* self = static_cast<SessionManagerService*>(ctx);
    if (self) self->handleWakeWord(msg);
}

void SessionManagerService::onVoiceActivity(const VoiceActivityEvent& msg, void* ctx) {
    auto* self = static_cast<SessionManagerService*>(ctx);
    if (!self) return;
    self->isSpeaking_ = (msg.vadState == 1);
}

void SessionManagerService::onAttributeUpdate(
    const cogitor::iot::AttributeUpdate& msg, void* ctx)
{
    auto* self = static_cast<SessionManagerService*>(ctx);
    if (self) self->handleAttrUpdate(msg.payload.c_str());
}

// ── RPC handler ────────────────────────────────────────────────────────

void SessionManagerService::onSessionControlRpc(
    cogitor::iot::IotService& iot,
    std::string_view requestId,
    const cogitor::iot::RpcParams& params,
    void* ctx)
{
    auto* self = static_cast<SessionManagerService*>(ctx);
    if (!self) return;

    std::string action;
    params.get("action", action);
    ESP_LOGI(TAG, "RPC cogitor.session.control: action=%s", action.c_str());

    if (action == "start") {
        self->handleSessionStart(params);
        iot.respondRpc(requestId, 0, "ok");
    } else if (action == "stop") {
        self->handleSessionStop(params);
        iot.respondRpc(requestId, 0, "ok");
    } else {
        ESP_LOGW(TAG, "unknown action: %s", action.c_str());
        iot.respondRpc(requestId, -1, "unknown action");
    }
}

void SessionManagerService::onPourRpc(
    cogitor::iot::IotService& iot,
    std::string_view requestId,
    const cogitor::iot::RpcParams& params,
    void* ctx)
{
    auto* self = static_cast<SessionManagerService*>(ctx);
    if (!self) return;

    const int tapId = params.getInt("tap_id");
    int ml = params.getInt("ml");
    if (ml <= 0) {
        ml = params.getInt("duration_ms");
    }

    if (tapId <= 0) {
        ESP_LOGW(TAG, "RPC bar.pour: invalid tap_id=%d", tapId);
        iot.respondRpc(requestId, -1, "invalid params");
        return;
    }

    ESP_LOGI(TAG, "RPC bar.pour STUB: tap_id=%d ml=%d (no GPIO)", tapId, ml);

    char result[160];
    snprintf(result, sizeof(result),
             "{\"ok\":true,\"status\":\"stub\",\"tap_id\":%d,\"ml\":%d}",
             tapId, ml);
    iot.respondRpc(requestId, 0, result);

    char event[192];
    snprintf(event, sizeof(event),
             "{\"type\":\"pour_stub\",\"tap_id\":%d,\"ml\":%d,\"ts\":%lld}",
             tapId, ml,
             static_cast<long long>(esp_timer_get_time() / 1000));
    if (self->iot_) {
        self->iot_->publishEvents(event);
    }
}

// ── Event handlers ─────────────────────────────────────────────────────

void SessionManagerService::handleWakeWord(const WakeWordDetected& msg) {
    ESP_LOGI(TAG, "=== WAKE WORD detected (cmd_id=%u) ===", msg.wakeWordId);

    // Wake word only starts a session from Idle. In every other state the
    // state machine silently drops EvtWakeWord — log it instead of losing it.
    if (!std::holds_alternative<SIdle*>(getCurrentState())) {
        ESP_LOGW(TAG, "wake word ignored — session already active (state != Idle)");
        return;
    }

    // Publish wake_word event via MQTT — this is the trigger that makes
    // ctrl-center send the cogitor.session.control(start) RPC back to us.
    if (iot_) {
        char json[256];
        snprintf(json, sizeof(json),
                 "{\"type\":\"wake_word\",\"ts\":%lld}",
                 static_cast<long long>(esp_timer_get_time() / 1000));
        const int msgId = iot_->publishEvents(json);
        if (msgId < 0) {
            ESP_LOGE(TAG, "wake_word event NOT published (MQTT down?) — session will not start");
        } else {
            ESP_LOGI(TAG, "wake_word event published (msgId=%d) — waiting for cogitor.session.control", msgId);
        }
    } else {
        ESP_LOGE(TAG, "no IotService — wake_word event not published, session impossible");
    }

    // Dispatch through state machine (only handled in SIdle)
    this->handle(EvtWakeWord{});
}

void SessionManagerService::handleSessionStart(
    const cogitor::iot::RpcParams& params)
{
    // Extract session params
    std::string serverIp = params.getString("server_ip");
    int udpPort          = params.getInt("udp_port");
    udpSessionId_        = static_cast<uint32_t>(params.getInt("udp_session_id"));
    std::string sessId   = params.getString(kKeySessionId);

    strncpy(sessionId_, sessId.c_str(), sizeof(sessionId_) - 1);
    sessionId_[sizeof(sessionId_) - 1] = '\0';

    ESP_LOGI(TAG, "SESSION START: %s:%d session=%s udp_sess=%u",
             serverIp.c_str(), udpPort, sessionId_, udpSessionId_);

    // Open UDP transport
    uint16_t localPort = udp_.open(serverIp.c_str(),
                                   static_cast<uint16_t>(udpPort),
                                   udpSessionId_);
    if (localPort == 0) {
        ESP_LOGE(TAG, "UDP open failed");
        goIdle();
        return;
    }
    // Receive task already running (created at boot in udp_.init())

    // Activate playback (codec task checks this flag before decoding)
    playbackRunning_ = true;
    followUpRecording_ = false;  // new session — not a follow-up

    // Codec task already running at boot — just drain leftover encode frames
    {
        EncodeFrameMsg drain;
        while (xQueueReceive(encodePendQ_, &drain, 0) == pdTRUE) {
            if (drain.buf) xQueueSend(encodeFreeQ_, &drain.buf, 0);
        }
    }

    // Dispatch through state machine (only handled in SWaitSession)
    this->handle(EvtSessionStart{});
}

void SessionManagerService::handleSessionStop(
    const cogitor::iot::RpcParams& params)
{
    std::string stopSessId = params.getString(kKeySessionId);
    ESP_LOGI(TAG, "SESSION STOP for session_id=%s", stopSessId.c_str());

    if (sessionId_[0] != '\0' && stopSessId != sessionId_) {
        ESP_LOGW(TAG, "wrong session: expected=%s got=%s",
                 sessionId_, stopSessId.c_str());
        return;
    }

    goIdle();
}

void SessionManagerService::handleAttrUpdate(const char* json) {
    if (!json || json[0] == '\0') return;

    cJSON* root = cJSON_Parse(json);
    if (!root) {
        ESP_LOGW(TAG, "attr JSON parse failed");
        return;
    }

    // Check session_status = "closed"
    cJSON* sessStatus = cJSON_GetObjectItem(root, kKeySessionStatus);
    if (sessStatus && cJSON_IsString(sessStatus)) {
        if (strcmp(sessStatus->valuestring, kSessStatusClosed) == 0) {
            ESP_LOGI(TAG, "session %s closed by server", sessionId_);
            cJSON_Delete(root);
            goIdle();
            return;
        }
    }

    // Check ui_state
    cJSON* uiState = cJSON_GetObjectItem(root, kKeyUiState);
    if (uiState && cJSON_IsString(uiState)) {
        const char* state = uiState->valuestring;
        ESP_LOGI(TAG, "ui_state: %s", state);

        // "thinking"/"speaking" → transition to Playback
        // This works for both initial Recording and follow-up Recording —
        // the server signals it's generating a response, so we must play it.
        if (strcmp(state, kUiStateThinking) == 0 || strcmp(state, kUiStateSpeaking) == 0) {
            this->handle(EvtUiThinking{});
            // Server signals it's generating/speaking — reset audio gap timer
            // so playback timeout doesn't fire before audio arrives.
            if (strcmp(state, kUiStateSpeaking) == 0) {
                responseAudioStarted_ = true;
                lastAudioTick_ = tickCount_;
            }
        } else if (strcmp(state, kUiStateIdle) == 0) {
            // ui_state=idle from server means it's done generating.
            // Do NOT transition to Followup here — the server sends idle
            // before all UDP audio frames have arrived. Transition happens
            // when the playback queue actually drains (EvtResumeListening
            // from playback task) or END_OF_RESPONSE flag is received.
        }
    }

    // Check ui_text
    cJSON* uiText = cJSON_GetObjectItem(root, kKeyUiText);
    if (uiText && cJSON_IsString(uiText)) {
        ESP_LOGI(TAG, "ui_text: %s", uiText->valuestring);
    }

    // Check session_id update
    cJSON* sessId = cJSON_GetObjectItem(root, kKeySessionId);
    if (sessId && cJSON_IsString(sessId)) {
        if (strcmp(sessionId_, sessId->valuestring) != 0) {
            ESP_LOGI(TAG, "session_id updated: %s → %s",
                     sessionId_, sessId->valuestring);
            strncpy(sessionId_, sessId->valuestring, sizeof(sessionId_) - 1);
            sessionId_[sizeof(sessionId_) - 1] = '\0';
        }
    }

    cJSON_Delete(root);
}

void SessionManagerService::goIdle(bool force) {
    // Drain encode queue — return all pending buffers to free pool
    if (encodePendQ_) {
        EncodeFrameMsg msg;
        while (xQueueReceive(encodePendQ_, &msg, 0) == pdTRUE) {
            if (msg.buf) xQueueSend(encodeFreeQ_, &msg.buf, 0);
        }
    }

    // Always signal the playback task to stop after draining remaining frames.
    // This must be set BEFORE any state checks so it's visible to the playback
    // task regardless of which state the machine is currently in.
    pendingStop_ = true;

    // If the playback task is still running (processing frames or about to
    // finish), let it complete naturally. It will fire EvtResumeListening
    // to transition to follow-up Recording when done.
    // Check both queue depth AND playbackRunning_ — the task may be between
    // frames (queue temporarily empty) but still active.
    if (!force && playbackRunning_) {
        ESP_LOGI(TAG, "goIdle: playback task still running (%u frames queued) — deferring",
                 playPendQ_ ? (unsigned)uxQueueMessagesWaiting(playPendQ_) : 0);
        return;  // playback task will fire EvtResumeListening when done
    }

    // Playback task is not running — drain any stale frames from the queue.
    // This prevents goIdle from being blocked by leftover frames when the
    // playback task has already stopped (e.g. late TTS frames during follow-up).
    if (playPendQ_ && playFreeQ_) {
        uint8_t* item;
        uint32_t drained = 0;
        while (xQueueReceive(playPendQ_, &item, 0) == pdTRUE) {
            xQueueSend(playFreeQ_, &item, 0);
            drained++;
        }
        if (drained > 0) {
            ESP_LOGI(TAG, "goIdle: drained %u stale frames from playback queue", drained);
        }
    }

    // Full cleanup + transition (queue is empty or force=true)
    // Reset UDP session ONLY when actually transitioning to Idle.
    // This prevents sending packets with session_id=0 while encode task
    // might still be running.
    udp_.resetSession();
    pendingStop_ = false;
    playbackRunning_ = false;
    followUpRecording_ = false;

    // Notify server that we closed the session (so server can clean up).
    // Published BEFORE clearing sessionId_ so the ID is still available.
    if (sessionId_[0] != '\0' && iot_) {
        char json[256];
        snprintf(json, sizeof(json),
                 "{\"type\":\"session_closed\",\"session_id\":\"%s\",\"ts\":%lld}",
                 sessionId_,
                 static_cast<long long>(esp_timer_get_time() / 1000));
        iot_->publishEvents(json);
        ESP_LOGI(TAG, "published session_closed event for %s", sessionId_);
    }

    micAccumLen_ = 0;
    sessionId_[0] = '\0';
    udpSessionId_ = 0;
    this->handle(EvtSessionStop{});
}

// ── Display task ───────────────────────────────────────────────────────

void SessionManagerService::requestDisplayAsset(const char* asset) {
    if (!display_ || !asset) return;
    pendingAsset_ = asset;
    if (displayTask_) {
        xTaskNotifyGive(displayTask_);
    }
}

void SessionManagerService::displayTaskEntry(void* arg) {
    auto* self = static_cast<SessionManagerService*>(arg);
    self->displayTaskLoop();
    vTaskDelete(nullptr);
}

void SessionManagerService::displayTaskLoop() {
    while (displayTaskRunning_) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(500));
        if (!displayTaskRunning_) break;

        const char* asset = const_cast<const char*>(pendingAsset_);
        pendingAsset_ = nullptr;
        if (asset && display_) {
            display_->showAsset(asset);
        }
    }
}

// ── Mic audio → UDP ────────────────────────────────────────────────────

void SessionManagerService::onMicAudioFrame(const int16_t* samples,
                                             size_t count, void* ctx) {
    auto* self = static_cast<SessionManagerService*>(ctx);
    if (!self) return;

    // NOTE: This callback runs in the vd_feed task context (limited stack).
    // NO OPUS encoding here — it would overflow the feed task stack.
    // Pre-roll buffering disabled (heap too expensive).

    // Forward raw PCM to UDP when recording
    bool isRecording = std::holds_alternative<SRecording*>(self->getCurrentState());
    if (isRecording) {
        self->processMicFrame(samples, count);
    }
}

void SessionManagerService::processMicFrame(const int16_t* samples,
                                             size_t count) {
    // NOTE: runs in vd_feed task context — NO OPUS encoding here.
    // VAD-gated sending: only encode and send when speech is detected.
    // During silence, frames are dropped (no bandwidth wasted on silence).
    // Voice processing is disabled during Playback (EnableVoiceProcessing(false))
    // so no frames arrive here during TTS — no speaker→mic feedback possible.
    // When we're in Recording, voice processing is ON and all frames are valid.

    // Detect speech start (rising edge of isSpeaking_)
    if (isSpeaking_ && !wasSpeaking_) {
        trailingFrames_ = 5;  // send a few frames after speech ends
        // Queue a silence frame with START_OF_SPEECH flag to mark beginning
        uint8_t* buf = nullptr;
        if (xQueueReceive(encodeFreeQ_, &buf, 0) == pdTRUE) {
            memset(buf, 0, kUdpFrameBytes);
            EncodeFrameMsg msg{buf, kFlagStartOfSpeech};
            xQueueSendToBack(encodePendQ_, &msg, pdMS_TO_TICKS(5));
            micFramesQueued_++;
            ESP_LOGI(TAG, "START_OF_SPEECH sent");
        }
    }
    wasSpeaking_ = isSpeaking_;

    // Only accumulate and send when VAD detects speech or trailing frames remain
    if (!isSpeaking_ && trailingFrames_ == 0) {
        // No speech — drop frame
        return;
    }

    const auto* bytes = reinterpret_cast<const uint8_t*>(samples);
    size_t bytesLeft = count * sizeof(int16_t);
    size_t offset = 0;

    while (bytesLeft > 0) {
        size_t space = kUdpFrameBytes - micAccumLen_;
        size_t copy  = (bytesLeft < space) ? bytesLeft : space;
        memcpy(micAccum_ + micAccumLen_, bytes + offset, copy);
        micAccumLen_ += copy;
        offset       += copy;
        bytesLeft    -= copy;

        if (micAccumLen_ >= kUdpFrameBytes) {
            // Acquire a free pool buffer
            uint8_t* buf = nullptr;
            if (xQueueReceive(encodeFreeQ_, &buf, 0) != pdTRUE) {
                // All buffers in use — encode task is behind, drop frame
                micAccumLen_ = 0;
                return;
            }
            memcpy(buf, micAccum_, kUdpFrameBytes);
            micAccumLen_ = 0;

            // Determine flags
            uint8_t flags = 0;
            if (!isSpeaking_) {
                // Trailing frame after speech ended
                if (trailingFrames_ > 0) {
                    trailingFrames_--;
                }
                if (trailingFrames_ == 0 && !endOfSpeechSent_) {
                    // Last trailing frame — mark end of speech
                    flags |= kFlagEndOfSpeech;
                    endOfSpeechSent_ = true;
                    ESP_LOGI(TAG, "END_OF_SPEECH sent (after %u trailing frames)", 5);
                }
            }

            // Send buffer pointer + flags to encode task
            EncodeFrameMsg msg{buf, flags};
            xQueueSendToBack(encodePendQ_, &msg, pdMS_TO_TICKS(5));
            micFramesQueued_++;
            if (micFramesQueued_ <= 3) {
                ESP_LOGI(TAG, "mic frame #%u queued to encode (count=%zu, speaking=%d)",
                         micFramesQueued_, count, (int)isSpeaking_);
            }
        }
    }
}

void SessionManagerService::sendMicFrame(const uint8_t* payload, size_t len,
                                          uint8_t flags) {
    if (!udp_.isOpen()) return;
    udp_.sendFrame(payload, len, flags);
}

// ── UDP response → speaker ─────────────────────────────────────────────

void SessionManagerService::onResponseFrame(const uint8_t* payload,
                                             size_t len, uint8_t flags,
                                             void* ctx) {
    auto* self = static_cast<SessionManagerService*>(ctx);
    if (!self) return;

    self->responseFrames_++;

    if (payload && len > 0) {
        if (!self->responseAudioStarted_) {
            ESP_LOGI(TAG, "=== RESPONSE AUDIO: first real frame %zu bytes, flags=0x%02x (after %u empty) ===",
                     len, flags, self->responseFrames_ - 1);
            self->responseAudioStarted_ = true;
            self->lastAudioTick_ = self->tickCount_;

            // Switch display to speaking via display task
            self->requestDisplayAsset("barmen-speaking");
        }
    } else {
        if (self->responseFrames_ <= 3) {
            ESP_LOGW(TAG, "response frame #%u: 0 bytes, skipping", self->responseFrames_);
        }
    }

    if (self->responseFrames_ % 50 == 0) {
        ESP_LOGI(TAG, "response: frame #%u, flags=0x%02x",
                 self->responseFrames_, flags);
    }

    // Write raw OPUS packet to playback queue for playback task to decode.
    // Format: [uint16_t opusLen][opusLen bytes] in a pooled buffer.
    if (payload && len > 0) {
        if (!self->playFreeQ_ || !self->playPendQ_) {
            if (self->responseFrames_ <= 3) {
                ESP_LOGE(TAG, "playback queues not initialized!");
            }
        } else {
            uint8_t* buf = nullptr;
            if (xQueueReceive(self->playFreeQ_, &buf, 0) == pdTRUE && buf) {
                uint16_t opusLen = static_cast<uint16_t>(len);
                memcpy(buf, &opusLen, sizeof(opusLen));
                memcpy(buf + sizeof(opusLen), payload, len);
                if (xQueueSend(self->playPendQ_, &buf, 0) != pdTRUE) {
                    xQueueSend(self->playFreeQ_, &buf, 0);  // return to pool
                    self->ringBufDrops_++;
                    if (self->ringBufDrops_ <= 5 || self->ringBufDrops_ % 50 == 0) {
                        ESP_LOGW(TAG, "playback queue full, dropping frame (drop #%u)", self->ringBufDrops_);
                    }
                } else {
                    self->ringBufWrites_++;
                }
            } else {
                self->ringBufDrops_++;
                if (self->ringBufDrops_ <= 5 || self->ringBufDrops_ % 50 == 0) {
                    ESP_LOGW(TAG, "no free playback buffers (drop #%u)", self->ringBufDrops_);
                }
            }
            self->lastAudioTick_ = self->tickCount_;
        }
    }

    // Check END_OF_RESPONSE
    if (flags & kFlagEndOfResponse) {
        ESP_LOGI(TAG, "=== END_OF_RESPONSE: %u frames, ring_writes=%u ring_drops=%u ===",
                 self->responseFrames_, self->ringBufWrites_, self->ringBufDrops_);
        self->endOfResponseReceived_ = true;  // playbackLoop will transition after draining
    }
}

// ── Tick / timeouts ────────────────────────────────────────────────────

void SessionManagerService::tickTimerEntry(void* arg) {
    auto* self = static_cast<SessionManagerService*>(arg);
    self->tickCount_++;
    self->tick();
}

void SessionManagerService::tick() {
    // WaitSession timeout
    if (std::holds_alternative<SWaitSession*>(getCurrentState())) {
        waitTicks_++;
        constexpr uint32_t kWaitTimeout = 5 * 50;  // 5 seconds
        if (waitTicks_ >= kWaitTimeout) {
            ESP_LOGW(TAG, "wait session timeout (5s) — no cogitor.session.control "
                          "(MQTT down or server did not react to wake_word)");
            goIdle();
        }
        return;
    }

    // Recording: silence tracking + timeouts
    if (std::holds_alternative<SRecording*>(getCurrentState())) {
        // Capture session token at entry — if onStateChanged() runs concurrently
        // on another core and increments the token, we detect stale state and bail out.
        uint32_t token = recordingSessionToken_;
        recordingTicks_++;

        // Verify token hasn't changed (state machine transition on another core)
        if (recordingSessionToken_ != token) {
            return;  // stale tick — new session started
        }

        constexpr uint32_t kForceEndTicks = 15 * 50;  // 15 seconds — safety limit

        // Periodic VAD status log (every 5 seconds)
        if (recordingTicks_ % 250 == 0) {
            ESP_LOGI(TAG, "Recording tick=%u speaking=%d silence=%u endSent=%d followUp=%d",
                     recordingTicks_, (int)isSpeaking_, silenceTicks_,
                     (int)endOfSpeechSent_, (int)followUpRecording_);
        }

        if (isSpeaking_) {
            lastSpeechTick_ = tickCount_;
            silenceTicks_ = 0;
            endOfSpeechSent_ = false;  // reset — new speech can trigger END_OF_SPEECH again
        } else {
            silenceTicks_++;
        }

        // Follow-up recording: 30s of silence → Idle (user had their chance)
        if (followUpRecording_) {
            constexpr uint32_t kFollowUpSilenceTicks = 30 * 50;  // 30 seconds
            if (silenceTicks_ >= kFollowUpSilenceTicks) {
                ESP_LOGI(TAG, "follow-up listening timeout (30s silence) → Idle");
                goIdle(true);  // force — no playback to wait for
            }
            // SAFETY: same 15s max sending limit for follow-up
            if (recordingTicks_ > 100 && recordingTicks_ >= kForceEndTicks && !endOfSpeechSent_) {
                ESP_LOGW(TAG, "SAFETY: forcing END_OF_SPEECH in follow-up after %u ticks → Idle",
                         recordingTicks_);
                endOfSpeechSent_ = true;
                {
                    uint8_t* buf = nullptr;
                    if (xQueueReceive(encodeFreeQ_, &buf, 0) == pdTRUE) {
                        memset(buf, 0, kUdpFrameBytes);
                        EncodeFrameMsg msg{buf, kFlagEndOfSpeech};
                        xQueueSendToBack(encodePendQ_, &msg, pdMS_TO_TICKS(5));
                    }
                }
                goIdle(true);  // force — bypass playback queue defer
                return;
            }
            // If user speaks, silenceTicks_ resets → window extends naturally
            return;
        }

        // Normal recording: max duration + end-of-speech detection
        // Max recording duration
        constexpr uint32_t kMaxRecordTicks = 30 * 50;  // 30 seconds
        if (recordingTicks_ >= kMaxRecordTicks) {
            ESP_LOGW(TAG, "max recording duration reached");
            endOfSpeechSent_ = true;
            // Send OPUS-encoded silence with END_OF_SPEECH flag
            {
                uint8_t* buf = nullptr;
                if (xQueueReceive(encodeFreeQ_, &buf, 0) == pdTRUE) {
                    memset(buf, 0, kUdpFrameBytes);
                    EncodeFrameMsg msg{buf, kFlagEndOfSpeech};
                    xQueueSendToBack(encodePendQ_, &msg, pdMS_TO_TICKS(5));
                }
            }
            this->handle(EvtMaxDuration{});
            return;
        }

        // SAFETY: if we've been sending for 15s without END_OF_SPEECH (VAD stuck
        // or continuous noise), force it now to prevent server recording timeout.
        // Guard: only check after recordingTicks_ > 100 (2 seconds) to prevent
        // false triggers from stale values during cross-core race conditions.
        if (recordingTicks_ > 100 && recordingTicks_ >= kForceEndTicks && !endOfSpeechSent_) {
            ESP_LOGW(TAG, "SAFETY: forcing END_OF_SPEECH after %u ticks (VAD stuck?)",
                     recordingTicks_);
            endOfSpeechSent_ = true;
            {
                uint8_t* buf = nullptr;
                if (xQueueReceive(encodeFreeQ_, &buf, 0) == pdTRUE) {
                    memset(buf, 0, kUdpFrameBytes);
                    EncodeFrameMsg msg{buf, kFlagEndOfSpeech};
                    xQueueSendToBack(encodePendQ_, &msg, pdMS_TO_TICKS(5));
                }
            }
            this->handle(EvtSilenceTimeout{});
            return;
        }

        // End of speech: processMicFrame already sent END_OF_SPEECH via VAD gating.
        // Transition to Playback once the flag was sent (no need to wait for 1.5s
        // silence — VAD + trailing frames already provide a natural cutoff).
        if (endOfSpeechSent_ && recordingTicks_ > 50) {
            ESP_LOGI(TAG, "end of speech detected (VAD gated, silence=%u ticks)", silenceTicks_);
            this->handle(EvtSilenceTimeout{});
        }
        return;
    }

    // Playback timeouts
    if (std::holds_alternative<SPlayback*>(getCurrentState())) {
        playbackTicks_++;

        if (!responseAudioStarted_) {
            constexpr uint32_t kPlaybackTimeout = 30 * 50;  // 30 seconds — server needs time for STT + TTS
            if (playbackTicks_ >= kPlaybackTimeout) {
                ESP_LOGW(TAG, "playback timeout — no response audio (30s)");
                this->handle(EvtPlaybackTimeout{});
            }
        } else {
            constexpr uint32_t kGapTimeout = 5 * 50;  // 5 seconds
            if (tickCount_ - lastAudioTick_ >= kGapTimeout) {
                ESP_LOGI(TAG, "playback gap timeout → Followup (5s)");
                this->handle(EvtPlaybackGap{});
            }
        }
        return;
    }

    // Followup timeout
    if (std::holds_alternative<SFollowup*>(getCurrentState())) {
        followupTicks_++;
        constexpr uint32_t kFollowupTimeout = 30 * 50;  // 30 seconds
        if (followupTicks_ >= kFollowupTimeout) {
            ESP_LOGI(TAG, "followup timeout → Idle");
            goIdle();
        }
        return;
    }
}

// ── OPUS encode task ───────────────────────────────────────────────────

void SessionManagerService::startEncodeTask() {
    if (encodeTask_) return;
    auto ret = xTaskCreatePinnedToCore(encodeTaskEntry, "sess_encode", kEncodeTaskStack,
                                       this, 5, &encodeTask_, 0);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "encode task create FAILED (stack=%zu)", kEncodeTaskStack);
        ESP_LOGE(TAG, "  free_internal=%u, largest_block=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        encodeTask_ = nullptr;
        return;
    }
    ESP_LOGI(TAG, "encode task created (stack=%zu, handle=%p)", kEncodeTaskStack, encodeTask_);
}

void SessionManagerService::stopEncodeTask() {
    if (encodeTask_) {
        vTaskDelete(encodeTask_);
        encodeTask_ = nullptr;
    }
}

void SessionManagerService::encodeTaskEntry(void* arg) {
    auto* self = static_cast<SessionManagerService*>(arg);
    self->encodeLoop();
    // NOTE: do NOT call vTaskDelete here — stopEncodeTask() is the single owner of deletion
}

void SessionManagerService::encodeLoop() {
    uint8_t opusBuf[OpusCodec::kMaxPacketSize];

    while (true) {
        EncodeFrameMsg msg;
        if (xQueueReceive(encodePendQ_, &msg, pdMS_TO_TICKS(200)) == pdTRUE) {
            if (msg.buf) {
                int encLen = opus_.encode(
                    reinterpret_cast<const int16_t*>(msg.buf),
                    static_cast<int>(kUdpFrameSamples),
                    opusBuf, sizeof(opusBuf));
                xQueueSend(encodeFreeQ_, &msg.buf, 0);
                if (encLen > 0) {
                    sendMicFrame(opusBuf, static_cast<size_t>(encLen), msg.flags);
                    micFramesSent_++;
                    if (micFramesSent_ <= 3 || micFramesSent_ % 50 == 0) {
                        ESP_LOGI(TAG, "encode #%u: opus=%d bytes, flags=0x%02x, stack_hwm=%u",
                                 micFramesSent_, encLen, msg.flags,
                                 (unsigned)uxTaskGetStackHighWaterMark(nullptr));
                    }
                } else {
                    ESP_LOGW(TAG, "OPUS encode failed: ret=%d", encLen);
                }
            }
        }
    }
}

// ── OPUS playback (decode) task ────────────────────────────────────────

void SessionManagerService::startPlaybackTask() {
    if (playbackTask_) return;
    playbackRunning_ = false;  // idle until session starts
    auto ret = xTaskCreatePinnedToCore(playbackTaskEntry, "sess_playback", kPlaybackTaskStack,
                                       this, 6, &playbackTask_, 0);  // core 0 — same as UDP rx, avoids cross-core ring buffer issues
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "playback task create FAILED (stack=%zu)", kPlaybackTaskStack);
        ESP_LOGE(TAG, "  free_internal=%u, largest_block=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        playbackTask_ = nullptr;
        return;
    }
    ESP_LOGI(TAG, "playback task created (stack=%zu, handle=%p, core=1)", kPlaybackTaskStack, playbackTask_);
}

void SessionManagerService::stopPlaybackTask() {
    if (playbackTask_) {
        vTaskDelete(playbackTask_);
        playbackTask_ = nullptr;
    }
}

void SessionManagerService::playbackTaskEntry(void* arg) {
    auto* self = static_cast<SessionManagerService*>(arg);
    self->playbackLoop();
    // NOTE: do NOT call vTaskDelete here — stopPlaybackTask() is the single owner of deletion
}

void SessionManagerService::playbackLoop() {
    int16_t pcm[kUdpFrameSamples];
    bool wasRunning = false;
    uint32_t loopIterCount = 0;
    uint32_t emptyRecvCount = 0;

    while (true) {
        if (playbackRunning_ && playPendQ_) {
            if (!wasRunning) {
                ESP_LOGI(TAG, "playback: session active, waiting for queue data");
                wasRunning = true;
                loopIterCount = 0;
                emptyRecvCount = 0;
            }

            // Receive one frame from the playback queue (blocks up to 100ms).
            // FreeRTOS queue is reliable across cores — unlike ring buffer in PSRAM.
            uint8_t* buf = nullptr;
            if (xQueueReceive(playPendQ_, &buf, pdMS_TO_TICKS(100)) == pdTRUE && buf) {
                loopIterCount++;
                uint16_t opusLen = 0;
                memcpy(&opusLen, buf, sizeof(opusLen));
                if (opusLen > 0) {
                    uint32_t t0 = xTaskGetTickCount();
                    int decSamples = opus_.decode(
                        buf + sizeof(opusLen), static_cast<int>(opusLen),
                        pcm, static_cast<int>(kUdpFrameSamples));
                    uint32_t t1 = xTaskGetTickCount();
                    decodeCount_++;
                    if (decSamples > 0) {
                        if (decodeCount_ <= 5 || decodeCount_ % 10 == 0) {
                            ESP_LOGI(TAG, "decode #%u: opus=%u -> pcm=%d samples (decode=%ums, stack_hwm=%u)",
                                     decodeCount_, opusLen, decSamples,
                                     (unsigned)((t1 - t0) * portTICK_PERIOD_MS),
                                     (unsigned)uxTaskGetStackHighWaterMark(nullptr));
                        }
                        if (audioBus_ && audioBus_->isReady()) {
                            bool ok = audioBus_->play(pcm, static_cast<size_t>(decSamples));
                            uint32_t t2 = xTaskGetTickCount();
                            playCount_++;
                            if (playCount_ <= 5 || playCount_ % 10 == 0) {
                                ESP_LOGI(TAG, "play #%u: i2s=%ums loops=%u empty=%u",
                                         playCount_,
                                         (unsigned)((t2 - t1) * portTICK_PERIOD_MS),
                                         loopIterCount, emptyRecvCount);
                            }
                            if (!ok) {
                                playErrors_++;
                                if (playErrors_ <= 3) {
                                    ESP_LOGW(TAG, "I2S play failed #%u", playErrors_);
                                }
                            }
                        }
                    } else if (decSamples <= 0) {
                        decodeErrors_++;
                        ESP_LOGW(TAG, "OPUS decode failed #%u: ret=%d len=%u",
                                 decodeErrors_, decSamples, opusLen);
                    }
                }
                // Return buffer to free pool
                xQueueSend(playFreeQ_, &buf, 0);
            } else {
                emptyRecvCount++;
                // Queue empty — check if we should stop
                if (endOfResponseReceived_ || pendingStop_) {
                    ESP_LOGI(TAG, "playback queue drained — all frames played (loops=%u empty=%u stop=%s)",
                             loopIterCount, emptyRecvCount,
                             pendingStop_ ? "forced" : "natural");
                    endOfResponseReceived_ = false;
                    pendingStop_ = false;
                    playbackRunning_ = false;
                    // Transition back to Recording for 30s follow-up listening.
                    // Set followUpRecording_ BEFORE the transition so onStateChanged
                    // knows this is a follow-up entry (not initial Recording).
                    followUpRecording_ = true;
                    this->handle(EvtResumeListening{});
                }
            }
        } else {
            // No session active — idle until playback is activated
            wasRunning = false;
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
}

} // namespace embed::audio
