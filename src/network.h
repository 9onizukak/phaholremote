#pragma once

#include <winsock2.h>
#include <ws2tcpip.h>
#include <bcrypt.h>
#include <iphlpapi.h>

#include "video.h"
#include "subnets.h"

#include <atomic>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>

namespace remote {

class Socket {
public:
    Socket() = default;
    explicit Socket(SOCKET socket) : socket_(socket) {}
    ~Socket() { if (socket_ != INVALID_SOCKET) closesocket(socket_); }

    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    SOCKET Get() const { return socket_; }
    explicit operator bool() const { return socket_ != INVALID_SOCKET; }

private:
    SOCKET socket_ = INVALID_SOCKET;
};

inline bool Nonblocking(SOCKET socket) {
    u_long enable = 1;
    return ioctlsocket(socket, FIONBIO, &enable) == 0;
}

inline bool Exclusive(SOCKET socket) {
    BOOL enable = TRUE;
    return setsockopt(socket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                      reinterpret_cast<const char*>(&enable), sizeof(enable)) == 0;
}

inline bool SetupTcp(SOCKET socket) {
    BOOL enable = TRUE;
    return Nonblocking(socket) && setsockopt(socket, IPPROTO_TCP, TCP_NODELAY,
        reinterpret_cast<const char*>(&enable), sizeof(enable)) == 0;
}

inline sockaddr_in Address(uint16_t port) {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    return address;
}

inline bool Bind(SOCKET socket, const sockaddr_in& address) {
    return bind(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0;
}

inline bool Connect(SOCKET socket, const sockaddr_in& address) {
    return connect(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0;
}

inline bool LocalAddress(SOCKET socket, sockaddr_in& address) {
    int length = sizeof(address);
    return getsockname(socket, reinterpret_cast<sockaddr*>(&address), &length) == 0;
}

inline bool IsLocalPeer(in_addr local, in_addr peer) {
    if (local.s_addr == peer.s_addr) return true;

    ULONG size = 0;
    if (GetIpAddrTable(nullptr, &size, FALSE) != ERROR_INSUFFICIENT_BUFFER) return false;
    auto storage = std::make_unique<uint8_t[]>(size);
    const auto table = reinterpret_cast<MIB_IPADDRTABLE*>(storage.get());
    if (GetIpAddrTable(table, &size, FALSE) != NO_ERROR) return false;

    for (DWORD i = 0; i < table->dwNumEntries; ++i) {
        const auto& row = table->table[i];
        if (row.dwAddr == local.s_addr && row.dwMask != 0 &&
            (row.dwAddr & row.dwMask) == (peer.s_addr & row.dwMask)) return true;
    }
    return false;
}

inline std::optional<std::vector<Ipv4Subnet>> LoadTrustedSubnets() {
    std::array<wchar_t, 32768> module{};
    const auto length = GetModuleFileNameW(nullptr, module.data(), static_cast<DWORD>(module.size()));
    if (!length || length >= module.size()) return std::nullopt;

    const auto path = std::filesystem::path(module.data()).parent_path() / "trusted-subnets.txt";
    std::error_code error;
    const bool exists = std::filesystem::exists(path, error);
    if (error) return std::nullopt;
    if (!exists) return std::vector<Ipv4Subnet>{};
    const auto size = std::filesystem::file_size(path, error);
    if (error || size > 4096) return std::nullopt;
    std::ifstream file(path, std::ios::binary);
    if (!file) return std::nullopt;
    std::string text(static_cast<size_t>(size), '\0');
    if (!file.read(text.data(), static_cast<std::streamsize>(text.size()))) return std::nullopt;
    return ParseSubnets(text);
}

// -1 is a socket error, 0 is a timeout, 1 is ready.
inline int WaitSocket(SOCKET socket, bool write, long waitMs) {
    fd_set ready, errors;
    FD_ZERO(&ready);
    FD_SET(socket, &ready);
    FD_ZERO(&errors);
    FD_SET(socket, &errors);
    timeval timeout{waitMs / 1000, (waitMs % 1000) * 1000};
    const int result = select(0, write ? nullptr : &ready, write ? &ready : nullptr,
                              &errors, &timeout);
    return result > 0 && FD_ISSET(socket, &errors) ? -1 : result;
}

inline bool ConnectTcp(SOCKET socket, const sockaddr_in& address, const std::atomic_bool& stop) {
    if (Connect(socket, address)) return true;
    if (WSAGetLastError() != WSAEWOULDBLOCK) return false;
    const auto deadline = Clock::now() + 3s;
    while (!stop.load() && Clock::now() < deadline) {
        const int ready = WaitSocket(socket, true, 50);
        if (ready < 0) return false;
        if (ready == 0) continue;
        int error = 0;
        int size = sizeof(error);
        return getsockopt(socket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &size) == 0 &&
               error == 0;
    }
    return false;
}

inline bool Transfer(SOCKET socket, uint8_t* data, size_t size, bool write,
                     const std::atomic_bool& stop, Clock::time_point deadline) {
    while (size && !stop.load() && Clock::now() < deadline) {
        auto* buffer = reinterpret_cast<char*>(data);
        const auto length = static_cast<int>(size);
        const int result = write ? send(socket, buffer, length, 0) : recv(socket, buffer, length, 0);
        if (result > 0) {
            data += result;
            size -= result;
            continue;
        }
        if (result == 0 || WSAGetLastError() != WSAEWOULDBLOCK ||
            WaitSocket(socket, write, 40) < 0) return false;
    }
    return size == 0;
}

struct ControlMessage {
    Message type{};
    std::array<uint32_t, 5> payload{};
};

inline size_t PayloadWords(Message type) {
    switch (type) {
        case Message::hello:
        case Message::fileBegin: return 2;
        case Message::welcome: return 4;
        case Message::ready:
        case Message::rejected:
        case Message::ping:
        case Message::fileChunk:
        case Message::fileEnd:
        case Message::systemAction:
        case Message::fileResult: return 1;
        case Message::input: return 5;
        case Message::feedback: return 4;
        case Message::pong:
        case Message::systemActionResult:
        case Message::mouseResult: return 2;
    }
    return 0;
}

inline bool SendControl(SOCKET socket, Message type, std::array<uint32_t, 5> payload,
                        const std::atomic_bool& stop, Clock::duration timeout = 1s) {
    std::array<uint8_t, 24> bytes{};
    WriteWord(bytes.data(), static_cast<uint32_t>(type));
    WriteWords(bytes.data() + 4, payload);
    return Transfer(socket, bytes.data(), 4 + PayloadWords(type) * 4, true, stop, Clock::now() + timeout);
}

inline bool ReceiveControl(SOCKET socket, ControlMessage& message, const std::atomic_bool& stop,
                           Clock::duration timeout = 3s) {
    std::array<uint8_t, 20> bytes{};
    const auto deadline = Clock::now() + timeout;
    if (!Transfer(socket, bytes.data(), 4, false, stop, deadline)) return false;
    message.type = static_cast<Message>(ReadWord(bytes.data()));
    const auto count = PayloadWords(message.type);
    if (!count || !Transfer(socket, bytes.data(), count * 4, false, stop, deadline)) return false;
    for (size_t i = 0; i < count; ++i) message.payload[i] = ReadWord(bytes.data() + i * 4);
    return true;
}

struct Status {
    void Set(std::string text) {
        std::lock_guard<std::mutex> lock(mutex);
        value = std::move(text);
    }

    std::string Get() {
        std::lock_guard<std::mutex> lock(mutex);
        return value;
    }

    std::mutex mutex;
    std::string value;
};

class RemoteInput {
public:
    ~RemoteInput() { ReleaseAll(); }

    MouseResult Apply(const InputPacket& packet) {
        switch (packet.kind) {
            case InputKind::move:
                return MoveCursor(packet);
            case InputKind::button:
                if (packet.value >= 1 && packet.value <= buttons_.size()) {
                    const auto index = packet.value - 1;
                    const bool down = (packet.flags & kDown) != 0;
                    if (!down) {
                        if (!buttons_[index]) return MouseResult::unknown;
                        return ReleaseButton(index) ? MouseResult::accepted : MouseResult::blocked;
                    }
                    if (HasPendingMouseRelease()) return MouseResult::blocked;

                    const auto action = ButtonEvent(index, true);
                    auto result = MoveCursor(packet);
                    if (result == MouseResult::accepted) result = SendMouseAt(packet, &action);
                    if (result == MouseResult::accepted) buttons_[index] = true;
                    return result;
                }
                break;
            case InputKind::wheel: {
                const auto action = MouseEvent(packet.flags & kHorizontal ? MOUSEEVENTF_HWHEEL :
                                                                          MOUSEEVENTF_WHEEL, packet.value);
                return SendMouseAt(packet, &action);
            }
            case InputKind::key:
                if (packet.value > 0 && packet.value < 256) {
                    const bool extended = (packet.flags & kExtended) != 0;
                    const bool down = (packet.flags & kDown) != 0;
                    auto& held = keys_[packet.value + (extended ? 256 : 0)];
                    if (InjectKey(packet.value, extended, down)) held = down;
                }
                break;
            case InputKind::releaseAll:
                ReleaseAll();
                break;
        }
        return MouseResult::unknown;
    }

    bool HasPendingMouseRelease() const {
        return std::any_of(pendingMouseRelease_.begin(), pendingMouseRelease_.end(), [](bool pending) {
            return pending;
        });
    }

    void RetryMouseReleases() {
        for (size_t i = 0; i < pendingMouseRelease_.size(); ++i) {
            if (pendingMouseRelease_[i]) ReleaseButton(i);
        }
    }

    bool ReleaseAll() {
        bool released = true;
        for (size_t i = 0; i < buttons_.size(); ++i) {
            if (!buttons_[i]) continue;
            released &= ReleaseButton(i);
        }
        for (uint32_t i = 0; i < keys_.size(); ++i) {
            if (!keys_[i]) continue;
            keys_[i] = !InjectKey(i % 256, i >= 256, false);
            released &= !keys_[i];
        }
        return released;
    }

private:
    bool ReleaseButton(size_t index) {
        // Release capture without repositioning the cursor across a title-bar or focus change.
        auto release = ButtonEvent(index, false);
        const bool released = SendInput(1, &release, sizeof(release)) == 1;
        buttons_[index] = !released;
        pendingMouseRelease_[index] = !released;
        return released;
    }

    static bool IsInputDesktopCurrent() {
        const HDESK input = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
        if (!input) return false;
        wchar_t inputName[256]{};
        wchar_t threadName[256]{};
        DWORD needed = 0;
        const bool same =
            GetUserObjectInformationW(input, UOI_NAME, inputName, sizeof(inputName), &needed) &&
            GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()), UOI_NAME,
                                      threadName, sizeof(threadName), &needed) &&
            std::wstring_view(inputName) == threadName;
        CloseDesktop(input);
        return same;
    }

    static MouseResult MoveCursor(const InputPacket& packet) {
        const auto sent = SendMouseAt(packet);
        if (sent == MouseResult::unknown) return sent;

        const auto width = GetSystemMetrics(SM_CXSCREEN);
        const auto height = GetSystemMetrics(SM_CYSCREEN);
        if (width <= 0 || height <= 0) return MouseResult::desktopUnavailable;
        const POINT target{MulDiv(static_cast<int>(packet.x), width - 1, kNormalizedMax),
                           MulDiv(static_cast<int>(packet.y), height - 1, kNormalizedMax)};
        POINT current{};
        if (sent == MouseResult::accepted && GetCursorPos(&current) &&
            std::abs(current.x - target.x) <= 1 && std::abs(current.y - target.y) <= 1) {
            return MouseResult::accepted;
        }

        // Change only the pointer position on the active desktop. Buttons still use SendInput.
        if (!IsInputDesktopCurrent()) return MouseResult::desktopUnavailable;
        RECT clip{};
        if (GetClipCursor(&clip) && !PtInRect(&clip, target)) return MouseResult::clipped;
        if (SetCursorPos(target.x, target.y) && GetCursorPos(&current) &&
            current.x == target.x && current.y == target.y) return MouseResult::accepted;
        return sent == MouseResult::blocked ? MouseResult::blocked : MouseResult::positionUnconfirmed;
    }

    static INPUT MouseEvent(DWORD flags, DWORD data = 0, LONG x = 0, LONG y = 0) {
        INPUT input{};
        input.type = INPUT_MOUSE;
        input.mi.dx = x;
        input.mi.dy = y;
        input.mi.mouseData = data;
        input.mi.dwFlags = flags;
        return input;
    }

    static MouseResult SendMouseAt(const InputPacket& packet, const INPUT* action = nullptr) {
        if (packet.x > kNormalizedMax || packet.y > kNormalizedMax) return MouseResult::unknown;
        INPUT events[2]{MouseEvent(MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE, 0,
                                   static_cast<LONG>(packet.x), static_cast<LONG>(packet.y))};
        if (action) events[1] = *action;
        const UINT count = action ? 2 : 1;
        const UINT sent = SendInput(count, events, sizeof(INPUT));
        if (sent == count) return MouseResult::accepted;
        // Retry the pair once if Windows queued the move but not its accompanying action.
        if (action && sent == 1 && SendInput(count, events, sizeof(INPUT)) == count) {
            return MouseResult::accepted;
        }
        return MouseResult::blocked;
    }

    static INPUT ButtonEvent(size_t index, bool down) {
        constexpr DWORD downs[] = {MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_RIGHTDOWN, MOUSEEVENTF_MIDDLEDOWN,
                                   MOUSEEVENTF_XDOWN, MOUSEEVENTF_XDOWN};
        constexpr DWORD ups[] = {MOUSEEVENTF_LEFTUP, MOUSEEVENTF_RIGHTUP, MOUSEEVENTF_MIDDLEUP,
                                 MOUSEEVENTF_XUP, MOUSEEVENTF_XUP};
        constexpr DWORD data[] = {0, 0, 0, XBUTTON1, XBUTTON2};
        return MouseEvent(down ? downs[index] : ups[index], data[index]);
    }

    static bool InjectKey(uint32_t scan, bool extended, bool down) {
        INPUT input{};
        input.type = INPUT_KEYBOARD;
        input.ki.wScan = static_cast<WORD>(scan);
        input.ki.dwFlags = KEYEVENTF_SCANCODE;
        if (extended) input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
        if (!down) input.ki.dwFlags |= KEYEVENTF_KEYUP;
        return SendInput(1, &input, sizeof(input)) == 1;
    }

    std::array<bool, 5> buttons_{};
    std::array<bool, 5> pendingMouseRelease_{};
    std::array<bool, 512> keys_{};
};

} // namespace remote
