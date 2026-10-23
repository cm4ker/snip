#pragma once
#include "common.h"

enum class Btn { Tool, Color, Size, Flip, Undo, Redo, Save, Copy, Close };
struct Button {
    Btn kind;
    int arg;
    D2D1_RECT_F r;
    std::wstring tip;
};

enum class Drag { None, NewSel, MoveSel, Resize, Draw, Pan };
enum class Action { Copy, Save, SaveAs };

struct Overlay {
    HWND hwnd = nullptr;
    ComPtr<ID2D1HwndRenderTarget> rt;
    ComPtr<ID2D1SolidColorBrush> br;
    ComPtr<ID2D1Bitmap> shot;
    CaptureData cap;
    bool active = false;
    DWORD demoHold = 0;

    bool editing = false;  // selection fixed, annotating
    bool hasSel = false;
    RECT sel{};            // image px, right/bottom exclusive
    DWORD selTick = 0;     // when selection was fixed (double-click detection)

    Drag drag = Drag::None;
    int edges = 0;         // resize edges: 1=L 2=T 4=R 8=B
    D2D1_POINT_2F dragImg{}, dragScreen{};
    RECT selStart{};
    float panOx = 0, panOy = 0;
    bool dragMoved = false;

    Tool tool = Tool::Arrow;
    int colorIdx = 0, sizeIdx = 1;
    std::vector<Shape> shapes, redo;
    Shape cur;
    bool drawing = false;
    bool typing = false;
    Shape text;

    float zoom = 1, ox = 0, oy = 0;
    D2D1_POINT_2F mouse{-1000, -1000};
    bool shift = false;

    RECT hover{};
    bool hasHover = false;
    int hoverBtn = -1;
    std::vector<Button> buttons;
    D2D1_RECT_F toolbar{};
    bool toolbarAbove = false;
};

extern Overlay g_ov;

constexpr int kColors = 8;
extern const uint32_t kPalette[kColors];
constexpr int kSizes = 5;

D2D1_POINT_2F ToImage(D2D1_POINT_2F screen);
D2D1_POINT_2F ToScreen(D2D1_POINT_2F img);
D2D1_RECT_F ToScreen(const RECT& r);
float ScaleAtScreen(D2D1_POINT_2F p);
float ToolSize(Tool t, int sizeIdx, float scale);
void LayoutToolbar();
void UpdateHover();
void OverlayRender(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* br, ID2D1Bitmap* shot);
void OverlayHide();
void Finish(Action a);
void CommitTyping();
void ClampView();
void FlipSelection(bool vertical);
void Undo();
bool EnsureTarget();
void OverlayPrewarm();
LRESULT CALLBACK OverlayProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
