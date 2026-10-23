// Overlay drawing: frozen screenshot, dimming, selection, annotations, toolbar, loupe.
#include "overlay.h"

Overlay g_ov;

const uint32_t kPalette[kColors] = {0xFF3B30, 0xFF9500, 0xFFD60A, 0x34C759, 0x0A84FF, 0xBF5AF2, 0x111111, 0xFFFFFF};
static const D2D1_COLOR_F kAccent = ColorFromRGB(0x2F8CFF);
static std::vector<float> s_separators;

static const wchar_t* kToolTips[] = {
    L"Выделение / перемещение (V)", L"Карандаш (P)", L"Линия (L)", L"Стрелка (A)", L"Прямоугольник (R)",
    L"Эллипс (E)", L"Маркер (M)", L"Текст (T)", L"Размытие (B)", L"Нумерация (N)"};

D2D1_POINT_2F ToImage(D2D1_POINT_2F p) { return {(p.x - g_ov.ox) / g_ov.zoom, (p.y - g_ov.oy) / g_ov.zoom}; }
D2D1_POINT_2F ToScreen(D2D1_POINT_2F p) { return {p.x * g_ov.zoom + g_ov.ox, p.y * g_ov.zoom + g_ov.oy}; }
D2D1_RECT_F ToScreen(const RECT& r) {
    auto a = ToScreen(D2D1::Point2F((float)r.left, (float)r.top));
    auto b = ToScreen(D2D1::Point2F((float)r.right, (float)r.bottom));
    return {a.x, a.y, b.x, b.y};
}

static POINT Pt(D2D1_POINT_2F p) { return {(LONG)std::floor(p.x), (LONG)std::floor(p.y)}; }

float ScaleAtScreen(D2D1_POINT_2F p) {
    auto& c = g_ov.cap;
    if (c.monitors.empty()) return 1.f;
    return c.monitorScale[c.MonitorAt(Pt(p))];
}

float ToolSize(Tool t, int i, float s) {
    static const float stroke[kSizes] = {2, 3, 5, 8, 12};
    static const float marker[kSizes] = {10, 14, 20, 28, 40};
    static const float text[kSizes] = {14, 18, 24, 32, 44};
    static const float number[kSizes] = {9, 11, 14, 18, 24};
    static const float blur[kSizes] = {6, 9, 12, 16, 22};
    i = std::clamp(i, 0, kSizes - 1);
    switch (t) {
    case Tool::Marker: return marker[i] * s;
    case Tool::Text: return text[i] * s;
    case Tool::Number: return number[i] * s;
    case Tool::Blur: return blur[i] * s;
    default: return stroke[i] * s;
    }
}

void ClampView() {
    auto& o = g_ov;
    if (o.zoom <= 1.0001f) {
        o.zoom = 1;
        o.ox = o.oy = 0;
        return;
    }
    o.ox = std::clamp(o.ox, o.cap.w - o.cap.w * o.zoom, 0.f);
    o.oy = std::clamp(o.oy, o.cap.h - o.cap.h * o.zoom, 0.f);
}

void UpdateHover() {
    auto& o = g_ov;
    o.hasHover = false;
    if (o.editing || o.cap.monitors.empty()) return;
    POINT p = Pt(ToImage(o.mouse));
    for (auto& r : o.cap.windows)
        if (PtInRect(&r, p)) {
            o.hover = r;
            o.hasHover = true;
            return;
        }
    o.hover = o.cap.monitors[o.cap.MonitorAt(p)];
    o.hasHover = true;
}

void LayoutToolbar() {
    auto& o = g_ov;
    o.buttons.clear();
    s_separators.clear();
    if (!o.editing || !o.hasSel || o.cap.monitors.empty()) return;
    D2D1_RECT_F hr = ToScreen(o.sel);
    D2D1_POINT_2F anchor{std::clamp((hr.left + hr.right) / 2, 0.f, o.cap.w - 1.f), std::clamp(hr.bottom, 0.f, o.cap.h - 1.f)};
    int mi = o.cap.MonitorAt(Pt(anchor));
    RECT mon = o.cap.monitors[mi];
    float s = o.cap.monitorScale[mi];
    float bs = 34 * s, pad = 5 * s, sep = 11 * s, sw = 25 * s;

    struct Item { Btn kind; int arg; float w; std::wstring tip; };
    std::vector<Item> items;
    for (int t = 0; t < (int)Tool::COUNT; t++) items.push_back({Btn::Tool, t, bs, kToolTips[t]});
    items.push_back({Btn::Tool, -1, sep, L""});
    for (int c = 0; c < kColors; c++) items.push_back({Btn::Color, c, sw, L"Цвет (" + std::to_wstring(c + 1) + L")"});
    items.push_back({Btn::Size, 0, bs, L"Толщина / размер  ( [  ]  или Ctrl+колесо )"});
    items.push_back({Btn::Tool, -1, sep, L""});
    items.push_back({Btn::Flip, 0, bs, L"Отразить слева направо (H)"});
    items.push_back({Btn::Flip, 1, bs, L"Отразить сверху вниз (Shift+V или Shift+H)"});
    items.push_back({Btn::Tool, -1, sep, L""});
    items.push_back({Btn::Undo, 0, bs, L"Отменить (Ctrl+Z)"});
    items.push_back({Btn::Redo, 0, bs, L"Повторить (Ctrl+Y)"});
    items.push_back({Btn::Tool, -1, sep, L""});
    items.push_back({Btn::Save, 0, bs, L"Сохранить в папку (Ctrl+S),  Ctrl+Shift+S — сохранить как"});
    items.push_back({Btn::Copy, 0, bs, L"Копировать (Enter / Ctrl+C)"});
    items.push_back({Btn::Close, 0, bs, L"Закрыть (Esc)"});

    float total = pad * 2;
    for (auto& it : items) total += it.w;
    float height = bs + pad * 2, margin = 8 * s;
    float x = (hr.left + hr.right) / 2 - total / 2;
    float y = hr.bottom + margin;
    o.toolbarAbove = false;
    if (y + height > mon.bottom - 4 * s) {
        y = hr.top - margin - height;
        o.toolbarAbove = true;
        if (y < mon.top + 4 * s) {
            y = std::min(hr.bottom, (float)mon.bottom) - height - margin;
            o.toolbarAbove = false;
        }
    }
    x = std::clamp(x, mon.left + 4 * s, std::max(mon.left + 4 * s, mon.right - total - 4 * s));
    o.toolbar = {x, y, x + total, y + height};

    float cx = x + pad;
    for (auto& it : items) {
        if (it.arg == -1 && it.kind == Btn::Tool) {
            s_separators.push_back(cx + it.w / 2);
        } else {
            o.buttons.push_back({it.kind, it.arg, {cx, y + pad, cx + it.w, y + pad + bs}, it.tip});
        }
        cx += it.w;
    }
}

// pill-shaped label; returns its rect. anchor: 0 = left/top at (x,y), 1 = centered horizontally on x
static D2D1_RECT_F Pill(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* br, const std::wstring& t, float x, float y,
                        float s, int anchor, bool draw = true, float alpha = 0.82f) {
    auto* fmt = GetFont(12.5f * s);
    D2D1_SIZE_F sz = MeasureText(t, fmt);
    float px = 8 * s, py = 3.5f * s, w = sz.width + px * 2, h = sz.height + py * 2;
    float left = anchor == 1 ? x - w / 2 : x;
    D2D1_RECT_F r{left, y, left + w, y + h};
    if (draw && fmt) {
        br->SetColor(D2D1::ColorF(0.07f, 0.07f, 0.08f, alpha));
        rt->FillRoundedRectangle(D2D1::RoundedRect(r, 6 * s, 6 * s), br);
        br->SetColor(D2D1::ColorF(1, 1, 1, 0.95f));
        rt->DrawText(t.c_str(), (UINT32)t.size(), fmt, D2D1::RectF(left + px, y + py, r.right, r.bottom), br);
    }
    return r;
}

static std::wstring Hex(uint32_t c) {
    wchar_t b[16];
    swprintf_s(b, L"#%02X%02X%02X", (c >> 16) & 255, (c >> 8) & 255, c & 255);
    return b;
}

static void DrawLoupe(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* br, ID2D1Bitmap* shot, const std::wstring& line1) {
    auto& o = g_ov;
    auto& c = o.cap;
    D2D1_POINT_2F m = o.mouse;
    int mi = c.MonitorAt(Pt(m));
    RECT mon = c.monitors[mi];
    float s = c.monitorScale[mi];
    POINT ip = Pt(ToImage(m));
    const int N = 15;
    float cell = std::round(8 * s), L = N * cell;
    float lineH = 17 * s, infoH = lineH * 2 + 8 * s, gap = 24 * s;
    float x = m.x + gap, y = m.y + gap;
    if (x + L > mon.right) x = m.x - gap - L;
    if (y + L + infoH + 4 * s > mon.bottom) y = m.y - gap - L - infoH - 4 * s;
    D2D1_RECT_F box{x, y, x + L, y + L};

    br->SetColor(D2D1::ColorF(0, 0, 0, 0.45f));
    rt->DrawRectangle(D2D1::RectF(box.left - 2 * s, box.top - 2 * s, box.right + 2 * s, box.bottom + 2 * s), br, 1);
    br->SetColor(D2D1::ColorF(0.1f, 0.1f, 0.1f));
    rt->FillRectangle(box, br);
    int sx0 = ip.x - N / 2, sy0 = ip.y - N / 2;
    int cx0 = std::max(sx0, 0), cy0 = std::max(sy0, 0), cx1 = std::min(sx0 + N, c.w), cy1 = std::min(sy0 + N, c.h);
    if (cx1 > cx0 && cy1 > cy0)
        rt->DrawBitmap(shot, D2D1::RectF(x + (cx0 - sx0) * cell, y + (cy0 - sy0) * cell, x + (cx1 - sx0) * cell, y + (cy1 - sy0) * cell),
                       1.f, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR,
                       D2D1::RectF((float)cx0, (float)cy0, (float)cx1, (float)cy1));
    br->SetColor(D2D1::ColorF(1, 1, 1, 0.08f));
    for (int i = 1; i < N; i++) {
        rt->DrawLine({x + i * cell, y}, {x + i * cell, y + L}, br, 1);
        rt->DrawLine({x, y + i * cell}, {x + L, y + i * cell}, br, 1);
    }
    float c0 = (N / 2) * cell;
    br->SetColor(D2D1::ColorF(kAccent.r, kAccent.g, kAccent.b, 0.22f));
    rt->FillRectangle(D2D1::RectF(x, y + c0, x + c0, y + c0 + cell), br);
    rt->FillRectangle(D2D1::RectF(x + c0 + cell, y + c0, x + L, y + c0 + cell), br);
    rt->FillRectangle(D2D1::RectF(x + c0, y, x + c0 + cell, y + c0), br);
    rt->FillRectangle(D2D1::RectF(x + c0, y + c0 + cell, x + c0 + cell, y + L), br);
    D2D1_RECT_F cc{x + c0, y + c0, x + c0 + cell, y + c0 + cell};
    br->SetColor(D2D1::ColorF(0, 0, 0));
    rt->DrawRectangle(cc, br, 1.5f);
    br->SetColor(D2D1::ColorF(1, 1, 1));
    rt->DrawRectangle(D2D1::RectF(cc.left + 1.5f, cc.top + 1.5f, cc.right - 1.5f, cc.bottom - 1.5f), br, 1);
    rt->DrawRectangle(box, br, 2 * s);

    auto* fmt = GetFont(12 * s);
    if (!fmt) return;
    uint32_t px = c.Pixel(ip.x, ip.y);
    std::wstring hex = Hex(px) + L"   C — копировать";
    float textW = std::max(MeasureText(line1, fmt).width, 17 * s + MeasureText(hex, fmt).width);
    float ibW = std::max(L, textW + 16 * s);
    float ibX = x + L + gap < m.x ? x + L - ibW : x;  // grow away from the cursor
    D2D1_RECT_F ib{ibX, y + L + 4 * s, ibX + ibW, y + L + 4 * s + infoH};
    br->SetColor(D2D1::ColorF(0.07f, 0.07f, 0.08f, 0.88f));
    rt->FillRoundedRectangle(D2D1::RoundedRect(ib, 6 * s, 6 * s), br);
    br->SetColor(D2D1::ColorF(1, 1, 1, 0.95f));
    rt->DrawText(line1.c_str(), (UINT32)line1.size(), fmt, D2D1::RectF(ib.left + 8 * s, ib.top + 4 * s, ib.right, ib.top + 4 * s + lineH), br);
    D2D1_RECT_F sw{ib.left + 8 * s, ib.top + 4 * s + lineH + 3 * s, ib.left + 8 * s + 11 * s, ib.top + 4 * s + lineH + 14 * s};
    br->SetColor(ColorFromRGB(px));
    rt->FillRectangle(sw, br);
    br->SetColor(D2D1::ColorF(1, 1, 1, 0.6f));
    rt->DrawRectangle(sw, br, 1);
    br->SetColor(D2D1::ColorF(1, 1, 1, 0.95f));
    rt->DrawText(hex.c_str(), (UINT32)hex.size(), fmt,
                 D2D1::RectF(sw.right + 6 * s, ib.top + 4 * s + lineH, ib.right, ib.bottom), br);
}

static void DrawToolbar(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* br) {
    auto& o = g_ov;
    if (o.buttons.empty()) return;
    float s = (o.toolbar.bottom - o.toolbar.top) / 44.f;
    br->SetColor(D2D1::ColorF(0.11f, 0.11f, 0.12f, 0.96f));
    rt->FillRoundedRectangle(D2D1::RoundedRect(o.toolbar, 9 * s, 9 * s), br);
    br->SetColor(D2D1::ColorF(1, 1, 1, 0.12f));
    rt->DrawRoundedRectangle(D2D1::RoundedRect(o.toolbar, 9 * s, 9 * s), br, 1);
    for (float sx : s_separators) rt->DrawLine({sx, o.toolbar.top + 10 * s}, {sx, o.toolbar.bottom - 10 * s}, br, 1);

    for (size_t i = 0; i < o.buttons.size(); i++) {
        auto& b = o.buttons[i];
        bool hovered = (int)i == o.hoverBtn;
        D2D1_RECT_F r = b.r;
        D2D1_POINT_2F mid{(r.left + r.right) / 2, (r.top + r.bottom) / 2};
        D2D1_RECT_F ir{r.left + 7 * s, r.top + 7 * s, r.right - 7 * s, r.bottom - 7 * s};
        auto rr = D2D1::RoundedRect(D2D1::RectF(r.left + 1 * s, r.top, r.right - 1 * s, r.bottom), 6 * s, 6 * s);
        if (b.kind == Btn::Color) {
            D2D1_COLOR_F col = ColorFromRGB(kPalette[b.arg]);
            if (b.arg == o.colorIdx || hovered) {
                br->SetColor(D2D1::ColorF(1, 1, 1, b.arg == o.colorIdx ? 1.f : 0.4f));
                rt->DrawEllipse(D2D1::Ellipse(mid, 10.5f * s, 10.5f * s), br, 2 * s);
            }
            br->SetColor(col);
            rt->FillEllipse(D2D1::Ellipse(mid, 7.5f * s, 7.5f * s), br);
            if (b.arg >= 6) {
                br->SetColor(D2D1::ColorF(0.5f, 0.5f, 0.5f, 0.8f));
                rt->DrawEllipse(D2D1::Ellipse(mid, 7.5f * s, 7.5f * s), br, 1);
            }
            continue;
        }
        bool active = b.kind == Btn::Tool && b.arg == (int)o.tool;
        if (active) {
            br->SetColor(kAccent);
            rt->FillRoundedRectangle(rr, br);
        } else if (hovered) {
            br->SetColor(D2D1::ColorF(1, 1, 1, 0.13f));
            rt->FillRoundedRectangle(rr, br);
        }
        float alpha = 0.95f;
        if ((b.kind == Btn::Undo && o.shapes.empty()) || (b.kind == Btn::Redo && o.redo.empty())) alpha = 0.3f;
        br->SetColor(D2D1::ColorF(1, 1, 1, alpha));
        Icon icon = Icon::Close;
        switch (b.kind) {
        case Btn::Tool: icon = (Icon)b.arg; break;
        case Btn::Size: icon = Icon::Size; break;
        case Btn::Flip: icon = b.arg ? Icon::FlipV : Icon::FlipH; break;
        case Btn::Undo: icon = Icon::Undo; break;
        case Btn::Redo: icon = Icon::Redo; break;
        case Btn::Save: icon = Icon::Save; break;
        case Btn::Copy: icon = Icon::Copy; break;
        default: break;
        }
        DrawIcon(rt, br, icon, ir, 1.6f * s, 2.f + o.sizeIdx * 1.35f);
    }
    if (o.hoverBtn >= 0 && o.hoverBtn < (int)o.buttons.size()) {
        auto& b = o.buttons[o.hoverBtn];
        float cx = (b.r.left + b.r.right) / 2;
        D2D1_RECT_F pr = Pill(rt, br, b.tip, cx, 0, s, 1, false);
        float h = pr.bottom - pr.top;
        float y = o.toolbarAbove ? o.toolbar.top - h - 6 * s : o.toolbar.bottom + 6 * s;
        if (!o.toolbarAbove && y + h > o.cap.h) y = o.toolbar.top - h - 6 * s;
        Pill(rt, br, b.tip, cx, y, s, 1, true, 0.92f);
    }
}

void OverlayRender(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* br, ID2D1Bitmap* shot) {
    auto& o = g_ov;
    float W = (float)o.cap.w, H = (float)o.cap.h;
    rt->SetTransform(D2D1::Matrix3x2F::Identity());
    rt->Clear(D2D1::ColorF(0, 0, 0));
    rt->SetTransform(D2D1::Matrix3x2F::Scale(o.zoom, o.zoom) * D2D1::Matrix3x2F::Translation(o.ox, o.oy));
    rt->DrawBitmap(shot, D2D1::RectF(0, 0, W, H), 1.f,
                   o.zoom >= 2 ? D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR : D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    if (o.hasSel) {
        rt->PushAxisAlignedClip(D2D1::RectF((float)o.sel.left, (float)o.sel.top, (float)o.sel.right, (float)o.sel.bottom),
                                D2D1_ANTIALIAS_MODE_ALIASED);
        for (auto& sh : o.shapes) DrawShape(rt, br, sh, true);
        if (o.drawing) DrawShape(rt, br, o.cur, false);
        if (o.typing) {
            DrawShape(rt, br, o.text, false);
            D2D1_RECT_F box;
            D2D1_POINT_2F ct;
            float ch;
            TextCaretPos(o.text, box, ct, ch);
            float px = 1.f / o.zoom, m = 4 * px;
            br->SetColor(D2D1::ColorF(1, 1, 1, 0.8f));
            rt->DrawRectangle(D2D1::RectF(box.left - m, box.top - m, box.right + m, box.bottom + m), br, px, g_dash.Get());
            br->SetColor(o.text.color);
            rt->DrawLine(ct, {ct.x, ct.y + ch}, br, std::max(2 * px, o.text.size / 14));
        }
        rt->PopAxisAlignedClip();
    }
    rt->SetTransform(D2D1::Matrix3x2F::Identity());

    // dim everything except the selection (or the hovered window)
    bool hole = o.hasSel || o.hasHover;
    D2D1_RECT_F hr = o.hasSel ? ToScreen(o.sel) : (o.hasHover ? ToScreen(o.hover) : D2D1_RECT_F{});
    br->SetColor(D2D1::ColorF(0, 0, 0, 0.45f));
    if (hole) {
        rt->FillRectangle(D2D1::RectF(0, 0, W, hr.top), br);
        rt->FillRectangle(D2D1::RectF(0, hr.bottom, W, H), br);
        rt->FillRectangle(D2D1::RectF(0, hr.top, hr.left, hr.bottom), br);
        rt->FillRectangle(D2D1::RectF(hr.right, hr.top, W, hr.bottom), br);
    } else {
        rt->FillRectangle(D2D1::RectF(0, 0, W, H), br);
    }

    float ms = ScaleAtScreen(o.mouse);
    if (hole) {
        float s = ScaleAtScreen({(hr.left + hr.right) / 2, (hr.top + hr.bottom) / 2});
        br->SetColor(kAccent);
        rt->DrawRectangle(D2D1::RectF(hr.left - 1, hr.top - 1, hr.right + 1, hr.bottom + 1), br, o.hasSel ? 2.f : 3.f);
        if (o.editing) {
            float hs = 4 * s;
            float xs[3] = {hr.left, (hr.left + hr.right) / 2, hr.right}, ys[3] = {hr.top, (hr.top + hr.bottom) / 2, hr.bottom};
            for (int i = 0; i < 3; i++)
                for (int j = 0; j < 3; j++) {
                    if (i == 1 && j == 1) continue;
                    D2D1_RECT_F h{xs[i] - hs, ys[j] - hs, xs[i] + hs, ys[j] + hs};
                    br->SetColor(D2D1::ColorF(1, 1, 1));
                    rt->FillRectangle(h, br);
                    br->SetColor(kAccent);
                    rt->DrawRectangle(h, br, 1.5f);
                }
        }
        RECT r = o.hasSel ? o.sel : o.hover;
        std::wstring label = std::to_wstring(r.right - r.left) + L" × " + std::to_wstring(r.bottom - r.top);
        if (o.zoom > 1.001f) {
            wchar_t z[32];
            swprintf_s(z, L"   %.0f%%", o.zoom * 100);
            label += z;
        }
        D2D1_RECT_F pr = Pill(rt, br, label, 0, 0, s, 0, false);
        float lh = pr.bottom - pr.top;
        float lx = std::max(hr.left, 0.f), ly = hr.top - lh - 6 * s;
        RECT mon = o.cap.monitors[o.cap.MonitorAt(Pt({lx, std::max(hr.top, 0.f)}))];
        if (ly < mon.top) ly = std::max(hr.top, 0.f) + 6 * s, lx += 6 * s;
        Pill(rt, br, label, lx, ly, s, 0);
    }

    if (!o.editing && o.drag != Drag::Pan) {
        // crosshair
        D2D1_POINT_2F ip = ToImage(o.mouse);
        auto cp = ToScreen(D2D1::Point2F(std::floor(ip.x) + 0.5f, std::floor(ip.y) + 0.5f));
        br->SetColor(D2D1::ColorF(kAccent.r, kAccent.g, kAccent.b, 0.55f));
        rt->DrawLine({cp.x, 0}, {cp.x, H}, br, 1);
        rt->DrawLine({0, cp.y}, {W, cp.y}, br, 1);
        if (o.drag == Drag::None) {
            int mi = o.cap.MonitorAt(Pt(o.mouse));
            RECT wa = o.cap.workAreas[mi];
            std::wstring hint = L"Выделите область   ·   клик — окно   ·   F — весь экран   ·   Ctrl+A — все экраны   ·   "
                                L"колесо — зум   ·   Esc — выход";
            D2D1_RECT_F pr = Pill(rt, br, hint, (wa.left + wa.right) / 2.f, wa.top + 14 * ms, ms, 1, false);
            D2D1_RECT_F zone{pr.left - 60 * ms, pr.top - 60 * ms, pr.right + 60 * ms, pr.bottom + 60 * ms};
            if (!(o.mouse.x > zone.left && o.mouse.x < zone.right && o.mouse.y > zone.top && o.mouse.y < zone.bottom))
                Pill(rt, br, hint, (wa.left + wa.right) / 2.f, wa.top + 14 * ms, ms, 1, true, 0.75f);
        }
        POINT p = Pt(ip);
        std::wstring line1 = o.drag == Drag::NewSel && o.hasSel
                                 ? std::to_wstring(o.sel.right - o.sel.left) + L" × " + std::to_wstring(o.sel.bottom - o.sel.top)
                                 : L"X " + std::to_wstring(p.x) + L"   Y " + std::to_wstring(p.y);
        DrawLoupe(rt, br, shot, line1);
    } else if (o.editing) {
        LayoutToolbar();
        if (o.drag == Drag::Resize) {
            POINT p = Pt(ToImage(o.mouse));
            DrawLoupe(rt, br, shot, L"X " + std::to_wstring(p.x) + L"   Y " + std::to_wstring(p.y));
        }
        if (o.drag != Drag::Draw && o.drag != Drag::MoveSel && o.drag != Drag::Resize) DrawToolbar(rt, br);
    }
}
