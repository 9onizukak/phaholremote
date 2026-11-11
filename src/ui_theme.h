#pragma once

namespace theme {

inline constexpr COLORREF canvas = RGB(241, 246, 249);
inline constexpr COLORREF surface = RGB(255, 255, 255);
inline constexpr COLORREF navy = RGB(16, 39, 62);
inline constexpr COLORREF text = RGB(22, 46, 67);
inline constexpr COLORREF muted = RGB(68, 91, 111);
inline constexpr COLORREF border = RGB(209, 222, 230);
inline constexpr COLORREF teal = RGB(0, 109, 106);
inline constexpr COLORREF tealPressed = RGB(0, 85, 83);
inline constexpr COLORREF mint = RGB(225, 245, 241);

inline int Px(int logical) {
    static const int dpi = [] {
        const HDC screen = GetDC(nullptr);
        if (!screen) return 96;
        const int result = GetDeviceCaps(screen, LOGPIXELSX);
        ReleaseDC(nullptr, screen);
        return result > 0 ? result : 96;
    }();
    return MulDiv(logical, dpi, 96);
}

struct Fonts {
    HFONT body = Create(15, FW_NORMAL);
    HFONT label = Create(15, FW_SEMIBOLD);
    HFONT heading = Create(22, FW_SEMIBOLD);
    HFONT title = Create(32, FW_SEMIBOLD);

    ~Fonts() {
        for (const auto font : {body, label, heading, title}) if (font) DeleteObject(font);
    }

    static HFONT Create(int height, int weight) {
        return CreateFontW(-Px(height), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                           OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                           DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    }
};

inline const Fonts& Type() {
    static const Fonts fonts;
    return fonts;
}

inline RECT Box(int left, int top, int right, int bottom) {
    return {Px(left), Px(top), Px(right), Px(bottom)};
}

inline void Fill(HDC dc, const RECT& rect, COLORREF color) {
    SetDCBrushColor(dc, color);
    FillRect(dc, &rect, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
}

inline void Rounded(HDC dc, const RECT& rect, COLORREF color, COLORREF outline, int radius) {
    const auto oldBrush = SelectObject(dc, GetStockObject(DC_BRUSH));
    const auto oldPen = SelectObject(dc, GetStockObject(DC_PEN));
    SetDCBrushColor(dc, color);
    SetDCPenColor(dc, outline);
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, Px(radius * 2), Px(radius * 2));
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
}

inline void Text(HDC dc, const char* label, RECT rect, HFONT font, COLORREF color, UINT format) {
    const auto old = font ? SelectObject(dc, font) : nullptr;
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    DrawTextA(dc, label, -1, &rect, format | DT_NOPREFIX);
    if (old) SelectObject(dc, old);
}

inline bool Button(const DRAWITEMSTRUCT& item, bool primary, COLORREF background) {
    if (item.CtlType != ODT_BUTTON) return false;
    const bool disabled = (item.itemState & ODS_DISABLED) != 0;
    const bool pressed = (item.itemState & ODS_SELECTED) != 0;
    const bool focused = (item.itemState & ODS_FOCUS) != 0;
    const COLORREF face = disabled ? canvas : primary ? (pressed ? tealPressed : teal) :
        (pressed ? mint : surface);
    const COLORREF ink = primary && !disabled ? surface : disabled ? muted : text;
    Fill(item.hDC, item.rcItem, background);
    Rounded(item.hDC, item.rcItem, face, primary && !disabled ? face : border, 8);
    char caption[80]{};
    GetWindowTextA(item.hwndItem, caption, sizeof(caption));
    Text(item.hDC, caption, item.rcItem, Type().label, ink,
         DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (focused && !(item.itemState & ODS_NOFOCUSRECT)) {
        RECT focus = item.rcItem;
        InflateRect(&focus, -Px(4), -Px(4));
        DrawFocusRect(item.hDC, &focus);
    }
    return true;
}

} // namespace theme
