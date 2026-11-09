// Foyer - power and account actions
#include "common.h"
#include <powrprof.h>
#include <wtsapi32.h>
#include <shellapi.h>
#define SECURITY_WIN32
#include <security.h>
#include <secext.h>

static bool EnableShutdownPrivilege() {
    HANDLE token;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) return false;
    TOKEN_PRIVILEGES tp{};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    bool ok = LookupPrivilegeValueW(nullptr, SE_SHUTDOWN_NAME, &tp.Privileges[0].Luid) &&
              AdjustTokenPrivileges(token, FALSE, &tp, 0, nullptr, nullptr) && GetLastError() == ERROR_SUCCESS;
    CloseHandle(token);
    return ok;
}

void PowerSleep() {
    Log(L"Power: sleep");
    SetSuspendState(FALSE, FALSE, FALSE);
}

void PowerHibernate() {
    Log(L"Power: hibernate");
    if (!SetSuspendState(TRUE, FALSE, FALSE)) {
        MessageBoxW(nullptr, L"Hibernate isn't available. It may be turned off on this PC (powercfg /hibernate on).",
                    L"Foyer", MB_ICONINFORMATION | MB_OK);
    }
}

void PowerShutdown() {
    Log(L"Power: shut down");
    EnableShutdownPrivilege();
    // SHUTDOWN_HYBRID matches the Start menu's behaviour when Fast Startup is on.
    DWORD r = InitiateShutdownW(nullptr, nullptr, 0, SHUTDOWN_POWEROFF | SHUTDOWN_HYBRID | SHUTDOWN_INSTALL_UPDATES,
                                SHTDN_REASON_MAJOR_OTHER | SHTDN_REASON_FLAG_PLANNED);
    if (r != ERROR_SUCCESS)
        InitiateShutdownW(nullptr, nullptr, 0, SHUTDOWN_POWEROFF, SHTDN_REASON_MAJOR_OTHER | SHTDN_REASON_FLAG_PLANNED);
}

void PowerRestart() {
    Log(L"Power: restart");
    EnableShutdownPrivilege();
    DWORD r = InitiateShutdownW(nullptr, nullptr, 0, SHUTDOWN_RESTART | SHUTDOWN_INSTALL_UPDATES,
                                SHTDN_REASON_MAJOR_OTHER | SHTDN_REASON_FLAG_PLANNED);
    if (r != ERROR_SUCCESS)
        InitiateShutdownW(nullptr, nullptr, 0, SHUTDOWN_RESTART, SHTDN_REASON_MAJOR_OTHER | SHTDN_REASON_FLAG_PLANNED);
}

void PowerRestartToFirmware(HWND owner) {
    if (MessageBoxW(owner, L"Restart now and open the BIOS/UEFI settings?\n\nSave your work first. Windows will ask for administrator permission.",
                    L"Foyer", MB_ICONQUESTION | MB_OKCANCEL) != IDOK) return;
    Log(L"Power: restart to firmware");
    ShellExecuteW(owner, L"runas", L"shutdown.exe", L"/r /fw /t 0", nullptr, SW_HIDE);
}

void AccountLock() { Log(L"Account: lock"); LockWorkStation(); }

void AccountSignOut() {
    Log(L"Account: sign out");
    ExitWindowsEx(EWX_LOGOFF, SHTDN_REASON_MAJOR_OTHER | SHTDN_REASON_FLAG_PLANNED);
}

void AccountSwitchUser() {
    Log(L"Account: switch user");
    WTSDisconnectSession(WTS_CURRENT_SERVER_HANDLE, WTS_CURRENT_SESSION, FALSE);
}

std::wstring CurrentUserDisplayName() {
    wchar_t buf[256];
    ULONG n = 256;
    if (GetUserNameExW(NameDisplay, buf, &n) && n > 1) return std::wstring(buf);
    DWORD m = 256;
    if (GetUserNameW(buf, &m)) return std::wstring(buf);
    return L"User";
}
