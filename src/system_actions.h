#pragma once

#include "network.h"

namespace remote {

inline bool RemotePowerEnabled() {
    DWORD enabled = 0;
    DWORD size = sizeof(enabled);
    return RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Phahol Remote", L"AllowRemotePower",
                        RRF_RT_REG_DWORD | RRF_SUBKEY_WOW6464KEY, nullptr, &enabled, &size) == ERROR_SUCCESS &&
           enabled == 1;
}

inline DWORD RequestSystemAction(SystemAction action, bool allowPower) {
    if (action != SystemAction::secureAttention && action != SystemAction::shutdown &&
        action != SystemAction::restart) return ERROR_INVALID_PARAMETER;
    if (action != SystemAction::secureAttention && !allowPower) return ERROR_ACCESS_DISABLED_BY_POLICY;

    HANDLE rawToken = nullptr;
    const DWORD access = action == SystemAction::secureAttention ? TOKEN_QUERY : TOKEN_QUERY | TOKEN_ADJUST_PRIVILEGES;
    if (!OpenProcessToken(GetCurrentProcess(), access, &rawToken)) return GetLastError();
    const std::unique_ptr<void, decltype(&CloseHandle)> token(rawToken, &CloseHandle);

    if (action == SystemAction::secureAttention) {
        DWORD uiAccess = 0;
        DWORD size = 0;
        if (!GetTokenInformation(token.get(), TokenUIAccess, &uiAccess, sizeof(uiAccess), &size)) {
            return GetLastError();
        }
        if (!uiAccess) return ERROR_NOT_SUPPORTED;
        DWORD policy = 0;
        size = sizeof(policy);
        if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\System",
                         L"SoftwareSASGeneration", RRF_RT_REG_DWORD, nullptr, &policy, &size) != ERROR_SUCCESS ||
            (policy != 2 && policy != 3)) return ERROR_ACCESS_DISABLED_BY_POLICY;
        const HMODULE sas = LoadLibraryExW(L"sas.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!sas) return GetLastError();
        using SendSas = void (WINAPI*)(BOOL);
        const auto proc = GetProcAddress(sas, "SendSAS");
        if (!proc) {
            const auto error = GetLastError();
            FreeLibrary(sas);
            return error;
        }
        static_assert(sizeof(proc) == sizeof(SendSas));
        SendSas sendSas = nullptr;
        std::memcpy(&sendSas, &proc, sizeof(sendSas));
        sendSas(TRUE);
        FreeLibrary(sas);
        return ERROR_SUCCESS;
    }

    TOKEN_PRIVILEGES enable{};
    enable.PrivilegeCount = 1;
    enable.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (!LookupPrivilegeValueW(nullptr, L"SeShutdownPrivilege", &enable.Privileges[0].Luid)) return GetLastError();
    TOKEN_PRIVILEGES previous{};
    DWORD previousSize = sizeof(previous);
    if (!AdjustTokenPrivileges(token.get(), FALSE, &enable, sizeof(previous), &previous, &previousSize)) {
        return GetLastError();
    }
    const auto privilegeError = GetLastError();
    if (privilegeError != ERROR_SUCCESS) return privilegeError;
    wchar_t message[] = L"Phahol Remote requested a power action. Save any open work.";
    const bool accepted = InitiateSystemShutdownExW(nullptr, message, 15, FALSE,
        action == SystemAction::restart, SHTDN_REASON_MAJOR_APPLICATION | SHTDN_REASON_FLAG_PLANNED) != FALSE;
    const auto result = accepted ? ERROR_SUCCESS : GetLastError();
    AdjustTokenPrivileges(token.get(), FALSE, &previous, 0, nullptr, nullptr);
    return result;
}

} // namespace remote
