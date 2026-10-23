#include "common.h"

ComPtr<ID2D1Factory> g_d2d;
ComPtr<IDWriteFactory> g_dw;
ComPtr<IWICImagingFactory> g_wic;
ComPtr<ID2D1StrokeStyle> g_round;
ComPtr<ID2D1StrokeStyle> g_dash;

bool GfxInit() {
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, g_d2d.GetAddressOf()))) return false;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   (IUnknown**)g_dw.GetAddressOf())))
        return false;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g_wic))))
        return false;
    auto round = D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                             D2D1_LINE_JOIN_ROUND);
    g_d2d->CreateStrokeStyle(round, nullptr, 0, &g_round);
    auto dash = D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT,
                                            D2D1_LINE_JOIN_MITER, 10.f, D2D1_DASH_STYLE_DASH);
    g_d2d->CreateStrokeStyle(dash, nullptr, 0, &g_dash);
    return true;
}

IDWriteTextFormat* GetFont(float size, DWRITE_FONT_WEIGHT weight, DWRITE_TEXT_ALIGNMENT align) {
    struct Entry { int key; ComPtr<IDWriteTextFormat> fmt; };
    static std::vector<Entry> cache;
    int key = (int)std::lround(size * 4) * 10000 + weight * 10 + (int)align;
    for (auto& e : cache)
        if (e.key == key) return e.fmt.Get();
    ComPtr<IDWriteTextFormat> fmt;
    g_dw->CreateTextFormat(L"Segoe UI", nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                           size, L"ru-ru", &fmt);
    if (!fmt) return nullptr;
    fmt->SetTextAlignment(align);
    fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    if (cache.size() > 64) cache.erase(cache.begin());
    cache.push_back({key, fmt});
    return fmt.Get();
}

D2D1_SIZE_F MeasureText(const std::wstring& text, IDWriteTextFormat* fmt) {
    ComPtr<IDWriteTextLayout> layout;
    if (!fmt || FAILED(g_dw->CreateTextLayout(text.c_str(), (UINT32)text.size(), fmt, 100000.f, 100000.f, &layout)))
        return {0, 0};
    DWRITE_TEXT_METRICS m{};
    layout->GetMetrics(&m);
    return {m.widthIncludingTrailingWhitespace, m.height};
}

// ---------------------------------------------------------------- shapes

D2D1_RECT_F ShapeBounds(const Shape& s) {
    if (s.pts.size() < 2) return {};
    auto a = s.pts[0], b = s.pts[1];
    return {std::min(a.x, b.x), std::min(a.y, b.y), std::max(a.x, b.x), std::max(a.y, b.y)};
}

void ComputePixelate(Shape& s, const CaptureData& cap) {
    s.blockBmp.Reset();
    s.blocks.clear();
    s.cols = s.rows = 0;
    D2D1_RECT_F r = ShapeBounds(s);
    int x0 = std::clamp((int)std::floor(r.left), 0, cap.w), y0 = std::clamp((int)std::floor(r.top), 0, cap.h);
    int x1 = std::clamp((int)std::ceil(r.right), 0, cap.w), y1 = std::clamp((int)std::ceil(r.bottom), 0, cap.h);
    if (x1 - x0 < 1 || y1 - y0 < 1) return;
    int b = std::max(2, (int)std::lround(s.size));
    s.bx = x0;
    s.by = y0;
    s.cols = (x1 - x0 + b - 1) / b;
    s.rows = (y1 - y0 + b - 1) / b;
    s.blocks.resize((size_t)s.cols * s.rows);
    for (int row = 0; row < s.rows; row++) {
        int ya = y0 + row * b, yb = std::min(ya + b, y1);
        for (int col = 0; col < s.cols; col++) {
            int xa = x0 + col * b, xb = std::min(xa + b, x1);
            uint32_t sr = 0, sg = 0, sb = 0, n = 0;
            for (int y = ya; y < yb; y++) {
                const uint32_t* p = cap.px + (size_t)y * cap.w;
                for (int x = xa; x < xb; x++) {
                    uint32_t c = p[x];
                    sr += (c >> 16) & 255;
                    sg += (c >> 8) & 255;
                    sb += c & 255;
                    n++;
                }
            }
            if (!n) n = 1;
            s.blocks[(size_t)row * s.cols + col] = 0xFF000000u | ((sr / n) << 16) | ((sg / n) << 8) | (sb / n);
        }
    }
}

static ComPtr<ID2D1PathGeometry> Polyline(const std::vector<D2D1_POINT_2F>& pts, bool closed) {
    ComPtr<ID2D1PathGeometry> g;
    ComPtr<ID2D1GeometrySink> sink;
    if (pts.empty() || FAILED(g_d2d->CreatePathGeometry(&g)) || FAILED(g->Open(&sink))) return nullptr;
    sink->BeginFigure(pts[0], closed ? D2D1_FIGURE_BEGIN_FILLED : D2D1_FIGURE_BEGIN_HOLLOW);
    if (pts.size() > 1) sink->AddLines(pts.data() + 1, (UINT32)pts.size() - 1);
    sink->EndFigure(closed ? D2D1_FIGURE_END_CLOSED : D2D1_FIGURE_END_OPEN);
    sink->Close();
    return g;
}

static void DrawArrow(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* br, D2D1_POINT_2F a, D2D1_POINT_2F b, float w) {
    float dx = b.x - a.x, dy = b.y - a.y, len = std::sqrt(dx * dx + dy * dy);
    if (len < 1) return;
    float ux = dx / len, uy = dy / len, px = -uy, py = ux;
    float head = std::min(std::max(w * 4.5f, 14.f), len);
    float half = head * 0.5f;
    D2D1_POINT_2F base{b.x - ux * head, b.y - uy * head};
    D2D1_POINT_2F notch{b.x - ux * head * 0.78f, b.y - uy * head * 0.78f};
    if (len > head * 0.8f) rt->DrawLine(a, notch, br, w, g_round.Get());
    std::vector<D2D1_POINT_2F> tri = {b, {base.x + px * half, base.y + py * half}, notch,
                                      {base.x - px * half, base.y - py * half}};
    if (auto g = Polyline(tri, true)) {
        rt->FillGeometry(g.Get(), br);
        rt->DrawGeometry(g.Get(), br, std::max(1.f, w * 0.3f), g_round.Get());
    }
}

static bool IsLight(D2D1_COLOR_F c) { return 0.299f * c.r + 0.587f * c.g + 0.114f * c.b > 0.6f; }

static ComPtr<IDWriteTextLayout> TextLayout(const Shape& s) {
    ComPtr<IDWriteTextLayout> layout;
    std::wstring t = s.text.empty() ? L" " : s.text;
    g_dw->CreateTextLayout(t.c_str(), (UINT32)t.size(), GetFont(s.size), 100000.f, 100000.f, &layout);
    return layout;
}

void TextCaretPos(const Shape& s, D2D1_RECT_F& box, D2D1_POINT_2F& caretTop, float& caretH) {
    auto layout = TextLayout(s);
    D2D1_POINT_2F o = s.pts.empty() ? D2D1_POINT_2F{} : s.pts[0];
    caretTop = o;
    caretH = s.size * 1.3f;
    box = {o.x, o.y, o.x + s.size * 0.5f, o.y + caretH};
    if (!layout) return;
    DWRITE_TEXT_METRICS m{};
    layout->GetMetrics(&m);
    box = {o.x, o.y, o.x + std::max(m.widthIncludingTrailingWhitespace, s.size * 0.5f), o.y + m.height};
    DWRITE_HIT_TEST_METRICS hm{};
    float cx = 0, cy = 0;
    UINT32 pos = (UINT32)s.text.size();
    if (pos == 0) {
        caretTop = o;
    } else {
        layout->HitTestTextPosition(pos - 1, TRUE, &cx, &cy, &hm);
        caretTop = {o.x + cx, o.y + hm.top};
        caretH = hm.height;
        if (s.text.back() == L'\n') caretTop = {o.x, o.y + hm.top + hm.height};
    }
}

void DrawShape(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* br, Shape& s, bool cacheBitmaps) {
    if (s.pts.empty()) return;
    br->SetColor(s.color);
    D2D1_POINT_2F a = s.pts[0], b = s.pts.size() > 1 ? s.pts[1] : s.pts[0];
    switch (s.tool) {
    case Tool::Pen:
    case Tool::Marker:
        if (s.pts.size() == 1) {
            rt->FillEllipse(D2D1::Ellipse(a, s.size / 2, s.size / 2), br);
        } else if (auto g = Polyline(s.pts, false)) {
            rt->DrawGeometry(g.Get(), br, s.size, g_round.Get());
        }
        break;
    case Tool::Line:
        rt->DrawLine(a, b, br, s.size, g_round.Get());
        break;
    case Tool::Arrow:
        DrawArrow(rt, br, a, b, s.size);
        break;
    case Tool::Rect:
        rt->DrawRectangle(ShapeBounds(s), br, s.size, g_round.Get());
        break;
    case Tool::Ellipse: {
        D2D1_RECT_F r = ShapeBounds(s);
        rt->DrawEllipse(D2D1::Ellipse({(r.left + r.right) / 2, (r.top + r.bottom) / 2}, (r.right - r.left) / 2,
                                      (r.bottom - r.top) / 2),
                        br, s.size);
        break;
    }
    case Tool::Text:
        if (!s.text.empty()) {
            if (auto layout = TextLayout(s)) {
                // thin contrasting halo keeps text readable on any background
                br->SetColor(IsLight(s.color) ? D2D1::ColorF(0, 0, 0, 0.55f) : D2D1::ColorF(1, 1, 1, 0.6f));
                float d = std::max(1.f, s.size / 16.f);
                const float offs[4][2] = {{-d, 0}, {d, 0}, {0, -d}, {0, d}};
                for (auto& o : offs) rt->DrawTextLayout({a.x + o[0], a.y + o[1]}, layout.Get(), br);
                br->SetColor(s.color);
                rt->DrawTextLayout(a, layout.Get(), br);
            }
        }
        break;
    case Tool::Blur: {
        if (s.blocks.empty()) break;
        ComPtr<ID2D1Bitmap> bmp = cacheBitmaps ? s.blockBmp : nullptr;
        if (!bmp) {
            auto props = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
            rt->CreateBitmap(D2D1::SizeU(s.cols, s.rows), s.blocks.data(), s.cols * 4, props, &bmp);
            if (cacheBitmaps) s.blockBmp = bmp;
        }
        if (!bmp) break;
        float blk = (float)std::max(2, (int)std::lround(s.size));
        D2D1_RECT_F r = ShapeBounds(s);
        rt->PushAxisAlignedClip(r, D2D1_ANTIALIAS_MODE_ALIASED);
        rt->DrawBitmap(bmp.Get(), D2D1::RectF((float)s.bx, (float)s.by, s.bx + s.cols * blk, s.by + s.rows * blk), 1.f,
                       D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
        rt->PopAxisAlignedClip();
        break;
    }
    case Tool::Number: {
        float r = s.size;
        rt->FillEllipse(D2D1::Ellipse(a, r, r), br);
        br->SetColor(IsLight(s.color) ? D2D1::ColorF(0, 0, 0, 0.35f) : D2D1::ColorF(1, 1, 1, 0.9f));
        rt->DrawEllipse(D2D1::Ellipse(a, r, r), br, std::max(1.f, r / 9));
        br->SetColor(IsLight(s.color) ? D2D1::ColorF(0.1f, 0.1f, 0.1f) : D2D1::ColorF(1, 1, 1));
        std::wstring t = std::to_wstring(s.number);
        float fs = r * (t.size() > 1 ? 1.05f : 1.25f);
        auto* fmt = GetFont(fs, DWRITE_FONT_WEIGHT_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
        if (fmt) {
            fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            rt->DrawText(t.c_str(), (UINT32)t.size(), fmt, D2D1::RectF(a.x - r * 2, a.y - r * 1.06f, a.x + r * 2, a.y + r * 0.94f), br);
        }
        break;
    }
    default:
        break;
    }
}

// ---------------------------------------------------------------- icons (20x20 design grid)

void DrawIcon(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* br, Icon icon, D2D1_RECT_F r, float stroke, float sizeHint) {
    float k = std::min(r.right - r.left, r.bottom - r.top) / 20.f;
    float ox = (r.left + r.right) / 2 - 10 * k, oy = (r.top + r.bottom) / 2 - 10 * k;
    auto P = [&](float x, float y) { return D2D1::Point2F(ox + x * k, oy + y * k); };
    auto L = [&](float x1, float y1, float x2, float y2) { rt->DrawLine(P(x1, y1), P(x2, y2), br, stroke, g_round.Get()); };
    auto Poly = [&](std::initializer_list<D2D1_POINT_2F> pts, bool closed, bool fill) {
        std::vector<D2D1_POINT_2F> v(pts);
        if (auto g = Polyline(v, closed)) {
            if (fill) rt->FillGeometry(g.Get(), br);
            rt->DrawGeometry(g.Get(), br, stroke, g_round.Get());
        }
    };
    switch (icon) {
    case Icon::Move:
        Poly({P(6, 3), P(6, 16.5f), P(9.3f, 13.3f), P(11.6f, 18), P(13.6f, 17.1f), P(11.4f, 12.5f), P(15.8f, 12.5f)}, true, false);
        break;
    case Icon::Pen: {
        ComPtr<ID2D1PathGeometry> g;
        ComPtr<ID2D1GeometrySink> sink;
        if (SUCCEEDED(g_d2d->CreatePathGeometry(&g)) && SUCCEEDED(g->Open(&sink))) {
            sink->BeginFigure(P(3, 15), D2D1_FIGURE_BEGIN_HOLLOW);
            sink->AddBezier(D2D1::BezierSegment(P(5, 5), P(8, 5), P(9.5f, 10)));
            sink->AddBezier(D2D1::BezierSegment(P(11, 15), P(14, 15), P(17, 5)));
            sink->EndFigure(D2D1_FIGURE_END_OPEN);
            sink->Close();
            rt->DrawGeometry(g.Get(), br, stroke, g_round.Get());
        }
        break;
    }
    case Icon::Line: L(4, 16, 16, 4); break;
    case Icon::Arrow:
        L(4, 16, 15, 5);
        Poly({P(16, 4), P(9, 5.5f), P(14.5f, 11)}, true, true);
        break;
    case Icon::Rect: rt->DrawRectangle(D2D1::RectF(ox + 3 * k, oy + 5 * k, ox + 17 * k, oy + 15 * k), br, stroke); break;
    case Icon::Ellipse: rt->DrawEllipse(D2D1::Ellipse(P(10, 10), 7 * k, 5.5f * k), br, stroke); break;
    case Icon::Marker: {
        D2D1_COLOR_F c = br->GetColor();
        br->SetColor(D2D1::ColorF(1.f, 0.85f, 0.1f, 0.75f));
        rt->DrawLine(P(4, 12), P(16, 12), br, 5 * k, g_round.Get());
        br->SetColor(c);
        L(4, 16.5f, 16, 16.5f);
        break;
    }
    case Icon::Text: L(4.5f, 4, 15.5f, 4); L(10, 4, 10, 17); L(7.5f, 17, 12.5f, 17); break;
    case Icon::Blur: {
        D2D1_COLOR_F c = br->GetColor();
        const float alpha[9] = {0.9f, 0.35f, 0.7f, 0.5f, 1.f, 0.25f, 0.8f, 0.45f, 0.65f};
        for (int i = 0; i < 9; i++) {
            float x = 3.5f + (i % 3) * 4.6f, y = 3.5f + (i / 3) * 4.6f;
            br->SetColor(D2D1::ColorF(c.r, c.g, c.b, c.a * alpha[i]));
            rt->FillRectangle(D2D1::RectF(ox + x * k, oy + y * k, ox + (x + 4) * k, oy + (y + 4) * k), br);
        }
        br->SetColor(c);
        break;
    }
    case Icon::Number: {
        rt->DrawEllipse(D2D1::Ellipse(P(10, 10), 7.5f * k, 7.5f * k), br, stroke);
        auto* fmt = GetFont(10 * k, DWRITE_FONT_WEIGHT_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
        if (fmt) {
            fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            rt->DrawText(L"1", 1, fmt, D2D1::RectF(ox, oy - 0.6f * k, ox + 20 * k, oy + 19.4f * k), br);
        }
        break;
    }
    case Icon::Undo:
    case Icon::Redo: {
        bool redo = icon == Icon::Redo;
        auto X = [&](float x) { return redo ? 20 - x : x; };
        ComPtr<ID2D1PathGeometry> g;
        ComPtr<ID2D1GeometrySink> sink;
        if (SUCCEEDED(g_d2d->CreatePathGeometry(&g)) && SUCCEEDED(g->Open(&sink))) {
            sink->BeginFigure(P(X(5), 8), D2D1_FIGURE_BEGIN_HOLLOW);
            sink->AddLine(P(X(12), 8));
            sink->AddBezier(D2D1::BezierSegment(P(X(18), 8), P(X(18), 16), P(X(12), 16)));
            sink->AddLine(P(X(8), 16));
            sink->EndFigure(D2D1_FIGURE_END_OPEN);
            sink->Close();
            rt->DrawGeometry(g.Get(), br, stroke, g_round.Get());
        }
        L(X(5), 8, X(8.5f), 4.5f);
        L(X(5), 8, X(8.5f), 11.5f);
        break;
    }
    case Icon::Save: L(10, 3, 10, 13); L(6, 9, 10, 13); L(14, 9, 10, 13); L(4, 17, 16, 17); break;
    case Icon::Copy:
        rt->DrawRectangle(D2D1::RectF(ox + 7 * k, oy + 7 * k, ox + 17 * k, oy + 17 * k), br, stroke);
        Poly({P(4, 13), P(4, 3.5f), P(13, 3.5f)}, false, false);
        break;
    case Icon::Close: L(5, 5, 15, 15); L(15, 5, 5, 15); break;
    case Icon::Size: {
        float rad = std::clamp(sizeHint, 1.5f, 7.5f) * k;
        rt->FillEllipse(D2D1::Ellipse(P(10, 10), rad, rad), br);
        break;
    }
    case Icon::FlipH:
    case Icon::FlipV: {
        // mirror axis plus a filled shape and its outlined reflection
        bool v = icon == Icon::FlipV;
        auto Q = [&](float a, float b) { return v ? P(b, a) : P(a, b); };
        D2D1_COLOR_F c = br->GetColor();
        br->SetColor(D2D1::ColorF(c.r, c.g, c.b, c.a * 0.55f));
        rt->DrawLine(Q(10, 2), Q(10, 18), br, stroke * 0.8f, g_round.Get());
        br->SetColor(c);
        Poly({Q(7.5f, 5), Q(7.5f, 15), Q(2, 15)}, true, true);
        Poly({Q(12.5f, 5), Q(12.5f, 15), Q(18, 15)}, true, false);
        break;
    }
    }
}

// ---------------------------------------------------------------- tray icon, drawn at runtime

HICON CreateAppIcon(int size) {
    ComPtr<IWICBitmap> wb;
    if (FAILED(g_wic->CreateBitmap(size, size, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &wb))) return nullptr;
    ComPtr<ID2D1RenderTarget> rt;
    auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
                                              D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
    if (FAILED(g_d2d->CreateWicBitmapRenderTarget(wb.Get(), props, &rt))) return nullptr;
    ComPtr<ID2D1SolidColorBrush> br;
    rt->CreateSolidColorBrush(ColorFromRGB(0x2F7BFF), &br);
    float s = (float)size, m = s * 0.06f;
    rt->BeginDraw();
    rt->Clear(D2D1::ColorF(0, 0, 0, 0));
    rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(m, m, s - m, s - m), s * 0.2f, s * 0.2f), br.Get());
    br->SetColor(D2D1::ColorF(1, 1, 1));
    float a = s * 0.24f, b = s * 0.76f, t = s * 0.2f, w = std::max(1.5f, s * 0.09f);
    rt->DrawLine({a, a + t}, {a, a}, br.Get(), w, g_round.Get());
    rt->DrawLine({a, a}, {a + t, a}, br.Get(), w, g_round.Get());
    rt->DrawLine({b, b - t}, {b, b}, br.Get(), w, g_round.Get());
    rt->DrawLine({b, b}, {b - t, b}, br.Get(), w, g_round.Get());
    rt->FillEllipse(D2D1::Ellipse({s / 2, s / 2}, s * 0.08f, s * 0.08f), br.Get());
    rt->EndDraw();

    std::vector<uint32_t> px((size_t)size * size);
    wb->CopyPixels(nullptr, size * 4, (UINT)px.size() * 4, (BYTE*)px.data());
    BITMAPV5HEADER bh{};
    bh.bV5Size = sizeof(bh);
    bh.bV5Width = size;
    bh.bV5Height = -size;
    bh.bV5Planes = 1;
    bh.bV5BitCount = 32;
    bh.bV5Compression = BI_BITFIELDS;
    bh.bV5RedMask = 0x00FF0000;
    bh.bV5GreenMask = 0x0000FF00;
    bh.bV5BlueMask = 0x000000FF;
    bh.bV5AlphaMask = 0xFF000000;
    void* bits = nullptr;
    HDC dc = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(dc, (BITMAPINFO*)&bh, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, dc);
    if (!color) return nullptr;
    memcpy(bits, px.data(), px.size() * 4);
    HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
    ICONINFO ii{TRUE, 0, 0, mask, color};
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(mask);
    DeleteObject(color);
    return icon;
}
