// Foyer - installation, admin mode and accessibility plumbing
#include "common.h"
#include <functional>
#include "system.h"
#include <algorithm>
#include "config.h"
#include <shlobj.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <shldisp.h>
#include <exdisp.h>
#include <oleacc.h>
#include <propkey.h>
#include <propvarutil.h>
#define SECURITY_WIN32
#include <security.h>
#include <secext.h>

// ================================================================ elevation
bool ProcessIsElevated() {
    static int cached = -1;
    if (cached < 0) {
        cached = 0;
        HANDLE tok;
        if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
            TOKEN_ELEVATION e{}; DWORD sz;
            if (GetTokenInformation(tok, TokenElevation, &e, sizeof(e), &sz)) cached = e.TokenIsElevated ? 1 : 0;
            CloseHandle(tok);
        }
    }
    return cached == 1;
}

#pragma push_macro("ShellExecute")
#undef ShellExecute
// Raymond Chen's technique: ask the desktop's shell window (running as the user) to do the
// ShellExecute, so the new process isn't elevated.
static bool ExplorerShellExecute(const std::wstring& file, const std::wstring& args, const std::wstring& dir, const wchar_t* verb) {
    bool ok = false;
    IShellWindows* psw = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&psw))) || !psw) return false;
    VARIANT loc; VariantInit(&loc); loc.vt = VT_I4; loc.lVal = CSIDL_DESKTOP;
    VARIANT empty; VariantInit(&empty);
    long hwnd = 0;
    IDispatch* disp = nullptr;
    if (SUCCEEDED(psw->FindWindowSW(&loc, &empty, SWC_DESKTOP, &hwnd, SWFO_NEEDDISPATCH, &disp)) && disp) {
        IServiceProvider* sp = nullptr;
        if (SUCCEEDED(disp->QueryInterface(IID_PPV_ARGS(&sp))) && sp) {
            IShellBrowser* sb = nullptr;
            if (SUCCEEDED(sp->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&sb))) && sb) {
                IShellView* sv = nullptr;
                if (SUCCEEDED(sb->QueryActiveShellView(&sv)) && sv) {
                    IDispatch* bg = nullptr;
                    if (SUCCEEDED(sv->GetItemObject(SVGIO_BACKGROUND, IID_PPV_ARGS(&bg))) && bg) {
                        IShellFolderViewDual* fvd = nullptr;
                        if (SUCCEEDED(bg->QueryInterface(IID_PPV_ARGS(&fvd))) && fvd) {
                            IDispatch* app = nullptr;
                            if (SUCCEEDED(fvd->get_Application(&app)) && app) {
                                IShellDispatch2* sd = nullptr;
                                if (SUCCEEDED(app->QueryInterface(IID_PPV_ARGS(&sd))) && sd) {
                                    BSTR bFile = SysAllocString(file.c_str());
                                    VARIANT vArgs, vDir, vVerb, vShow;
                                    VariantInit(&vArgs); VariantInit(&vDir); VariantInit(&vVerb); VariantInit(&vShow);
                                    vArgs.vt = VT_BSTR; vArgs.bstrVal = SysAllocString(args.c_str());
                                    vDir.vt = VT_BSTR; vDir.bstrVal = SysAllocString(dir.c_str());
                                    vVerb.vt = VT_BSTR; vVerb.bstrVal = SysAllocString(verb ? verb : L"");
                                    vShow.vt = VT_I4; vShow.lVal = SW_SHOWNORMAL;
                                    ok = SUCCEEDED(sd->ShellExecute(bFile, vArgs, vDir, vVerb, vShow));
                                    SysFreeString(bFile);
                                    VariantClear(&vArgs); VariantClear(&vDir); VariantClear(&vVerb);
                                    sd->Release();
                                }
                                app->Release();
                            }
                            fvd->Release();
                        }
                        bg->Release();
                    }
                    sv->Release();
                }
                sb->Release();
            }
            sp->Release();
        }
        disp->Release();
    }
    psw->Release();
    return ok;
}

#pragma pop_macro("ShellExecute")

bool ShellExecAsUser(const std::wstring& file, const std::wstring& args, const std::wstring& dir, const wchar_t* verb) {
    bool runas = verb && _wcsicmp(verb, L"runas") == 0;
    if (ProcessIsElevated() && !runas && ExplorerShellExecute(file, args, dir, verb)) return true;
    SHELLEXECUTEINFOW sei{sizeof(sei)};
    sei.fMask = SEE_MASK_NOASYNC | SEE_MASK_INVOKEIDLIST;
    sei.lpVerb = verb;
    sei.lpFile = file.c_str();
    sei.lpParameters = args.empty() ? nullptr : args.c_str();
    sei.lpDirectory = dir.empty() ? nullptr : dir.c_str();
    sei.nShow = SW_SHOWNORMAL;
    return ShellExecuteExW(&sei) != FALSE;
}

static const wchar_t* kTaskName = L"Foyer (admin)";

static DWORD RunHidden(const std::wstring& exe, const std::wstring& args, DWORD timeout = 15000) {
    std::wstring cmd = L"\"" + exe + L"\" " + args;
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return (DWORD)-1;
    WaitForSingleObject(pi.hProcess, timeout);
    DWORD code = (DWORD)-1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code;
}

static std::wstring System32(const wchar_t* exe) {
    wchar_t dir[MAX_PATH];
    GetSystemDirectoryW(dir, MAX_PATH);
    return std::wstring(dir) + L"\\" + exe;
}

bool AdminModeTaskExists() {
    return RunHidden(System32(L"schtasks.exe"), L"/Query /TN \"" + std::wstring(kTaskName) + L"\"") == 0;
}

static std::wstring XmlEscape(const std::wstring& in) {
    std::wstring out;
    for (wchar_t c : in) {
        switch (c) {
            case L'&': out += L"&amp;"; break;
            case L'<': out += L"&lt;"; break;
            case L'>': out += L"&gt;"; break;
            case L'"': out += L"&quot;"; break;
            default: out += c;
        }
    }
    return out;
}

// A full task definition: no 3-day time limit, runs on battery, normal priority, only for
// this user when they sign in. (Plain "schtasks /Create" gets none of these right.)
static bool CreateAdminTask() {
    wchar_t user[512];
    ULONG n = 512;
    if (!GetUserNameExW(NameSamCompatible, user, &n)) { DWORD m = 512; GetUserNameW(user, &m); }
    std::wstring u = XmlEscape(user);
    std::wstring xml =
        L"<?xml version=\"1.0\" encoding=\"UTF-16\"?>\r\n"
        L"<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\r\n"
        L"  <RegistrationInfo><Author>Foyer</Author><Description>Starts Foyer as administrator when you sign in, so it can open over admin windows.</Description></RegistrationInfo>\r\n"
        L"  <Triggers><LogonTrigger><Enabled>true</Enabled><UserId>" + u + L"</UserId></LogonTrigger></Triggers>\r\n"
        L"  <Principals><Principal id=\"Author\"><UserId>" + u + L"</UserId><LogonType>InteractiveToken</LogonType><RunLevel>HighestAvailable</RunLevel></Principal></Principals>\r\n"
        L"  <Settings>\r\n"
        L"    <MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>\r\n"
        L"    <DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>\r\n"
        L"    <StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>\r\n"
        L"    <AllowHardTerminate>true</AllowHardTerminate>\r\n"
        L"    <StartWhenAvailable>false</StartWhenAvailable>\r\n"
        L"    <RunOnlyIfNetworkAvailable>false</RunOnlyIfNetworkAvailable>\r\n"
        L"    <IdleSettings><StopOnIdleEnd>false</StopOnIdleEnd><RestartOnIdle>false</RestartOnIdle></IdleSettings>\r\n"
        L"    <AllowStartOnDemand>true</AllowStartOnDemand>\r\n"
        L"    <Enabled>true</Enabled>\r\n"
        L"    <Hidden>false</Hidden>\r\n"
        L"    <RunOnlyIfIdle>false</RunOnlyIfIdle>\r\n"
        L"    <ExecutionTimeLimit>PT0S</ExecutionTimeLimit>\r\n"
        L"    <Priority>4</Priority>\r\n"
        L"  </Settings>\r\n"
        L"  <Actions Context=\"Author\"><Exec><Command>" + XmlEscape(ExePath()) + L"</Command><Arguments>--tray</Arguments></Exec></Actions>\r\n"
        L"</Task>\r\n";
    wchar_t tmpDir[MAX_PATH];
    GetTempPathW(MAX_PATH, tmpDir);
    std::wstring file = std::wstring(tmpDir) + L"foyer-admin-task.xml";
    HANDLE h = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0;
    const wchar_t bom = 0xFEFF;
    WriteFile(h, &bom, 2, &w, nullptr);
    WriteFile(h, xml.c_str(), (DWORD)(xml.size() * sizeof(wchar_t)), &w, nullptr);
    CloseHandle(h);
    DWORD code = RunHidden(System32(L"schtasks.exe"), L"/Create /F /TN \"" + std::wstring(kTaskName) + L"\" /XML \"" + file + L"\"");
    DeleteFileW(file.c_str());
    Log(L"Admin mode: create task -> %lu", code);
    return code == 0;
}

static bool DeleteAdminTask() {
    DWORD code = RunHidden(System32(L"schtasks.exe"), L"/Delete /F /TN \"" + std::wstring(kTaskName) + L"\"");
    Log(L"Admin mode: delete task -> %lu", code);
    return code == 0;
}

void RestartFoyer(const std::wstring& exe, const std::wstring& extraArgs, bool asUser) {
    std::wstring args = L"--wait-pid " + std::to_wstring(GetCurrentProcessId()) + L" " + extraArgs;
    std::wstring dir = exe.substr(0, exe.find_last_of(L'\\'));
    bool ok = asUser ? ShellExecAsUser(exe, args, dir) : ((INT_PTR)ShellExecuteW(nullptr, nullptr, exe.c_str(), args.c_str(), dir.c_str(), SW_SHOWNORMAL) > 32);
    if (ok) PostMessageW(MenuWindow(), WM_FOYER_QUIT, 0, 0);
    else Log(L"Restart failed (%lu)", GetLastError());
}

bool AdminModeEnable(HWND owner) {
    // The elevated copy creates the task, closes this copy and takes over.
    std::wstring args = L"--enable-admin-mode --wait-pid " + std::to_wstring(GetCurrentProcessId());
    SHELLEXECUTEINFOW sei{sizeof(sei)};
    sei.fMask = SEE_MASK_NOASYNC;
    sei.hwnd = owner;
    sei.lpVerb = L"runas";
    std::wstring exe = ExePath();
    sei.lpFile = exe.c_str();
    sei.lpParameters = args.c_str();
    sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei)) {
        Log(L"Admin mode: elevation declined or failed (%lu)", GetLastError());
        return false;
    }
    PostMessageW(MenuWindow(), WM_FOYER_QUIT, 0, 0);   // the elevated copy waits for us to exit
    return true;
}

bool AdminModeDisable(HWND owner) {
    if (!ProcessIsElevated()) {
        // Not elevated: deleting the task needs one UAC prompt.
        std::wstring args = L"/Delete /F /TN \"" + std::wstring(kTaskName) + L"\"";
        SHELLEXECUTEINFOW sei{sizeof(sei)};
        sei.fMask = SEE_MASK_NOASYNC | SEE_MASK_NOCLOSEPROCESS;
        sei.hwnd = owner;
        sei.lpVerb = L"runas";
        std::wstring exe = System32(L"schtasks.exe");
        sei.lpFile = exe.c_str();
        sei.lpParameters = args.c_str();
        sei.nShow = SW_HIDE;
        if (!ShellExecuteExW(&sei)) return false;
        if (sei.hProcess) { WaitForSingleObject(sei.hProcess, 15000); CloseHandle(sei.hProcess); }
        g_config.elevatedMode = false;
        ConfigSave();
        return true;
    }
    DeleteAdminTask();
    g_config.elevatedMode = false;
    ConfigSave();
    SetStartWithWindows(true);
    RestartFoyer(ExePath(), L"--tray", true);   // come back as the normal user
    return true;
}

static bool ArgValue(const wchar_t* cmdLine, const wchar_t* flag, std::wstring* value) {
    if (!cmdLine) return false;
    const wchar_t* p = wcsstr(cmdLine, flag);
    if (!p) return false;
    p += wcslen(flag);
    while (*p == L' ') ++p;
    std::wstring v;
    while (*p && *p != L' ') v += *p++;
    if (value) *value = v;
    return true;
}

static bool s_adminTaskCreated = false;

void ApplyStartupSwitches(const wchar_t* cmdLine) {
    if (!cmdLine) return;
    if (s_adminTaskCreated) { g_config.elevatedMode = true; ConfigSave(); }
    if (wcsstr(cmdLine, L"--startup") && !g_config.elevatedMode) SetStartWithWindows(true);   // path now points at this copy
    if (wcsstr(cmdLine, L"--installed")) Log(L"Running the installed copy");
}

bool HandleStartupSwitches(const wchar_t* cmdLine) {
    std::wstring pid;
    if (ArgValue(cmdLine, L"--wait-pid", &pid)) {
        // Another Foyer is handing over to us: let it finish exiting first.
        if (HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, (DWORD)_wtol(pid.c_str()))) {
            WaitForSingleObject(h, 10000);
            CloseHandle(h);
        }
    }
    if (cmdLine && wcsstr(cmdLine, L"--enable-admin-mode")) {
        LogInit();
        if (!ProcessIsElevated()) return false;
        s_adminTaskCreated = CreateAdminTask();
        if (s_adminTaskCreated) {
            // the normal Run entry would start a second, unelevated copy at sign-in
            HKEY key;
            if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_SET_VALUE, &key) == ERROR_SUCCESS) {
                RegDeleteValueW(key, L"Foyer");
                RegCloseKey(key);
            }
            Log(L"Admin mode enabled; this copy takes over");
        } else {
            MessageBoxW(nullptr, L"Foyer couldn't set itself up to start as administrator at sign-in. It's running as administrator for now.",
                        L"Foyer", MB_ICONWARNING);
        }
        return false;   // carry on as the (elevated) main instance
    }
    if (cmdLine && wcsstr(cmdLine, L"--readmin") && ProcessIsElevated()) {
        LogInit();
        s_adminTaskCreated = CreateAdminTask();   // the installed copy re-points the task at itself
    }
    if (cmdLine && wcsstr(cmdLine, L"--uninstall")) {
        LogInit();
        ExitProcess((UINT)UninstallFoyer());
    }
    return false;
}

// ================================================================ install
static std::wstring KnownFolderPath(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::wstring r;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_CREATE, nullptr, &p)) && p) r = p;
    CoTaskMemFree(p);
    return r;
}

std::wstring InstallDir() { return KnownFolderPath(FOLDERID_UserProgramFiles) + L"\\Foyer"; }

bool IsInstalledCopy() { return _wcsicmp(ExeDir().c_str(), InstallDir().c_str()) == 0; }

bool IsPortableCopy() {
    return GetFileAttributesW((ExeDir() + L"\\portable").c_str()) != INVALID_FILE_ATTRIBUTES ||
           GetFileAttributesW((ExeDir() + L"\\portable.txt").c_str()) != INVALID_FILE_ATTRIBUTES;
}

static std::wstring ShortcutPath() { return KnownFolderPath(FOLDERID_Programs) + L"\\Foyer.lnk"; }
static const wchar_t* kUninstallKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Foyer";

static bool MakeShortcut(const std::wstring& lnk, const std::wstring& target) {
    IShellLinkW* link = nullptr;
    IPersistFile* pf = nullptr;
    bool ok = false;
    if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))) && link) {
        link->SetPath(target.c_str());
        link->SetWorkingDirectory(target.substr(0, target.find_last_of(L'\\')).c_str());
        link->SetDescription(L"Foyer - a fast, clean Start menu");
        link->SetIconLocation(target.c_str(), 0);
        if (SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&pf))) && pf) ok = SUCCEEDED(pf->Save(lnk.c_str(), TRUE));
    }
    if (pf) pf->Release();
    if (link) link->Release();
    return ok;
}

static void SetStr(HKEY k, const wchar_t* name, const std::wstring& v) {
    RegSetValueExW(k, name, 0, REG_SZ, (const BYTE*)v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t)));
}

static void RegisterUninstall(const std::wstring& exe) {
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kUninstallKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) != ERROR_SUCCESS) return;
    SetStr(k, L"DisplayName", L"Foyer");
    SetStr(k, L"DisplayVersion", FOYER_VERSION);
    SetStr(k, L"Publisher", L"larosa023");
    SetStr(k, L"DisplayIcon", exe + L",0");
    SetStr(k, L"InstallLocation", exe.substr(0, exe.find_last_of(L'\\')));
    SetStr(k, L"UninstallString", L"\"" + exe + L"\" --uninstall");
    SetStr(k, L"URLInfoAbout", L"https://github.com/larosa023/Foyer");
    DWORD one = 1, size = 1024;
    RegSetValueExW(k, L"NoModify", 0, REG_DWORD, (const BYTE*)&one, sizeof(one));
    RegSetValueExW(k, L"NoRepair", 0, REG_DWORD, (const BYTE*)&one, sizeof(one));
    RegSetValueExW(k, L"EstimatedSize", 0, REG_DWORD, (const BYTE*)&size, sizeof(size));
    RegCloseKey(k);
}

bool InstallFoyer(HWND owner, std::wstring* error) {
    std::wstring dir = InstallDir();
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    std::wstring target = dir + L"\\Foyer.exe";
    if (_wcsicmp(target.c_str(), ExePath().c_str()) != 0) {
        MoveFileExW(target.c_str(), (target + L".old").c_str(), MOVEFILE_REPLACE_EXISTING);
        if (!CopyFileW(ExePath().c_str(), target.c_str(), FALSE)) {
            if (error) *error = L"Couldn't copy Foyer into " + dir + L" (error " + std::to_wstring(GetLastError()) + L").";
            MoveFileExW((target + L".old").c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING);
            return false;
        }
        DeleteFileW((target + L".old").c_str());
    }
    MakeShortcut(ShortcutPath(), target);
    RegisterUninstall(target);
    bool startup = StartWithWindowsEnabled();
    Log(L"Installed to %ls", dir.c_str());
    if (_wcsicmp(target.c_str(), ExePath().c_str()) == 0) return true;
    // hand over to the installed copy (it re-registers start-up and admin mode with its own path)
    std::wstring extra = L"--tray --installed";
    if (startup) extra += L" --startup";
    if (g_config.elevatedMode) extra += L" --readmin";
    RestartFoyer(target, extra, !g_config.elevatedMode);
    (void)owner;
    return true;
}

void MaybeOfferInstall(HWND owner) {
    if (IsInstalledCopy() || IsPortableCopy() || g_config.askedInstall) return;
    g_config.askedInstall = true;
    ConfigSave();
    int r = MessageBoxW(owner,
        L"Install Foyer on this PC?\n\n"
        L"Foyer will copy itself to your Programs folder, add itself to the Start menu and Apps & features "
        L"(so it can be uninstalled like any other app), and keep updating itself there.\n\n"
        L"Choose No to keep running it from where it is now. You can install it later from Settings \x2192 About.",
        L"Foyer", MB_YESNO | MB_ICONQUESTION | MB_SETFOREGROUND);
    if (r != IDYES) return;
    std::wstring err;
    if (!InstallFoyer(owner, &err)) MessageBoxW(owner, err.c_str(), L"Foyer", MB_ICONWARNING);
}

int UninstallFoyer() {
    if (MessageBoxW(nullptr, L"Uninstall Foyer?\n\nThe Windows Start menu will work exactly as before.", L"Uninstall Foyer",
                    MB_OKCANCEL | MB_ICONQUESTION | MB_SETFOREGROUND) != IDOK) return 1;
    bool keepSettings = MessageBoxW(nullptr, L"Keep your Foyer settings, pins and themes in case you reinstall?", L"Uninstall Foyer",
                                    MB_YESNO | MB_ICONQUESTION | MB_SETFOREGROUND) == IDYES;
    // close the running copy
    if (HWND w = FindWindowW(L"FoyerMenu", nullptr)) {
        DWORD pid = 0;
        GetWindowThreadProcessId(w, &pid);
        PostMessageW(w, WM_FOYER_QUIT, 0, 0);
        if (HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid)) { WaitForSingleObject(h, 8000); CloseHandle(h); }
    }
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_SET_VALUE, &key) == ERROR_SUCCESS) {
        RegDeleteValueW(key, L"Foyer");
        RegCloseKey(key);
    }
    if (AdminModeTaskExists()) {
        SHELLEXECUTEINFOW sei{sizeof(sei)};
        sei.fMask = SEE_MASK_NOASYNC | SEE_MASK_NOCLOSEPROCESS;
        sei.lpVerb = L"runas";
        std::wstring exe = System32(L"schtasks.exe"), args = L"/Delete /F /TN \"" + std::wstring(kTaskName) + L"\"";
        sei.lpFile = exe.c_str();
        sei.lpParameters = args.c_str();
        sei.nShow = SW_HIDE;
        if (ShellExecuteExW(&sei) && sei.hProcess) { WaitForSingleObject(sei.hProcess, 15000); CloseHandle(sei.hProcess); }
    }
    DeleteFileW(ShortcutPath().c_str());
    RegDeleteTreeW(HKEY_CURRENT_USER, kUninstallKey);
    std::wstring exe = ExePath(), dir = ExeDir(), data = DataDir();
    std::wstring cmd = L"cmd.exe /c timeout /t 2 /nobreak >nul & del /f /q \"" + exe + L"\" \"" + exe + L".old\" \"" + exe + L".bad\"";
    if (IsInstalledCopy()) cmd += L" & rmdir \"" + dir + L"\"";
    if (!keepSettings && !IsPortableCopy()) cmd += L" & rmdir /s /q \"" + data + L"\"";
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    Log(L"Uninstalled");
    MessageBoxW(nullptr, L"Foyer was uninstalled. Thanks for trying it!", L"Foyer", MB_ICONINFORMATION | MB_SETFOREGROUND);
    return 0;
}

// ================================================================ accessibility
// A minimal MSAA object for the custom-drawn menu: the window plus one child, the item
// that has keyboard focus. Narrator (through the UIA bridge) reads its name and role
// whenever we raise a focus event.
namespace a11y {

struct State {
    std::wstring name, description, value;
    RECT rect{};
    bool searchBox = true;
    std::function<void()> activate;
};
static State s_state;

class MenuAccessible final : public IAccessible {
    LONG ref_ = 1;
    HWND hwnd_;
public:
    explicit MenuAccessible(HWND h) : hwnd_(h) {}
    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == IID_IDispatch || riid == IID_IAccessible) { *ppv = static_cast<IAccessible*>(this); AddRef(); return S_OK; }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&ref_); }
    ULONG STDMETHODCALLTYPE Release() override { LONG r = InterlockedDecrement(&ref_); if (!r) delete this; return r; }
    // IDispatch
    HRESULT STDMETHODCALLTYPE GetTypeInfoCount(UINT* n) override { *n = 0; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetTypeInfo(UINT, LCID, ITypeInfo**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetIDsOfNames(REFIID, LPOLESTR*, UINT, LCID, DISPID*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Invoke(DISPID, REFIID, LCID, WORD, DISPPARAMS*, VARIANT*, EXCEPINFO*, UINT*) override { return E_NOTIMPL; }
    // IAccessible
    static bool Self(const VARIANT& v) { return v.vt == VT_I4 && v.lVal == CHILDID_SELF; }
    static bool Child(const VARIANT& v) { return v.vt == VT_I4 && v.lVal == 1; }
    HRESULT STDMETHODCALLTYPE get_accParent(IDispatch** p) override {
        return AccessibleObjectFromWindow(hwnd_, (DWORD)OBJID_WINDOW, IID_IDispatch, (void**)p);
    }
    HRESULT STDMETHODCALLTYPE get_accChildCount(long* n) override { *n = 1; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_accChild(VARIANT, IDispatch** p) override { *p = nullptr; return S_FALSE; }
    HRESULT STDMETHODCALLTYPE get_accName(VARIANT v, BSTR* out) override {
        if (Self(v)) { *out = SysAllocString(L"Foyer Start menu"); return S_OK; }
        if (Child(v)) { *out = SysAllocString(s_state.name.c_str()); return S_OK; }
        return E_INVALIDARG;
    }
    HRESULT STDMETHODCALLTYPE get_accValue(VARIANT v, BSTR* out) override {
        if (Child(v) && s_state.searchBox) { *out = SysAllocString(s_state.value.c_str()); return S_OK; }
        *out = nullptr;
        return S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE get_accDescription(VARIANT v, BSTR* out) override {
        if (Child(v) && !s_state.description.empty()) { *out = SysAllocString(s_state.description.c_str()); return S_OK; }
        *out = nullptr;
        return S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE get_accRole(VARIANT v, VARIANT* out) override {
        VariantInit(out);
        out->vt = VT_I4;
        if (Self(v)) out->lVal = ROLE_SYSTEM_DIALOG;
        else if (Child(v)) out->lVal = s_state.searchBox ? ROLE_SYSTEM_TEXT : ROLE_SYSTEM_LISTITEM;
        else return E_INVALIDARG;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_accState(VARIANT v, VARIANT* out) override {
        VariantInit(out);
        out->vt = VT_I4;
        out->lVal = Self(v) ? STATE_SYSTEM_FOCUSABLE
                            : (STATE_SYSTEM_FOCUSABLE | STATE_SYSTEM_FOCUSED | (s_state.searchBox ? 0 : (STATE_SYSTEM_SELECTABLE | STATE_SYSTEM_SELECTED)));
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_accHelp(VARIANT, BSTR* out) override { *out = nullptr; return S_FALSE; }
    HRESULT STDMETHODCALLTYPE get_accHelpTopic(BSTR* f, VARIANT, long*) override { *f = nullptr; return S_FALSE; }
    HRESULT STDMETHODCALLTYPE get_accKeyboardShortcut(VARIANT, BSTR* out) override { *out = nullptr; return S_FALSE; }
    HRESULT STDMETHODCALLTYPE get_accFocus(VARIANT* out) override { VariantInit(out); out->vt = VT_I4; out->lVal = 1; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_accSelection(VARIANT* out) override { VariantInit(out); out->vt = VT_I4; out->lVal = 1; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_accDefaultAction(VARIANT v, BSTR* out) override {
        if (Child(v) && !s_state.searchBox) { *out = SysAllocString(L"Open"); return S_OK; }
        *out = nullptr;
        return S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE accSelect(long, VARIANT) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE accLocation(long* x, long* y, long* w, long* h, VARIANT v) override {
        RECT r{};
        if (Child(v)) r = s_state.rect;
        else GetWindowRect(hwnd_, &r);
        *x = r.left; *y = r.top; *w = r.right - r.left; *h = r.bottom - r.top;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE accNavigate(long, VARIANT, VARIANT* out) override { VariantInit(out); return S_FALSE; }
    HRESULT STDMETHODCALLTYPE accHitTest(long, long, VARIANT* out) override { VariantInit(out); out->vt = VT_I4; out->lVal = CHILDID_SELF; return S_OK; }
    HRESULT STDMETHODCALLTYPE accDoDefaultAction(VARIANT v) override {
        if (Child(v) && s_state.activate) { s_state.activate(); return S_OK; }
        return DISP_E_MEMBERNOTFOUND;
    }
    HRESULT STDMETHODCALLTYPE put_accName(VARIANT, BSTR) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE put_accValue(VARIANT, BSTR) override { return E_NOTIMPL; }
};

static MenuAccessible* s_acc = nullptr;

void Attach(HWND hwnd, std::function<void()> activate) {
    s_state.activate = std::move(activate);
    if (!s_acc) s_acc = new MenuAccessible(hwnd);
}

LRESULT OnGetObject(HWND hwnd, WPARAM wParam, LPARAM lParam) {
    if ((LONG)lParam == (LONG)OBJID_CLIENT && s_acc) return LresultFromObject(IID_IAccessible, wParam, s_acc);
    return DefWindowProcW(hwnd, WM_GETOBJECT, wParam, lParam);
}

void Focus(HWND hwnd, const std::wstring& name, const std::wstring& description, RECT screen, bool isSearchBox, const std::wstring& value) {
    bool changed = name != s_state.name || description != s_state.description || isSearchBox != s_state.searchBox || value != s_state.value;
    s_state.name = name;
    s_state.description = description;
    s_state.rect = screen;
    s_state.searchBox = isSearchBox;
    s_state.value = value;
    if (!changed) return;
    NotifyWinEvent(EVENT_OBJECT_FOCUS, hwnd, OBJID_CLIENT, 1);
    if (isSearchBox) NotifyWinEvent(EVENT_OBJECT_VALUECHANGE, hwnd, OBJID_CLIENT, 1);
}

bool HighContrast() {
    HIGHCONTRASTW hc{sizeof(hc)};
    return SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, 0) && (hc.dwFlags & HCF_HIGHCONTRASTON);
}

float TextScale() {
    DWORD v = 100, size = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Accessibility", L"TextScaleFactor", RRF_RT_REG_DWORD, nullptr, &v, &size) != ERROR_SUCCESS) return 1.0f;
    return std::clamp(v / 100.0f, 1.0f, 2.25f);
}

bool ReduceMotion() {
    BOOL on = TRUE;
    return SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &on, 0) && !on;
}

} // namespace a11y
