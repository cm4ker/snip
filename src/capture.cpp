#include "common.h"

int CaptureData::MonitorAt(POINT p) const {
    int best = 0;
    long bestDist = LONG_MAX;
    for (size_t i = 0; i < monitors.size(); i++) {
        const RECT& r = monitors[i];
        if (PtInRect(&r, p)) return (int)i;
        long dx = p.x < r.left ? r.left - p.x : (p.x >= r.right ? p.x - r.right + 1 : 0);
        long dy = p.y < r.top ? r.top - p.y : (p.y >= r.bottom ? p.y - r.bottom + 1 : 0);
        long d = dx + dy;
        if (d < bestDist) { bestDist = d; best = (int)i; }
    }
    return best;
}

void CaptureData::Release() {
    if (dib) DeleteObject(dib);
    dib = nullptr;
    px = nullptr;
    w = h = 0;
    windows.clear();
    monitors.clear();
    workAreas.clear();
    monitorScale.clear();
}

static BOOL CALLBACK EnumMonitorProc(HMONITOR hm, HDC, LPRECT, LPARAM lp) {
    auto* cap = (CaptureData*)lp;
    MONITORINFO mi{sizeof(mi)};
    if (!GetMonitorInfoW(hm, &mi)) return TRUE;
    RECT r = mi.rcMonitor, wa = mi.rcWork;
    OffsetRect(&r, -cap->originX, -cap->originY);
    OffsetRect(&wa, -cap->originX, -cap->originY);
    UINT dx = 96, dy = 96;
    GetDpiForMonitor(hm, MDT_EFFECTIVE_DPI, &dx, &dy);
    cap->monitors.push_back(r);
    cap->workAreas.push_back(wa);
    cap->monitorScale.push_back(dx / 96.f);
    return TRUE;
}

static BOOL CALLBACK EnumWindowProc(HWND hw, LPARAM lp) {
    auto* cap = (CaptureData*)lp;
    if (!IsWindowVisible(hw) || IsIconic(hw)) return TRUE;
    DWORD cloaked = 0;
    DwmGetWindowAttribute(hw, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    if (cloaked) return TRUE;
    LONG ex = GetWindowLongW(hw, GWL_EXSTYLE);
    if ((ex & WS_EX_LAYERED) && (ex & WS_EX_TRANSPARENT)) return TRUE;  // click-through overlays
    wchar_t cls[64] = {};
    GetClassNameW(hw, cls, 64);
    if (!wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW")) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(hw, &pid);
    if (pid == GetCurrentProcessId()) return TRUE;

    RECT r{};
    if (FAILED(DwmGetWindowAttribute(hw, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r))))
        GetWindowRect(hw, &r);
    if (r.right - r.left < 16 || r.bottom - r.top < 16) return TRUE;
    OffsetRect(&r, -cap->originX, -cap->originY);
    RECT img{0, 0, cap->w, cap->h}, clipped;
    if (!IntersectRect(&clipped, &r, &img)) return TRUE;
    cap->windows.push_back(clipped);
    return TRUE;
}

double BenchBlt(DWORD rop) {
    int x = GetSystemMetrics(SM_XVIRTUALSCREEN), y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int w = GetSystemMetrics(SM_CXVIRTUALSCREEN), h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB};
    void* bits;
    HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(mem, dib);
    BitBlt(mem, 0, 0, w, h, screen, x, y, rop);
    GdiFlush();
    SelectObject(mem, old);
    DeleteObject(dib);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    QueryPerformanceCounter(&t1);
    return (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart;
}

bool CaptureScreen(CaptureData& cap) {
    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);

    cap.Release();
    cap.originX = GetSystemMetrics(SM_XVIRTUALSCREEN);
    cap.originY = GetSystemMetrics(SM_YVIRTUALSCREEN);
    cap.w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    cap.h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (cap.w <= 0 || cap.h <= 0) return false;

    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = cap.w;
    bi.bmiHeader.biHeight = -cap.h;  // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    cap.dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    bool ok = false;
    if (cap.dib) {
        HGDIOBJ old = SelectObject(mem, cap.dib);
        ok = BitBlt(mem, 0, 0, cap.w, cap.h, screen, cap.originX, cap.originY, SRCCOPY | CAPTUREBLT) != 0;
        GdiFlush();
        SelectObject(mem, old);
        cap.px = (uint32_t*)bits;
    }
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    if (!ok) {
        cap.Release();
        return false;
    }

    EnumDisplayMonitors(nullptr, nullptr, EnumMonitorProc, (LPARAM)&cap);
    EnumWindows(EnumWindowProc, (LPARAM)&cap);

    QueryPerformanceCounter(&t1);
    cap.ms = (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart;
    return true;
}
