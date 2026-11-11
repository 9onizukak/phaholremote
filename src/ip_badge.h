#pragma once

#include "network.h"

#include <tuple>
#include <windowsx.h>

namespace remote {

inline std::string PreferredLocalIpv4() {
    constexpr ULONG flags = GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_ANYCAST |
        GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_SKIP_FRIENDLY_NAME;
    ULONG size = 15 * 1024;
    std::vector<uint8_t> buffer;
    ULONG result = ERROR_BUFFER_OVERFLOW;
    for (int attempt = 0; attempt < 3 && result == ERROR_BUFFER_OVERFLOW; ++attempt) {
        buffer.resize(size);
        result = GetAdaptersAddresses(AF_INET, flags, nullptr,
                                      reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
    }
    if (result != NO_ERROR) return {};

    using Rank = std::tuple<bool, bool, bool, ULONG, ULONG, uint32_t>;
    std::optional<Rank> best;
    in_addr selected{};
    for (auto* adapter = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()); adapter;
         adapter = adapter->Next) {
        if (adapter->OperStatus != IfOperStatusUp || adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        const bool lan = adapter->IfType == IF_TYPE_ETHERNET_CSMACD || adapter->IfType == IF_TYPE_IEEE80211;
        for (auto* address = adapter->FirstUnicastAddress; address; address = address->Next) {
            const auto* socket = address->Address.lpSockaddr;
            if (address->DadState != IpDadStatePreferred || !socket || socket->sa_family != AF_INET ||
                address->Address.iSockaddrLength < static_cast<int>(sizeof(sockaddr_in))) continue;
            const auto ip = reinterpret_cast<const sockaddr_in*>(socket)->sin_addr;
            const uint32_t hostOrder = ntohl(ip.s_addr);
            if (!hostOrder || (hostOrder >> 24) == 127) continue;
            const Rank rank{(hostOrder >> 16) == 0xa9fe, !lan, !adapter->FirstGatewayAddress,
                            adapter->Ipv4Metric, adapter->IfIndex, hostOrder};
            if (!best || rank < *best) {
                best = rank;
                selected = ip;
            }
        }
    }
    char text[INET_ADDRSTRLEN]{};
    if (!best || !inet_ntop(AF_INET, &selected, text, sizeof(text))) return {};
    return text;
}

inline int BadgeDpi(HDC dc) {
    return dc ? GetDeviceCaps(dc, LOGPIXELSX) : 96;
}

inline void RefreshIpBadge(HWND window) {
    const auto address = PreferredLocalIpv4();
    const auto label = address.empty() ? "IPv4 unavailable" : "IPv4  " + address;
    char previous[40]{};
    GetWindowTextA(window, previous, sizeof(previous));
    if (label != previous) {
        SetWindowTextA(window, label.c_str());
        InvalidateRect(window, nullptr, FALSE);
    }
}

inline void PositionIpBadge(HWND window) {
    RECT old{};
    GetWindowRect(window, &old);
    const HMONITOR monitor = IsRectEmpty(&old) ? nullptr : MonitorFromRect(&old, MONITOR_DEFAULTTONULL);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    RECT work{};
    const bool keepPosition = monitor && GetMonitorInfoW(monitor, &info);
    if (keepPosition) work = info.rcWork;
    else if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0)) return;
    const HDC dc = GetDC(window);
    const int dpi = BadgeDpi(dc);
    if (dc) ReleaseDC(window, dc);
    const auto width = MulDiv(198, dpi, 96);
    const auto height = MulDiv(34, dpi, 96);
    const auto margin = MulDiv(12, dpi, 96);
    const auto right = std::max(work.left, work.right - width);
    const auto bottom = std::max(work.top, work.bottom - height);
    const LONG x = keepPosition ? std::clamp(old.left, work.left, right) : std::max(work.left, right - margin);
    const LONG y = keepPosition ? std::clamp(old.top, work.top, bottom) : std::min(bottom, work.top + margin);
    if (old.left != x || old.top != y ||
        old.right - old.left != width || old.bottom - old.top != height) {
        SetWindowPos(window, HWND_TOPMOST, x, y, width, height, SWP_NOACTIVATE);
    }
}

inline void ShowIpBadgeMenu(HWND window, LPARAM position) {
    const HMENU menu = CreatePopupMenu();
    if (!menu) return;
    POINT point{GET_X_LPARAM(position), GET_Y_LPARAM(position)};
    if (point.x == -1 && point.y == -1) {
        RECT bounds{};
        GetWindowRect(window, &bounds);
        point = {bounds.left, bounds.bottom};
    }
    AppendMenuA(menu, MF_STRING, 1, "Hide IPv4 badge");
    SetForegroundWindow(window);
    const auto command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, point.x, point.y, 0, window, nullptr);
    PostMessageA(window, WM_NULL, 0, 0);
    DestroyMenu(menu);
    if (command == 1) ShowWindow(window, SW_HIDE);
}

inline LRESULT CALLBACK IpBadgeProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_TIMER:
            RefreshIpBadge(window);
            return 0;
        case WM_DISPLAYCHANGE:
        case WM_SETTINGCHANGE:
        case WM_EXITSIZEMOVE:
            PositionIpBadge(window);
            return 0;
        case WM_NCHITTEST:
            return HTCAPTION;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_CONTEXTMENU:
            ShowIpBadgeMenu(window, lParam);
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            const HDC dc = BeginPaint(window, &paint);
            RECT client{};
            GetClientRect(window, &client);
            const HBRUSH background = CreateSolidBrush(RGB(33, 26, 75));
            const HBRUSH accent = CreateSolidBrush(RGB(255, 155, 198));
            FillRect(dc, &client, background);
            const int dpi = BadgeDpi(dc);
            RECT edge = client;
            edge.right = MulDiv(4, dpi, 96);
            FillRect(dc, &edge, accent);
            DeleteObject(background);
            DeleteObject(accent);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(251, 246, 255));
            const HFONT font = CreateFontW(-MulDiv(11, dpi, 72), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
            const auto oldFont = SelectObject(dc, font ? font : GetStockObject(DEFAULT_GUI_FONT));
            char label[40]{};
            GetWindowTextA(window, label, sizeof(label));
            client.left += MulDiv(15, dpi, 96);
            DrawTextA(dc, label, -1, &client, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            if (oldFont && oldFont != HGDI_ERROR) SelectObject(dc, oldFont);
            if (font) DeleteObject(font);
            EndPaint(window, &paint);
            return 0;
        }
        case WM_DESTROY:
            KillTimer(window, 1);
            return 0;
    }
    return DefWindowProcA(window, message, wParam, lParam);
}

inline HWND CreateIpBadge(HINSTANCE instance) {
    constexpr char className[] = "PhaholRemoteIpBadge";
    WNDCLASSA wc{};
    wc.lpfnWndProc = IpBadgeProc;
    wc.hInstance = instance;
    wc.lpszClassName = className;
    if (!RegisterClassA(&wc)) return nullptr;
    const HWND window = CreateWindowExA(WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
        className, "", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
    if (!window) return nullptr;
    if (!SetLayeredWindowAttributes(window, 0, 245, LWA_ALPHA) || !SetTimer(window, 1, 5000, nullptr)) {
        DestroyWindow(window);
        return nullptr;
    }
    RefreshIpBadge(window);
    PositionIpBadge(window);
    ShowWindow(window, SW_SHOWNOACTIVATE);
    return window;
}

} // namespace remote
