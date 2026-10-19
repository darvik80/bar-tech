#pragma once

#include "embed/embed.hpp"
#include "embed/state_machine.hpp"
#include "embed_core/mqtt_service.hpp"
#include "cogitor_iot/cogitor_iot_service.hpp"
#include "voice_detection/voice_detection_service.hpp"
#include "display/display_service.hpp"
#include "audio_bus/audio_bus_service.hpp"
#include "udp_audio/udp_audio_transport.hpp"
#include "opus_codec.hpp"

#include <cstdint>
#include <cstddef>
#include <string_view>

namespace embed::audio {

// ── Session events ─────────────────────────────────────────────────────

struct EvtWakeWord {};
struct EvtSessionStart {};
struct EvtSessionStop {};
struct EvtUiThinking {};       // server sent ui_state=thinking/speaking
struct EvtUiIdle {};           // server sent ui_state=idle
struct EvtSessionClosed {};
struct EvtEndOfResponse {};
struct EvtAudioResponseStarted {};
struct EvtResumeListening {};   // playback drained → back to Recording for follow-up

// Tick-driven timeout events
struct EvtSilenceTimeout {};
struct EvtMaxDuration {};
struct EvtPlaybackTimeout {};
struct EvtPlaybackGap {};

// ── Session states ─────────────────────────────────────────────────────

struct SIdle;
struct SWaitSession;
struct SRecording;
struct SPlayback;
struct SFollowup;

struct SIdle        : embed::State<embed::On<EvtWakeWord, SWaitSession>> {};
struct SWaitSession : embed::State<embed::On<EvtSessionStart, SRecording>,
                                   embed::On<EvtSessionStop, SIdle>> {};
struct SRecording   : embed::State<embed::On<EvtUiThinking, SPlayback>,
                                   embed::On<EvtSilenceTimeout, SPlayback>,
                                   embed::On<EvtMaxDuration, SPlayback>,
                                   embed::On<EvtSessionStop, SIdle>,
                                   embed::On<EvtSessionClosed, SIdle>> {};
struct SPlayback    : embed::State<embed::On<EvtPlaybackTimeout, SFollowup>,
                                   embed::On<EvtPlaybackGap, SFollowup>,
                                   embed::On<EvtResumeListening, SRecording>,
                                   embed::On<EvtUiThinking, SPlayback>,  // repeated thinking — stay in Playback
                                   embed::On<EvtSessionStop, SIdle>,
                                   embed::On<EvtSessionClosed, SIdle>> {};
struct SFollowup    : embed::State<embed::On<EvtUiThinking, SPlayback>,  // user spoke during follow-up → server responds
                                   embed::On<EvtResumeListening, SRecording>,
                                   embed::On<EvtSessionStop, SIdle>> {};

/// Session state machine for Cogitor voice conversation.
///
/// Uses embed::StateMachine for state tracking.
/// States: Idle → WaitSession → Recording → Playback → Followup → Idle
///
/// Wires together:
///   • VoiceDetectionService — wake word + VAD + raw mic audio
///   • IotService            — MQTT events, RPC, attribute updates
///   • UdpAudioTransport     — bidirectional audio streaming
///   • AudioBusService       — speaker playback of response audio
///   • DisplayService        — visual state feedback
///
/// Mirrors the Python simulator's session.SessionManager.
class SessionManagerService
    : public embed::Service
    , public embed::StateMachine<SessionManagerService,
          SIdle, SWaitSession, SRecording, SPlayback, SFollowup>
{
public:
    SessionManagerService() = default;
    const char* serviceName() const override { return "SessionManager"; }

    void start() override;
    void stop() override;

    /// Called by StateMachine after every transition.
    void onStateChanged(const embed::TransitionTo<SWaitSession>&);
    void onStateChanged(const embed::TransitionTo<SRecording>&);
    void onStateChanged(const embed::TransitionTo<SPlayback>&);
    void onStateChanged(const embed::TransitionTo<SFollowup>&);
    void onStateChanged(const embed::TransitionTo<SIdle>&);
    void onStateChanged(const embed::Nothing&);

private:
    // ── Signal/Slot handlers ───────────────────────────────────────────

    Slot<WakeWordDetected>                   wakeWordSlot_{onWakeWord, this};
    Slot<VoiceActivityEvent>                 voiceActivitySlot_{onVoiceActivity, this};
    Slot<cogitor::iot::AttributeUpdate>      attrUpdateSlot_{onAttributeUpdate, this};

    static void onWakeWord(const WakeWordDetected& msg, void* ctx);
    static void onVoiceActivity(const VoiceActivityEvent& msg, void* ctx);
    static void onAttributeUpdate(const cogitor::iot::AttributeUpdate& msg, void* ctx);

    // ── Audio frame callback (from VoiceDetectionService feed task) ────

    static void onMicAudioFrame(const int16_t* samples, size_t count, void* ctx);

    // ── UDP response callback ──────────────────────────────────────────

    static void onResponseFrame(const uint8_t* payload, size_t len,
                                uint8_t flags, void* ctx);

    // ── RPC handler ────────────────────────────────────────────────────

    static void onSessionControlRpc(cogitor::iot::IotService& iot,
                                    std::string_view requestId,
                                    const cogitor::iot::RpcParams& params,
                                    void* ctx);

    /// Phase-1 pour stub — no GPIO; acknowledges tap/ml for dialog tools.
    static void onPourRpc(cogitor::iot::IotService& iot,
                          std::string_view requestId,
                          const cogitor::iot::RpcParams& params,
                          void* ctx);

    // ── Event handlers (side-effects + state dispatch) ─────────────────

    void handleWakeWord(const WakeWordDetected& msg);
    void handleSessionStart(const cogitor::iot::RpcParams& params);
    void handleSessionStop(const cogitor::iot::RpcParams& params);
    void handleAttrUpdate(const char* json);

    /// Transition + cleanup (UDP, playback, pre-roll).
    /// @param force If true, skip the "let playback finish" check (used by playback task itself).
    void goIdle(bool force = false);

    // ── Display ────────────────────────────────────────────────────────

    void requestDisplayAsset(const char* asset);

    // ── Mic audio → UDP forwarding ─────────────────────────────────────

    void processMicFrame(const int16_t* samples, size_t count);
    void sendMicFrame(const uint8_t* payload, size_t len, uint8_t flags);

    // ── Tick / timeout handling ────────────────────────────────────────

    static void tickTimerEntry(void* arg);
    void tick();

    // ── Mic frame re-chunking constants ───────────────────────────────

    static constexpr size_t kUdpFrameSamples = 960;  // 60ms @ 16kHz (matches xiaozhi + server)
    static constexpr size_t kUdpFrameBytes   = kUdpFrameSamples * 2;

    // ── OPUS codec tasks (separate encode + decode) ───────────────────

    static constexpr size_t kEncodePoolSize    = 4;
    static constexpr size_t kEncodeTaskStack   = 28000;  // encode (SILK path) is stack-heavy
    // Decode-only task — far lighter than encode (xiaozhi runs BOTH directions
    // in one 24KB task). 16KB leaves headroom while fitting the largest
    // internal-RAM block alongside esp-sr (~19KB largest block at boot).
    static constexpr size_t kPlaybackTaskStack = 16384;

    void startEncodeTask();
    void stopEncodeTask();
    static void encodeTaskEntry(void* arg);
    void encodeLoop();

    void startPlaybackTask();
    void stopPlaybackTask();
    static void playbackTaskEntry(void* arg);
    void playbackLoop();

    // ── Services (resolved in start()) ─────────────────────────────────

    cogitor::iot::IotService*  iot_       = nullptr;
    VoiceDetectionService*     voice_     = nullptr;
    display::DisplayService*   display_   = nullptr;
    AudioBusService*           audioBus_  = nullptr;

    // ── Owned transport ────────────────────────────────────────────────

    UdpAudioTransport udp_;
    OpusCodec         opus_;

    // ── State ──────────────────────────────────────────────────────────

    portMUX_TYPE lock_ = portMUX_INITIALIZER_UNLOCKED;

    // Session params (from RPC start)
    uint32_t udpSessionId_ = 0;
    char     sessionId_[64]{};

    // VAD tracking
    bool     isSpeaking_       = false;
    uint32_t lastSpeechTick_   = 0;
    uint32_t silenceTicks_     = 0;
    bool     endOfSpeechSent_  = false;  ///< true once END_OF_SPEECH flag sent for current silence period
    bool     wasSpeaking_      = false;  ///< previous isSpeaking_ state for edge detection
    uint8_t  trailingFrames_   = 0;      ///< frames to send after speech ends (natural cutoff)
    // NOTE: speaker→mic feedback prevention is now handled by
    // VoiceDetectionService::EnableVoiceProcessing(false) during Playback state.

    // Recording tracking
    uint32_t recordingTicks_   = 0;
    volatile uint32_t recordingSessionToken_ = 0;  ///< incremented on each SRecording entry; tick() checks to detect stale state
    bool     followUpRecording_ = false;  ///< true when Recording re-entered after Playback (30s listen window)

    // Playback tracking
    uint32_t playbackTicks_    = 0;
    bool     responseAudioStarted_ = false;
    uint32_t responseFrames_   = 0;
    uint32_t lastAudioTick_    = 0;

    // Followup tracking
    uint32_t followupTicks_    = 0;

    // Wait session tracking
    uint32_t waitTicks_        = 0;

    // Tick counter (incremented every 20ms by tick timer)
    volatile uint32_t tickCount_ = 0;

    // ── OPUS codec task state ─────────────────────────────────────────

    uint8_t* encodePool_       = nullptr;  ///< kEncodePoolSize × kUdpFrameBytes
    QueueHandle_t encodeFreeQ_ = nullptr;  ///< available pool buffers
    QueueHandle_t encodePendQ_ = nullptr;  ///< frames to encode
    TaskHandle_t encodeTask_   = nullptr;  ///< OPUS encode task
    TaskHandle_t playbackTask_ = nullptr;  ///< OPUS decode + playback task

    // ── Speaker playback queue (replaces ring buffer) ──────────────────

    static constexpr size_t kPlayPoolSize  = 64;   ///< max buffered playback frames
    static constexpr size_t kPlayBufSize   = 512;  ///< bytes per frame buffer (header + OPUS)

    uint8_t*      playPool_       = nullptr;  ///< kPlayPoolSize × kPlayBufSize
    QueueHandle_t playFreeQ_      = nullptr;  ///< available pool buffers (holds uint8_t*)
    QueueHandle_t playPendQ_      = nullptr;  ///< frames to decode (holds uint8_t*)
    volatile bool playbackRunning_ = false;
    volatile bool endOfResponseReceived_ = false;
    volatile bool pendingStop_ = false;  ///< set by goIdle() during Playback — playback task transitions when queue drained

    // ── Tick timer ─────────────────────────────────────────────────────

    esp_timer_handle_t tickTimer_ = nullptr;

    // ── Display task (SPI transfers require task context) ─────────────
    TaskHandle_t displayTask_    = nullptr;
    volatile bool displayTaskRunning_ = false;
    volatile const char* pendingAsset_ = nullptr;

    static void displayTaskEntry(void* arg);
    void displayTaskLoop();

    // ── Mic frame re-chunking buffer (heap-allocated) ─────────────────

    uint8_t* micAccum_    = nullptr;
    size_t   micAccumLen_ = 0;

    // ── Encode/Decode diagnostics ─────────────────────────────────────

    uint32_t micFramesQueued_ = 0;   ///< frames queued to encode task
    uint32_t micFramesSent_   = 0;   ///< frames actually encoded & sent via UDP

    // Playback diagnostics (updated by playbackLoop, logged on state transitions)
    volatile uint32_t decodeCount_    = 0;
    volatile uint32_t decodeErrors_   = 0;
    volatile uint32_t playCount_      = 0;
    volatile uint32_t playErrors_     = 0;
    volatile uint32_t ringBufDrops_   = 0;  ///< frames dropped because ring buffer was full
    volatile uint32_t ringBufWrites_  = 0;  ///< frames successfully written to ring buffer
};

} // namespace embed::audio
