#pragma once

#include "file_transfer.h"
#include "system_actions.h"

namespace remote {

inline void StreamScreen(SOCKET udp, uint32_t high, uint32_t low, const std::atomic_bool& stop,
                         const std::atomic<int>& quality, std::atomic<uint32_t>& published,
                         std::atomic_bool& failed) {
    ScreenCapture capture;
    if (!capture.Ready()) {
        failed.store(true);
        return;
    }

    std::vector<uint8_t> jpeg;
    std::array<uint8_t, UdpChunkHeader::wireBytes + kChunkBytes> packet{};
    uint32_t frameId = 0;
    unsigned captureFailures = 0;
    while (!stop.load() && frameId < UINT32_MAX) {
        const auto next = Clock::now() + 66ms;
        if (capture.Encode(quality.load(std::memory_order_relaxed), jpeg)) {
            captureFailures = 0;
            ++frameId;
            const auto size = static_cast<uint32_t>(jpeg.size());
            const auto count = ChunkCount(size);
            for (uint32_t index = 0; index < count && !stop.load(); ++index) {
                const auto offset = index * kChunkBytes;
                const auto chunk = std::min(kChunkBytes, size - offset);
                WriteWords(packet.data(), std::array{kMagic, high, low, frameId, size, index, count});
                std::copy_n(jpeg.data() + offset, chunk, packet.data() + UdpChunkHeader::wireBytes);
                const auto length = static_cast<int>(UdpChunkHeader::wireBytes + chunk);
                if (send(udp, reinterpret_cast<const char*>(packet.data()), length, 0) != length) break;
                if ((index + 1) % 64 == 0 && index + 1 < count) std::this_thread::sleep_for(1ms);
            }
            // Even a frame that could not be queued in full is part of the stream's loss count.
            published.store(frameId, std::memory_order_relaxed);
        } else if (++captureFailures == 30) {
            failed.store(true);
            return;
        }
        std::this_thread::sleep_until(next);
    }
}

inline void HostSession(SOCKET tcp, sockaddr_in peer, const std::atomic_bool& stop, Status& status,
                        const std::vector<Ipv4Subnet>& trustedSubnets, RemoteInput& input,
                        bool allowPower) {
    sockaddr_in source{};
    if (!LocalAddress(tcp, source)) return;

    ControlMessage message;
    if (!SetupTcp(tcp) || !ReceiveControl(tcp, message, stop) || message.type != Message::hello ||
        message.payload[0] != kMagic) return;
    const auto version = message.payload[1];
    if (version < kVideoOnlyVersion || version > kVersion) {
        SendControl(tcp, Message::rejected, {kRejectedVersion}, stop);
        return;
    }

    const auto peerAddress = ntohl(peer.sin_addr.s_addr);
    if (!std::any_of(trustedSubnets.begin(), trustedSubnets.end(),
                     [peerAddress](const Ipv4Subnet& subnet) { return subnet.Contains(peerAddress); }) &&
        !IsLocalPeer(source.sin_addr, peer.sin_addr)) {
        SendControl(tcp, Message::rejected, {kRejectedSubnet}, stop);
        return;
    }

    char ip[INET_ADDRSTRLEN]{};
    inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
    const std::string peerName(ip);

    Socket udp(socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
    peer.sin_port = htons(kViewerPort);
    if (!udp || !Nonblocking(udp.Get())) return;
    source.sin_port = 0;
    if (!Bind(udp.Get(), source) || !Connect(udp.Get(), peer) || !LocalAddress(udp.Get(), source)) return;

    std::array<uint32_t, 2> session{};
    if (BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(session.data()), sizeof(session),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) return;
    if (!SendControl(tcp, Message::welcome, {version, session[0], session[1], ntohs(source.sin_port)}, stop) ||
        !ReceiveControl(tcp, message, stop) || message.type != Message::ready ||
        message.payload[0] != version) return;

    std::atomic_bool streamStop{false};
    std::atomic_bool streamFailed{false};
    std::atomic<int> quality{kDefaultJpegQuality};
    std::atomic<uint32_t> published{0};
    AdaptiveQuality adaptive;
    MouseResult lastMove = MouseResult::unknown;
    MouseResult lastClick = MouseResult::unknown;
    IncomingFile incoming;
    status.Set("Sharing with " + peerName + " | JPEG " + std::to_string(kDefaultJpegQuality) + "%");
    std::thread stream([&] {
        StreamScreen(udp.Get(), session[0], session[1], streamStop, quality, published, streamFailed);
    });

    auto idleDeadline = Clock::now() + 3s;
    while (!stop.load() && !streamFailed.load()) {
        input.RetryMouseReleases();
        const int ready = WaitSocket(tcp, false, 40);
        if (ready < 0 || (ready == 0 && Clock::now() >= idleDeadline)) break;
        if (ready == 0) continue;
        if (!ReceiveControl(tcp, message, stop)) break;
        idleDeadline = Clock::now() + 3s;
        const auto& p = message.payload;
        if (message.type == Message::input) {
            const auto kind = static_cast<InputKind>(p[0]);
            const auto result = input.Apply({kind, p[1], p[2], p[3], p[4]});
            if (version >= kMouseResultVersion && result != MouseResult::unknown && kind != InputKind::wheel) {
                auto& previous = kind == InputKind::move ? lastMove : lastClick;
                const auto reported = version == kMouseResultVersion && result > MouseResult::blocked
                    ? MouseResult::blocked : result;
                if (reported != previous) {
                    previous = reported;
                    if (!SendControl(tcp, Message::mouseResult,
                                     {p[0], static_cast<uint32_t>(reported)}, stop)) break;
                }
            }
        } else if (message.type == Message::systemAction && version >= kSystemActionVersion) {
            input.ReleaseAll();
            const auto result = RequestSystemAction(static_cast<SystemAction>(p[0]), allowPower);
            if (!SendControl(tcp, Message::systemActionResult, {p[0], result}, stop)) break;
        } else if (message.type == Message::feedback) {
            const FeedbackPacket feedback{p[0], p[1], p[2], p[3]};
            const int nextQuality = adaptive.Update(feedback);
            quality.store(nextQuality, std::memory_order_relaxed);
            status.Set("Sharing with " + peerName + " | JPEG " + std::to_string(nextQuality) +
                       "% | Ack " + std::to_string(feedback.ackFrameId) +
                       " | Drops/500 ms " + std::to_string(feedback.droppedFrames) +
                       " | RTT " + (feedback.rttMs == kUnknownRtt ? "-" :
                                    std::to_string(feedback.rttMs) + " ms"));
        } else if (message.type == Message::ping) {
            if (!SendControl(tcp, Message::pong, {p[0], published.load(std::memory_order_relaxed)}, stop)) break;
        } else if (version < kFileTransferVersion || !ReceiveFileMessage(tcp, message, incoming, stop)) {
            break;
        }
    }

    input.ReleaseAll();
    shutdown(tcp, SD_SEND);
    streamStop.store(true);
    stream.join();
    if (streamFailed.load()) status.Set("Screen capture failed.");
}

inline void RunHost(const std::atomic_bool& stop, Status& status) {
    const bool allowPower = RemotePowerEnabled();
    const auto trustedSubnets = LoadTrustedSubnets();
    const std::vector<Ipv4Subnet> localOnly;
    RemoteInput input;
    Socket listener(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (!listener || !Nonblocking(listener.Get()) || !Exclusive(listener.Get()) ||
        !Bind(listener.Get(), Address(kControlPort)) || listen(listener.Get(), 1) != 0) {
        const auto error = WSAGetLastError();
        status.Set("Cannot listen on TCP " + std::to_string(kControlPort) +
                   " (Winsock " + std::to_string(error) + ").");
        return;
    }

    const std::string listening = "Listening on TCP " + std::to_string(kControlPort) +
        (trustedSubnets ? ". Local and trusted peers connect automatically." :
                          ". Invalid trusted-subnets.txt; local subnet only.");
    status.Set(listening);
    while (!stop.load()) {
        const bool released = input.ReleaseAll();
        const int ready = WaitSocket(listener.Get(), false, 50);
        if (ready < 0) {
            status.Set("TCP listener failed.");
            return;
        }
        if (ready == 0) continue;
        sockaddr_in peer{};
        int length = sizeof(peer);
        Socket connection(accept(listener.Get(), reinterpret_cast<sockaddr*>(&peer), &length));
        if (connection && released) {
            HostSession(connection.Get(), peer, stop, status,
                        trustedSubnets ? *trustedSubnets : localOnly, input, allowPower);
        }
        if (!stop.load()) {
            status.Set(status.Get() == "Screen capture failed." ?
                       "Screen capture failed. Listening for another peer." : listening);
        }
    }
    for (int attempt = 0; attempt < 8 && !input.ReleaseAll(); ++attempt) {
        std::this_thread::sleep_for(20ms);
    }
    status.Set("Host stopped.");
}

class ViewerSession {
public:
    std::atomic_bool stop{false};
    std::atomic_bool connected{false};
    Status status;
    Status transferStatus;
    Status mouseStatus;
    Status systemActionStatus;

    bool QueueSystemAction(SystemAction action) {
        if (!connected.load() || stop.load()) return false;
        auto expected = SystemAction::none;
        return systemAction_.compare_exchange_strong(expected, action);
    }

    bool QueueFiles(std::vector<std::filesystem::path> files) {
        std::lock_guard<std::mutex> lock(fileMutex_);
        if (!connected.load() || stop.load() || files.empty() ||
            files.size() > kMaxQueuedFiles - fileQueue_.size()) return false;
        for (auto& file : files) fileQueue_.push_back(std::move(file));
        transferStatus.Set(std::to_string(fileQueue_.size()) + " file(s) queued for Host");
        return true;
    }

    bool Enqueue(InputPacket input) {
        if (!connected.load() || stop.load()) return false;
        std::lock_guard<std::mutex> lock(inputMutex_);
        if (input.kind == InputKind::move && !inputs_.empty() && inputs_.back().kind == InputKind::move) {
            inputs_.back() = input;
        } else if (inputs_.size() >= 256) {
            // End the session rather than silently losing a key or button release.
            inputOverflow_.store(true);
            stop.store(true);
            return false;
        } else {
            inputs_.push_back(input);
        }
        return true;
    }

    std::shared_ptr<const Pixels> Latest() {
        std::lock_guard<std::mutex> lock(videoMutex_);
        return Clock::now() - lastDecoded_ <= 1s ? latest_ : nullptr;
    }

    void Run(sockaddr_in target) {
        status.Set("Connecting...");
        Socket udp(socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
        if (!udp || !Nonblocking(udp.Get()) || !Exclusive(udp.Get()) || !Bind(udp.Get(), Address(kViewerPort))) {
            const auto error = WSAGetLastError();
            status.Set("Cannot bind viewer UDP " + std::to_string(kViewerPort) +
                       " (Winsock " + std::to_string(error) + ").");
            return;
        }
        const int receiveBuffer = 2 * 1024 * 1024;
        setsockopt(udp.Get(), SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&receiveBuffer),
                   sizeof(receiveBuffer));

        Socket tcp(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
        if (!tcp || !SetupTcp(tcp.Get()) || !ConnectTcp(tcp.Get(), target, stop) ||
            !SendControl(tcp.Get(), Message::hello, {kMagic, kVersion}, stop)) {
            status.Set("Connection to TCP " + std::to_string(kControlPort) + " failed.");
            return;
        }

        ControlMessage message;
        if (!ReceiveControl(tcp.Get(), message, stop)) {
            status.Set(stop.load() ? "Disconnected." : "Host unavailable or busy. Check the LAN and app version.");
            return;
        }
        if (message.type == Message::rejected) {
            if (message.payload[0] == kRejectedSubnet) {
                status.Set("The host does not allow this source subnet.");
            } else if (message.payload[0] == kRejectedVersion) {
                status.Set("App versions differ. Update both the host and viewer.");
            } else {
                status.Set("The host rejected the connection.");
            }
            return;
        }
        const auto& p = message.payload;
        if (message.type != Message::welcome || p[0] != kVersion || p[3] == 0 || p[3] > 65535) {
            status.Set("Invalid host handshake.");
            return;
        }

        target.sin_port = htons(static_cast<uint16_t>(p[3]));
        if (!Connect(udp.Get(), target) || !SendControl(tcp.Get(), Message::ready, {kVersion}, stop)) {
            status.Set("Could not start UDP video.");
            return;
        }

        connected.store(true);
        status.Set("Connected. Waiting for video...");
        std::thread video([&] { ReceiveVideo(udp.Get(), p[1], p[2]); });
        RunControl(tcp.Get());
        connected.store(false);
        stop.store(true);
        video.join();
        bool transferInterrupted = outgoing_.phase != OutgoingFile::Phase::idle;
        {
            std::lock_guard<std::mutex> lock(fileMutex_);
            transferInterrupted |= !fileQueue_.empty();
            fileQueue_.clear();
        }
        outgoing_.Reset();
        if (inputOverflow_.load()) status.Set("Disconnected because the input queue filled.");
        else if (videoFailed_.load()) status.Set("UDP video socket failed.");
        else if (transferInterrupted) {
            status.Set("Disconnected during file transfer. " + transferStatus.Get());
        }
        else status.Set("Disconnected.");
    }

private:
    bool HandleMouseResult(const ControlMessage& message) {
        const auto kind = static_cast<InputKind>(message.payload[0]);
        const auto result = static_cast<MouseResult>(message.payload[1]);
        if ((kind != InputKind::move && kind != InputKind::button) ||
            result < MouseResult::accepted || result > MouseResult::positionUnconfirmed) return false;
        (kind == InputKind::move ? lastMove_ : lastClick_) = result;
        const auto failure = lastMove_ > MouseResult::accepted ? lastMove_ : lastClick_;
        switch (failure) {
            case MouseResult::desktopUnavailable:
                mouseStatus.Set("Host input desktop unavailable. Unlock the host.");
                break;
            case MouseResult::clipped:
                mouseStatus.Set("Host application restricted cursor movement.");
                break;
            case MouseResult::positionUnconfirmed:
                mouseStatus.Set("Host cursor position could not be confirmed.");
                break;
            case MouseResult::blocked:
                mouseStatus.Set(lastMove_ == MouseResult::blocked ?
                    "Host Windows blocked mouse movement" : "Host Windows blocked click");
                break;
            default:
                mouseStatus.Set(lastClick_ == MouseResult::accepted ?
                    "Host accepted click" : "Host accepted mouse movement");
                break;
        }
        return true;
    }

    void FinishUpload(std::string_view error = {}) {
        outgoing_.Reset();
        if (error.empty()) ++savedFiles_;
        else ++failedFiles_;
        std::string result;
        if (!error.empty()) {
            result = error;
            result += " | ";
        }
        result += std::to_string(savedFiles_) + " saved on host (Downloads / Phahol Remote)";
        if (failedFiles_) result += " | " + std::to_string(failedFiles_) + " failed";
        transferStatus.Set(std::move(result));
    }

    bool PumpUpload(SOCKET tcp) {
        using Phase = OutgoingFile::Phase;
        if (outgoing_.phase == Phase::idle) {
            std::filesystem::path path;
            {
                std::lock_guard<std::mutex> lock(fileMutex_);
                if (fileQueue_.empty()) return true;
                path = std::move(fileQueue_.front());
                fileQueue_.pop_front();
            }
            if (!outgoing_.Begin(path)) {
                FinishUpload("Viewer could not open the file. Select a readable file up to 1 GB.");
                return true;
            }
            transferStatus.Set("Waiting for host to accept file...");
            uploadDeadline_ = Clock::now() + 15s;
            const auto nameSize = static_cast<uint32_t>(outgoing_.nameBytes.size());
            return SendControl(tcp, Message::fileBegin, {outgoing_.total, nameSize}, stop) &&
                   Transfer(tcp, outgoing_.nameBytes.data(), nameSize, true, stop, Clock::now() + 1s);
        }

        if (outgoing_.phase != Phase::sending) return Clock::now() < uploadDeadline_;
        const auto size = std::min(kFileChunkBytes, outgoing_.remaining);
        if (size && outgoing_.Read(uploadBuffer_.data(), size)) {
            const auto progress = static_cast<unsigned>(100 - uint64_t(outgoing_.remaining) * 100 / outgoing_.total);
            if (progress != uploadProgress_) {
                uploadProgress_ = progress;
                transferStatus.Set("Sending file " + std::to_string(progress) + "%");
            }
            return SendControl(tcp, Message::fileChunk, {size}, stop) &&
                   Transfer(tcp, uploadBuffer_.data(), size, true, stop, Clock::now() + 1s);
        }

        outgoing_.phase = Phase::waitingForSave;
        uploadDeadline_ = Clock::now() + 15s;
        uploadReadFailed_ = size != 0;
        transferStatus.Set("Waiting for host to save file...");
        return SendControl(tcp, Message::fileEnd, {size == 0 ? 1u : 0u}, stop);
    }

    bool HandleUploadResult(FileResult result) {
        using Phase = OutgoingFile::Phase;
        if (outgoing_.phase == Phase::waitingForHost && result == FileResult::ready) {
            outgoing_.phase = Phase::sending;
            uploadProgress_ = 0;
            transferStatus.Set("Sending file 0%");
            return true;
        }
        if (outgoing_.phase == Phase::waitingForHost && result == FileResult::refused) {
            FinishUpload("Host refused the file. Check the filename and Host Downloads access.");
            return true;
        }
        if (outgoing_.phase == Phase::waitingForSave && result == FileResult::saved && !uploadReadFailed_) {
            FinishUpload();
            return true;
        }
        if (outgoing_.phase == Phase::waitingForSave && result == FileResult::failed) {
            FinishUpload(uploadReadFailed_ ? "Viewer stopped reading the file." :
                                            "Host could not save the file in Downloads.");
            return true;
        }
        return false;
    }

    bool SendInputs(SOCKET tcp) {
        std::deque<InputPacket> pending;
        {
            std::lock_guard<std::mutex> lock(inputMutex_);
            pending.swap(inputs_);
        }
        for (const auto& input : pending) {
            if (!SendControl(tcp, Message::input, {static_cast<uint32_t>(input.kind), input.x, input.y,
                                                  input.value, input.flags}, stop)) return false;
        }
        return true;
    }

    FeedbackPacket TakeFeedback(uint32_t rtt) {
        std::lock_guard<std::mutex> lock(videoMutex_);
        return {ack_, std::exchange(decoded_, 0), std::exchange(drops_, 0), rtt};
    }

    void RunControl(SOCKET tcp) {
        auto nextReport = Clock::now() + kFeedbackPeriod;
        auto pingTime = Clock::now();
        uint32_t pingId = 0;
        bool pingPending = false;
        uint32_t rtt = kUnknownRtt;
        uint32_t previousWatermark = 0;
        bool actionSent = false;
        auto actionDeadline = Clock::now();

        while (!videoFailed_.load()) {
            if (stop.load()) {
                // This is a message boundary; a failed partial send takes the separate break path.
                const std::atomic_bool cleanupStop{false};
                SendControl(tcp, Message::input, {static_cast<uint32_t>(InputKind::releaseAll)},
                            cleanupStop, 150ms);
                break;
            }
            if (!SendInputs(tcp)) break;
            const auto now = Clock::now();
            const auto action = systemAction_.load();
            if (action != SystemAction::none && !actionSent) {
                systemActionStatus.Set("Waiting for Host system action...");
                if (!SendControl(tcp, Message::systemAction, {static_cast<uint32_t>(action)}, stop)) break;
                actionSent = true;
                actionDeadline = now + 5s;
            }
            if (actionSent && now >= actionDeadline) {
                systemActionStatus.Set("Host did not confirm the system action.");
                break;
            }
            if (pingPending && now - pingTime > 3s) break;
            if (now >= nextReport) {
                const auto f = TakeFeedback(rtt);
                if (!SendControl(tcp, Message::feedback, {f.ackFrameId, f.decodedFrames,
                                                         f.droppedFrames, f.rttMs}, stop)) break;
                status.Set("Ack " + std::to_string(f.ackFrameId) + " | Drops/500 ms " +
                           std::to_string(f.droppedFrames) + " | RTT " +
                           (rtt == kUnknownRtt ? "-" : std::to_string(rtt) + " ms"));
                if (!pingPending) {
                    pingTime = Clock::now();
                    pingPending = true;
                    if (!SendControl(tcp, Message::ping, {++pingId}, stop)) break;
                }
                nextReport = now + kFeedbackPeriod;
            }

            if (!PumpUpload(tcp)) break;
            const int ready = WaitSocket(tcp, false, outgoing_.phase == OutgoingFile::Phase::sending ? 2 : 8);
            if (ready < 0) break;
            if (ready == 0) continue;
            ControlMessage pong;
            if (!ReceiveControl(tcp, pong, stop, 1s)) break;
            if (pong.type == Message::fileResult) {
                if (!HandleUploadResult(static_cast<FileResult>(pong.payload[0]))) break;
                continue;
            }
            if (pong.type == Message::mouseResult) {
                if (!HandleMouseResult(pong)) break;
                continue;
            }
            if (pong.type == Message::systemActionResult) {
                if (!actionSent || pong.payload[0] != static_cast<uint32_t>(action)) break;
                const auto error = pong.payload[1];
                if (error == ERROR_SUCCESS) {
                    systemActionStatus.Set(action == SystemAction::secureAttention ? "Ctrl+Alt+Del requested." :
                        action == SystemAction::restart ? "Host restart scheduled." : "Host shutdown scheduled.");
                } else if (action == SystemAction::secureAttention &&
                           (error == ERROR_NOT_SUPPORTED || error == ERROR_ACCESS_DISABLED_BY_POLICY)) {
                    systemActionStatus.Set("Ctrl+Alt+Del requires a signed UIAccess Host and Windows SAS policy.");
                } else if (error == ERROR_ACCESS_DISABLED_BY_POLICY) {
                    systemActionStatus.Set("Remote shutdown and restart are disabled in Host setup.");
                } else {
                    systemActionStatus.Set("Host system action failed (Windows error " + std::to_string(error) + ").");
                }
                actionSent = false;
                systemAction_.store(SystemAction::none);
                continue;
            }
            if (pong.type != Message::pong ||
                !pingPending || pong.payload[0] != pingId) break;
            rtt = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                            Clock::now() - pingTime).count());
            pingPending = false;
            safeWatermark_.store(previousWatermark, std::memory_order_relaxed);
            previousWatermark = pong.payload[1];
        }
    }

    void ReceiveVideo(SOCKET udp, uint32_t high, uint32_t low) {
        FrameAssembler assembler(high, low);
        std::array<uint8_t, UdpChunkHeader::wireBytes + kChunkBytes> bytes{};
        while (!stop.load()) {
            const int ready = WaitSocket(udp, false, 15);
            if (ready < 0) break;
            const auto now = Clock::now();
            assembler.Expire(now);
            assembler.DropThrough(safeWatermark_.load(std::memory_order_relaxed));
            std::optional<CompletedFrame> completed;
            if (ready) {
                const int length = recv(udp, reinterpret_cast<char*>(bytes.data()),
                                        static_cast<int>(bytes.size()), 0);
                if (length > 0) {
                    completed = assembler.Push(bytes.data(), static_cast<size_t>(length), now);
                } else if (length == SOCKET_ERROR) {
                    const int error = WSAGetLastError();
                    if (error != WSAEWOULDBLOCK && error != WSAEMSGSIZE) break;
                }
            }

            auto drops = assembler.TakeDrops();
            std::shared_ptr<Pixels> pixels;
            if (completed) {
                pixels = Decode(completed->jpeg);
                if (!pixels) ++drops;
            }
            if (drops || pixels) {
                std::lock_guard<std::mutex> lock(videoMutex_);
                drops_ += drops;
                if (pixels) {
                    ack_ = completed->id;
                    ++decoded_;
                    latest_ = std::move(pixels);
                    lastDecoded_ = Clock::now();
                }
            }
        }
        if (!stop.load()) videoFailed_.store(true);
    }

    std::mutex inputMutex_;
    std::deque<InputPacket> inputs_;
    std::atomic_bool inputOverflow_{false};
    std::atomic_bool videoFailed_{false};
    std::atomic<SystemAction> systemAction_{SystemAction::none};
    MouseResult lastMove_ = MouseResult::unknown;
    MouseResult lastClick_ = MouseResult::unknown;
    std::mutex fileMutex_;
    std::deque<std::filesystem::path> fileQueue_;
    OutgoingFile outgoing_;
    std::array<uint8_t, kFileChunkBytes> uploadBuffer_{};
    Clock::time_point uploadDeadline_{};
    unsigned uploadProgress_ = 0;
    bool uploadReadFailed_ = false;
    size_t savedFiles_ = 0;
    size_t failedFiles_ = 0;
    std::atomic<uint32_t> safeWatermark_{0};
    std::mutex videoMutex_;
    std::shared_ptr<const Pixels> latest_;
    Clock::time_point lastDecoded_{};
    uint32_t ack_ = 0;
    uint32_t decoded_ = 0;
    uint32_t drops_ = 0;
};

} // namespace remote
