#pragma once
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shellscalingapi.h>
#include <dwmapi.h>
#include <commdlg.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

#define APP_NAME L"Snip"
#define MAIN_CLASS L"SnipMainWnd"
#define OVERLAY_CLASS L"SnipOverlayWnd"

constexpr UINT WM_APP_CAPTURE = WM_APP + 1;
constexpr UINT WM_APP_TRAY = WM_APP + 2;
constexpr UINT WM_APP_SAVED = WM_APP + 3;
constexpr UINT WM_APP_EXIT = WM_APP + 4;

// ---------------------------------------------------------------- capture

struct CaptureData {
    int originX = 0, originY = 0;   // virtual screen origin, physical px
    int w = 0, h = 0;
    uint32_t* px = nullptr;         // BGRA top-down (alpha undefined), owned by dib
    HBITMAP dib = nullptr;
    std::vector<RECT> windows;      // image coords, z-order (topmost first)
    std::vector<RECT> monitors;     // image coords
    std::vector<RECT> workAreas;    // image coords
    std::vector<float> monitorScale;  // dpi / 96
    double ms = 0;                  // capture duration

    uint32_t Pixel(int x, int y) const {
        x = std::clamp(x, 0, w - 1);
        y = std::clamp(y, 0, h - 1);
        return px[(size_t)y * w + x];
    }
    int MonitorAt(POINT p) const;
    void Release();
};

bool CaptureScreen(CaptureData& cap);

// ---------------------------------------------------------------- shapes

// Flip is not a drawing tool: it is an undoable mirror operation stored in the shape list.
enum class Tool { Move, Pen, Line, Arrow, Rect, Ellipse, Marker, Text, Blur, Number, COUNT, Flip };

struct Shape {
    Tool tool = Tool::Pen;
    D2D1_COLOR_F color{1, 0, 0, 1};
    float size = 3;                     // stroke width / font size / badge radius / pixel block
    std::vector<D2D1_POINT_2F> pts;     // Flip: the mirrored rect (top-left, bottom-right)
    std::wstring text;
    int number = 0;
    bool vertical = false;              // Flip: mirror top-bottom instead of left-right
    // pixelate: averaged blocks starting at (bx, by)
    int bx = 0, by = 0, cols = 0, rows = 0;
    std::vector<uint32_t> blocks;
    ComPtr<ID2D1Bitmap> blockBmp;       // cached for the overlay render target only
};

D2D1_RECT_F ShapeBounds(const Shape& s);  // normalized rect from pts[0], pts[1]
void ComputePixelate(Shape& s, const CaptureData& cap);
void DrawShape(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* br, Shape& s, bool cacheBitmaps);
void TextCaretPos(const Shape& s, D2D1_RECT_F& box, D2D1_POINT_2F& caretTop, float& caretH);

// ---------------------------------------------------------------- graphics

extern ComPtr<ID2D1Factory> g_d2d;
extern ComPtr<IDWriteFactory> g_dw;
extern ComPtr<IWICImagingFactory> g_wic;
extern ComPtr<ID2D1StrokeStyle> g_round;
extern ComPtr<ID2D1StrokeStyle> g_dash;

bool GfxInit();
IDWriteTextFormat* GetFont(float size, DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_SEMI_BOLD,
                           DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING);
D2D1_SIZE_F MeasureText(const std::wstring& text, IDWriteTextFormat* fmt);

enum class Icon { Move, Pen, Line, Arrow, Rect, Ellipse, Marker, Text, Blur, Number,
                  Undo, Redo, Save, Copy, Close, Size, FlipH, FlipV };
void DrawIcon(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* br, Icon icon, D2D1_RECT_F r,
              float stroke, float sizeHint);
HICON CreateAppIcon(int size);

// ---------------------------------------------------------------- export

bool RenderSelection(const CaptureData& cap, RECT sel, std::vector<Shape>& shapes,
                     std::vector<uint32_t>& out);
bool EncodePng(const uint32_t* px, int w, int h, std::vector<BYTE>& png);
bool CopyImageToClipboard(HWND owner, const uint32_t* px, int w, int h, const std::vector<BYTE>& png);
bool CopyTextToClipboard(HWND owner, const std::wstring& text);
std::wstring ReadClipboardText(HWND owner);
std::wstring ScreenshotsFolder();
std::wstring NewScreenshotPath();
bool WriteFileBytes(const std::wstring& path, const std::vector<BYTE>& data);

// ---------------------------------------------------------------- overlay / app

extern HWND g_mainWnd;
bool OverlayInit(HINSTANCE inst);
void OverlayStart();
bool OverlayActive();
void OverlayDemo(DWORD holdMs);
int SelfTest(const std::wstring& outDir);
void NotifySaved(const std::wstring& path);

// autostart: an elevated Task Scheduler logon task (falls back to the HKCU Run key)
bool IsElevated();
bool AutostartEnabled();
bool InstallAutostartTask();
bool StartAutostartTask();
bool RemoveAutostart();
DWORD RunSelfElevated(const wchar_t* args, bool wait);

void LoadSettings(int& tool, int& color, int& size);
void SaveSettings(int tool, int color, int size);

inline D2D1_COLOR_F ColorFromRGB(uint32_t rgb, float a = 1.f) {
    return D2D1::ColorF(((rgb >> 16) & 255) / 255.f, ((rgb >> 8) & 255) / 255.f, (rgb & 255) / 255.f, a);
}
