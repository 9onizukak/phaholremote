#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

namespace remote {
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

constexpr uint16_t kControlPort = 53540;
constexpr uint16_t kViewerPort = 53541;
constexpr uint32_t kMagic = 0x50485231; // PHR1
constexpr uint32_t kVersion = 6;
constexpr uint32_t kSystemActionVersion = 6;
constexpr uint32_t kVideoOnlyVersion = 2;
constexpr uint32_t kFileTransferVersion = 3;
constexpr uint32_t kMouseResultVersion = 4;
constexpr uint32_t kChunkBytes = 1200;
constexpr uint32_t kMaxJpegBytes = 2 * 1024 * 1024;
constexpr uint32_t kMaxWidth = 1920;
constexpr uint32_t kMaxHeight = 1080;
constexpr int kDefaultJpegQuality = 90;
constexpr uint32_t kNormalizedMax = 65535;
constexpr uint32_t kUnknownRtt = std::numeric_limits<uint32_t>::max();
constexpr uint32_t kRejectedSubnet = 2;
constexpr uint32_t kRejectedVersion = 3;
constexpr uint32_t kFileChunkBytes = 32 * 1024;
constexpr uint32_t kMaxFileBytes = 1024 * 1024 * 1024;
constexpr uint32_t kMaxFileNameBytes = 1024;
constexpr size_t kMaxQueuedFiles = 32;
constexpr auto kFeedbackPeriod = 500ms;

enum class Message : uint32_t {
    hello = 1, welcome, ready, rejected, input, feedback, ping, pong,
    fileBegin, fileChunk, fileEnd, fileResult, mouseResult, systemAction, systemActionResult
};
enum class SystemAction : uint32_t { none, secureAttention, shutdown, restart };
enum class FileResult : uint32_t { ready = 1, saved, refused, failed };
enum class MouseResult : uint32_t { unknown, accepted, blocked, desktopUnavailable, clipped, positionUnconfirmed };
enum class InputKind : uint32_t { move = 1, button, wheel, key, releaseAll };
enum class Button : uint32_t { left = 1, right, middle, x1, x2 };
constexpr uint32_t kDown = 1;
constexpr uint32_t kExtended = 2;
constexpr uint32_t kHorizontal = 4;

struct UdpChunkHeader {
    uint32_t magic;
    uint32_t sessionHigh;
    uint32_t sessionLow;
    uint32_t frameId;
    uint32_t jpegBytes;
    uint32_t chunkIndex;
    uint32_t chunkCount;
    static constexpr size_t wireBytes = 7 * 4;
};

struct InputPacket {
    InputKind kind;
    uint32_t x;       // 0..65535 maps to [0, 1] inclusive
    uint32_t y;
    uint32_t value;   // button ID, signed wheel delta in two's complement, or scan code
    uint32_t flags;
};

struct FeedbackPacket {
    uint32_t ackFrameId;     // latest successfully decoded frame, or 0
    uint32_t decodedFrames;  // successful frames since previous report
    uint32_t droppedFrames;  // missing, superseded, expired or undecodable frames
    uint32_t rttMs;          // TCP ping/pong RTT, or kUnknownRtt
};

inline uint32_t ReadWord(const uint8_t* bytes) {
    return (uint32_t(bytes[0]) << 24) | (uint32_t(bytes[1]) << 16) |
           (uint32_t(bytes[2]) << 8) | uint32_t(bytes[3]);
}

inline void WriteWord(uint8_t* bytes, uint32_t word) {
    bytes[0] = static_cast<uint8_t>(word >> 24);
    bytes[1] = static_cast<uint8_t>(word >> 16);
    bytes[2] = static_cast<uint8_t>(word >> 8);
    bytes[3] = static_cast<uint8_t>(word);
}

template <size_t N>
void WriteWords(uint8_t* bytes, const std::array<uint32_t, N>& words) {
    for (size_t i = 0; i < N; ++i) WriteWord(bytes + i * 4, words[i]);
}

inline UdpChunkHeader ReadChunkHeader(const uint8_t* bytes) {
    return {ReadWord(bytes), ReadWord(bytes + 4), ReadWord(bytes + 8), ReadWord(bytes + 12),
            ReadWord(bytes + 16), ReadWord(bytes + 20), ReadWord(bytes + 24)};
}

inline uint32_t ChunkCount(uint32_t bytes) {
    return (bytes + kChunkBytes - 1) / kChunkBytes;
}

class AdaptiveQuality {
public:
    int Update(const FeedbackPacket& feedback) {
        const bool highRtt = feedback.rttMs != kUnknownRtt && feedback.rttMs > 70;
        badReports_ = feedback.droppedFrames ? badReports_ + 1 : 0;
        if (highRtt || badReports_ >= 2) {
            quality_ = std::max(20, quality_ - 20);
            badReports_ = 0;
            cleanReports_ = 0;
            return quality_;
        }

        const bool healthy = feedback.droppedFrames == 0 && feedback.decodedFrames > 0 &&
                             feedback.rttMs != kUnknownRtt && feedback.rttMs <= 50;
        cleanReports_ = healthy ? cleanReports_ + 1 : 0;
        if (cleanReports_ == 6) {
            quality_ = std::min(kDefaultJpegQuality, quality_ + 5);
            cleanReports_ = 0;
        }
        return quality_;
    }

private:
    int quality_ = kDefaultJpegQuality;
    int badReports_ = 0;
    int cleanReports_ = 0;
};

struct CompletedFrame {
    uint32_t id;
    std::vector<uint8_t> jpeg;
};

class FrameAssembler {
public:
    FrameAssembler(uint32_t high, uint32_t low) : sessionHigh_(high), sessionLow_(low) {}

    std::optional<CompletedFrame> Push(const uint8_t* packet, size_t size, Clock::time_point now) {
        if (size < UdpChunkHeader::wireBytes) return std::nullopt;
        const auto h = ReadChunkHeader(packet);
        if (h.magic != kMagic || h.sessionHigh != sessionHigh_ || h.sessionLow != sessionLow_ ||
            h.frameId <= finalized_ || h.jpegBytes == 0 || h.jpegBytes > kMaxJpegBytes ||
            h.chunkCount != ChunkCount(h.jpegBytes) || h.chunkIndex >= h.chunkCount) {
            return std::nullopt;
        }
        const uint32_t offset = h.chunkIndex * kChunkBytes;
        const uint32_t chunkSize = std::min(kChunkBytes, h.jpegBytes - offset);
        if (size != UdpChunkHeader::wireBytes + chunkSize) return std::nullopt;

        Expire(now);
        if (h.frameId <= finalized_ || h.frameId < active_) return std::nullopt;
        if (h.frameId != active_) {
            DropThrough(h.frameId - 1);
            active_ = h.frameId;
            deadline_ = now + 120ms;
            jpeg_.resize(h.jpegBytes);
            seen_.assign(h.chunkCount, 0);
            received_ = 0;
        }
        if (h.jpegBytes != jpeg_.size() || seen_[h.chunkIndex]) return std::nullopt;
        std::copy_n(packet + UdpChunkHeader::wireBytes, chunkSize, jpeg_.data() + offset);
        seen_[h.chunkIndex] = 1;
        if (++received_ != seen_.size()) return std::nullopt;

        finalized_ = active_;
        active_ = 0;
        return CompletedFrame{finalized_, std::move(jpeg_)};
    }

    void Expire(Clock::time_point now) {
        if (active_ && now >= deadline_) DropThrough(active_);
    }

    // A TCP watermark is applied only after an additional feedback interval.
    void DropThrough(uint32_t frameId) {
        if (frameId <= finalized_) return;
        drops_ += frameId - finalized_;
        finalized_ = frameId;
        if (active_ && active_ <= frameId) {
            active_ = 0;
            jpeg_.clear();
            seen_.clear();
        }
    }

    uint32_t TakeDrops() { return std::exchange(drops_, 0); }

private:
    uint32_t sessionHigh_;
    uint32_t sessionLow_;
    uint32_t finalized_ = 0;
    uint32_t active_ = 0;
    uint32_t drops_ = 0;
    size_t received_ = 0;
    Clock::time_point deadline_{};
    std::vector<uint8_t> jpeg_;
    std::vector<uint8_t> seen_;
};

} // namespace remote
