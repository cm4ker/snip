// Entry point: single instance, tray icon, global keyboard hook (Win+Shift+S, PrtSc).
#include "overlay.h"

HWND g_mainWnd = nullptr;
static HINSTANCE g_inst;
static NOTIFYICONDATAW g_nid{};
static UINT g_taskbarCreated;
static HICON g_icon;
static std::wstring g_lastSaved;
static volatile bool g_hookWinShiftS = true, g_hookPrtSc = true;
static const ULONG_PTR kInjectTag = 0x534E4950;  // marks our own SendInput events

enum { ID_CAPTURE = 1, ID_FOLDER, ID_HOOK_WSS, ID_HOOK_PRTSC, ID_AUTOSTART, ID_EXIT };

// ---------------------------------------------------------------- settings

static int RegGetInt(const wchar_t* name, int def) {
    DWORD v = 0, cb = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Snip", name, RRF_RT_REG_DWORD, nullptr, &v, &cb) == ERROR_SUCCESS) return (int)v;
    return def;
}

static void RegSetInt(const wchar_t* name, int value) {
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Snip", 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) != ERROR_SUCCESS) return;
    DWORD v = (DWORD)value;
    RegSetValueExW(k, name, 0, REG_DWORD, (BYTE*)&v, 4);
    RegCloseKey(k);
}

static void ToggleAutostart() {
    // changing an elevated task needs elevation: from a normal instance ask UAC via a helper process
    bool on = AutostartEnabled();
    if (IsElevated()) {
        if (on) RemoveAutostart();
        else InstallAutostartTask();
    } else {
        RunSelfElevated(on ? L"--uninstall-task" : L"--install-task", false);
    }
}

// Replaces a running (possibly non-elevated) instance with one started by the elevated logon task.
static void RestartViaTask() {
    if (HWND other = FindWindowW(MAIN_CLASS, nullptr)) {
        DWORD pid = 0;
        GetWindowThreadProcessId(other, &pid);
        HANDLE proc = OpenProcess(SYNCHRONIZE, FALSE, pid);
        PostMessageW(other, WM_APP_EXIT, 0, 0);
        PostMessageW(other, WM_CLOSE, 0, 0);
        if (proc) {
            WaitForSingleObject(proc, 5000);
            CloseHandle(proc);
        }
    }
    StartAutostartTask();
}

// ---------------------------------------------------------------- keyboard hook (own thread: never stalls input)

static bool Down(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

static void SendMaskKey() {
    // An unassigned virtual key while Win is held: the shell then treats Win as part of a combo
    // and does not open the Start menu when Win is released (same trick AutoHotkey uses).
    INPUT in[2]{};
    for (int i = 0; i < 2; i++) {
        in[i].type = INPUT_KEYBOARD;
        in[i].ki.wVk = 0xE8;
        in[i].ki.dwExtraInfo = kInjectTag;
    }
    in[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, in, sizeof(INPUT));
}

static LRESULT CALLBACK KeyboardProc(int code, WPARAM wp, LPARAM lp) {
    static bool sHeld = false, prtHeld = false;
    if (code == HC_ACTION) {
        auto* k = (KBDLLHOOKSTRUCT*)lp;
        if (k->dwExtraInfo != kInjectTag) {
            bool down = wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN;
            bool win = Down(VK_LWIN) || Down(VK_RWIN);
            bool ctrl = Down(VK_CONTROL), alt = Down(VK_MENU), shift = Down(VK_SHIFT);
            if (k->vkCode == 'S') {
                if (down && g_hookWinShiftS && win && shift && !ctrl && !alt) {
                    if (!sHeld) {
                        sHeld = true;
                        SendMaskKey();
                        PostMessageW(g_mainWnd, WM_APP_CAPTURE, 0, 0);
                    }
                    return 1;
                }
                if (!down && sHeld) {
                    sHeld = false;
                    return 1;
                }
            } else if (k->vkCode == VK_SNAPSHOT) {
                if (down && g_hookPrtSc && !win && !alt) {
                    if (!prtHeld) {
                        prtHeld = true;
                        PostMessageW(g_mainWnd, WM_APP_CAPTURE, 0, 0);
                    }
                    return 1;
                }
                if (!down && prtHeld) {
                    prtHeld = false;
                    return 1;
                }
            }
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

static DWORD WINAPI HookThread(LPVOID) {
    HHOOK hook = SetWindowsHookExW(WH_KEYBOARD_LL, KeyboardProc, g_inst, 0);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {}
    if (hook) UnhookWindowsHookEx(hook);
    return 0;
}

// ---------------------------------------------------------------- tray

static void AddTrayIcon() {
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_mainWnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_APP_TRAY;
    g_nid.hIcon = g_icon;
    wcscpy_s(g_nid.szTip, IsElevated() ? L"Snip — Win+Shift+S / PrtSc (администратор)" : L"Snip — Win+Shift+S / PrtSc");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}

static void Balloon(const wchar_t* title, const std::wstring& text) {
    NOTIFYICONDATAW n = g_nid;
    n.uFlags = NIF_INFO;
    n.dwInfoFlags = NIIF_USER | NIIF_NOSOUND;
    n.hBalloonIcon = g_icon;
    wcscpy_s(n.szInfoTitle, title);
    wcsncpy_s(n.szInfo, text.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &n);
}

void NotifySaved(const std::wstring& path) {
    g_lastSaved = path;
    Balloon(L"Снимок сохранён", path.substr(path.find_last_of(L'\\') + 1) + L"\nНажмите, чтобы открыть папку");
}

static void OpenFolder(const std::wstring& select) {
    if (!select.empty() && GetFileAttributesW(select.c_str()) != INVALID_FILE_ATTRIBUTES) {
        std::wstring args = L"/select,\"" + select + L"\"";
        ShellExecuteW(nullptr, nullptr, L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
    } else {
        ShellExecuteW(nullptr, L"open", ScreenshotsFolder().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
}

static void ShowMenu() {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, ID_CAPTURE, L"Снимок экрана\tWin+Shift+S");
    AppendMenuW(m, MF_STRING, ID_FOLDER, L"Открыть папку снимков");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (g_hookWinShiftS ? MF_CHECKED : 0), ID_HOOK_WSS, L"Заменить Win+Shift+S");
    AppendMenuW(m, MF_STRING | (g_hookPrtSc ? MF_CHECKED : 0), ID_HOOK_PRTSC, L"Заменить PrtSc");
    AppendMenuW(m, MF_STRING | (AutostartEnabled() ? MF_CHECKED : 0), ID_AUTOSTART,
                L"Запускать вместе с Windows (с правами администратора)");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_EXIT, L"Выход");
    SetMenuDefaultItem(m, ID_CAPTURE, FALSE);
    POINT p;
    GetCursorPos(&p);
    SetForegroundWindow(g_mainWnd);
    TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, p.x, p.y, 0, g_mainWnd, nullptr);
    PostMessageW(g_mainWnd, WM_NULL, 0, 0);
    DestroyMenu(m);
}

static LRESULT CALLBACK MainProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == g_taskbarCreated && g_taskbarCreated) {
        AddTrayIcon();
        return 0;
    }
    switch (msg) {
    case WM_APP_CAPTURE:
        OverlayStart();
        return 0;
    case WM_APP_TRAY:
        switch (LOWORD(lp)) {
        case WM_LBUTTONUP: OverlayStart(); break;
        case WM_RBUTTONUP: ShowMenu(); break;
        case NIN_BALLOONUSERCLICK: OpenFolder(g_lastSaved); break;
        }
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_CAPTURE: OverlayStart(); break;
        case ID_FOLDER: OpenFolder(g_lastSaved); break;
        case ID_HOOK_WSS: g_hookWinShiftS = !g_hookWinShiftS; RegSetInt(L"HookWinShiftS", g_hookWinShiftS); break;
        case ID_HOOK_PRTSC: g_hookPrtSc = !g_hookPrtSc; RegSetInt(L"HookPrtSc", g_hookPrtSc); break;
        case ID_AUTOSTART: ToggleAutostart(); break;
        case ID_EXIT: DestroyWindow(hwnd); break;
        }
        return 0;
    case WM_APP_EXIT:
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        OverlayHide();
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------- entry

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    g_inst = inst;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    int argc = 0;
    PWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring cmd = argc > 1 ? argv[1] : L"";
    std::wstring arg2 = argc > 2 ? argv[2] : L"";

    if (!GfxInit()) return 1;

    if (cmd == L"--selftest") {
        if (arg2.empty()) arg2 = L".";
        return SelfTest(arg2);
    }

    if (cmd == L"--install-task" || cmd == L"--uninstall-task") {
        if (!IsElevated()) return (int)RunSelfElevated(cmd.c_str(), true);
        if (cmd == L"--uninstall-task") return RemoveAutostart() ? 0 : 1;
        if (!InstallAutostartTask()) return 1;
        RestartViaTask();
        return 0;
    }

    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\SnipSingleInstance");
    DWORD mutexErr = GetLastError();
    // ACCESS_DENIED: the running instance is elevated and we are not
    if (mutexErr == ERROR_ALREADY_EXISTS || mutexErr == ERROR_ACCESS_DENIED) {
        HWND other = nullptr;
        for (int i = 0; i < 50 && !(other = FindWindowW(MAIN_CLASS, nullptr)); i++) Sleep(20);
        if (other) {
            if (cmd == L"--exit") {
                PostMessageW(other, WM_APP_EXIT, 0, 0);
                PostMessageW(other, WM_CLOSE, 0, 0);
            } else if (cmd != L"--background") {
                PostMessageW(other, WM_APP_CAPTURE, 0, 0);
            }
        }
        return 0;
    }
    if (cmd == L"--exit") return 0;

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = MainProc;
    wc.hInstance = inst;
    wc.lpszClassName = MAIN_CLASS;
    RegisterClassExW(&wc);
    g_mainWnd = CreateWindowExW(WS_EX_TOOLWINDOW, MAIN_CLASS, APP_NAME, WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, inst, nullptr);
    if (!g_mainWnd || !OverlayInit(inst)) return 1;

    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    // when elevated, UIPI drops messages from normal processes (explorer, `snip --exit`) unless allowed
    for (UINT m : {WM_APP_CAPTURE, WM_APP_EXIT, WM_APP_TRAY, g_taskbarCreated})
        ChangeWindowMessageFilterEx(g_mainWnd, m, MSGFLT_ALLOW, nullptr);
    g_icon = CreateAppIcon(GetSystemMetricsForDpi(SM_CXSMICON, GetDpiForSystem()));
    if (!g_icon) g_icon = LoadIconW(nullptr, IDI_APPLICATION);
    g_hookWinShiftS = RegGetInt(L"HookWinShiftS", 1) != 0;
    g_hookPrtSc = RegGetInt(L"HookPrtSc", 1) != 0;
    if (cmd != L"--demo") {
        AddTrayIcon();
        CloseHandle(CreateThread(nullptr, 0, HookThread, nullptr, 0, nullptr));
    }

    OverlayPrewarm();
    if (cmd == L"--capture") PostMessageW(g_mainWnd, WM_APP_CAPTURE, 0, 0);
    else if (cmd == L"--demo") OverlayDemo(arg2.empty() ? 3000 : (DWORD)_wtoi(arg2.c_str()));
    else if (cmd.empty() && RegGetInt(L"Welcomed", 0) == 0) {
        RegSetInt(L"Welcomed", 1);
        Balloon(L"Snip работает в трее", L"Win+Shift+S или PrtSc — снимок. Правый клик по иконке — настройки.");
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (g_nid.hWnd) Shell_NotifyIconW(NIM_DELETE, &g_nid);
    LocalFree(argv);
    ReleaseMutex(mutex);
    return 0;
}
