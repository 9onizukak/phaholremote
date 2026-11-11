#pragma once

#include "file_transfer.h"
#include "version.h"

#include <winhttp.h>
#include <wincred.h>
#include <roapi.h>
#include <winstring.h>
#include <windows.data.json.h>
#include <shellapi.h>
#include <charconv>
#include <future>

namespace remote {

inline constexpr wchar_t kReleasePath[] = L"/9onizukak/phaholremote/releases/";
inline constexpr wchar_t kReleaseApiPath[] = L"/repos/9onizukak/phaholremote/releases/";
inline constexpr wchar_t kUpdateCredential[] = L"PhaholRemote/GitHub/9onizukak/phaholremote";
inline constexpr wchar_t kSetupAsset[] = L"PhaholRemote-Setup.exe";
inline constexpr wchar_t kApplyUpdateMessage[] = L"PhaholRemote.ApplyVerifiedUpdate.v1";
inline constexpr wchar_t kUninstallKey[] =
    L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\{46B59A5B-4975-459C-852F-E142DD2D5A25}_is1";
inline constexpr size_t kMaxSetupBytes = 32 * 1024 * 1024;

using ReleaseVersion = std::array<unsigned, 3>;

inline std::optional<ReleaseVersion> ParseReleaseVersion(std::string_view text) {
    ReleaseVersion version{};
    for (size_t index = 0; index < version.size(); ++index) {
        const auto separator = text.find('.');
        if ((separator == text.npos) != (index == version.size() - 1)) return {};
        const auto part = text.substr(0, separator);
        if (part.empty() || part.size() > 5 || (part.size() > 1 && part.front() == '0')) return {};
        const auto end = part.data() + part.size();
        const auto parsed = std::from_chars(part.data(), end, version[index]);
        if (parsed.ec != std::errc{} || parsed.ptr != end) return {};
        if (separator != text.npos) text.remove_prefix(separator + 1);
    }
    return version;
}

inline std::string SetupDigest(std::vector<uint8_t>& bytes) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return {};
    std::array<uint8_t, 32> digest{};
    const auto status = BCryptHash(algorithm, nullptr, 0, bytes.data(), static_cast<ULONG>(bytes.size()),
                                   digest.data(), static_cast<ULONG>(digest.size()));
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (status < 0) return {};
    std::string hex;
    constexpr char digits[] = "0123456789abcdef";
    for (const auto byte : digest) {
        hex += digits[byte >> 4];
        hex += digits[byte & 0xf];
    }
    return hex;
}

struct HttpAsset {
    DWORD status = 0;
    std::vector<uint8_t> bytes;
};

inline HttpAsset ReadReleaseAsset(std::wstring url, size_t limit, const std::atomic_bool& stop,
                                  std::wstring_view token = {}, bool binary = false) {
    using HttpHandle = std::unique_ptr<void, decltype(&WinHttpCloseHandle)>;
    HttpHandle session(WinHttpOpen(L"Phahol Remote Updater/1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                   WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0), WinHttpCloseHandle);
    DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    if (!session || !WinHttpSetTimeouts(session.get(), 4000, 4000, 4000, 4000) ||
        !WinHttpSetOption(session.get(), WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy,
                          sizeof(redirectPolicy))) return {};

    const auto deadline = Clock::now() + 60s;
    for (int redirect = 0; redirect < 5 && !stop.load() && Clock::now() < deadline; ++redirect) {
        URL_COMPONENTS parts{};
        parts.dwStructSize = sizeof(parts);
        parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength =
            parts.dwUserNameLength = parts.dwPasswordLength = DWORD(-1);
        if (!WinHttpCrackUrl(url.c_str(), 0, ICU_REJECT_USERPWD, &parts) ||
            parts.nScheme != INTERNET_SCHEME_HTTPS || parts.nPort != INTERNET_DEFAULT_HTTPS_PORT ||
            !parts.dwHostNameLength || !parts.dwUrlPathLength || parts.dwUserNameLength ||
            parts.dwPasswordLength) return {};
        const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
        std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
        if (parts.dwExtraInfoLength) path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
        if (host == L"github.com") {
            if (path.compare(0, std::size(kReleasePath) - 1, kReleasePath) != 0) return {};
        } else if (host == L"api.github.com") {
            if (path.compare(0, std::size(kReleaseApiPath) - 1, kReleaseApiPath) != 0) return {};
        } else if (host != L"release-assets.githubusercontent.com" &&
                   host != L"objects.githubusercontent.com") {
            return {};
        }

        HttpHandle server(WinHttpConnect(session.get(), host.c_str(), parts.nPort, 0), WinHttpCloseHandle);
        if (!server) return {};
        HttpHandle request(WinHttpOpenRequest(server.get(), L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                              WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE),
                           WinHttpCloseHandle);
        DWORD disabled = WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_AUTHENTICATION;
        if (!request || !WinHttpSetOption(request.get(), WINHTTP_OPTION_DISABLE_FEATURE, &disabled,
                                          sizeof(disabled))) return {};
        if (host == L"api.github.com") {
            const wchar_t* accept = binary ? L"Accept: application/octet-stream" : L"Accept: application/vnd.github+json";
            if (!WinHttpAddRequestHeaders(request.get(), accept, DWORD(-1), WINHTTP_ADDREQ_FLAG_ADD)) return {};
            if (!token.empty()) {
                std::wstring header = L"Authorization: Bearer ";
                header.append(token);
                const bool added = WinHttpAddRequestHeaders(request.get(), header.c_str(),
                                                            DWORD(-1), WINHTTP_ADDREQ_FLAG_ADD);
                SecureZeroMemory(header.data(), header.size() * sizeof(wchar_t));
                if (!added) return {};
            }
        }
        if (!WinHttpSendRequest(request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA,
                                0, 0, 0) ||
            !WinHttpReceiveResponse(request.get(), nullptr)) return {};
        DWORD status = 0;
        DWORD length = sizeof(status);
        if (!WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                 WINHTTP_HEADER_NAME_BY_INDEX, &status, &length, WINHTTP_NO_HEADER_INDEX)) return {};
        if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
            std::array<wchar_t, 8192> location{};
            length = static_cast<DWORD>(sizeof(location));
            if (!WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX,
                                     location.data(), &length, WINHTTP_NO_HEADER_INDEX)) return {};
            url.assign(location.data());
            if (!url.empty() && url.front() == L'/' && host == L"github.com") url.insert(0, L"https://github.com");
            continue;
        }
        if (status != 200) return {status, {}};

        HttpAsset asset{status, {}};
        std::array<uint8_t, 16384> buffer{};
        while (!stop.load() && Clock::now() < deadline) {
            DWORD read = 0;
            if (!WinHttpReadData(request.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &read) ||
                read > limit - asset.bytes.size()) return {};
            if (!read) return asset;
            asset.bytes.insert(asset.bytes.end(), buffer.begin(), buffer.begin() + read);
        }
        return {};
    }
    return {};
}

struct UpdateResult {
    std::string message;
    std::string version;
    std::vector<uint8_t> installer;
};

class UpdateToken {
public:
    UpdateToken() {
        PCREDENTIALW credential = nullptr;
        if (!CredReadW(kUpdateCredential, CRED_TYPE_GENERIC, 0, &credential)) return;
        const auto size = credential->CredentialBlobSize;
        if (size && size % sizeof(wchar_t) == 0) {
            value_.resize(size / sizeof(wchar_t));
            std::memcpy(value_.data(), credential->CredentialBlob, size);
            if (!value_.empty() && value_.back() == L'\0') value_.pop_back();
            if (value_.find_first_not_of(L"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") !=
                std::wstring::npos) Clear();
        }
        if (size) SecureZeroMemory(credential->CredentialBlob, size);
        CredFree(credential);
    }
    ~UpdateToken() { Clear(); }
    UpdateToken(const UpdateToken&) = delete;
    UpdateToken& operator=(const UpdateToken&) = delete;
    std::wstring_view Value() const { return value_; }
private:
    void Clear() {
        SecureZeroMemory(value_.data(), value_.size() * sizeof(wchar_t));
        value_.clear();
    }
    std::wstring value_;
};

struct GitHubRelease {
    std::string tag;
    std::string versionText;
    ReleaseVersion version;
    uint64_t assetId = 0;
    std::string sha256;
};

inline std::optional<GitHubRelease> ParseGitHubRelease(const std::vector<uint8_t>& bytes) {
    if (bytes.empty()) return {};
    const auto data = reinterpret_cast<const char*>(bytes.data());
    const auto count = static_cast<int>(bytes.size());
    const int wideCount = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, data, count, nullptr, 0);
    if (wideCount <= 0) return {};
    std::wstring wide(wideCount, L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, data, count, wide.data(), wideCount) != wideCount) return {};

    namespace json = ABI::Windows::Data::Json;
    const auto release = [](IUnknown* object) { object->Release(); };
    const auto named = [](const wchar_t* key, HSTRING_HEADER& header, HSTRING& name) {
        return WindowsCreateStringReference(key, static_cast<UINT32>(wcslen(key)), &header, &name);
    };
    HSTRING className = nullptr;
    HSTRING_HEADER classHeader{};
    json::IJsonValueStatics* rawParser = nullptr;
    if (FAILED(named(L"Windows.Data.Json.JsonValue", classHeader, className)) ||
        FAILED(RoGetActivationFactory(className, IID_PPV_ARGS(&rawParser)))) return {};
    const std::unique_ptr<json::IJsonValueStatics, decltype(release)> parser(rawParser, release);
    HSTRING jsonText = nullptr;
    HSTRING_HEADER textHeader{};
    json::IJsonValue* rawValue = nullptr;
    if (FAILED(WindowsCreateStringReference(wide.c_str(), static_cast<UINT32>(wide.size()),
                                            &textHeader, &jsonText)) ||
        FAILED(parser->Parse(jsonText, &rawValue))) return {};
    const std::unique_ptr<json::IJsonValue, decltype(release)> parsed(rawValue, release);
    json::IJsonObject* rawRoot = nullptr;
    if (FAILED(parsed->GetObject(&rawRoot))) return {};
    const std::unique_ptr<json::IJsonObject, decltype(release)> root(rawRoot, release);

    const auto readString = [&](json::IJsonObject* object, const wchar_t* key) {
        HSTRING name = nullptr;
        HSTRING_HEADER header{};
        HSTRING value = nullptr;
        if (FAILED(named(key, header, name)) || FAILED(object->GetNamedString(name, &value))) return std::wstring{};
        UINT32 length = 0;
        const auto buffer = WindowsGetStringRawBuffer(value, &length);
        std::wstring result(buffer, length);
        WindowsDeleteString(value);
        return result;
    };
    const auto wideTag = readString(root.get(), L"tag_name");
    if (wideTag.empty() || wideTag.find_first_not_of(L"v0123456789.") != wideTag.npos) return {};
    const std::string tag(wideTag.begin(), wideTag.end());
    const auto versionText = tag.front() == 'v' ? tag.substr(1) : tag;
    const auto version = ParseReleaseVersion(versionText);
    if (!version) return {};
    GitHubRelease result{tag, versionText, *version, 0, {}};

    HSTRING assetsName = nullptr, idName = nullptr;
    HSTRING_HEADER assetsHeader{}, idHeader{};
    json::IJsonArray* rawAssets = nullptr;
    if (FAILED(named(L"assets", assetsHeader, assetsName)) || FAILED(named(L"id", idHeader, idName)) ||
        FAILED(root->GetNamedArray(assetsName, &rawAssets))) return {};
    const std::unique_ptr<json::IJsonArray, decltype(release)> assets(rawAssets, release);
    for (UINT32 index = 0; index < 128; ++index) {
        json::IJsonObject* rawAsset = nullptr;
        if (FAILED(assets->GetObjectAt(index, &rawAsset))) break;
        const std::unique_ptr<json::IJsonObject, decltype(release)> asset(rawAsset, release);
        if (readString(asset.get(), L"name") != kSetupAsset) continue;
        const auto digest = readString(asset.get(), L"digest");
        double id = 0;
        if (digest.size() != 71 || digest.compare(0, 7, L"sha256:") != 0 ||
            digest.find_first_not_of(L"0123456789abcdef", 7) != digest.npos ||
            FAILED(asset->GetNamedNumber(idName, &id)) || !(id >= 1 && id < 9007199254740992.0)) return {};
        result.assetId = static_cast<uint64_t>(id);
        if (static_cast<double>(result.assetId) != id) return {};
        result.sha256.assign(digest.begin() + 7, digest.end());
        break;
    }
    return result;
}

inline UpdateResult FetchGitHubUpdate(const std::atomic_bool& stop, std::wstring_view token) {
    const auto api = std::wstring(L"https://api.github.com") + kReleaseApiPath;
    const auto latest = ReadReleaseAsset(api + L"latest", 1024 * 1024, stop, token);
    if (latest.status == 404 && token.empty()) return {"Private release. Add the GitHub update credential.", {}, {}};
    if (latest.status == 401 || latest.status == 403 || latest.status == 404) {
        return {"GitHub release unavailable. Check repository access.", {}, {}};
    }
    if (latest.status != 200) return {"Could not reach GitHub Releases.", {}, {}};
    const auto release = ParseGitHubRelease(latest.bytes);
    const auto current = ParseReleaseVersion(PHAHOL_REMOTE_VERSION);
    if (!release || !current) return {"GitHub returned invalid update information.", {}, {}};
    if (release->version <= *current) return {"Phahol Remote is up to date.", {}, {}};
    if (!release->assetId) return {"The GitHub release is missing the Setup asset.", {}, {}};

    const std::wstring tag(release->tag.begin(), release->tag.end());
    const auto url = token.empty() ? std::wstring(L"https://github.com") + kReleasePath +
        L"download/" + tag + L"/" + kSetupAsset : api + L"assets/" + std::to_wstring(release->assetId);
    auto setup = ReadReleaseAsset(url, kMaxSetupBytes, stop, token, true);
    if (setup.status != 200) return {"Could not download the GitHub installer.", {}, {}};
    if (setup.bytes.size() < 2 || setup.bytes[0] != 'M' || setup.bytes[1] != 'Z' ||
        SetupDigest(setup.bytes) != release->sha256) {
        return {"The installer failed SHA-256 verification.", {}, {}};
    }
    return {{}, release->versionText, std::move(setup.bytes)};
}

inline UpdateResult FetchUpdate(const std::atomic_bool& stop) {
    if (FAILED(RoInitialize(RO_INIT_MULTITHREADED))) return {"Windows could not start the update checker.", {}, {}};
    const UpdateToken token;
    auto result = FetchGitHubUpdate(stop, token.Value());
    RoUninitialize();
    return result;
}

inline bool RunningInstalledCopy() {
    std::array<wchar_t, 32768> executable{};
    const auto length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (!length || length >= executable.size()) return false;

    std::array<wchar_t, 32768> directory{};
    DWORD bytes = static_cast<DWORD>(sizeof(directory));
    if (RegGetValueW(HKEY_LOCAL_MACHINE, kUninstallKey, L"InstallLocation",
                     RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, nullptr, directory.data(), &bytes) != ERROR_SUCCESS ||
        !directory[0]) return false;
    const auto installed = std::filesystem::path(directory.data()) / L"PhaholRemote.exe";
    return CompareStringOrdinal(installed.c_str(), -1, executable.data(), -1, TRUE) == CSTR_EQUAL;
}

using UpdateProcess = std::unique_ptr<void, decltype(&CloseHandle)>;

inline UpdateProcess LaunchUpdate(const std::vector<uint8_t>& installer, HWND window, uint32_t token,
                                  std::wstring& stagedPath) {
    std::array<wchar_t, MAX_PATH + 1> directory{};
    const auto length = GetTempPathW(static_cast<DWORD>(directory.size()), directory.data());
    GUID id{};
    wchar_t unique[40]{};
    if (!length || length >= directory.size() || FAILED(CoCreateGuid(&id)) ||
        !StringFromGUID2(id, unique, static_cast<int>(std::size(unique)))) return {nullptr, CloseHandle};
    const auto path = std::wstring(directory.data()) + L"PhaholRemote-Setup-" + unique + L".exe";
    FileHandle file;
    file.Reset(CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file) return {nullptr, CloseHandle};
    DWORD bytes = 0;
    const bool written = WriteFile(file.Get(), installer.data(), static_cast<DWORD>(installer.size()),
                                   &bytes, nullptr) && bytes == installer.size() && FlushFileBuffers(file.Get());
    file.Reset();
    const auto zonePath = path + L":Zone.Identifier";
    constexpr char zone[] = "[ZoneTransfer]\r\nZoneId=3\r\n";
    bool marked = false;
    if (written) {
        file.Reset(CreateFileW(zonePath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                               FILE_ATTRIBUTE_NORMAL, nullptr));
        marked = file && WriteFile(file.Get(), zone, sizeof(zone) - 1, &bytes, nullptr) &&
            bytes == sizeof(zone) - 1;
        file.Reset();
    }
    bool verified = false;
    if (marked) {
        // Keep the verified executable open without write or delete sharing until Windows launches it.
        file.Reset(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                               FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        LARGE_INTEGER size{};
        std::vector<uint8_t> staged(installer.size());
        verified = file && GetFileSizeEx(file.Get(), &size) && size.QuadPart == static_cast<LONGLONG>(staged.size()) &&
            ReadFile(file.Get(), staged.data(), static_cast<DWORD>(staged.size()), &bytes, nullptr) &&
            bytes == staged.size() && staged == installer;
    }
    SHELLEXECUTEINFOW launch{};
    launch.cbSize = sizeof(launch);
    launch.fMask = SEE_MASK_NOASYNC | SEE_MASK_NOCLOSEPROCESS;
    launch.lpFile = path.c_str();
    const auto arguments = L"/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /NOCLOSEAPPLICATIONS "
        L"/NORESTARTAPPLICATIONS /PHAHOLUPDATE=1 /PHUPWINDOW=" +
        std::to_wstring(reinterpret_cast<UINT_PTR>(window)) + L" /PHUPTOKEN=" + std::to_wstring(token);
    launch.lpParameters = arguments.c_str();
    launch.hwnd = window;
    launch.nShow = SW_SHOWNORMAL;
    const bool started = verified && ShellExecuteExW(&launch);
    file.Reset();
    if (started && launch.hProcess) stagedPath = path;
    else DeleteFileW(path.c_str());
    return {started ? launch.hProcess : nullptr, CloseHandle};
}

} // namespace remote
