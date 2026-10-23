// Autostart via a Task Scheduler logon task that runs elevated ("highest privileges"), so the keyboard
// hook also sees keys typed into admin windows. Falls back to the HKCU Run key without elevation.
#include "common.h"
#include <taskschd.h>
#include <sddl.h>

static const wchar_t* kTaskName = L"Snip";
static const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

struct Bstr {
    BSTR s;
    explicit Bstr(const wchar_t* v) : s(SysAllocString(v)) {}
    ~Bstr() { SysFreeString(s); }
    operator BSTR() const { return s; }
};

static std::wstring ExePath() {
    wchar_t p[MAX_PATH];
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    return p;
}

bool IsElevated() {
    HANDLE token;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION e{};
    DWORD cb = sizeof(e);
    bool elevated = GetTokenInformation(token, TokenElevation, &e, sizeof(e), &cb) && e.TokenIsElevated;
    CloseHandle(token);
    return elevated;
}

static std::wstring UserSid() {
    std::wstring sid;
    HANDLE token;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return sid;
    DWORD cb = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &cb);
    std::vector<BYTE> buf(cb);
    LPWSTR str = nullptr;
    if (GetTokenInformation(token, TokenUser, buf.data(), cb, &cb) &&
        ConvertSidToStringSidW(((TOKEN_USER*)buf.data())->User.Sid, &str)) {
        sid = str;
        LocalFree(str);
    }
    CloseHandle(token);
    return sid;
}

static std::wstring XmlEscape(const std::wstring& s) {
    std::wstring r;
    for (wchar_t c : s) {
        if (c == L'&') r += L"&amp;";
        else if (c == L'<') r += L"&lt;";
        else if (c == L'>') r += L"&gt;";
        else r += c;
    }
    return r;
}

static ComPtr<ITaskFolder> RootFolder() {
    ComPtr<ITaskService> svc;
    if (FAILED(CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&svc)))) return nullptr;
    VARIANT empty;
    VariantInit(&empty);
    if (FAILED(svc->Connect(empty, empty, empty, empty))) return nullptr;
    ComPtr<ITaskFolder> root;
    svc->GetFolder(Bstr(L"\\"), &root);
    return root;
}

static bool RunKeyExists() {
    wchar_t v[MAX_PATH * 2];
    DWORD cb = sizeof(v);
    return RegGetValueW(HKEY_CURRENT_USER, kRunKey, APP_NAME, RRF_RT_REG_SZ, nullptr, v, &cb) == ERROR_SUCCESS;
}

static void DeleteRunKey() {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    RegDeleteValueW(k, APP_NAME);
    RegCloseKey(k);
}

static ComPtr<IRegisteredTask> FindTask() {
    ComPtr<IRegisteredTask> task;
    if (auto root = RootFolder()) root->GetTask(Bstr(kTaskName), &task);
    return task;
}

bool AutostartEnabled() { return RunKeyExists() || FindTask() != nullptr; }

bool InstallAutostartTask() {
    auto root = RootFolder();
    std::wstring sid = UserSid();
    if (!root || sid.empty()) return false;
    std::wstring exe = ExePath();
    std::wstring dir = exe.substr(0, exe.find_last_of(L'\\'));
    // Priority 4 = normal (tasks default to below-normal); no battery restrictions; no 72h time limit.
    std::wstring xml =
        L"<?xml version=\"1.0\" encoding=\"UTF-16\"?>"
        L"<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">"
        L"<RegistrationInfo><Description>Snip screenshot tool (Win+Shift+S / PrtSc)</Description></RegistrationInfo>"
        L"<Triggers><LogonTrigger><Enabled>true</Enabled><UserId>" + sid + L"</UserId></LogonTrigger></Triggers>"
        L"<Principals><Principal id=\"Author\"><UserId>" + sid + L"</UserId>"
        L"<LogonType>InteractiveToken</LogonType><RunLevel>HighestAvailable</RunLevel></Principal></Principals>"
        L"<Settings><MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>"
        L"<DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>"
        L"<StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>"
        L"<AllowHardTerminate>true</AllowHardTerminate><StartWhenAvailable>false</StartWhenAvailable>"
        L"<RunOnlyIfNetworkAvailable>false</RunOnlyIfNetworkAvailable>"
        L"<IdleSettings><StopOnIdleEnd>false</StopOnIdleEnd><RestartOnIdle>false</RestartOnIdle></IdleSettings>"
        L"<AllowStartOnDemand>true</AllowStartOnDemand><Enabled>true</Enabled><Hidden>false</Hidden>"
        L"<RunOnlyIfIdle>false</RunOnlyIfIdle><WakeToRun>false</WakeToRun>"
        L"<ExecutionTimeLimit>PT0S</ExecutionTimeLimit><Priority>4</Priority></Settings>"
        L"<Actions Context=\"Author\"><Exec><Command>" + XmlEscape(exe) + L"</Command>"
        L"<Arguments>--background</Arguments><WorkingDirectory>" + XmlEscape(dir) + L"</WorkingDirectory></Exec></Actions>"
        L"</Task>";
    VARIANT empty;
    VariantInit(&empty);
    ComPtr<IRegisteredTask> task;
    HRESULT hr = root->RegisterTask(Bstr(kTaskName), Bstr(xml.c_str()), TASK_CREATE_OR_UPDATE, empty, empty,
                                    TASK_LOGON_INTERACTIVE_TOKEN, empty, &task);
    if (FAILED(hr)) return false;
    DeleteRunKey();  // the task replaces the old non-elevated autostart
    return true;
}

bool StartAutostartTask() {
    auto task = FindTask();
    if (!task) return false;
    VARIANT empty;
    VariantInit(&empty);
    ComPtr<IRunningTask> running;
    return SUCCEEDED(task->Run(empty, &running));
}

bool RemoveAutostart() {
    DeleteRunKey();
    auto root = RootFolder();
    if (!root || !FindTask()) return true;
    return SUCCEEDED(root->DeleteTask(Bstr(kTaskName), 0));
}

DWORD RunSelfElevated(const wchar_t* args, bool wait) {
    std::wstring exe = ExePath();
    SHELLEXECUTEINFOW sei{sizeof(sei)};
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    sei.lpVerb = L"runas";
    sei.lpFile = exe.c_str();
    sei.lpParameters = args;
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei)) return 1;  // UAC declined
    DWORD code = 0;
    if (wait && sei.hProcess) {
        WaitForSingleObject(sei.hProcess, INFINITE);
        GetExitCodeProcess(sei.hProcess, &code);
    }
    if (sei.hProcess) CloseHandle(sei.hProcess);
    return code;
}
