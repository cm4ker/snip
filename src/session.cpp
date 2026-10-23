// Overlay lifecycle: capture -> show -> finish (copy / save), plus self-test and demo.
#include "overlay.h"

static const wchar_t* kRegKey = L"Software\\Snip";

void LoadSettings(int& tool, int& color, int& size) {
    auto get = [](const wchar_t* name, int def) {
        DWORD v = 0, cb = sizeof(v);
        if (RegGetValueW(HKEY_CURRENT_USER, kRegKey, name, RRF_RT_REG_DWORD, nullptr, &v, &cb) == ERROR_SUCCESS) return (int)v;
        return def;
    };
    tool = std::clamp(get(L"Tool", (int)Tool::Arrow), 0, (int)Tool::COUNT - 1);
    color = std::clamp(get(L"Color", 0), 0, kColors - 1);
    size = std::clamp(get(L"Size", 1), 0, kSizes - 1);
}

void SaveSettings(int tool, int color, int size) {
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) != ERROR_SUCCESS) return;
    DWORD v[3] = {(DWORD)tool, (DWORD)color, (DWORD)size};
    RegSetValueExW(k, L"Tool", 0, REG_DWORD, (BYTE*)&v[0], 4);
    RegSetValueExW(k, L"Color", 0, REG_DWORD, (BYTE*)&v[1], 4);
    RegSetValueExW(k, L"Size", 0, REG_DWORD, (BYTE*)&v[2], 4);
    RegCloseKey(k);
}

bool OverlayInit(HINSTANCE inst) {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = OverlayProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_CROSS);
    wc.lpszClassName = OVERLAY_CLASS;
    RegisterClassExW(&wc);
    g_ov.hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, OVERLAY_CLASS, APP_NAME, WS_POPUP, 0, 0, 1, 1, nullptr,
                                nullptr, inst, nullptr);
    int tool, color, size;
    LoadSettings(tool, color, size);
    g_ov.tool = (Tool)tool;
    g_ov.colorIdx = color;
    g_ov.sizeIdx = size;
    return g_ov.hwnd != nullptr;
}

bool EnsureTarget() {
    auto& o = g_ov;
    D2D1_SIZE_U size = D2D1::SizeU(o.cap.w, o.cap.h);
    if (o.rt) {
        D2D1_SIZE_U cur = o.rt->GetPixelSize();
        if (cur.width != size.width || cur.height != size.height) {
            if (FAILED(o.rt->Resize(size))) o.rt.Reset();
        }
    }
    if (!o.rt) {
        o.br.Reset();
        auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
                                                  D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96, 96);
        if (FAILED(g_d2d->CreateHwndRenderTarget(props, D2D1::HwndRenderTargetProperties(o.hwnd, size), &o.rt))) return false;
        o.rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    }
    if (!o.br) o.rt->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1), &o.br);
    if (!o.shot && o.cap.px) {
        auto bp = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
        if (FAILED(o.rt->CreateBitmap(size, o.cap.px, o.cap.w * 4, bp, &o.shot))) return false;
    }
    return true;
}

// Create the render target and load fonts up front so the first capture is as fast as the rest.
void OverlayPrewarm() {
    auto& o = g_ov;
    o.cap.w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    o.cap.h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (EnsureTarget()) {
        o.rt->BeginDraw();
        auto* fmt = GetFont(12.5f);
        if (fmt) o.rt->DrawText(L"Snip ×", 6, fmt, D2D1::RectF(0, 0, 100, 30), o.br.Get());
        o.rt->EndDraw();
        o.br.Reset();
        o.rt.Reset();
    }
    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
    o.cap.w = o.cap.h = 0;
}

static void ForceForeground(HWND h) {
    if (GetForegroundWindow() == h) return;
    SetForegroundWindow(h);
    if (GetForegroundWindow() == h) return;
    // a zero keyboard event makes this process the last input source, which unlocks SetForegroundWindow
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = 0;
    in.ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(1, &in, sizeof(in));
    SetForegroundWindow(h);
    if (GetForegroundWindow() == h) return;
    HWND fg = GetForegroundWindow();
    DWORD fgThread = GetWindowThreadProcessId(fg, nullptr), me = GetCurrentThreadId();
    AttachThreadInput(me, fgThread, TRUE);
    SetForegroundWindow(h);
    SetFocus(h);
    AttachThreadInput(me, fgThread, FALSE);
}

static void ResetState() {
    auto& o = g_ov;
    o.editing = o.hasSel = o.hasHover = o.drawing = o.typing = false;
    o.drag = Drag::None;
    o.shapes.clear();
    o.redo.clear();
    o.cur = Shape{};
    o.text = Shape{};
    o.buttons.clear();
    o.hoverBtn = -1;
    o.zoom = 1;
    o.ox = o.oy = 0;
}

bool OverlayActive() { return g_ov.active; }

void OverlayStart() {
    auto& o = g_ov;
    if (o.active) return;
    o.shot.Reset();
    if (!CaptureScreen(o.cap)) return;
    ResetState();
    POINT cp;
    GetCursorPos(&cp);
    o.mouse = {(float)(cp.x - o.cap.originX), (float)(cp.y - o.cap.originY)};
    UpdateHover();

    SetWindowPos(o.hwnd, HWND_TOPMOST, o.cap.originX, o.cap.originY, o.cap.w, o.cap.h, SWP_NOACTIVATE);
    if (!EnsureTarget()) {
        o.cap.Release();
        return;
    }
    o.active = true;
    // cloak while the first frame is rendered so a stale frame never flashes
    BOOL cloak = TRUE;
    DwmSetWindowAttribute(o.hwnd, DWMWA_CLOAK, &cloak, sizeof(cloak));
    ShowWindow(o.hwnd, SW_SHOWNA);
    o.rt->BeginDraw();
    OverlayRender(o.rt.Get(), o.br.Get(), o.shot.Get());
    o.rt->EndDraw();
    cloak = FALSE;
    DwmSetWindowAttribute(o.hwnd, DWMWA_CLOAK, &cloak, sizeof(cloak));
    ForceForeground(o.hwnd);
    SetFocus(o.hwnd);
}

void OverlayHide() {
    auto& o = g_ov;
    if (!o.active) return;
    o.active = false;
    if (GetCapture() == o.hwnd) ReleaseCapture();
    ShowWindow(o.hwnd, SW_HIDE);
    SaveSettings((int)o.tool, o.colorIdx, o.sizeIdx);
    ResetState();
    o.shot.Reset();
    o.cap.Release();
    // drop the full-screen swap chain while idle: a resident tool should not sit on hundreds of MB
    o.br.Reset();
    o.rt.Reset();
    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
}

static std::wstring AskSavePath() {
    std::wstring def = NewScreenshotPath();
    std::wstring dir = def.substr(0, def.find_last_of(L'\\'));
    wchar_t file[MAX_PATH];
    wcscpy_s(file, def.substr(def.find_last_of(L'\\') + 1).c_str());
    OPENFILENAMEW ofn{sizeof(ofn)};
    ofn.hwndOwner = g_mainWnd;
    ofn.lpstrFilter = L"PNG\0*.png\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrInitialDir = dir.c_str();
    ofn.lpstrDefExt = L"png";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    SetForegroundWindow(g_mainWnd);
    return GetSaveFileNameW(&ofn) ? std::wstring(file) : std::wstring();
}

void Finish(Action a) {
    auto& o = g_ov;
    CommitTyping();
    if (!o.hasSel) return;
    std::vector<uint32_t> px;
    RECT sel = o.sel;
    if (!RenderSelection(o.cap, sel, o.shapes, px)) {
        OverlayHide();
        return;
    }
    int w = sel.right - sel.left, h = sel.bottom - sel.top;
    OverlayHide();  // screen is back instantly; encoding happens after
    std::vector<BYTE> png;
    EncodePng(px.data(), w, h, png);
    CopyImageToClipboard(g_mainWnd, px.data(), w, h, png);
    if (a == Action::Copy) return;
    std::wstring path = a == Action::Save ? NewScreenshotPath() : AskSavePath();
    if (!path.empty() && WriteFileBytes(path, png)) NotifySaved(path);
}

// ---------------------------------------------------------------- testing

static void SetupDemoState() {
    auto& o = g_ov;
    auto& c = o.cap;
    int mi = c.MonitorAt({0, 0});
    for (size_t i = 0; i < c.monitors.size(); i++)
        if (c.monitors[i].left == -c.originX && c.monitors[i].top == -c.originY) mi = (int)i;
    RECT m = c.monitors[mi];
    float s = c.monitorScale[mi];
    int mw = m.right - m.left, mh = m.bottom - m.top;
    o.sel = {m.left + mw / 5, m.top + mh / 6, m.left + mw * 3 / 5, m.top + mh * 3 / 5};
    o.hasSel = o.editing = true;
    float x0 = (float)o.sel.left, y0 = (float)o.sel.top, sw = (float)(o.sel.right - o.sel.left), sh = (float)(o.sel.bottom - o.sel.top);
    auto P = [&](float fx, float fy) { return D2D1::Point2F(x0 + sw * fx, y0 + sh * fy); };
    auto add = [&](Tool t, int color, std::vector<D2D1_POINT_2F> pts) {
        Shape sh;
        sh.tool = t;
        sh.color = ColorFromRGB(kPalette[color], t == Tool::Marker ? 0.42f : 1.f);
        sh.size = ToolSize(t, 1, s);
        sh.pts = std::move(pts);
        o.shapes.push_back(std::move(sh));
        return &o.shapes.back();
    };
    add(Tool::Blur, 0, {P(0.05f, 0.62f), P(0.40f, 0.92f)});
    ComputePixelate(o.shapes.back(), c);
    add(Tool::Rect, 0, {P(0.05f, 0.08f), P(0.45f, 0.40f)});
    add(Tool::Ellipse, 3, {P(0.55f, 0.08f), P(0.92f, 0.40f)});
    add(Tool::Arrow, 0, {P(0.80f, 0.85f), P(0.50f, 0.50f)});
    add(Tool::Line, 4, {P(0.05f, 0.50f), P(0.40f, 0.50f)});
    std::vector<D2D1_POINT_2F> wave;
    for (int i = 0; i <= 40; i++) wave.push_back(P(0.5f + i * 0.01f, 0.95f - 0.04f * std::sin(i * 0.4f)));
    add(Tool::Pen, 5, wave);
    add(Tool::Marker, 2, {P(0.08f, 0.45f), P(0.38f, 0.45f)});
    add(Tool::Text, 0, {P(0.10f, 0.18f)})->text = L"Привет, Snip! 123";
    o.shapes.back().size = ToolSize(Tool::Text, 2, s);
    add(Tool::Number, 0, {P(0.62f, 0.25f)})->number = 1;
    add(Tool::Number, 4, {P(0.72f, 0.25f)})->number = 2;
    o.mouse = {(float)o.sel.left + 200 * s, (float)o.sel.bottom + 30 * s};
    LayoutToolbar();
    if (o.buttons.size() > 3) {
        auto& b = o.buttons[3].r;
        o.mouse = {(b.left + b.right) / 2, (b.top + b.bottom) / 2};
        o.hoverBtn = 3;
    }
}

static double NowMs() {
    LARGE_INTEGER f, t;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return t.QuadPart * 1000.0 / f.QuadPart;
}

static bool SaveRender(const std::wstring& path, std::wstring& log) {
    auto& o = g_ov;
    ComPtr<IWICBitmap> wb;
    ComPtr<ID2D1RenderTarget> rt;
    ComPtr<ID2D1SolidColorBrush> br;
    ComPtr<ID2D1Bitmap> shot;
    g_wic->CreateBitmap(o.cap.w, o.cap.h, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &wb);
    auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
                                              D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
    if (!wb || FAILED(g_d2d->CreateWicBitmapRenderTarget(wb.Get(), props, &rt))) return false;
    rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    rt->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1), &br);
    auto bp = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
    rt->CreateBitmap(D2D1::SizeU(o.cap.w, o.cap.h), o.cap.px, o.cap.w * 4, bp, &shot);
    for (auto& s : o.shapes) s.blockBmp.Reset();
    double t0 = NowMs();
    rt->BeginDraw();
    OverlayRender(rt.Get(), br.Get(), shot.Get());
    HRESULT hr = rt->EndDraw();
    for (auto& s : o.shapes) s.blockBmp.Reset();
    wchar_t b[128];
    swprintf_s(b, L"render %s: %.1f ms hr=0x%08X\n", path.substr(path.find_last_of(L'\\') + 1).c_str(), NowMs() - t0, (unsigned)hr);
    log += b;
    std::vector<uint32_t> px((size_t)o.cap.w * o.cap.h);
    wb->CopyPixels(nullptr, o.cap.w * 4, (UINT)px.size() * 4, (BYTE*)px.data());
    std::vector<BYTE> png;
    return EncodePng(px.data(), o.cap.w, o.cap.h, png) && WriteFileBytes(path, png);
}

int SelfTest(const std::wstring& dir) {
    auto& o = g_ov;
    std::wstring log;
    wchar_t b[256];
    if (!CaptureScreen(o.cap)) {
        log = L"capture failed\n";
    } else {
        swprintf_s(b, L"capture %dx%d at (%d,%d): %.1f ms, %zu windows, %zu monitors\n", o.cap.w, o.cap.h, o.cap.originX,
                   o.cap.originY, o.cap.ms, o.cap.windows.size(), o.cap.monitors.size());
        log += b;
        for (size_t i = 0; i < o.cap.monitors.size(); i++) {
            auto& m = o.cap.monitors[i];
            swprintf_s(b, L"  monitor %zu: %d,%d %dx%d scale %.2f\n", i, m.left, m.top, m.right - m.left, m.bottom - m.top,
                       o.cap.monitorScale[i]);
            log += b;
        }
        extern double BenchBlt(DWORD rop);
        for (int i = 0; i < 3; i++) {
            double a = BenchBlt(SRCCOPY | CAPTUREBLT), b2 = BenchBlt(SRCCOPY);
            swprintf_s(b, L"  bitblt: CAPTUREBLT %.1f ms, SRCCOPY %.1f ms\n", a, b2);
            log += b;
        }
        // selecting state with hover + loupe
        o.mouse = {o.cap.w * 0.3f, o.cap.h * 0.4f};
        UpdateHover();
        SaveRender(dir + L"\\overlay_select.png", log);
        // editing state with every annotation type
        SetupDemoState();
        SaveRender(dir + L"\\overlay_edit.png", log);
        // zoomed
        D2D1_POINT_2F ip{(float)o.sel.left + 40, (float)o.sel.top + 40};
        o.zoom = 4;
        o.ox = o.cap.w * 0.5f - ip.x * o.zoom;
        o.oy = o.cap.h * 0.3f - ip.y * o.zoom;
        ClampView();
        SaveRender(dir + L"\\overlay_zoom.png", log);
        o.zoom = 1;
        ClampView();
        // export
        double t0 = NowMs();
        std::vector<uint32_t> px;
        bool ok = RenderSelection(o.cap, o.sel, o.shapes, px);
        double t1 = NowMs();
        std::vector<BYTE> png;
        int w = o.sel.right - o.sel.left, h = o.sel.bottom - o.sel.top;
        ok = ok && EncodePng(px.data(), w, h, png);
        double t2 = NowMs();
        ok = ok && WriteFileBytes(dir + L"\\export.png", png);
        swprintf_s(b, L"export %dx%d: render %.1f ms, png %.1f ms (%zu KB) ok=%d\n", w, h, t1 - t0, t2 - t1, png.size() / 1024, ok);
        log += b;
        // mirror: flip, export, undo must restore the original pixels exactly
        uint32_t before = o.cap.Pixel(o.sel.left + 3, o.sel.top + 5);
        FlipSelection(false);
        SaveRender(dir + L"\\overlay_flip.png", log);
        ok = RenderSelection(o.cap, o.sel, o.shapes, px) && EncodePng(px.data(), w, h, png) &&
             WriteFileBytes(dir + L"\\export_flip.png", png);
        uint32_t mirrored = o.cap.Pixel(o.sel.right - 1 - 3, o.sel.top + 5);
        Undo();
        uint32_t after = o.cap.Pixel(o.sel.left + 3, o.sel.top + 5);
        swprintf_s(b, L"flip: export ok=%d, pixel moved=%d, undo restored=%d, shapes=%zu\n", ok, mirrored == before,
                   after == before, o.shapes.size());
        log += b;
        FlipSelection(true);
        ok = RenderSelection(o.cap, o.sel, o.shapes, px) && EncodePng(px.data(), w, h, png) &&
             WriteFileBytes(dir + L"\\export_flipv.png", png);
        mirrored = o.cap.Pixel(o.sel.left + 3, o.sel.bottom - 1 - 5);
        Undo();
        after = o.cap.Pixel(o.sel.left + 3, o.sel.top + 5);
        swprintf_s(b, L"flipv: export ok=%d, pixel moved=%d, undo restored=%d, shapes=%zu\n", ok, mirrored == before,
                   after == before, o.shapes.size());
        log += b;
    }
    std::string utf8(WideCharToMultiByte(CP_UTF8, 0, log.c_str(), -1, nullptr, 0, nullptr, nullptr), '\0');
    WideCharToMultiByte(CP_UTF8, 0, log.c_str(), -1, utf8.data(), (int)utf8.size(), nullptr, nullptr);
    utf8.pop_back();
    WriteFileBytes(dir + L"\\selftest.log", std::vector<BYTE>(utf8.begin(), utf8.end()));
    o.cap.Release();
    return 0;
}

void OverlayDemo(DWORD holdMs) {
    auto& o = g_ov;
    o.demoHold = holdMs;
    double t0 = NowMs();
    OverlayStart();
    double t1 = NowMs();
    if (!o.active) {
        PostQuitMessage(1);
        return;
    }
    SetupDemoState();
    InvalidateRect(o.hwnd, nullptr, FALSE);
    UpdateWindow(o.hwnd);
    wchar_t b[160];
    swprintf_s(b, L"hotkey-to-visible %.1f ms (capture %.1f ms), foreground=%d\n", t1 - t0, o.cap.ms, GetForegroundWindow() == o.hwnd);
    OutputDebugStringW(b);
    std::string s;
    for (const wchar_t* p = b; *p; p++) s += (char)*p;
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    WriteFileBytes(std::wstring(tmp) + L"snip_demo.log", std::vector<BYTE>(s.begin(), s.end()));
    SetTimer(o.hwnd, 1, holdMs, nullptr);
}
