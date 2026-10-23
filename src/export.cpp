#include "common.h"

bool RenderSelection(const CaptureData& cap, RECT sel, std::vector<Shape>& shapes, std::vector<uint32_t>& out) {
    RECT img{0, 0, cap.w, cap.h};
    IntersectRect(&sel, &sel, &img);
    int w = sel.right - sel.left, h = sel.bottom - sel.top;
    if (w <= 0 || h <= 0 || !cap.px) return false;

    ComPtr<IWICBitmap> wb;
    if (FAILED(g_wic->CreateBitmap(w, h, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &wb))) return false;
    ComPtr<ID2D1RenderTarget> rt;
    auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
                                              D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
    if (FAILED(g_d2d->CreateWicBitmapRenderTarget(wb.Get(), props, &rt))) return false;
    rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    ComPtr<ID2D1SolidColorBrush> br;
    rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0), &br);

    ComPtr<ID2D1Bitmap> crop;
    auto bp = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
    const uint32_t* src = cap.px + (size_t)sel.top * cap.w + sel.left;
    if (FAILED(rt->CreateBitmap(D2D1::SizeU(w, h), src, cap.w * 4, bp, &crop))) return false;

    rt->BeginDraw();
    rt->Clear(D2D1::ColorF(0, 0, 0));
    rt->SetTransform(D2D1::Matrix3x2F::Translation(-(float)sel.left, -(float)sel.top));
    rt->DrawBitmap(crop.Get(), D2D1::RectF((float)sel.left, (float)sel.top, (float)sel.right, (float)sel.bottom), 1.f,
                   D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
    for (auto& s : shapes) DrawShape(rt.Get(), br.Get(), s, false);
    if (FAILED(rt->EndDraw())) return false;

    out.resize((size_t)w * h);
    if (FAILED(wb->CopyPixels(nullptr, w * 4, (UINT)out.size() * 4, (BYTE*)out.data()))) return false;
    for (auto& p : out) p |= 0xFF000000u;
    return true;
}

bool EncodePng(const uint32_t* px, int w, int h, std::vector<BYTE>& png) {
    ComPtr<IStream> stream;
    ComPtr<IWICBitmapEncoder> enc;
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IWICBitmap> bmp;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream))) return false;
    if (FAILED(g_wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc))) return false;
    if (FAILED(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache))) return false;
    if (FAILED(enc->CreateNewFrame(&frame, nullptr)) || FAILED(frame->Initialize(nullptr))) return false;
    frame->SetSize(w, h);
    WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR;  // screenshots are opaque: smaller files
    frame->SetPixelFormat(&fmt);
    if (FAILED(g_wic->CreateBitmapFromMemory(w, h, GUID_WICPixelFormat32bppBGRA, w * 4, (UINT)w * h * 4, (BYTE*)px, &bmp)))
        return false;
    if (FAILED(frame->WriteSource(bmp.Get(), nullptr)) || FAILED(frame->Commit()) || FAILED(enc->Commit())) return false;

    HGLOBAL hg = nullptr;
    if (FAILED(GetHGlobalFromStream(stream.Get(), &hg))) return false;
    STATSTG st{};
    stream->Stat(&st, STATFLAG_NONAME);
    size_t n = (size_t)st.cbSize.QuadPart;
    void* p = GlobalLock(hg);
    if (!p) return false;
    png.assign((BYTE*)p, (BYTE*)p + n);
    GlobalUnlock(hg);
    return true;
}

static bool OpenClipboardRetry(HWND owner) {
    for (int i = 0; i < 20; i++) {
        if (OpenClipboard(owner)) return true;
        Sleep(10);
    }
    return false;
}

static HGLOBAL GlobalFrom(const void* data, size_t n) {
    HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, n);
    if (!hg) return nullptr;
    memcpy(GlobalLock(hg), data, n);
    GlobalUnlock(hg);
    return hg;
}

bool CopyImageToClipboard(HWND owner, const uint32_t* px, int w, int h, const std::vector<BYTE>& png) {
    if (!OpenClipboardRetry(owner)) return false;
    EmptyClipboard();

    size_t rowBytes = (size_t)w * 4;
    HGLOBAL dib = GlobalAlloc(GMEM_MOVEABLE, sizeof(BITMAPINFOHEADER) + rowBytes * h);
    if (dib) {
        auto* bh = (BITMAPINFOHEADER*)GlobalLock(dib);
        *bh = {};
        bh->biSize = sizeof(BITMAPINFOHEADER);
        bh->biWidth = w;
        bh->biHeight = h;  // bottom-up: most compatible
        bh->biPlanes = 1;
        bh->biBitCount = 32;
        bh->biCompression = BI_RGB;
        bh->biSizeImage = (DWORD)(rowBytes * h);
        BYTE* dst = (BYTE*)(bh + 1);
        for (int y = 0; y < h; y++) memcpy(dst + rowBytes * (h - 1 - y), px + (size_t)y * w, rowBytes);
        GlobalUnlock(dib);
        if (!SetClipboardData(CF_DIB, dib)) GlobalFree(dib);
    }
    if (!png.empty()) {
        if (HGLOBAL hg = GlobalFrom(png.data(), png.size()))
            if (!SetClipboardData(RegisterClipboardFormatW(L"PNG"), hg)) GlobalFree(hg);
    }
    CloseClipboard();
    return true;
}

bool CopyTextToClipboard(HWND owner, const std::wstring& text) {
    if (!OpenClipboardRetry(owner)) return false;
    EmptyClipboard();
    if (HGLOBAL hg = GlobalFrom(text.c_str(), (text.size() + 1) * sizeof(wchar_t)))
        if (!SetClipboardData(CF_UNICODETEXT, hg)) GlobalFree(hg);
    CloseClipboard();
    return true;
}

std::wstring ReadClipboardText(HWND owner) {
    std::wstring s;
    if (!OpenClipboardRetry(owner)) return s;
    if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
        if (auto* p = (const wchar_t*)GlobalLock(h)) {
            s = p;
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    s.erase(std::remove(s.begin(), s.end(), L'\r'), s.end());
    return s;
}

std::wstring ScreenshotsFolder() {
    std::wstring dir;
    PWSTR p = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Screenshots, KF_FLAG_CREATE, nullptr, &p))) dir = p;
    CoTaskMemFree(p);
    if (dir.empty()) {
        p = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Pictures, KF_FLAG_CREATE, nullptr, &p))) dir = std::wstring(p) + L"\\Screenshots";
        CoTaskMemFree(p);
    }
    if (dir.empty()) dir = L".";
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    return dir;
}

std::wstring NewScreenshotPath() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t name[96];
    swprintf_s(name, L"Snip_%04d-%02d-%02d_%02d-%02d-%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    std::wstring base = ScreenshotsFolder() + L"\\" + name;
    std::wstring path = base + L".png";
    for (int i = 2; GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; i++)
        path = base + L"_" + std::to_wstring(i) + L".png";
    return path;
}

bool WriteFileBytes(const std::wstring& path, const std::vector<BYTE>& data) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    BOOL ok = WriteFile(f, data.data(), (DWORD)data.size(), &written, nullptr);
    CloseHandle(f);
    return ok && written == data.size();
}
