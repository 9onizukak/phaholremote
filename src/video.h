#pragma once

#include "protocol.h"

#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>

#include <cstring>
#include <memory>
#include <cwchar>

namespace remote {

struct StreamRelease {
    void operator()(IStream* stream) const { stream->Release(); }
};
using Stream = std::unique_ptr<IStream, StreamRelease>;

inline Stream MakeStream() {
    IStream* stream = nullptr;
    CreateStreamOnHGlobal(nullptr, TRUE, &stream);
    return Stream(stream);
}

class ScreenCapture {
public:
    ScreenCapture() {
        const int width = GetSystemMetrics(SM_CXSCREEN);
        const int height = GetSystemMetrics(SM_CYSCREEN);
        if (width <= 0 || height <= 0) return;
        const auto scale = std::min({1.0, double(kMaxWidth) / width, double(kMaxHeight) / height});
        width_ = std::max(1, static_cast<int>(width * scale));
        height_ = std::max(1, static_cast<int>(height * scale));

        screen_ = GetDC(nullptr);
        if (!screen_) return;
        memory_ = CreateCompatibleDC(screen_);
        if (!memory_) return;
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width_;
        info.bmiHeader.biHeight = -height_;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        bitmap_ = CreateDIBSection(screen_, &info, DIB_RGB_COLORS, &bits_, nullptr, 0);
        if (!bitmap_) return;
        previous_ = SelectObject(memory_, bitmap_);
        if (!previous_ || previous_ == HGDI_ERROR) return;
        SetStretchBltMode(memory_, HALFTONE);
        SetBrushOrgEx(memory_, 0, 0, nullptr);

        UINT count = 0;
        UINT size = 0;
        if (Gdiplus::GetImageEncodersSize(&count, &size) != Gdiplus::Ok || !size) return;
        auto storage = std::make_unique<uint8_t[]>(size);
        auto* encoders = reinterpret_cast<Gdiplus::ImageCodecInfo*>(storage.get());
        if (Gdiplus::GetImageEncoders(count, size, encoders) != Gdiplus::Ok) return;
        for (UINT i = 0; i < count; ++i) {
            if (std::wcscmp(encoders[i].MimeType, L"image/jpeg") == 0) {
                jpegEncoder_ = encoders[i].Clsid;
                ready_ = true;
                break;
            }
        }
    }

    ~ScreenCapture() {
        if (previous_ && previous_ != HGDI_ERROR) SelectObject(memory_, previous_);
        if (bitmap_) DeleteObject(bitmap_);
        if (memory_) DeleteDC(memory_);
        if (screen_) ReleaseDC(nullptr, screen_);
    }

    ScreenCapture(const ScreenCapture&) = delete;
    ScreenCapture& operator=(const ScreenCapture&) = delete;

    bool Ready() const { return ready_; }

    bool Encode(int quality, std::vector<uint8_t>& jpeg) const {
        const int width = GetSystemMetrics(SM_CXSCREEN);
        const int height = GetSystemMetrics(SM_CYSCREEN);
        const bool captured = width == width_ && height == height_
            ? BitBlt(memory_, 0, 0, width_, height_, screen_, 0, 0, SRCCOPY | CAPTUREBLT)
            : StretchBlt(memory_, 0, 0, width_, height_, screen_, 0, 0, width, height, SRCCOPY | CAPTUREBLT);
        if (!captured) return false;
        // The viewer's native cursor stays responsive independently of video frames.
        GdiFlush();
        Gdiplus::Bitmap image(width_, height_, width_ * 4, PixelFormat32bppRGB,
                              static_cast<BYTE*>(bits_));
        auto stream = MakeStream();
        if (!stream || image.GetLastStatus() != Gdiplus::Ok) return false;

        ULONG encoderQuality = static_cast<ULONG>(quality);
        Gdiplus::EncoderParameters parameters{};
        parameters.Count = 1;
        parameters.Parameter[0] = {Gdiplus::EncoderQuality, 1, Gdiplus::EncoderParameterValueTypeLong,
                                   &encoderQuality};
        if (image.Save(stream.get(), &jpegEncoder_, &parameters) != Gdiplus::Ok) return false;

        STATSTG info{};
        HGLOBAL memory = nullptr;
        if (FAILED(stream->Stat(&info, STATFLAG_NONAME)) || info.cbSize.QuadPart == 0 ||
            info.cbSize.QuadPart > kMaxJpegBytes ||
            FAILED(GetHGlobalFromStream(stream.get(), &memory))) return false;
        jpeg.resize(static_cast<size_t>(info.cbSize.QuadPart));
        const void* bytes = GlobalLock(memory);
        if (!bytes) return false;
        std::memcpy(jpeg.data(), bytes, jpeg.size());
        GlobalUnlock(memory);
        return true;
    }

private:
    HDC screen_ = nullptr;
    HDC memory_ = nullptr;
    HBITMAP bitmap_ = nullptr;
    void* bits_ = nullptr;
    HGDIOBJ previous_ = nullptr;
    CLSID jpegEncoder_{};
    int width_ = 0;
    int height_ = 0;
    bool ready_ = false;
};

struct Pixels {
    uint32_t width;
    uint32_t height;
    std::vector<uint8_t> bgra;
};

inline std::shared_ptr<Pixels> Decode(const std::vector<uint8_t>& jpeg) {
    auto stream = MakeStream();
    ULONG written = 0;
    LARGE_INTEGER zero{};
    if (!stream || FAILED(stream->Write(jpeg.data(), static_cast<ULONG>(jpeg.size()), &written)) ||
        written != jpeg.size() || FAILED(stream->Seek(zero, STREAM_SEEK_SET, nullptr))) return {};

    Gdiplus::Bitmap image(stream.get(), FALSE);
    if (image.GetLastStatus() != Gdiplus::Ok) return {};
    GUID format{};
    if (image.GetRawFormat(&format) != Gdiplus::Ok || !IsEqualGUID(format, Gdiplus::ImageFormatJPEG)) return {};
    const auto width = image.GetWidth();
    const auto height = image.GetHeight();
    if (!width || width > kMaxWidth || !height || height > kMaxHeight) return {};

    auto frame = std::make_shared<Pixels>();
    frame->width = width;
    frame->height = height;
    const size_t rowBytes = size_t(width) * 4;
    frame->bgra.resize(rowBytes * height);
    Gdiplus::Rect rectangle(0, 0, static_cast<INT>(width), static_cast<INT>(height));
    Gdiplus::BitmapData bitmap{};
    if (image.LockBits(&rectangle, Gdiplus::ImageLockModeRead, PixelFormat32bppRGB, &bitmap) != Gdiplus::Ok) {
        return {};
    }
    const auto* row = static_cast<const uint8_t*>(bitmap.Scan0);
    for (uint32_t y = 0; y < height; ++y) {
        std::memcpy(frame->bgra.data() + y * rowBytes, row + ptrdiff_t(y) * bitmap.Stride, rowBytes);
    }
    image.UnlockBits(&bitmap);
    return frame;
}

} // namespace remote
