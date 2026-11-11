#include "session.h"
#include "ip_badge.h"
#include "resource.h"
#include "update.h"
#include "ui_theme.h"

#include <shellapi.h>
#include <ole2.h>
#include <shobjidl.h>
#include <uxtheme.h>
#include <windowsx.h>

namespace {
using namespace remote;

constexpr char kMainClass[] = "PhaholRemoteMain";
constexpr char kViewerClass[] = "PhaholRemoteViewer";
constexpr char kAppTitle[] = "Phahol Remote v" PHAHOL_REMOTE_VERSION;
constexpr char kViewerTitle[] = "Phahol Remote v" PHAHOL_REMOTE_VERSION " - Viewer";
constexpr int kConnectButton = 104;
constexpr int kUpdateButton = 106;
constexpr int kLocalIpLabel = 107;
constexpr COLORREF kLocalIpBackground = RGB(31, 62, 87);
constexpr UINT kSecureAttention = 201;
constexpr UINT kShutdownHost = 202;
constexpr UINT kRestartHost = 203;
constexpr UINT kPasteFiles = 204;
constexpr UINT kChooseFiles = 205;
constexpr int kMainWidth = 728;
constexpr int kMainHeight = 452;
constexpr int kTransferBarHeight = 56;
constexpr int kTransferButtonWidth = 112;
constexpr int kTransferBarPadding = 12;
constexpr int kTransferStatusLeft = 2 * kTransferButtonWidth + 3 * kTransferBarPadding;
constexpr char kTransferIdle[] = "Drag files here or paste files copied from Explorer.";
constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT kTrayIconId = 1;
constexpr UINT kTrayOpen = 1;
constexpr UINT kTrayExit = 2;
constexpr UINT kTrayToggleIpBadge = 3;
constexpr UINT kTrayCheckUpdates = 4;

struct App {
    HINSTANCE instance = nullptr;
    HWND main = nullptr;
    HWND viewer = nullptr;
    HWND viewerTransferStatus = nullptr;
    HWND connectButton = nullptr;
    HWND address = nullptr;
    HWND hostText = nullptr;
    HWND localIpText = nullptr;
    HWND clientText = nullptr;
    HWND updateText = nullptr;
    HWND ipBadge = nullptr;
    UINT taskbarCreated = 0;
    bool trayAdded = false;
    bool trayVersion4 = false;
    bool closing = false;

    std::atomic_bool hostStop{false};
    std::atomic_bool hostRunning{false};
    std::mutex hostSessionGate;
    Status hostStatus;
    std::thread hostThread;
    std::unique_ptr<ViewerSession> session;
    std::atomic_bool clientRunning{false};
    std::thread clientThread;

    std::atomic_bool updateStop{false};
    std::future<UpdateResult> updateTask;
    Clock::time_point nextUpdateCheck{};
    Clock::time_point nextInstallAttempt{};
    UpdateResult pendingUpdate;
    UpdateProcess updateProcess{nullptr, CloseHandle};
    std::wstring stagedSetup;
    UINT applyUpdateMessage = 0;
    uint32_t updateToken = 0;
    bool deferInstall = false;

    std::shared_ptr<const Pixels> displayed;
    std::string hostTextValue;
    std::string clientTextValue;
    std::string localIpValue;
    bool disconnectLabel = false;
    std::string viewerTitle;
    std::string viewerTransferText;
    unsigned mouseButtons = 0;
    bool pasteKeyDown = false;
    bool oleDropRegistered = false;

    void RequestStop() {
        updateStop.store(true);
        hostStop.store(true);
        if (session) session->stop.store(true);
    }

    void Stop() {
        RequestStop();
        if (hostThread.joinable()) hostThread.join();
        if (clientThread.joinable()) clientThread.join();
        if (updateTask.valid()) updateTask.wait();
    }
};

App app;

void CheckForUpdates(bool manual) {
    if (app.closing) return;
    if (manual) {
        app.nextInstallAttempt = {};
        if (app.updateTask.valid() || app.updateProcess || !app.pendingUpdate.installer.empty()) return;
    }
    app.nextUpdateCheck = Clock::now() + std::chrono::hours(24);
    SetWindowTextA(app.updateText, "Checking GitHub Releases...");
    app.updateTask = std::async(std::launch::async, [] { return FetchUpdate(app.updateStop); });
}

void PollUpdate(HWND window) {
    if (app.updateToken) {
        if (!app.updateProcess || WaitForSingleObject(app.updateProcess.get(), 0) == WAIT_TIMEOUT) return;
        app.updateProcess.reset();
        app.updateToken = 0;
        DeleteFileW(app.stagedSetup.c_str());
        app.stagedSetup.clear();
        if (std::exchange(app.deferInstall, false)) {
            app.nextInstallAttempt = Clock::now() + 1min;
            SetWindowTextA(app.updateText, "Update waiting for remote sessions to end.");
        } else {
            app.pendingUpdate = {};
            SetWindowTextA(app.updateText, "Update cancelled or Setup could not complete.");
        }
        return;
    }
    if (app.updateTask.valid()) {
        if (app.updateTask.wait_for(0ms) != std::future_status::ready) return;
        app.pendingUpdate = app.updateTask.get();
        if (app.pendingUpdate.installer.empty()) {
            SetWindowTextA(app.updateText, app.pendingUpdate.message.c_str());
        } else if (!app.applyUpdateMessage || !RunningInstalledCopy()) {
            app.pendingUpdate = {};
            SetWindowTextA(app.updateText, "Open the installed copy to apply the update.");
        } else {
            SetWindowTextA(app.updateText, "Update ready. Waiting for remote sessions to end.");
        }
        return;
    }
    if (app.pendingUpdate.installer.empty()) {
        if (Clock::now() >= app.nextUpdateCheck) CheckForUpdates(false);
        return;
    }
    if (app.clientRunning.load() || Clock::now() < app.nextInstallAttempt) return;
    {
        std::unique_lock<std::mutex> host(app.hostSessionGate, std::try_to_lock);
        if (!host) return;
    }

    uint32_t token = 0;
    if (BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(&token), sizeof(token),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) >= 0) {
        app.updateToken = (token & 0x7fffffffu) | 1u;
        const auto message = "Starting Setup " + app.pendingUpdate.version + "...";
        SetWindowTextA(app.updateText, message.c_str());
        app.updateProcess = LaunchUpdate(app.pendingUpdate.installer, window, app.updateToken, app.stagedSetup);
        if (app.updateProcess) return;
    }
    app.updateToken = 0;
    app.pendingUpdate = {};
    SetWindowTextA(app.updateText, "Setup could not start. Check for updates to retry.");
}

HICON LoadAppIcon(int width, int height) {
    const auto icon = static_cast<HICON>(LoadImageA(app.instance, MAKEINTRESOURCEA(IDI_PHAHOL_REMOTE),
                                                  IMAGE_ICON, width, height, LR_SHARED));
    return icon ? icon : LoadIcon(nullptr, IDI_APPLICATION);
}

NOTIFYICONDATAA TrayIcon(HWND window) {
    NOTIFYICONDATAA icon{};
    icon.cbSize = sizeof(icon);
    icon.hWnd = window;
    icon.uID = kTrayIconId;
    return icon;
}

void AddTrayIcon(HWND window) {
    auto icon = TrayIcon(window);
    icon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    icon.uCallbackMessage = kTrayMessage;
    icon.hIcon = LoadAppIcon(GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON));
    strcpy_s(icon.szTip, sizeof(icon.szTip), "Phahol Remote");
    app.trayAdded = Shell_NotifyIconA(NIM_ADD, &icon) != FALSE;
    icon.uVersion = NOTIFYICON_VERSION_4;
    app.trayVersion4 = app.trayAdded && Shell_NotifyIconA(NIM_SETVERSION, &icon) != FALSE;
}

void RestoreMain(HWND window) {
    if (app.closing) return;
    ShowWindow(window, SW_RESTORE);
    SetForegroundWindow(window);
}

void ShowTrayMenu(HWND window, POINT position) {
    const HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuA(menu, MF_STRING, kTrayOpen, "Open Phahol Remote");
    if (app.ipBadge) {
        AppendMenuA(menu, MF_STRING, kTrayToggleIpBadge,
                    IsWindowVisible(app.ipBadge) ? "Hide IPv4 badge" : "Show IPv4 badge");
    }
    AppendMenuA(menu, MF_STRING, kTrayCheckUpdates, "Check for updates");
    AppendMenuA(menu, MF_STRING, kTrayExit, "Exit");
    SetMenuDefaultItem(menu, kTrayOpen, FALSE);

    SetForegroundWindow(window);
    const UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, position.x, position.y,
                                        0, window, nullptr);
    PostMessageA(window, WM_NULL, 0, 0);
    DestroyMenu(menu);
    if (command == kTrayOpen) RestoreMain(window);
    if (command == kTrayToggleIpBadge && app.ipBadge) {
        ShowWindow(app.ipBadge, IsWindowVisible(app.ipBadge) ? SW_HIDE : SW_SHOWNOACTIVATE);
    }
    if (command == kTrayExit) PostMessageA(window, WM_CLOSE, 0, 0);
    if (command == kTrayCheckUpdates) CheckForUpdates(true);
    if (!command) {
        auto icon = TrayIcon(window);
        Shell_NotifyIconA(NIM_SETFOCUS, &icon);
    }
}

RECT VideoArea(HWND window) {
    RECT client{};
    GetClientRect(window, &client);
    client.top = std::min(client.bottom, static_cast<LONG>(theme::Px(kTransferBarHeight)));
    return client;
}

RECT VideoRectangle(HWND window) {
    const auto client = VideoArea(window);
    if (!app.displayed || IsRectEmpty(&client)) return {};
    const auto& image = *app.displayed;
    const auto availableHeight = client.bottom - client.top;
    const auto scale = std::min(double(client.right) / image.width, double(availableHeight) / image.height);
    const auto width = std::max(1L, static_cast<LONG>(image.width * scale));
    const auto height = std::max(1L, static_cast<LONG>(image.height * scale));
    const auto left = (client.right - width) / 2;
    const auto top = client.top + (availableHeight - height) / 2;
    return {left, top, left + width, top + height};
}

void RenderViewer(HDC windowDc, RECT client, const RECT& target, const Pixels* image) {
    if (IsRectEmpty(&client)) return;
    HDC bufferDc = nullptr;
    const auto buffer = BeginBufferedPaint(windowDc, &client, BPBF_COMPATIBLEBITMAP, nullptr, &bufferDc);
    const HDC dc = buffer ? bufferDc : windowDc;

    FillRect(dc, &client, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
    if (image && !IsRectEmpty(&target)) {
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = static_cast<LONG>(image->width);
        info.bmiHeader.biHeight = -static_cast<LONG>(image->height);
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        SetStretchBltMode(dc, HALFTONE);
        SetBrushOrgEx(dc, 0, 0, nullptr);
        StretchDIBits(dc, target.left, target.top, target.right - target.left, target.bottom - target.top,
                      0, 0, image->width, image->height, image->bgra.data(), &info, DIB_RGB_COLORS, SRCCOPY);
    } else {
        SetTextColor(dc, RGB(235, 235, 235));
        SetBkMode(dc, TRANSPARENT);
        DrawTextA(dc, "Waiting for video...", -1, &client, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    if (buffer) EndBufferedPaint(buffer, TRUE);
}

void PaintViewer(HWND window) {
    PAINTSTRUCT paint{};
    const HDC dc = BeginPaint(window, &paint);
    const auto video = VideoArea(window);
    const RECT bar{video.left, 0, video.right, video.top};
    theme::Fill(dc, bar, theme::canvas);
    const RECT divider{bar.left, bar.bottom - 1, bar.right, bar.bottom};
    theme::Fill(dc, divider, theme::border);
    RenderViewer(dc, video, VideoRectangle(window), app.displayed.get());
    EndPaint(window, &paint);
}

bool Pointer(HWND window, POINT point, InputKind kind, uint32_t value, uint32_t flags) {
    const auto rect = VideoRectangle(window);
    if (!app.session || rect.right <= rect.left || rect.bottom <= rect.top) return false;
    if (!PtInRect(&rect, point) && !app.mouseButtons) return false;
    const auto x = std::clamp(point.x, rect.left, rect.right - 1) - rect.left;
    const auto y = std::clamp(point.y, rect.top, rect.bottom - 1) - rect.top;
    const auto normalizedX = uint32_t(uint64_t(x) * kNormalizedMax / std::max(1L, rect.right - rect.left - 1));
    const auto normalizedY = uint32_t(uint64_t(y) * kNormalizedMax / std::max(1L, rect.bottom - rect.top - 1));
    return app.session->Enqueue({kind, normalizedX, normalizedY, value, flags});
}

void ReleaseViewerInput(HWND window) {
    app.mouseButtons = 0;
    if (app.session) app.session->Enqueue({InputKind::releaseAll, 0, 0, 0, 0});
    if (GetCapture() == window) ReleaseCapture();
}

std::optional<std::vector<std::filesystem::path>> ReadTransferPaths(HDROP drop) {
    if (!drop) return std::nullopt;
    const UINT count = DragQueryFileW(drop, 0xffffffff, nullptr, 0);
    if (!count || count > kMaxQueuedFiles) return std::nullopt;
    std::vector<std::filesystem::path> files;
    files.reserve(count);
    for (UINT index = 0; index < count; ++index) {
        const UINT length = DragQueryFileW(drop, index, nullptr, 0);
        if (!length || length > 32767) return std::nullopt;
        std::wstring path(length + 1, L'\0');
        if (DragQueryFileW(drop, index, path.data(), length + 1) != length) return std::nullopt;
        path.resize(length);
        files.emplace_back(std::move(path));
    }
    return files;
}

bool QueueViewerFiles(std::optional<std::vector<std::filesystem::path>> files) {
    if (!app.session) return false;
    if (!files) {
        app.session->transferStatus.Set("Select up to 32 files in Explorer, then copy or drag them here.");
    } else if (!app.session->QueueFiles(std::move(*files))) {
        app.session->transferStatus.Set("Files not queued. Check the connection or wait for the queue to clear.");
    } else {
        return true;
    }
    return false;
}

void SendDroppedFiles(HDROP drop) {
    auto files = ReadTransferPaths(drop);
    DragFinish(drop);
    QueueViewerFiles(std::move(files));
}

bool PasteViewerFiles(HWND window) {
    if (!IsClipboardFormatAvailable(CF_HDROP)) return false;
    if (!OpenClipboard(window)) {
        if (app.session) app.session->transferStatus.Set("Clipboard is busy. Try pasting the files again.");
        return true;
    }
    auto files = ReadTransferPaths(static_cast<HDROP>(GetClipboardData(CF_HDROP)));
    CloseClipboard();
    QueueViewerFiles(std::move(files));
    return true;
}

void ChooseViewerFiles(HWND window) {
    auto* const session = app.session.get();
    if (!session) return;
    const auto reportError = [&] {
        if (app.viewer == window && app.session.get() == session) {
            session->transferStatus.Set("Could not select local files.");
        }
    };
    IFileOpenDialog* rawDialog = nullptr;
    const auto release = [](IUnknown* object) { object->Release(); };
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&rawDialog)))) {
        reportError();
        return;
    }
    const std::unique_ptr<IFileOpenDialog, decltype(release)> dialog(rawDialog, release);
    if (FAILED(dialog->SetOptions(FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST |
                                  FOS_ALLOWMULTISELECT | FOS_NOCHANGEDIR)) ||
        FAILED(dialog->SetTitle(L"Choose files to send to Host"))) {
        reportError();
        return;
    }
    const HRESULT result = dialog->Show(window);
    if (app.viewer != window || app.session.get() != session ||
        result == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return;
    IShellItemArray* rawItems = nullptr;
    if (FAILED(result) || FAILED(dialog->GetResults(&rawItems))) {
        reportError();
        return;
    }
    const std::unique_ptr<IShellItemArray, decltype(release)> items(rawItems, release);
    DWORD count = 0;
    if (FAILED(items->GetCount(&count)) || !count || count > kMaxQueuedFiles) {
        QueueViewerFiles(std::nullopt);
        return;
    }
    std::vector<std::filesystem::path> files;
    files.reserve(count);
    for (DWORD index = 0; index < count; ++index) {
        IShellItem* rawItem = nullptr;
        if (FAILED(items->GetItemAt(index, &rawItem))) {
            reportError();
            return;
        }
        const std::unique_ptr<IShellItem, decltype(release)> item(rawItem, release);
        PWSTR rawPath = nullptr;
        if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &rawPath))) {
            reportError();
            return;
        }
        const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> path(rawPath, &CoTaskMemFree);
        files.emplace_back(path.get());
    }
    QueueViewerFiles(std::move(files));
}

class ViewerDropTarget final : public IDropTarget {
public:
    explicit ViewerDropTarget(HWND window) : window_(window) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        *object = nullptr;
        if (iid != IID_IUnknown && iid != IID_IDropTarget) return E_NOINTERFACE;
        *object = static_cast<IDropTarget*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const auto remaining = --references_;
        if (!remaining) delete this;
        return remaining;
    }
    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data, DWORD, POINTL, DWORD* effect) override {
        auto format = FileFormat();
        hasFiles_ = data->QueryGetData(&format) == S_OK;
        *effect = CopyEffect(*effect);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL, DWORD* effect) override {
        *effect = CopyEffect(*effect);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override {
        hasFiles_ = false;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* data, DWORD, POINTL, DWORD* effect) override {
        const DWORD allowed = CopyEffect(*effect);
        *effect = DROPEFFECT_NONE;
        hasFiles_ = false;
        if (allowed == DROPEFFECT_NONE) return S_OK;

        auto* const session = app.session.get();
        auto format = FileFormat();
        STGMEDIUM medium{};
        std::optional<std::vector<std::filesystem::path>> files;
        if (SUCCEEDED(data->GetData(&format, &medium))) {
            if (medium.tymed == TYMED_HGLOBAL) {
                files = ReadTransferPaths(reinterpret_cast<HDROP>(medium.hGlobal));
            }
            ReleaseStgMedium(&medium);
        }
        if (app.viewer == window_ && app.session.get() == session && QueueViewerFiles(std::move(files))) {
            *effect = DROPEFFECT_COPY;
        }
        return S_OK;
    }

private:
    static FORMATETC FileFormat() { return {CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL}; }
    DWORD CopyEffect(DWORD allowed) const {
        if (!hasFiles_ || app.viewer != window_ || !app.session || !app.session->connected.load() ||
            app.session->stop.load()) return DROPEFFECT_NONE;
        return allowed & DROPEFFECT_COPY;
    }

    HWND window_;
    std::atomic<ULONG> references_{1};
    bool hasFiles_ = false;
};

bool CreateTransferBar(HWND window) {
    const auto button = [&](UINT id, const char* text, int left) {
        return CreateWindowExA(0, "BUTTON", text, WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                               theme::Px(left), theme::Px(10), theme::Px(kTransferButtonWidth),
                               theme::Px(36), window,
                               reinterpret_cast<HMENU>(static_cast<UINT_PTR>(id)), app.instance, nullptr);
    };
    const auto send = button(kChooseFiles, "Send files...", kTransferBarPadding);
    const auto paste = button(kPasteFiles, "Paste files", kTransferButtonWidth + 2 * kTransferBarPadding);
    app.viewerTransferStatus = CreateWindowExA(0, "STATIC", kTransferIdle,
        WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE | SS_NOPREFIX | SS_ENDELLIPSIS,
        theme::Px(kTransferStatusLeft), 0, 0, theme::Px(kTransferBarHeight), window, nullptr,
        app.instance, nullptr);
    if (!send || !paste || !app.viewerTransferStatus) return false;
    const auto font = reinterpret_cast<WPARAM>(theme::Type().body);
    for (const auto control : {send, paste, app.viewerTransferStatus}) SendMessageA(control, WM_SETFONT, font, FALSE);
    return true;
}

void UpdateTransferBar() {
    if (!app.viewerTransferStatus || !app.session) return;
    auto transfer = app.session->transferStatus.Get();
    if (transfer.empty()) transfer = kTransferIdle;
    if (transfer != app.viewerTransferText) {
        SetWindowTextA(app.viewerTransferStatus, transfer.c_str());
        app.viewerTransferText = std::move(transfer);
    }
}

LRESULT CALLBACK ViewerProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_CREATE: {
            const HMENU bar = CreateMenu();
            const HMENU actions = CreatePopupMenu();
            if (!bar || !actions) {
                if (bar) DestroyMenu(bar);
                if (actions) DestroyMenu(actions);
                return -1;
            }
            AppendMenuA(actions, MF_STRING, kSecureAttention, "Ctrl+Alt+Del");
            AppendMenuA(actions, MF_SEPARATOR, 0, nullptr);
            AppendMenuA(actions, MF_STRING, kShutdownHost, "Shut down...");
            AppendMenuA(actions, MF_STRING, kRestartHost, "Restart...");
            if (!AppendMenuA(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(actions), "Host")) {
                DestroyMenu(actions);
                DestroyMenu(bar);
                return -1;
            }
            const HMENU files = CreatePopupMenu();
            if (!files || !AppendMenuA(files, MF_STRING, kChooseFiles, "Send files...") ||
                !AppendMenuA(files, MF_STRING, kPasteFiles, "Paste files\tCtrl+V") ||
                !AppendMenuA(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(files), "Files")) {
                if (files) DestroyMenu(files);
                DestroyMenu(bar);
                return -1;
            }
            if (!SetMenu(window, bar)) {
                DestroyMenu(bar);
                return -1;
            }
            if (!CreateTransferBar(window)) return -1;
            auto* const target = new ViewerDropTarget(window);
            app.oleDropRegistered = SUCCEEDED(RegisterDragDrop(window, target));
            target->Release();
            if (!app.oleDropRegistered) DragAcceptFiles(window, TRUE);
            return 0;
        }
        case WM_COMMAND: {
            const auto id = LOWORD(wParam);
            if (id == kChooseFiles) {
                ChooseViewerFiles(window);
                if (app.viewer == window) SetFocus(window);
                UpdateTransferBar();
                return 0;
            }
            if (id == kPasteFiles) {
                if (!PasteViewerFiles(window) && app.session) {
                    app.session->transferStatus.Set("No files in the clipboard. Copy files in Explorer first.");
                }
                if (app.viewer == window) SetFocus(window);
                UpdateTransferBar();
                return 0;
            }
            if (id != kSecureAttention && id != kShutdownHost && id != kRestartHost) break;
            if (!app.session) return 0;
            const auto action = id == kSecureAttention ? SystemAction::secureAttention :
                id == kShutdownHost ? SystemAction::shutdown : SystemAction::restart;
            auto* const session = app.session.get();
            if (action != SystemAction::secureAttention) {
                const char* prompt = action == SystemAction::restart
                    ? "Restart the remote Host in 15 seconds?"
                    : "Shut down the remote Host in 15 seconds?";
                if (MessageBoxA(window, prompt, "Phahol Remote",
                                MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) return 0;
            }
            if (app.viewer != window || app.session.get() != session) return 0;
            if (!session->QueueSystemAction(action)) {
                MessageBoxA(window, "Connect to the Host and wait for any pending action to finish.",
                            "Phahol Remote", MB_OK | MB_ICONINFORMATION);
            }
            return 0;
        }
        case WM_DROPFILES:
            SendDroppedFiles(reinterpret_cast<HDROP>(wParam));
            UpdateTransferBar();
            return 0;
        case WM_MOUSEACTIVATE:
            if (LOWORD(lParam) == HTCLIENT) return MA_ACTIVATE;
            break;
        case WM_SETCURSOR:
            if (LOWORD(lParam) == HTCLIENT) {
                SetCursor(LoadCursor(nullptr, IDC_ARROW));
                return TRUE;
            }
            break;
        case WM_PAINT:
            PaintViewer(window);
            return 0;
        case WM_DRAWITEM:
            if (theme::Button(*reinterpret_cast<const DRAWITEMSTRUCT*>(lParam), wParam == kChooseFiles,
                              theme::canvas)) return TRUE;
            break;
        case WM_CTLCOLORSTATIC: {
            const auto dc = reinterpret_cast<HDC>(wParam);
            SetTextColor(dc, theme::muted);
            SetBkColor(dc, theme::canvas);
            SetDCBrushColor(dc, theme::canvas);
            return reinterpret_cast<LRESULT>(GetStockObject(DC_BRUSH));
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_SIZE:
            if (app.viewerTransferStatus) {
                const int left = theme::Px(kTransferStatusLeft);
                const int width = std::max(0, static_cast<int>(LOWORD(lParam)) - left - theme::Px(kTransferBarPadding));
                MoveWindow(app.viewerTransferStatus, left, 0, width, theme::Px(kTransferBarHeight), TRUE);
            }
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        case WM_MOUSEMOVE:
            Pointer(window, {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)}, InputKind::move, 0, 0);
            return 0;
        case WM_LBUTTONDOWN: case WM_LBUTTONUP:
        case WM_RBUTTONDOWN: case WM_RBUTTONUP:
        case WM_MBUTTONDOWN: case WM_MBUTTONUP:
        case WM_XBUTTONDOWN: case WM_XBUTTONUP: {
            Button button = Button::left;
            if (message == WM_RBUTTONDOWN || message == WM_RBUTTONUP) button = Button::right;
            if (message == WM_MBUTTONDOWN || message == WM_MBUTTONUP) button = Button::middle;
            const bool xButton = message == WM_XBUTTONDOWN || message == WM_XBUTTONUP;
            if (xButton) button = GET_XBUTTON_WPARAM(wParam) == XBUTTON1 ? Button::x1 : Button::x2;
            const bool down = message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN ||
                              message == WM_MBUTTONDOWN || message == WM_XBUTTONDOWN;
            const auto value = static_cast<uint32_t>(button);
            const unsigned mask = 1u << (value - 1);
            if ((down || (app.mouseButtons & mask)) &&
                Pointer(window, {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)}, InputKind::button,
                        value, down ? kDown : 0)) {
                if (down) {
                    SetFocus(window);
                    app.mouseButtons |= mask;
                    SetCapture(window);
                } else {
                    app.mouseButtons &= ~mask;
                    if (!app.mouseButtons && GetCapture() == window) ReleaseCapture();
                }
            }
            return xButton ? TRUE : 0;
        }
        case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL: {
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ScreenToClient(window, &point);
            Pointer(window, point, InputKind::wheel, static_cast<uint32_t>(GET_WHEEL_DELTA_WPARAM(wParam)),
                    message == WM_MOUSEHWHEEL ? kHorizontal : 0);
            return 0;
        }
        case WM_KEYDOWN: case WM_KEYUP: case WM_SYSKEYDOWN: case WM_SYSKEYUP: {
            const auto bits = static_cast<uint32_t>(lParam);
            const auto scan = (bits >> 16) & 0xff;
            const bool down = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
            if (wParam == 'V') {
                if (down && !(bits & (1u << 30))) {
                    app.pasteKeyDown = message == WM_KEYDOWN && GetKeyState(VK_CONTROL) < 0 &&
                        GetKeyState(VK_MENU) >= 0 && GetKeyState(VK_SHIFT) >= 0 && PasteViewerFiles(window);
                }
                if (app.pasteKeyDown) {
                    if (!down) app.pasteKeyDown = false;
                    return 0;
                }
            }
            const auto flags = (down ? kDown : 0) | ((bits & (1u << 24)) ? kExtended : 0);
            if (app.session && scan) app.session->Enqueue({InputKind::key, 0, 0, scan, flags});
            return 0;
        }
        case WM_CHAR: case WM_SYSCHAR:
            return 0;
        case WM_CAPTURECHANGED:
            if (app.mouseButtons) ReleaseViewerInput(window);
            return 0;
        case WM_KILLFOCUS: case WM_CANCELMODE:
            ReleaseViewerInput(window);
            return 0;
        case WM_CLOSE:
            if (app.session) app.session->stop.store(true);
            DestroyWindow(window);
            return 0;
        case WM_DESTROY:
            if (app.oleDropRegistered) {
                RevokeDragDrop(window);
                app.oleDropRegistered = false;
            } else {
                DragAcceptFiles(window, FALSE);
            }
            app.viewer = nullptr;
            app.viewerTransferStatus = nullptr;
            app.displayed.reset();
            app.viewerTitle.clear();
            app.viewerTransferText.clear();
            app.mouseButtons = 0;
            app.pasteKeyDown = false;
            return 0;
    }
    return DefWindowProcA(window, message, wParam, lParam);
}

void SetChangedText(HWND window, std::string& previous, std::string text) {
    if (text == previous) return;
    SetWindowTextA(window, text.c_str());
    previous = std::move(text);
}

void UpdateUi() {
    const auto host = app.hostStatus.Get();
    if (host != app.hostTextValue) {
        SetChangedText(app.hostText, app.hostTextValue, host);
        const auto indicator = theme::Box(47, 187, 63, 207);
        InvalidateRect(app.main, &indicator, FALSE);
    }
    if (app.ipBadge) {
        char label[40]{};
        GetWindowTextA(app.ipBadge, label, sizeof(label));
        const std::string_view address(label);
        if (address.substr(0, 6) == "IPv4  ") {
            SetChangedText(app.localIpText, app.localIpValue, std::string(address.substr(6)));
        } else if (!address.empty()) {
            SetChangedText(app.localIpText, app.localIpValue, "Unavailable");
        }
    }

    const bool running = app.clientRunning.load();
    if (running != app.disconnectLabel) {
        SetWindowTextA(app.connectButton, running ? "Disconnect" : "Connect");
        EnableWindow(app.address, !running);
        app.disconnectLabel = running;
    }
    if (!app.session) return;
    const auto action = app.session->systemActionStatus.Get();
    auto connection = app.session->status.Get();
    if (!action.empty()) connection += " | " + action;
    SetChangedText(app.clientText, app.clientTextValue, std::move(connection));
    if (!running || !app.session->connected.load() || app.session->stop.load()) {
        if (app.viewer) DestroyWindow(app.viewer);
        return;
    }
    if (!app.viewer) {
        app.viewer = CreateWindowExA(0, kViewerClass, kViewerTitle,
                                     WS_OVERLAPPEDWINDOW | WS_VISIBLE | WS_MAXIMIZE | WS_CLIPCHILDREN,
                                     CW_USEDEFAULT, CW_USEDEFAULT,
                                     1050, 650, app.main, nullptr, app.instance, nullptr);
        if (!app.viewer) {
            app.session->stop.store(true);
            return;
        }
        SetFocus(app.viewer);
    }

    UpdateTransferBar();
    const auto mouse = app.session->mouseStatus.Get();
    auto title = std::string(kViewerTitle);
    if (!action.empty()) title += " | " + action;
    if (!mouse.empty()) title += " | " + mouse;
    if (title != app.viewerTitle) {
        SetWindowTextA(app.viewer, title.c_str());
        app.viewerTitle = title;
    }

    auto latest = app.session->Latest();
    if (latest != app.displayed) {
        if (!latest && app.mouseButtons) ReleaseViewerInput(app.viewer);
        app.displayed = std::move(latest);
        InvalidateRect(app.viewer, nullptr, FALSE);
    }
}

void ToggleClient() {
    if (app.clientRunning.load()) {
        app.session->stop.store(true);
        if (app.viewer) DestroyWindow(app.viewer);
        return;
    }
    if (app.clientThread.joinable()) app.clientThread.join();
    char ip[32]{};
    GetWindowTextA(app.address, ip, sizeof(ip));
    auto target = Address(kControlPort);
    if (inet_pton(AF_INET, ip, &target.sin_addr) != 1) {
        if (app.session) app.session->status.Set("Enter a valid IPv4 address.");
        SetChangedText(app.clientText, app.clientTextValue, "Enter a valid IPv4 address.");
        return;
    }

    app.session = std::make_unique<ViewerSession>();
    app.clientRunning.store(true);
    app.clientThread = std::thread([target] {
        app.session->Run(target);
        app.clientRunning.store(false);
    });
}

HWND Control(const char* klass, const char* label, DWORD style, int x, int y, int width, int height,
             int id = 0) {
    HWND control = CreateWindowExA(0, klass, label, WS_CHILD | WS_VISIBLE | style,
                                   theme::Px(x), theme::Px(y), theme::Px(width), theme::Px(height), app.main,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), app.instance, nullptr);
    SendMessageA(control, WM_SETFONT, reinterpret_cast<WPARAM>(theme::Type().body), TRUE);
    return control;
}

void PaintMain(HWND window) {
    PAINTSTRUCT paint{};
    const auto windowDc = BeginPaint(window, &paint);
    RECT client{};
    GetClientRect(window, &client);
    HDC bufferedDc = nullptr;
    const auto buffer = BeginBufferedPaint(windowDc, &client, BPBF_COMPATIBLEBITMAP, nullptr, &bufferedDc);
    const auto dc = buffer ? bufferedDc : windowDc;
    const auto& fonts = theme::Type();
    constexpr auto line = DT_LEFT | DT_VCENTER | DT_SINGLELINE;

    theme::Fill(dc, client, theme::canvas);
    theme::Fill(dc, theme::Box(0, 0, kMainWidth, 112), theme::navy);
    theme::Fill(dc, theme::Box(0, 109, kMainWidth, 112), RGB(51, 178, 167));
    theme::Rounded(dc, theme::Box(24, 26, 84, 86), theme::mint, theme::mint, 14);
    DrawIconEx(dc, theme::Px(33), theme::Px(35), LoadAppIcon(theme::Px(42), theme::Px(42)),
               theme::Px(42), theme::Px(42), 0, nullptr, DI_NORMAL);
    theme::Text(dc, "Phahol Remote", theme::Box(101, 24, 480, 65), fonts.title, theme::surface, line);
    theme::Text(dc, "LAN remote desktop", theme::Box(103, 65, 470, 86), fonts.body, RGB(184, 210, 224), line);

    theme::Rounded(dc, theme::Box(516, 25, 704, 87), kLocalIpBackground, kLocalIpBackground, 11);
    theme::Text(dc, "This device IPv4", theme::Box(530, 31, 691, 55), fonts.body, RGB(184, 210, 224), line);

    theme::Rounded(dc, theme::Box(24, 132, 704, 230), theme::surface, theme::border, 14);
    theme::Text(dc, "Host", theme::Box(47, 148, 681, 181), fonts.heading, theme::text, line);
    const std::string_view host = app.hostTextValue;
    const bool healthy = host.substr(0, 10) == "Listening " || host.substr(0, 8) == "Sharing ";
    const auto indicator = healthy ? theme::teal : RGB(146, 93, 14);
    theme::Rounded(dc, theme::Box(48, 192, 58, 202), indicator, indicator, 5);

    theme::Rounded(dc, theme::Box(24, 248, 704, 398), theme::surface, theme::border, 14);
    theme::Text(dc, "Connect to a Host", theme::Box(47, 260, 681, 291), fonts.heading, theme::text, line);
    theme::Rounded(dc, theme::Box(47, 314, 510, 357), theme::surface,
                   GetFocus() == app.address ? theme::teal : theme::border, 8);
    theme::Text(dc, "v" PHAHOL_REMOTE_VERSION, theme::Box(24, 405, 280, 443), fonts.label, theme::muted, line);

    if (buffer) EndBufferedPaint(buffer, TRUE);
    EndPaint(window, &paint);
}

LRESULT CALLBACK MainProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message != 0 && message == app.applyUpdateMessage) {
        if (app.closing || !app.updateToken || wParam != app.updateToken) return 0;
        std::unique_lock<std::mutex> host(app.hostSessionGate, std::try_to_lock);
        if (!host || app.clientRunning.load()) {
            app.deferInstall = true;
            return 2;
        }
        SendMessageA(window, WM_CLOSE, 0, 0);
        return 1;
    }
    if (message == app.taskbarCreated && app.taskbarCreated != 0) {
        AddTrayIcon(window);
        if (app.trayAdded && IsIconic(window)) ShowWindow(window, SW_HIDE);
        else if (!app.trayAdded && !IsWindowVisible(window)) RestoreMain(window);
        return 0;
    }
    switch (message) {
        case WM_CREATE:
            app.main = window;
            app.hostStatus.Set("Starting LAN host...");
            app.hostText = Control("STATIC", "Starting LAN host...", SS_LEFT | SS_NOPREFIX, 69, 187, 609, 32);
            app.localIpValue = PreferredLocalIpv4();
            if (app.localIpValue.empty()) app.localIpValue = "Unavailable";
            app.localIpText = Control("STATIC", app.localIpValue.c_str(), SS_LEFT, 530, 57, 161, 22, kLocalIpLabel);
            SendMessageA(app.localIpText, WM_SETFONT, reinterpret_cast<WPARAM>(theme::Type().label), TRUE);
            Control("STATIC", "Target IPv4 address", SS_LEFT, 47, 293, 460, 19);
            app.address = Control("EDIT", "", WS_TABSTOP | ES_AUTOHSCROLL, 59, 326, 438, 23, 105);
            SendMessageA(app.address, EM_SETLIMITTEXT, 15, 0);
            app.connectButton = Control("BUTTON", "Connect", WS_TABSTOP | BS_OWNERDRAW, 523, 314, 158, 43, kConnectButton);
            Control("BUTTON", "Check for updates", WS_TABSTOP | BS_OWNERDRAW, 528, 405, 176, 38, kUpdateButton);
            app.clientText = Control("STATIC", "Enter the host's LAN IPv4 address.", SS_LEFT | SS_NOPREFIX, 47, 362, 634, 34);
            app.updateText = Control("STATIC", "", SS_LEFT | SS_NOPREFIX | SS_CENTERIMAGE, 99, 405, 416, 38);
            SetTimer(window, 1, 33, nullptr);
            app.hostRunning.store(true);
            app.hostThread = std::thread([] {
                RunHost(app.hostStop, app.hostStatus, app.hostSessionGate);
                app.hostRunning.store(false);
            });
            return 0;
        case WM_PAINT:
            PaintMain(window);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_DRAWITEM:
            if (theme::Button(*reinterpret_cast<const DRAWITEMSTRUCT*>(lParam), wParam == kConnectButton,
                              wParam == kConnectButton ? theme::surface : theme::canvas)) return TRUE;
            break;
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORSTATIC: {
            const auto dc = reinterpret_cast<HDC>(wParam);
            const bool localIp = reinterpret_cast<HWND>(lParam) == app.localIpText;
            const bool address = reinterpret_cast<HWND>(lParam) == app.address;
            const auto background = localIp ? kLocalIpBackground :
                reinterpret_cast<HWND>(lParam) == app.updateText ? theme::canvas : theme::surface;
            SetBkColor(dc, background);
            SetTextColor(dc, localIp ? theme::surface : address ? theme::text : theme::muted);
            SetDCBrushColor(dc, background);
            return reinterpret_cast<LRESULT>(GetStockObject(DC_BRUSH));
        }
        case WM_SYSCOMMAND:
            if ((wParam & 0xfff0) == SC_MINIMIZE && app.trayAdded) {
                ShowWindow(window, SW_HIDE);
                return 0;
            }
            break;
        case kTrayMessage: {
            const UINT event = LOWORD(lParam);
            if (event == NIN_SELECT || event == NIN_KEYSELECT || event == WM_LBUTTONDBLCLK) {
                RestoreMain(window);
            } else if (event == WM_CONTEXTMENU || (!app.trayVersion4 && event == WM_RBUTTONUP)) {
                POINT position{GET_X_LPARAM(wParam), GET_Y_LPARAM(wParam)};
                if (!app.trayVersion4) GetCursorPos(&position);
                ShowTrayMenu(window, position);
            }
            return 0;
        }
        case WM_TIMER:
            if (app.closing) {
                if (!app.hostRunning.load() && !app.clientRunning.load()) DestroyWindow(window);
            } else {
                UpdateUi();
                PollUpdate(window);
            }
            return 0;
        case WM_COMMAND:
            if (reinterpret_cast<HWND>(lParam) == app.address &&
                (HIWORD(wParam) == EN_SETFOCUS || HIWORD(wParam) == EN_KILLFOCUS)) {
                const auto input = theme::Box(47, 314, 510, 357);
                InvalidateRect(window, &input, FALSE);
            }
            if (!app.closing && HIWORD(wParam) == BN_CLICKED) {
                if (LOWORD(wParam) == kConnectButton) ToggleClient();
                if (LOWORD(wParam) == kUpdateButton) CheckForUpdates(true);
            }
            return 0;
        case WM_CLOSE:
            if (!app.closing) {
                app.closing = true;
                app.RequestStop();
                if (app.viewer) DestroyWindow(app.viewer);
                ShowWindow(window, SW_HIDE);
            }
            return 0;
        case WM_DESTROY:
            KillTimer(window, 1);
            if (app.ipBadge) {
                DestroyWindow(app.ipBadge);
                app.ipBadge = nullptr;
            }
            if (app.trayAdded) {
                auto icon = TrayIcon(window);
                Shell_NotifyIconA(NIM_DELETE, &icon);
                app.trayAdded = false;
            }
            app.Stop();
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcA(window, message, wParam, lParam);
}

bool RegisterWindow(const char* name, WNDPROC proc, HBRUSH background) {
    WNDCLASSEXA wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = proc;
    wc.hInstance = app.instance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadAppIcon(GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON));
    wc.hIconSm = LoadAppIcon(GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON));
    wc.hbrBackground = background;
    wc.lpszClassName = name;
    return RegisterClassExA(&wc) != 0;
}

int RunUi(HINSTANCE instance, int show, bool startInTray) {
    app.instance = instance;
    app.taskbarCreated = RegisterWindowMessageA("TaskbarCreated");
    app.applyUpdateMessage = RegisterWindowMessageW(kApplyUpdateMessage);
    if (!RegisterWindow(kMainClass, MainProc, nullptr) ||
        !RegisterWindow(kViewerClass, ViewerProc, nullptr)) return 1;

    constexpr DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    auto bounds = theme::Box(0, 0, kMainWidth, kMainHeight);
    AdjustWindowRectEx(&bounds, style, FALSE, 0);
    HWND window = CreateWindowExA(0, kMainClass, kAppTitle, style,
                                  CW_USEDEFAULT, CW_USEDEFAULT, bounds.right - bounds.left, bounds.bottom - bounds.top,
                                  nullptr, nullptr, instance, nullptr);
    if (!window) return 1;
    AddTrayIcon(window);
    app.ipBadge = CreateIpBadge(instance);
    if (!startInTray || !app.trayAdded) ShowWindow(window, startInTray ? SW_SHOWMINNOACTIVE : show);
    MSG message{};
    int result;
    while ((result = GetMessageA(&message, nullptr, 0, 0)) > 0) {
        if (message.hwnd == app.address && message.message == WM_KEYDOWN &&
            message.wParam == VK_RETURN && !app.closing && !app.clientRunning.load()) {
            ToggleClient();
            continue;
        }
        if ((message.hwnd == window || IsChild(window, message.hwnd)) && IsDialogMessageA(window, &message)) {
            continue;
        }
        TranslateMessage(&message);
        DispatchMessageA(&message);
    }
    app.Stop();
    return result < 0 ? 1 : static_cast<int>(message.wParam);
}
} // namespace

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR arguments, int show) {
    const std::string_view args(arguments);
    const bool retryLater = args == "--tray --defer-update";
    const bool startInTray = args == "--tray" || retryLater;
    const std::unique_ptr<void, decltype(&CloseHandle)> mutex(
        CreateMutexA(nullptr, FALSE, "Local\\PhaholRemote"), &CloseHandle);
    const auto mutexError = GetLastError();
    if (!mutex) return 1;
    if (mutexError == ERROR_ALREADY_EXISTS) {
        if (!startInTray) {
            if (HWND existing = FindWindowA(kMainClass, nullptr)) {
                ShowWindowAsync(existing, SW_RESTORE);
                SetForegroundWindow(existing);
            }
        }
        return 0;
    }
    SetProcessDPIAware();
    WSADATA winsock{};
    if (WSAStartup(MAKEWORD(2, 2), &winsock) != 0) {
        MessageBoxA(nullptr, "Winsock startup failed.", "Phahol Remote", MB_ICONERROR);
        return 1;
    }
    Gdiplus::GdiplusStartupInput gdiplusInput;
    ULONG_PTR gdiplusToken = 0;
    if (Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusInput, nullptr) != Gdiplus::Ok) {
        WSACleanup();
        MessageBoxA(nullptr, "GDI+ startup failed.", "Phahol Remote", MB_ICONERROR);
        return 1;
    }
    const bool bufferedPaintReady = SUCCEEDED(BufferedPaintInit());
    const bool oleReady = SUCCEEDED(OleInitialize(nullptr));
    if (retryLater) app.nextUpdateCheck = Clock::now() + std::chrono::hours(24);
    const int result = RunUi(instance, show, startInTray);
    if (oleReady) OleUninitialize();
    if (bufferedPaintReady) BufferedPaintUnInit();
    Gdiplus::GdiplusShutdown(gdiplusToken);
    WSACleanup();
    return result;
}
