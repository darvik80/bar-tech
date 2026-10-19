#include "udp_audio/udp_audio_transport.hpp"

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <lwip/sockets.h>
#include <lwip/netdb.h>
#include <arpa/inet.h>
#include <cstring>
#include <sdkconfig.h>

static constexpr char TAG[] = "UdpAudio";

namespace embed::audio {

// ── Destructor ──────────────────────────────────────────────────────────

UdpAudioTransport::~UdpAudioTransport() {
    stop();
}

// ── init (boot-time) ───────────────────────────────────────────────────

bool UdpAudioTransport::init() {
    if (sock_ >= 0) {
        ESP_LOGW(TAG, "init: already open");
        return true;
    }

    sock_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock_ < 0) {
        ESP_LOGE(TAG, "socket() failed: errno %d", errno);
        return false;
    }

    // Bind to ephemeral port
    struct sockaddr_in bindAddr{};
    bindAddr.sin_family      = AF_INET;
    bindAddr.sin_addr.s_addr = htonl(INADDR_ANY);
    bindAddr.sin_port        = htons(0);

    if (::bind(sock_, reinterpret_cast<struct sockaddr*>(&bindAddr),
               sizeof(bindAddr)) < 0) {
        ESP_LOGE(TAG, "bind() failed: errno %d", errno);
        ::close(sock_);
        sock_ = -1;
        return false;
    }

    // Increase socket receive buffer (absorb bursts from server)
    // lwIP allocates from CONFIG_LWIP_MEM_SIZE pool, so keep reasonable.
    int rcvBufSize = 8 * 1024;
    if (setsockopt(sock_, SOL_SOCKET, SO_RCVBUF, &rcvBufSize, sizeof(rcvBufSize)) < 0) {
        ESP_LOGW(TAG, "setsockopt SO_RCVBUF(%d) failed: errno %d", rcvBufSize, errno);
    }

    // Set receive timeout so the receive loop can check running_ periodically
    struct timeval tv;
    tv.tv_sec  = 1;
    tv.tv_usec = 0;
    setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    // Allocate receive buffer in PSRAM (keeps task stack small)
    if (!rxBuffer_) {
        rxBuffer_ = static_cast<uint8_t*>(
            heap_caps_calloc(1, kRxBufferSize, MALLOC_CAP_SPIRAM));
        if (!rxBuffer_) {
            ESP_LOGE(TAG, "rx buffer alloc failed (%zu bytes)", kRxBufferSize);
            ::close(sock_);
            sock_ = -1;
            return false;
        }
    }

    // Get local port
    struct sockaddr_in localAddr{};
    socklen_t addrLen = sizeof(localAddr);
    getsockname(sock_, reinterpret_cast<struct sockaddr*>(&localAddr), &addrLen);
    uint16_t localPort = ntohs(localAddr.sin_port);

    ESP_LOGI(TAG, "init: socket created, local_port=%u", localPort);

    // Start receive task at boot (it will loop on recvfrom, rejecting packets
    // until a valid session is set via open())
    startReceive();

    return true;
}

// ── resetSession ────────────────────────────────────────────────────────

void UdpAudioTransport::resetSession() {
    ESP_LOGI(TAG, "resetSession: received=%u frames this session", framesReceived_);
    sessionId_ = 0;
    recvLastSeq_ = -1;
    recvExpectedSeq_ = 0;
    outOfOrderCount_ = 0;
    framesReceived_ = 0;
}

// ── open ────────────────────────────────────────────────────────────────

uint16_t UdpAudioTransport::open(const char* serverIp, uint16_t port,
                                 uint32_t sessionId) {
    // If socket already exists (boot-created via init()), just update server/session
    if (sock_ >= 0) {
        // Resolve server address
        memset(&serverAddr_, 0, sizeof(serverAddr_));
        serverAddr_.sin_family = AF_INET;
        serverAddr_.sin_port   = htons(port);
        if (inet_pton(AF_INET, serverIp, &serverAddr_.sin_addr) != 1) {
            ESP_LOGE(TAG, "invalid server IP: %s", serverIp);
            return 0;
        }

        sessionId_ = sessionId;
        frameSeq_  = 0;
        recvExpectedSeq_ = 0;
        recvLastSeq_     = -1;
        outOfOrderCount_ = 0;
        framesSent_      = 0;
        bytesSent_       = 0;
        framesReceived_  = 0;

        // Get local port
        struct sockaddr_in localAddr{};
        socklen_t addrLen = sizeof(localAddr);
        getsockname(sock_, reinterpret_cast<struct sockaddr*>(&localAddr), &addrLen);
        uint16_t localPort = ntohs(localAddr.sin_port);

        ESP_LOGI(TAG, "opened: local_port=%u -> %s:%u (session=%u) [reuse socket]",
                 localPort, serverIp, port, sessionId);
        return localPort;
    }

    // Fallback: create socket from scratch (if init() was not called)
    sock_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock_ < 0) {
        ESP_LOGE(TAG, "socket() failed: errno %d", errno);
        return 0;
    }

    struct sockaddr_in bindAddr{};
    bindAddr.sin_family      = AF_INET;
    bindAddr.sin_addr.s_addr = htonl(INADDR_ANY);
    bindAddr.sin_port        = htons(0);

    if (::bind(sock_, reinterpret_cast<struct sockaddr*>(&bindAddr),
               sizeof(bindAddr)) < 0) {
        ESP_LOGE(TAG, "bind() failed: errno %d", errno);
        ::close(sock_);
        sock_ = -1;
        return 0;
    }

    int rcvBufSize = 32 * 1024;
    setsockopt(sock_, SOL_SOCKET, SO_RCVBUF, &rcvBufSize, sizeof(rcvBufSize));

    struct timeval tv;
    tv.tv_sec  = 1;
    tv.tv_usec = 0;
    setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    memset(&serverAddr_, 0, sizeof(serverAddr_));
    serverAddr_.sin_family = AF_INET;
    serverAddr_.sin_port   = htons(port);
    if (inet_pton(AF_INET, serverIp, &serverAddr_.sin_addr) != 1) {
        ESP_LOGE(TAG, "invalid server IP: %s", serverIp);
        ::close(sock_);
        sock_ = -1;
        return 0;
    }

    sessionId_ = sessionId;
    frameSeq_  = 0;
    recvExpectedSeq_ = 0;
    recvLastSeq_     = -1;
    outOfOrderCount_ = 0;
    framesSent_      = 0;
    bytesSent_       = 0;
    framesReceived_  = 0;

    // Allocate rx buffer if not already done
    if (!rxBuffer_) {
        rxBuffer_ = static_cast<uint8_t*>(
            heap_caps_calloc(1, kRxBufferSize, MALLOC_CAP_SPIRAM));
        if (!rxBuffer_) {
            ESP_LOGE(TAG, "rx buffer alloc failed (%zu bytes)", kRxBufferSize);
            ::close(sock_);
            sock_ = -1;
            return 0;
        }
    }

    struct sockaddr_in localAddr{};
    socklen_t addrLen = sizeof(localAddr);
    getsockname(sock_, reinterpret_cast<struct sockaddr*>(&localAddr), &addrLen);
    uint16_t localPort = ntohs(localAddr.sin_port);

    ESP_LOGI(TAG, "opened: local_port=%u -> %s:%u (session=%u)",
             localPort, serverIp, port, sessionId);
    return localPort;
}

// ── startReceive ────────────────────────────────────────────────────────

void UdpAudioTransport::startReceive() {
    if (running_) return;
    if (sock_ < 0) {
        ESP_LOGE(TAG, "cannot start receive — socket not open");
        return;
    }

    running_ = true;
    constexpr int kStack = CONFIG_EMBED_UDP_AUDIO_RECV_TASK_STACK;
    auto ret = xTaskCreate(receiveTaskEntry, "udp_audio_rx", kStack, this, 6,
                reinterpret_cast<TaskHandle_t*>(&recvTask_));
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "receive task create FAILED (stack=%d)", kStack);
        recvTask_ = nullptr;
        return;
    }
    ESP_LOGI(TAG, "receive task started (stack=%d, handle=%p)", kStack, recvTask_);
}

// ── stop ────────────────────────────────────────────────────────────────

void UdpAudioTransport::stop() {
    running_ = false;

    if (recvTask_) {
        // Give the task time to exit its loop
        vTaskDelay(pdMS_TO_TICKS(200));
        vTaskDelete(static_cast<TaskHandle_t>(recvTask_));
        recvTask_ = nullptr;
    }

    if (sock_ >= 0) {
        // Log stats before closing
        ESP_LOGI(TAG, "stats: sent=%u frames (%u bytes), received=%u frames",
                 framesSent_, bytesSent_, framesReceived_);
        ::close(sock_);
        sock_ = -1;
    }

    // Free receive buffer
    if (rxBuffer_) {
        heap_caps_free(rxBuffer_);
        rxBuffer_ = nullptr;
    }

    ESP_LOGI(TAG, "stopped");
}

// ── sendFrame ───────────────────────────────────────────────────────────

void UdpAudioTransport::sendFrame(const uint8_t* payload, size_t len,
                                  uint8_t flags) {
    if (sock_ < 0) return;

    // Build packet: header + payload
    const size_t pktLen = kHeaderSize + len;
    // Stack-allocate for small frames, heap for large ones
    uint8_t stackBuf[128];
    uint8_t* buf = (pktLen <= sizeof(stackBuf)) ? stackBuf
                                                : static_cast<uint8_t*>(malloc(pktLen));
    if (!buf) {
        ESP_LOGE(TAG, "alloc failed for %zu byte packet", pktLen);
        return;
    }

    // Pack header (big-endian)
    uint32_t sid = htonl(sessionId_);
    uint16_t seq = htons(frameSeq_);
    memcpy(buf, &sid, 4);
    memcpy(buf + 4, &seq, 2);
    buf[6] = flags;
    buf[7] = 0; // reserved

    // Copy payload
    if (payload && len > 0) {
        memcpy(buf + kHeaderSize, payload, len);
    }

    // Send
    ssize_t sent = ::sendto(sock_, buf, pktLen, 0,
                            reinterpret_cast<struct sockaddr*>(&serverAddr_),
                            sizeof(serverAddr_));
    if (sent < 0) {
        ESP_LOGE(TAG, "sendto() failed: errno %d", errno);
    }

    frameSeq_++;
    framesSent_++;
    bytesSent_ += static_cast<uint32_t>(len);

    // Log every 50th frame
    if (framesSent_ % 50 == 0) {
        ESP_LOGI(TAG, "tx: frame #%u -> %u bytes total",
                 framesSent_, bytesSent_);
    }
    // Log first 3 frames for diagnostics
    if (framesSent_ <= 3) {
        ESP_LOGI(TAG, "tx first #%u: payload=%zu bytes, flags=0x%02x, sent=%zd",
                 framesSent_, len, flags, sent);
    }

    if (buf != stackBuf) free(buf);
}

// ── sendPreRoll ─────────────────────────────────────────────────────────

void UdpAudioTransport::sendPreRoll(const uint8_t* const* frames,
                                    const size_t* lengths, size_t count) {
    if (!frames || count == 0) return;

    for (size_t i = 0; i < count; ++i) {
        uint8_t flags = (i == 0) ? kFlagStartOfSpeech : 0;
        sendFrame(frames[i], lengths[i], flags);
    }
    ESP_LOGI(TAG, "pre-roll sent: %zu frames", count);
}

// ── Receive task ────────────────────────────────────────────────────────

void UdpAudioTransport::receiveTaskEntry(void* arg) {
    auto* self = static_cast<UdpAudioTransport*>(arg);
    self->receiveLoop();
    vTaskDelete(nullptr);
}

void UdpAudioTransport::receiveLoop() {
    // Small delay to let UDP connection establish
    vTaskDelay(pdMS_TO_TICKS(100));

    ESP_LOGI(TAG, "rx loop entered: sock=%d sessionId=%u", sock_, sessionId_);

    uint32_t rxTimeouts = 0;  // local diagnostic counter

    while (running_ && sock_ >= 0) {
        struct sockaddr_in fromAddr{};
        socklen_t fromLen = sizeof(fromAddr);
        ssize_t n = ::recvfrom(sock_, rxBuffer_, kRxBufferSize, 0,
                               reinterpret_cast<struct sockaddr*>(&fromAddr),
                               &fromLen);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                rxTimeouts++;
                if (rxTimeouts == 1 || rxTimeouts % 10 == 0) {
                    ESP_LOGD(TAG, "rx timeout #%u (sock=%d running=%d)",
                             rxTimeouts, sock_, (int)running_);
                }
                continue; // timeout — check running_
            }
            if (running_) {
                ESP_LOGE(TAG, "recvfrom error: errno %d", errno);
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            continue;
        }

        if (static_cast<size_t>(n) < kHeaderSize) {
            ESP_LOGW(TAG, "packet too short: %zd bytes", n);
            continue;
        }

        // Unpack header (big-endian)
        uint32_t sid;
        uint16_t seq;
        uint8_t  flags;
        memcpy(&sid, rxBuffer_, 4);
        sid = ntohl(sid);
        memcpy(&seq, rxBuffer_ + 4, 2);
        seq = ntohs(seq);
        flags = rxBuffer_[6];

        // Validate session
        if (sid != sessionId_) {
            ESP_LOGW(TAG, "session mismatch: expected %u got %u",
                     sessionId_, sid);
            continue;
        }

        // START_OF_RESPONSE flag means a new TTS response — reset sequence
        // tracking so the first frame (seq=0) doesn't look out-of-order
        // relative to the previous response's last seq.
        // Also reset on seq=0 even without the flag — some server implementations
        // send an empty initial frame before the flagged start frame.
        if ((flags & kFlagStartOfResponse) || seq == 0) {
            recvExpectedSeq_  = seq;
            recvLastSeq_      = -1;
            outOfOrderCount_  = 0;
        }

        // Track sequence (log warnings but never drop — audio is best-effort)
        validateRecvSeq(seq);

        framesReceived_++;
        if (framesReceived_ <= 3) {
            ESP_LOGI(TAG, "rx first #%u: %zd bytes, seq=%u flags=0x%02x from %s:%u",
                     framesReceived_, n, seq, flags,
                     inet_ntoa(fromAddr.sin_addr), ntohs(fromAddr.sin_port));
        }
        const uint8_t* payload = rxBuffer_ + kHeaderSize;
        size_t payloadLen = static_cast<size_t>(n) - kHeaderSize;

        if (responseCb_) {
            responseCb_(payload, payloadLen, flags, responseCtx_);
        }
    }
}

bool UdpAudioTransport::validateRecvSeq(uint16_t seq) {
    if (recvLastSeq_ < 0) {
        // First packet — initialize from its seq
        recvExpectedSeq_ = seq;
        recvLastSeq_     = seq;
        return true;
    }
    if (seq < recvExpectedSeq_) {
        outOfOrderCount_++;
        if (outOfOrderCount_ <= 3) {
            ESP_LOGW(TAG, "out-of-order: seq=%u expected=%u (count=%u) — accepting",
                     seq, recvExpectedSeq_, outOfOrderCount_);
        }
        // Accept anyway — late audio is better than missing audio
        recvLastSeq_ = seq;
        return true;
    }
    if (seq > recvExpectedSeq_) {
        uint16_t gap = seq - recvExpectedSeq_;
        if (gap > 1) {
            ESP_LOGW(TAG, "seq gap: seq=%u expected=%u (gap=%u)",
                     seq, recvExpectedSeq_, gap);
        }
    }
    recvExpectedSeq_ = seq + 1;
    recvLastSeq_     = seq;
    return true;
}

} // namespace embed::audio
