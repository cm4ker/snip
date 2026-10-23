// Overlay window procedure: mouse, keyboard, wheel zoom.
#include "overlay.h"

// Set SNIP_TRACE=1 to log overlay input to %TEMP%\snip_trace.log
static void Trace(const char* fmt, ...) {
    static int enabled = -1;
    if (enabled < 0) enabled = GetEnvironmentVariableW(L"SNIP_TRACE", nullptr, 0) > 0;
    if (!enabled) return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf) - 2, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    buf[n] = '\n';
    wchar_t path[MAX_PATH];
    GetTempPathW(MAX_PATH, path);
    wcscat_s(path, L"snip_trace.log");
    HANDLE f = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD w;
    WriteFile(f, buf, n + 1, &w, nullptr);
    CloseHandle(f);
}

static D2D1_POINT_2F MousePt(LPARAM lp) { return {(float)GET_X_LPARAM(lp), (float)GET_Y_LPARAM(lp)}; }
static void Redraw() { InvalidateRect(g_ov.hwnd, nullptr, FALSE); }

static RECT NormRect(D2D1_POINT_2F a, D2D1_POINT_2F b) {
    RECT r{(LONG)std::lround(std::min(a.x, b.x)), (LONG)std::lround(std::min(a.y, b.y)),
           (LONG)std::lround(std::max(a.x, b.x)), (LONG)std::lround(std::max(a.y, b.y))};
    RECT img{0, 0, g_ov.cap.w, g_ov.cap.h};
    IntersectRect(&r, &r, &img);
    return r;
}

// Mirrors the screenshot pixels inside op's rect and, optionally, the geometry of every annotation
// drawn so far. A flip is its own inverse, so undo/redo just apply it again.
static void ApplyFlip(const Shape& op, bool shapesToo) {
    auto& o = g_ov;
    if (op.pts.size() < 2 || !o.cap.px) return;
    RECT r{(LONG)op.pts[0].x, (LONG)op.pts[0].y, (LONG)op.pts[1].x, (LONG)op.pts[1].y};
    uint32_t* px = o.cap.px;
    size_t W = (size_t)o.cap.w;
    if (op.vertical) {
        for (LONG a = r.top, b = r.bottom - 1; a < b; a++, b--)
            std::swap_ranges(px + a * W + r.left, px + a * W + r.right, px + b * W + r.left);
    } else {
        for (LONG y = r.top; y < r.bottom; y++) std::reverse(px + y * W + r.left, px + y * W + r.right);
    }
    if (o.shot) {
        D2D1_RECT_U dst{(UINT32)r.left, (UINT32)r.top, (UINT32)r.right, (UINT32)r.bottom};
        o.shot->CopyFromMemory(&dst, px + r.top * W + r.left, (UINT32)(W * 4));
    }
    if (!shapesToo) return;
    float axis = op.vertical ? (float)(r.top + r.bottom) : (float)(r.left + r.right);
    for (auto& s : o.shapes) {
        if (s.tool == Tool::Flip || s.pts.empty()) continue;
        if (s.tool == Tool::Text) {  // keep text readable: mirror its box, not its glyphs
            D2D1_RECT_F box;
            D2D1_POINT_2F caret;
            float caretH;
            TextCaretPos(s, box, caret, caretH);
            if (op.vertical) s.pts[0].y = axis - box.bottom;
            else s.pts[0].x = axis - box.right;
            continue;
        }
        for (auto& p : s.pts) (op.vertical ? p.y : p.x) = axis - (op.vertical ? p.y : p.x);
        if (s.tool == Tool::Blur) ComputePixelate(s, o.cap);
    }
}

static void FixSelection(RECT r) {
    auto& o = g_ov;
    if (r.right - r.left < 2 || r.bottom - r.top < 2) return;
    o.sel = r;
    o.hasSel = true;
    o.editing = true;
    o.hasHover = false;
    o.selTick = GetTickCount();
    LayoutToolbar();
}

static void ResetSelection() {
    auto& o = g_ov;
    for (auto it = o.shapes.rbegin(); it != o.shapes.rend(); ++it)  // restore mirrored pixels
        if (it->tool == Tool::Flip) ApplyFlip(*it, false);
    o.editing = o.hasSel = o.typing = o.drawing = false;
    o.shapes.clear();
    o.redo.clear();
    o.buttons.clear();
    UpdateHover();
}

static int HitButton(D2D1_POINT_2F p) {
    auto& o = g_ov;
    for (size_t i = 0; i < o.buttons.size(); i++) {
        auto& r = o.buttons[i].r;
        if (p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom) return (int)i;
    }
    return -1;
}

static bool InToolbar(D2D1_POINT_2F p) {
    auto& t = g_ov.toolbar;
    return !g_ov.buttons.empty() && p.x >= t.left && p.x < t.right && p.y >= t.top && p.y < t.bottom;
}

static int HitEdges(D2D1_POINT_2F p) {
    auto& o = g_ov;
    if (!o.editing) return 0;
    D2D1_RECT_F r = ToScreen(o.sel);
    float tol = 6 * ScaleAtScreen(p);
    if (p.x < r.left - tol || p.x > r.right + tol || p.y < r.top - tol || p.y > r.bottom + tol) return 0;
    int e = 0;
    if (std::fabs(p.x - r.left) <= tol) e |= 1;
    else if (std::fabs(p.x - r.right) <= tol) e |= 4;
    if (std::fabs(p.y - r.top) <= tol) e |= 2;
    else if (std::fabs(p.y - r.bottom) <= tol) e |= 8;
    // edges of a tiny selection: prefer moving the nearest corner
    return e;
}

static bool InSel(D2D1_POINT_2F screen) {
    D2D1_POINT_2F p = ToImage(screen);
    auto& s = g_ov.sel;
    return g_ov.hasSel && p.x >= s.left && p.x < s.right && p.y >= s.top && p.y < s.bottom;
}

static D2D1_COLOR_F CurrentColor(Tool t) {
    return ColorFromRGB(kPalette[g_ov.colorIdx], t == Tool::Marker ? 0.42f : 1.f);
}

static int NextNumber() {
    int n = 0;
    for (auto& s : g_ov.shapes)
        if (s.tool == Tool::Number) n = std::max(n, s.number);
    return n + 1;
}

static void PushShape(Shape&& s) {
    g_ov.shapes.push_back(std::move(s));
    g_ov.redo.clear();
}

void CommitTyping() {
    auto& o = g_ov;
    if (!o.typing) return;
    o.typing = false;
    if (!o.text.text.empty()) PushShape(std::move(o.text));
    o.text = Shape{};
}

static void SetTool(Tool t) {
    CommitTyping();
    g_ov.tool = t;
    Redraw();
}

static void ChangeSize(int delta) {
    auto& o = g_ov;
    o.sizeIdx = std::clamp(o.sizeIdx + delta, 0, kSizes - 1);
    if (o.typing) o.text.size = ToolSize(Tool::Text, o.sizeIdx, ScaleAtScreen(ToScreen(o.text.pts[0])));
    Redraw();
}

static void SetColor(int idx) {
    auto& o = g_ov;
    o.colorIdx = std::clamp(idx, 0, kColors - 1);
    if (o.typing) o.text.color = CurrentColor(Tool::Text);
    Redraw();
}

void Undo() {
    auto& o = g_ov;
    if (o.typing) {
        o.typing = false;
        o.text = Shape{};
    } else if (!o.shapes.empty()) {
        o.redo.push_back(std::move(o.shapes.back()));
        o.shapes.pop_back();
        if (o.redo.back().tool == Tool::Flip) ApplyFlip(o.redo.back(), true);
    }
    Redraw();
}

static void RedoAction() {
    auto& o = g_ov;
    if (o.redo.empty()) return;
    if (o.redo.back().tool == Tool::Flip) ApplyFlip(o.redo.back(), true);
    o.shapes.push_back(std::move(o.redo.back()));
    o.redo.pop_back();
    Redraw();
}

void FlipSelection(bool vertical) {
    auto& o = g_ov;
    Trace("flip vertical=%d editing=%d hasSel=%d", vertical, o.editing, o.hasSel);
    if (!o.editing || !o.hasSel) return;
    CommitTyping();
    Shape op;
    op.tool = Tool::Flip;
    op.vertical = vertical;
    op.pts = {{(float)o.sel.left, (float)o.sel.top}, {(float)o.sel.right, (float)o.sel.bottom}};
    ApplyFlip(op, true);
    PushShape(std::move(op));
    Redraw();
}

static void ZoomAt(D2D1_POINT_2F m, float factor) {
    auto& o = g_ov;
    D2D1_POINT_2F ip = ToImage(m);
    o.zoom = std::clamp(o.zoom * factor, 1.f, 32.f);
    o.ox = m.x - ip.x * o.zoom;
    o.oy = m.y - ip.y * o.zoom;
    ClampView();
    UpdateHover();
    Redraw();
}

static void PressButton(int i) {
    auto& b = g_ov.buttons[i];
    Trace("button %d kind=%d arg=%d rect=%.0f,%.0f,%.0f,%.0f", i, (int)b.kind, b.arg, b.r.left, b.r.top, b.r.right, b.r.bottom);
    switch (b.kind) {
    case Btn::Tool: SetTool((Tool)b.arg); break;
    case Btn::Color: SetColor(b.arg); break;
    case Btn::Size: ChangeSize(g_ov.sizeIdx + 1 >= kSizes ? -(kSizes - 1) : 1); break;
    case Btn::Flip: FlipSelection(b.arg == 1); break;
    case Btn::Undo: Undo(); break;
    case Btn::Redo: RedoAction(); break;
    case Btn::Save: Finish((GetKeyState(VK_SHIFT) < 0) ? Action::SaveAs : Action::Save); break;
    case Btn::Copy: Finish(Action::Copy); break;
    case Btn::Close: OverlayHide(); break;
    }
}

static void StartNewSelection(D2D1_POINT_2F m) {
    auto& o = g_ov;
    ResetSelection();
    o.drag = Drag::NewSel;
    o.dragImg = ToImage(m);
    o.dragScreen = m;
    o.dragMoved = false;
    SetCapture(o.hwnd);
}

static void OnLButtonDown(D2D1_POINT_2F m) {
    auto& o = g_ov;
    Trace("down %.0f,%.0f editing=%d tool=%d sel=%ld,%ld,%ld,%ld", m.x, m.y, o.editing, (int)o.tool, o.sel.left, o.sel.top,
          o.sel.right, o.sel.bottom);
    o.mouse = m;
    if (!o.editing) {
        StartNewSelection(m);
        return;
    }
    LayoutToolbar();
    int b = HitButton(m);
    if (b >= 0) {
        PressButton(b);
        return;
    }
    if (InToolbar(m)) return;
    D2D1_POINT_2F ip = ToImage(m);
    if (int e = HitEdges(m)) {
        CommitTyping();
        o.drag = Drag::Resize;
        o.edges = e;
        o.selStart = o.sel;
        o.dragImg = ip;
        SetCapture(o.hwnd);
        return;
    }
    bool inside = InSel(m);
    if (!inside && o.shapes.empty() && !o.typing) {
        StartNewSelection(m);
        return;
    }
    if (o.tool == Tool::Move) {
        CommitTyping();
        if (inside) {
            o.drag = Drag::MoveSel;
            o.selStart = o.sel;
            o.dragImg = ip;
            SetCapture(o.hwnd);
        } else {
            StartNewSelection(m);
        }
        return;
    }
    float scale = ScaleAtScreen(m);
    if (o.tool == Tool::Text) {
        CommitTyping();
        o.typing = true;
        o.text = Shape{};
        o.text.tool = Tool::Text;
        o.text.color = CurrentColor(Tool::Text);
        o.text.size = ToolSize(Tool::Text, o.sizeIdx, scale);
        o.text.pts = {{ip.x, ip.y - o.text.size * 0.65f}};
        Redraw();
        return;
    }
    CommitTyping();
    Shape s;
    s.tool = o.tool;
    s.color = CurrentColor(o.tool);
    s.size = ToolSize(o.tool, o.sizeIdx, scale);
    if (o.tool == Tool::Number) {
        s.pts = {ip};
        s.number = NextNumber();
        PushShape(std::move(s));
        Redraw();
        return;
    }
    s.pts = (o.tool == Tool::Pen || o.tool == Tool::Marker) ? std::vector<D2D1_POINT_2F>{ip}
                                                            : std::vector<D2D1_POINT_2F>{ip, ip};
    o.cur = std::move(s);
    o.drawing = true;
    o.drag = Drag::Draw;
    o.dragImg = ip;
    SetCapture(o.hwnd);
    Redraw();
}

static D2D1_POINT_2F Constrain(Tool t, D2D1_POINT_2F a, D2D1_POINT_2F b) {
    float dx = b.x - a.x, dy = b.y - a.y;
    if (t == Tool::Line || t == Tool::Arrow) {
        float len = std::sqrt(dx * dx + dy * dy);
        float ang = std::round(std::atan2(dy, dx) / (3.14159265f / 4)) * (3.14159265f / 4);
        return {a.x + len * std::cos(ang), a.y + len * std::sin(ang)};
    }
    float d = std::max(std::fabs(dx), std::fabs(dy));
    return {a.x + (dx < 0 ? -d : d), a.y + (dy < 0 ? -d : d)};
}

static void OnMouseMove(D2D1_POINT_2F m, bool shift) {
    auto& o = g_ov;
    o.mouse = m;
    D2D1_POINT_2F ip = ToImage(m);
    switch (o.drag) {
    case Drag::NewSel:
        if (!o.dragMoved && std::fabs(m.x - o.dragScreen.x) + std::fabs(m.y - o.dragScreen.y) > 4) o.dragMoved = true;
        if (o.dragMoved) {
            o.sel = NormRect(o.dragImg, ip);
            o.hasSel = true;
            o.hasHover = false;
        }
        break;
    case Drag::MoveSel: {
        int w = o.selStart.right - o.selStart.left, h = o.selStart.bottom - o.selStart.top;
        int x = std::clamp((int)o.selStart.left + (int)std::lround(ip.x - o.dragImg.x), 0, o.cap.w - w);
        int y = std::clamp((int)o.selStart.top + (int)std::lround(ip.y - o.dragImg.y), 0, o.cap.h - h);
        o.sel = {x, y, x + w, y + h};
        break;
    }
    case Drag::Resize: {
        int dx = (int)std::lround(ip.x - o.dragImg.x), dy = (int)std::lround(ip.y - o.dragImg.y);
        RECT r = o.selStart;
        if (o.edges & 1) r.left += dx;
        if (o.edges & 4) r.right += dx;
        if (o.edges & 2) r.top += dy;
        if (o.edges & 8) r.bottom += dy;
        RECT n{std::min(r.left, r.right), std::min(r.top, r.bottom), std::max(r.left, r.right), std::max(r.top, r.bottom)};
        RECT img{0, 0, o.cap.w, o.cap.h};
        IntersectRect(&n, &n, &img);
        if (n.right - n.left >= 1 && n.bottom - n.top >= 1) o.sel = n;
        break;
    }
    case Drag::Draw: {
        auto& c = o.cur;
        if (c.tool == Tool::Pen || c.tool == Tool::Marker) {
            auto& last = c.pts.back();
            float dx = ip.x - last.x, dy = ip.y - last.y;
            if (dx * dx + dy * dy >= 0.25f / (o.zoom * o.zoom)) c.pts.push_back(ip);
        } else {
            c.pts[1] = shift ? Constrain(c.tool, c.pts[0], ip) : ip;
            if (c.tool == Tool::Blur) ComputePixelate(c, o.cap);
        }
        break;
    }
    case Drag::Pan:
        o.ox = o.panOx + (m.x - o.dragScreen.x);
        o.oy = o.panOy + (m.y - o.dragScreen.y);
        ClampView();
        break;
    case Drag::None:
        if (!o.editing) UpdateHover();
        else {
            LayoutToolbar();
            o.hoverBtn = HitButton(m);
        }
        break;
    }
    Redraw();
}

static void OnLButtonUp() {
    auto& o = g_ov;
    Drag d = o.drag;
    Trace("up drag=%d moved=%d mouse=%.0f,%.0f sel=%ld,%ld,%ld,%ld", (int)d, o.dragMoved, o.mouse.x, o.mouse.y, o.sel.left,
          o.sel.top, o.sel.right, o.sel.bottom);
    o.drag = Drag::None;
    ReleaseCapture();
    if (d == Drag::NewSel) {
        if (!o.dragMoved) {
            UpdateHover();
            if (o.hasHover) FixSelection(o.hover);
        } else {
            FixSelection(o.sel);
            if (!o.editing) ResetSelection();
        }
    } else if (d == Drag::Draw && o.drawing) {
        o.drawing = false;
        auto& c = o.cur;
        bool ok = true;
        if (c.pts.size() >= 2 && c.tool != Tool::Pen && c.tool != Tool::Marker) {
            float dx = c.pts[1].x - c.pts[0].x, dy = c.pts[1].y - c.pts[0].y;
            ok = std::fabs(dx) + std::fabs(dy) >= 3;
        }
        if (c.tool == Tool::Blur) {
            ComputePixelate(c, o.cap);
            ok = ok && !c.blocks.empty();
        }
        if (ok) PushShape(std::move(c));
        c = Shape{};
    }
    Redraw();
}

static void NudgeSelection(int dx, int dy, bool resize) {
    auto& o = g_ov;
    RECT r = o.sel;
    if (resize) {
        r.right = std::clamp(r.right + dx, r.left + 1, (LONG)o.cap.w);
        r.bottom = std::clamp(r.bottom + dy, r.top + 1, (LONG)o.cap.h);
    } else {
        int w = r.right - r.left, h = r.bottom - r.top;
        r.left = std::clamp(r.left + dx, 0L, (LONG)(o.cap.w - w));
        r.top = std::clamp(r.top + dy, 0L, (LONG)(o.cap.h - h));
        r.right = r.left + w;
        r.bottom = r.top + h;
    }
    o.sel = r;
    Redraw();
}

static bool OnTypingKey(WPARAM vk, bool ctrl, bool shift) {
    auto& o = g_ov;
    switch (vk) {
    case VK_ESCAPE: CommitTyping(); Redraw(); return true;
    case VK_RETURN:
        if (shift) o.text.text += L'\n';
        else if (ctrl) Finish(Action::Copy);
        else CommitTyping();
        Redraw();
        return true;
    case 'V':
        if (ctrl) {
            o.text.text += ReadClipboardText(g_mainWnd);
            Redraw();
            return true;
        }
        return false;
    case 'C':
        if (ctrl) { Finish(Action::Copy); return true; }
        return false;
    case 'S':
        if (ctrl) { Finish(shift ? Action::SaveAs : Action::Save); return true; }
        return false;
    case 'Z':
        if (ctrl) { Undo(); return true; }
        return false;
    case VK_OEM_4:
        if (ctrl) { ChangeSize(-1); return true; }
        return false;
    case VK_OEM_6:
        if (ctrl) { ChangeSize(1); return true; }
        return false;
    }
    return false;  // printable keys arrive as WM_CHAR
}

static void OnKeyDown(WPARAM vk) {
    auto& o = g_ov;
    Trace("key %02X editing=%d typing=%d", (unsigned)vk, o.editing, o.typing);
    bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0;
    if (o.typing && OnTypingKey(vk, ctrl, shift)) return;
    if (o.typing) return;
    if (ctrl) {
        switch (vk) {
        case 'C': if (o.editing) Finish(Action::Copy); else if (o.hasHover) { FixSelection(o.hover); Finish(Action::Copy); } return;
        case 'S': if (o.editing) Finish(shift ? Action::SaveAs : Action::Save); return;
        case 'Z': if (shift) RedoAction(); else Undo(); return;
        case 'Y': RedoAction(); return;
        case 'A': ResetSelection(); FixSelection({0, 0, o.cap.w, o.cap.h}); Redraw(); return;
        case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN:
            if (o.editing) {
                int st = shift ? 10 : 1;
                NudgeSelection(vk == VK_LEFT ? -st : vk == VK_RIGHT ? st : 0, vk == VK_UP ? -st : vk == VK_DOWN ? st : 0, true);
            }
            return;
        }
        return;
    }
    switch (vk) {
    case VK_ESCAPE: OverlayHide(); return;
    case VK_RETURN:
        if (o.editing) Finish(Action::Copy);
        else if (o.hasHover) { FixSelection(o.hover); Finish(Action::Copy); }
        return;
    case VK_SPACE:
        if (!o.editing && o.hasHover) { FixSelection(o.hover); Redraw(); }
        return;
    case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN: {
        int st = shift ? 10 : 1;
        int dx = vk == VK_LEFT ? -st : vk == VK_RIGHT ? st : 0, dy = vk == VK_UP ? -st : vk == VK_DOWN ? st : 0;
        if (o.editing) NudgeSelection(dx, dy, false);
        else {
            POINT p;
            GetCursorPos(&p);
            SetCursorPos(p.x + dx, p.y + dy);
        }
        return;
    }
    case VK_OEM_4: ChangeSize(-1); return;   // [
    case VK_OEM_6: ChangeSize(1); return;    // ]
    case VK_OEM_PLUS: case VK_ADD: ZoomAt(o.mouse, 1.5f); return;
    case VK_OEM_MINUS: case VK_SUBTRACT: ZoomAt(o.mouse, 1 / 1.5f); return;
    case '0': case VK_NUMPAD0: o.zoom = 1; ClampView(); UpdateHover(); Redraw(); return;
    case VK_DELETE: case VK_BACK: Undo(); return;
    }
    bool flipKey = vk == 'H' || (vk == 'V' && shift);
    if (!o.editing) {
        if (flipKey && o.hasHover) {  // mirror the highlighted window/monitor right away
            FixSelection(o.hover);
            FlipSelection(vk == 'V' || shift);
        } else if (vk == 'F') {
            POINT p{(LONG)ToImage(o.mouse).x, (LONG)ToImage(o.mouse).y};
            FixSelection(o.cap.monitors[o.cap.MonitorAt(p)]);
            Redraw();
        } else if (vk == 'C') {
            D2D1_POINT_2F ip = ToImage(o.mouse);
            uint32_t c = o.cap.Pixel((int)ip.x, (int)ip.y);
            wchar_t hex[16];
            swprintf_s(hex, L"#%02X%02X%02X", (c >> 16) & 255, (c >> 8) & 255, c & 255);
            OverlayHide();
            CopyTextToClipboard(g_mainWnd, hex);
        }
        return;
    }
    if (vk >= '1' && vk <= '0' + kColors) { SetColor((int)(vk - '1')); return; }
    if (vk >= VK_NUMPAD1 && vk < VK_NUMPAD1 + kColors) { SetColor((int)(vk - VK_NUMPAD1)); return; }
    if (flipKey) { FlipSelection(vk == 'V' || shift); return; }
    switch (vk) {
    case 'V': SetTool(Tool::Move); break;
    case 'P': SetTool(Tool::Pen); break;
    case 'L': SetTool(Tool::Line); break;
    case 'A': SetTool(Tool::Arrow); break;
    case 'R': SetTool(Tool::Rect); break;
    case 'E': SetTool(Tool::Ellipse); break;
    case 'M': SetTool(Tool::Marker); break;
    case 'T': SetTool(Tool::Text); break;
    case 'B': SetTool(Tool::Blur); break;
    case 'N': SetTool(Tool::Number); break;
    }
}

static LPCWSTR CursorFor(D2D1_POINT_2F m) {
    auto& o = g_ov;
    if (o.drag == Drag::Pan || o.drag == Drag::MoveSel) return IDC_SIZEALL;
    if (!o.editing) return IDC_CROSS;
    if (o.drag == Drag::Draw) return IDC_CROSS;
    LayoutToolbar();
    if (HitButton(m) >= 0) return IDC_HAND;
    if (InToolbar(m)) return IDC_ARROW;
    int e = o.drag == Drag::Resize ? o.edges : HitEdges(m);
    if (e == 3 || e == 12) return IDC_SIZENWSE;
    if (e == 6 || e == 9) return IDC_SIZENESW;
    if (e == 1 || e == 4) return IDC_SIZEWE;
    if (e == 2 || e == 8) return IDC_SIZENS;
    bool inside = InSel(m);
    if (!inside && o.shapes.empty()) return IDC_CROSS;
    if (o.tool == Tool::Move) return inside ? IDC_SIZEALL : IDC_CROSS;
    if (o.tool == Tool::Text) return IDC_IBEAM;
    return IDC_CROSS;
}

LRESULT CALLBACK OverlayProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto& o = g_ov;
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        if (o.active && o.rt && o.shot) {
            o.rt->BeginDraw();
            OverlayRender(o.rt.Get(), o.br.Get(), o.shot.Get());
            if (o.rt->EndDraw() == D2DERR_RECREATE_TARGET) {
                // device lost: rebuild everything device-dependent on the next paint
                for (auto& s : o.shapes) s.blockBmp.Reset();
                o.shot.Reset();
                o.br.Reset();
                o.rt.Reset();
                if (EnsureTarget()) InvalidateRect(hwnd, nullptr, FALSE);
            }
        }
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_DPICHANGED: return 0;  // the overlay spans monitors: keep physical geometry
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            SetCursor(LoadCursorW(nullptr, CursorFor(o.mouse)));
            return TRUE;
        }
        break;
    case WM_MOUSEMOVE:
        if (o.drag != Drag::None) Trace("move %d,%d drag=%d", GET_X_LPARAM(lp), GET_Y_LPARAM(lp), (int)o.drag);
        OnMouseMove(MousePt(lp), (wp & MK_SHIFT) != 0);
        return 0;
    case WM_LBUTTONDOWN: OnLButtonDown(MousePt(lp)); return 0;
    case WM_LBUTTONDBLCLK: {
        D2D1_POINT_2F m = MousePt(lp);
        bool quick = GetTickCount() - o.selTick < GetDoubleClickTime() + 150;
        if (o.editing && InSel(m) && o.shapes.empty() && (quick || o.tool == Tool::Move)) {
            Finish(Action::Copy);
            return 0;
        }
        OnLButtonDown(m);
        return 0;
    }
    case WM_LBUTTONUP: OnLButtonUp(); return 0;
    case WM_RBUTTONDOWN:
        if (o.drag != Drag::None) return 0;
        if (o.typing) { o.typing = false; o.text = Shape{}; Redraw(); }
        else if (o.editing) { ResetSelection(); Redraw(); }
        else OverlayHide();
        return 0;
    case WM_MBUTTONDOWN:
        if (o.drag == Drag::None) {
            o.drag = Drag::Pan;
            o.dragScreen = MousePt(lp);
            o.panOx = o.ox;
            o.panOy = o.oy;
            SetCapture(hwnd);
        }
        return 0;
    case WM_MBUTTONUP:
        if (o.drag == Drag::Pan) { o.drag = Drag::None; ReleaseCapture(); Redraw(); }
        return 0;
    case WM_MOUSEWHEEL: {
        POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(hwnd, &p);
        float delta = GET_WHEEL_DELTA_WPARAM(wp) / (float)WHEEL_DELTA;
        if (GET_KEYSTATE_WPARAM(wp) & MK_CONTROL) ChangeSize(delta > 0 ? 1 : -1);
        else ZoomAt({(float)p.x, (float)p.y}, std::pow(1.25f, delta));
        return 0;
    }
    case WM_CAPTURECHANGED:
        if ((HWND)lp != hwnd && o.drag != Drag::None && o.drag != Drag::Pan) OnLButtonUp();
        else if ((HWND)lp != hwnd) o.drag = Drag::None;
        return 0;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (o.active) OnKeyDown(wp);
        return 0;
    case WM_CHAR:
        if (o.typing) {
            wchar_t ch = (wchar_t)wp;
            if (ch == 8) {
                if (!o.text.text.empty()) o.text.text.pop_back();
            } else if (ch >= 32 && ch != 127) {
                o.text.text += ch;
            }
            Redraw();
        }
        return 0;
    case WM_TIMER:
        if (wp == 1) {
            KillTimer(hwnd, 1);
            OverlayHide();
            if (o.demoHold) PostQuitMessage(0);
        }
        return 0;
    case WM_DISPLAYCHANGE:
        if (o.active) OverlayHide();
        return 0;
    case WM_CLOSE:
        OverlayHide();
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
