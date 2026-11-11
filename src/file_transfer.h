#pragma once

#include "network.h"

#include <shlobj.h>

namespace remote {

inline std::wstring ExtendedPath(const std::filesystem::path& path) {
    const auto& value = path.native();
    if (value.rfind(L"\\\\?\\", 0) == 0) return value;
    if (value.rfind(L"\\\\", 0) == 0) return L"\\\\?\\UNC\\" + value.substr(2);
    return L"\\\\?\\" + value;
}

inline std::string ToUtf8(const std::wstring& name) {
    const auto length = static_cast<int>(name.size());
    const int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, name.data(), length,
                                          nullptr, 0, nullptr, nullptr);
    if (bytes <= 0 || bytes > static_cast<int>(kMaxFileNameBytes)) return {};
    std::string result(bytes, '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, name.data(), length, result.data(), bytes,
                            nullptr, nullptr) != bytes) return {};
    return result;
}

inline std::wstring SafeFileName(const uint8_t* bytes, uint32_t size) {
    const auto* text = reinterpret_cast<const char*>(bytes);
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, static_cast<int>(size), nullptr, 0);
    if (length <= 0 || length > 255) return {};
    std::wstring name(length, L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, static_cast<int>(size), name.data(), length) ||
        name.back() == L'.' || name.back() == L' ' || name.find_first_of(L"<>:\"/\\|?*") != name.npos ||
        std::any_of(name.begin(), name.end(), [](wchar_t c) { return c < 32; })) return {};

    auto base = name.substr(0, name.find(L'.'));
    while (!base.empty() && base.back() == L' ') base.pop_back();
    for (auto& c : base) {
        if (c >= L'a' && c <= L'z') c -= L'a' - L'A';
    }
    if (base == L"CON" || base == L"PRN" || base == L"AUX" || base == L"NUL" ||
        base == L"CONIN$" || base == L"CONOUT$" ||
        (base.size() == 4 && (base.compare(0, 3, L"COM") == 0 || base.compare(0, 3, L"LPT") == 0) &&
         std::wstring_view(L"123456789\u00b9\u00b2\u00b3").find(base[3]) != std::wstring_view::npos)) return {};
    return name;
}

inline std::filesystem::path DownloadDirectory() {
    PWSTR path = nullptr;
    const auto result = SHGetKnownFolderPath(FOLDERID_Downloads, KF_FLAG_CREATE, nullptr, &path);
    const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> owned(path, &CoTaskMemFree);
    if (FAILED(result) || !owned) return {};
    return std::filesystem::path(owned.get()) / L"Phahol Remote";
}

class FileHandle {
public:
    ~FileHandle() { Reset(); }
    FileHandle() = default;
    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;

    void Reset(HANDLE handle = INVALID_HANDLE_VALUE) {
        if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
        handle_ = handle;
    }
    explicit operator bool() const { return handle_ != INVALID_HANDLE_VALUE; }
    HANDLE Get() const { return handle_; }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};

class IncomingFile {
public:
    ~IncomingFile() { Reset(); }

    bool Active() const { return !temporary_.empty(); }

    bool Begin(std::wstring name, uint32_t size) {
        if (name.empty() || size > kMaxFileBytes) return false;
        folder_ = DownloadDirectory();
        if (folder_.empty()) return false;

        const auto folderPath = ExtendedPath(folder_);
        if (!CreateDirectoryW(folderPath.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return false;
        const auto attributes = GetFileAttributesW(folderPath.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;

        GUID guid{};
        wchar_t randomName[40]{};
        if (FAILED(CoCreateGuid(&guid)) || !StringFromGUID2(guid, randomName, 40)) return false;
        const auto temporary = ExtendedPath(folder_ / (std::wstring(L".phahol-") + randomName + L".part"));
        file_.Reset(CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
        if (!file_) return false;
        temporary_ = temporary;
        name_ = std::move(name);
        remaining_ = size;
        return true;
    }

    bool Write(const uint8_t* data, uint32_t size) {
        if (!Active() || size > remaining_) return false;
        remaining_ -= size;
        DWORD written = 0;
        if (!failed_ && (!WriteFile(file_.Get(), data, size, &written, nullptr) || written != size)) {
            failed_ = true;
        }
        return true;
    }

    bool Finish(bool commit) {
        bool saved = commit && !remaining_ && !failed_ && FlushFileBuffers(file_.Get());
        file_.Reset();
        if (saved) saved = Publish();
        Reset();
        return saved;
    }

private:
    bool Publish() {
        const std::filesystem::path original(name_);
        for (int index = 0; index < 1000; ++index) {
            auto candidate = original;
            if (index) {
                const auto suffix = L" (" + std::to_wstring(index) + L")";
                const auto extension = original.extension().native();
                if (extension.size() + suffix.size() >= 255) return false;
                auto stem = original.stem().native().substr(0, 255 - extension.size() - suffix.size());
                if (!stem.empty() && stem.back() >= 0xd800 && stem.back() <= 0xdbff) stem.pop_back();
                candidate = stem + suffix + extension;
            }
            const auto final = ExtendedPath(folder_ / candidate);
            if (MoveFileExW(temporary_.c_str(), final.c_str(), 0)) {
                temporary_.clear();
                return true;
            }
            const auto error = GetLastError();
            if (error != ERROR_ALREADY_EXISTS && error != ERROR_FILE_EXISTS) return false;
        }
        return false;
    }

    void Reset() {
        file_.Reset();
        if (Active()) DeleteFileW(temporary_.c_str());
        temporary_.clear();
        remaining_ = 0;
        failed_ = false;
    }

    FileHandle file_;
    std::filesystem::path folder_;
    std::wstring temporary_;
    std::wstring name_;
    uint32_t remaining_ = 0;
    bool failed_ = false;
};

class OutgoingFile {
public:
    enum class Phase { idle, waitingForHost, sending, waitingForSave };

    bool Begin(const std::filesystem::path& path) {
        const auto name = ToUtf8(path.filename().native());
        if (name.empty()) return false;
        const auto full = ExtendedPath(path);
        file_.Reset(CreateFileW(full.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
        LARGE_INTEGER size{};
        if (!file_ || GetFileType(file_.Get()) != FILE_TYPE_DISK || !GetFileSizeEx(file_.Get(), &size) ||
            size.QuadPart < 0 || size.QuadPart > kMaxFileBytes) {
            file_.Reset();
            return false;
        }
        total = static_cast<uint32_t>(size.QuadPart);
        remaining = total;
        nameBytes.assign(name.begin(), name.end());
        phase = Phase::waitingForHost;
        return true;
    }

    bool Read(uint8_t* buffer, uint32_t size) {
        DWORD read = 0;
        if (!ReadFile(file_.Get(), buffer, size, &read, nullptr) || read != size) return false;
        remaining -= size;
        return true;
    }

    void Reset() {
        file_.Reset();
        phase = Phase::idle;
    }

    Phase phase = Phase::idle;
    std::vector<uint8_t> nameBytes;
    uint32_t total = 0;
    uint32_t remaining = 0;

private:
    FileHandle file_;
};

inline bool ReceiveFileMessage(SOCKET tcp, const ControlMessage& message, IncomingFile& file,
                               const std::atomic_bool& stop) {
    const auto& p = message.payload;
    const auto deadline = Clock::now() + 3s;
    FileResult result;
    switch (message.type) {
        case Message::fileBegin: {
            std::array<uint8_t, kMaxFileNameBytes> name{};
            if (file.Active() || !p[1] || p[1] > name.size() ||
                !Transfer(tcp, name.data(), p[1], false, stop, deadline)) return false;
            result = file.Begin(SafeFileName(name.data(), p[1]), p[0]) ? FileResult::ready : FileResult::refused;
            break;
        }
        case Message::fileChunk: {
            std::array<uint8_t, kFileChunkBytes> data;
            return file.Active() && p[0] && p[0] <= data.size() &&
                   Transfer(tcp, data.data(), p[0], false, stop, deadline) && file.Write(data.data(), p[0]);
        }
        case Message::fileEnd:
            if (!file.Active() || p[0] > 1) return false;
            result = file.Finish(p[0] == 1) ? FileResult::saved : FileResult::failed;
            break;
        default:
            return false;
    }
    return SendControl(tcp, Message::fileResult, {static_cast<uint32_t>(result)}, stop);
}

} // namespace remote
