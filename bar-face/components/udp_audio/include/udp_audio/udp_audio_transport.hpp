#pragma once

#include <cstdint>
#include <cstddef>
#include <lwip/sockets.h>

namespace embed::audio {

// ── UDP audio packet protocol ──────────────────────────────────────────
//
// Wire format (8-byte header + variable payload):
//
//   uint32_t  udp_session_id   (big-endian)
//   uint16_t  frame_seq        (big-endian)
//   uint8_t   flags
//   uint8_t   reserved
//
// Device → server flags:
constexpr uint8_t kFlagStartOfSpeech  = 0x01;
constexpr uint8_t kFlagEndOfSpeech    = 0x02;
constexpr uint8_t kFlagMute           = 0x04;
//
// Server → device flags:
constexpr uint8_t kFlagStartOfResponse = 0x01;
constexpr uint8_t kFlagEndOfResponse   = 0x02;
constexpr uint8_t kFlagError           = 0x04;

constexpr size_t kHeaderSize = 8;

// ── Callback types ─────────────────────────────────────────────────────

/// Called for each incoming audio frame from the server.
/// @param payload  pointer to audio data (after header)
/// @param len      payload length in bytes
/// @param flags    protocol flags from the header
using ResponseCallback = void(*)(const uint8_t* payload, size_t len,
                                 uint8_t flags, void* ctx);

// ── UdpAudioTransport ──────────────────────────────────────────────────

/// Low-level UDP audio transport for Cogitor voice sessions.
///
/// Matches the Python simulator's udp_audio.UDPAudioTransport protocol:
///   • open()   — create UDP socket, bind to ephemeral port
///   • sendFrame() — pack 8-byte header + payload, sendto server
///   • Receive task — recvfrom, validate session/seq, invoke callback
///
/// Thread-safe: sendFrame() can be called from any task. The receive
/// callback fires on the receive task's context (core 0).
class UdpAudioTransport {
public:
    UdpAudioTransport() = default;
    ~UdpAudioTransport();

    /// Open UDP socket and prepare for transmission.
    /// If socket already exists (boot-created), just updates server address and session.
    /// @return local ephemeral port, or 0 on failure
    uint16_t open(const char* serverIp, uint16_t port, uint32_t sessionId);

    /// Create UDP socket, allocate rx buffer and start receive task at boot.
    /// @return true on success
    bool init();

    /// Reset session ID (reject incoming packets until next open()).
    void resetSession();

    /// Start the receive task (call after init() or open()).
    void startReceive();

    /// Close socket and stop receive task.
    void stop();

    /// Send one audio frame to the server.
    /// @param payload  audio data (e.g. raw PCM16 bytes)
    /// @param len      payload length in bytes
    /// @param flags    protocol flag bits (kFlagStartOfSpeech, etc.)
    void sendFrame(const uint8_t* payload, size_t len, uint8_t flags = 0);

    /// Send pre-roll frames with START_OF_SPEECH on the first frame.
    void sendPreRoll(const uint8_t* const* frames, const size_t* lengths,
                     size_t count);

    /// Register callback for incoming response audio.
    void setResponseCallback(ResponseCallback cb, void* ctx) {
        responseCb_  = cb;
        responseCtx_ = ctx;
    }

    /// True if socket is open.
    bool isOpen() const { return sock_ >= 0; }

    /// Stats
    uint32_t framesSent()     const { return framesSent_; }
    uint32_t bytesSent()      const { return bytesSent_; }
    uint32_t framesReceived() const { return framesReceived_; }

private:
    static void receiveTaskEntry(void* arg);
    void receiveLoop();
    bool validateRecvSeq(uint16_t seq);

    int         sock_       = -1;
    uint32_t    sessionId_  = 0;
    uint16_t    frameSeq_   = 0;

    // Receive sequence tracking
    uint16_t    recvExpectedSeq_  = 0;
    int16_t     recvLastSeq_      = -1;
    uint32_t    outOfOrderCount_  = 0;

    // Stats
    uint32_t    framesSent_     = 0;
    uint32_t    bytesSent_      = 0;
    uint32_t    framesReceived_ = 0;

    // Server address
    struct sockaddr_in serverAddr_ {};

    // Receive task
    void*       recvTask_    = nullptr;   ///< TaskHandle_t
    volatile bool running_   = false;

    // Receive buffer (heap-allocated in PSRAM to keep task stack small)
    static constexpr size_t kRxBufferSize = 2048;
    uint8_t*    rxBuffer_    = nullptr;

    // Response callback
    ResponseCallback responseCb_  = nullptr;
    void*            responseCtx_ = nullptr;
};

} // namespace embed::audio
