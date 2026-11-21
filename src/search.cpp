// Foyer - search providers
#include "search.h"
#include "json.h"
#include "updater.h"
#include "apps.h"
#include "system.h"
#include <shlobj.h>
#include <shellapi.h>
#include <algorithm>
#include <cmath>
#include <ctime>
#include <map>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <functional>

namespace search {

static std::wstring Trim(const std::wstring& s) {
    size_t a = s.find_first_not_of(L" \t"), b = s.find_last_not_of(L" \t");
    return a == std::wstring::npos ? L"" : s.substr(a, b - a + 1);
}

Query Parse(const std::wstring& raw) {
    Query q;
    std::wstring t = raw;
    size_t lead = t.find_first_not_of(L' ');
    t = lead == std::wstring::npos ? L"" : t.substr(lead);
    if (!t.empty() && t[0] == L'>') { q.scope = Scope::Run; t = t.substr(1); }
    else if (t.size() >= 2 && t[1] == L':' && (t.size() == 2 || t[2] != L'\\')) {   // "f:" but not "C:\"
        switch (towlower(t[0])) {
            case L'f': q.scope = Scope::Files; t = t.substr(2); break;
            case L's': q.scope = Scope::Settings; t = t.substr(2); break;
            case L'a': q.scope = Scope::Apps; t = t.substr(2); break;
        }
    }
    q.text = Trim(t);
    q.lower = ToLower(q.text);
    return q;
}

static bool IsSep(wchar_t c) { return c == L' ' || c == L'-' || c == L'_' || c == L'.' || c == L'(' || c == L'[' || c == L'&' || c == L','; }

static int OneWord(const std::wstring& n, const std::wstring& w) {
    if (w.empty()) return 0;
    if (n.compare(0, w.size(), w) == 0) return n.size() == w.size() ? 1000 : 900;
    for (size_t i = 1; i + w.size() <= n.size(); ++i)
        if (IsSep(n[i - 1]) && n.compare(i, w.size(), w) == 0) return 750;
    // camelCase / digits boundaries are covered by "contains"
    if (n.find(w) != std::wstring::npos) return 500;
    return -1;
}

int MatchScore(const std::wstring& n, const std::wstring& q) {
    if (q.empty()) return -1;
    int whole = OneWord(n, q);
    if (whole >= 0) return whole - (int)std::min<size_t>(n.size(), 120) / 2;
    // initials: "vsc" -> "visual studio code"
    if (q.size() >= 2 && q.find(L' ') == std::wstring::npos) {
        std::wstring initials;
        bool start = true;
        for (wchar_t c : n) { if (start && !IsSep(c)) initials += c; start = IsSep(c); }
        if (initials.compare(0, q.size(), q) == 0) return 650;
    }
    // several words, any order: every word must match somewhere
    if (q.find(L' ') != std::wstring::npos) {
        int total = 0, words = 0;
        size_t i = 0;
        while (i < q.size()) {
            size_t j = q.find(L' ', i);
            std::wstring w = q.substr(i, j == std::wstring::npos ? std::wstring::npos : j - i);
            i = j == std::wstring::npos ? q.size() : j + 1;
            if (w.empty()) continue;
            int s = OneWord(n, w);
            if (s < 0) return -1;
            total += s; ++words;
        }
        if (words) return total / words - 60 - (int)std::min<size_t>(n.size(), 120) / 2;
    }
    return -1;
}

// ---------------------------------------------------------------- settings & Control Panel
struct SettingDef { const wchar_t* title; const wchar_t* section; const wchar_t* keywords; const wchar_t* target; wchar_t glyph; };

static const SettingDef kSettings[] = {
    // System
    {L"Display", L"System", L"screen resolution brightness scale night light hdr monitor refresh rate", L"ms-settings:display", 0xE7F4},
    {L"Night light", L"System", L"blue light warm evening", L"ms-settings:nightlight", 0xF08C},
    {L"Sound", L"System", L"audio volume speakers headphones microphone output input", L"ms-settings:sound", 0xE767},
    {L"Volume mixer", L"System", L"app volume audio per app", L"ms-settings:apps-volume", 0xE767},
    {L"Sound devices", L"System", L"audio output input device speaker microphone", L"ms-settings:sound-devices", 0xE7F5},
    {L"Notifications", L"System", L"alerts toasts do not disturb focus", L"ms-settings:notifications", 0xEA8F},
    {L"Focus", L"System", L"do not disturb focus assist quiet hours", L"ms-settings:focus", 0xF5B0},
    {L"Power & battery", L"System", L"sleep screen timeout power mode battery saver energy", L"ms-settings:powersleep", 0xE7E8},
    {L"Storage", L"System", L"disk space cleanup storage sense drives", L"ms-settings:storagesense", 0xEDA2},
    {L"Multitasking", L"System", L"snap windows alt tab virtual desktops", L"ms-settings:multitasking", 0xE7C4},
    {L"Clipboard", L"System", L"clipboard history copy paste", L"ms-settings:clipboard", 0xE77F},
    {L"Remote Desktop", L"System", L"rdp remote access", L"ms-settings:remotedesktop", 0xE8AF},
    {L"About this PC", L"System", L"system info specs rename pc device name windows version processor ram", L"ms-settings:about", 0xE946},
    {L"Activation", L"System", L"product key license windows activation", L"ms-settings:activation", 0xE8D7},
    {L"Troubleshoot", L"System", L"troubleshooters fix problems", L"ms-settings:troubleshoot", 0xE90F},
    {L"Recovery", L"System", L"reset this pc advanced startup recovery", L"ms-settings:recovery", 0xE777},
    {L"Projecting to this PC", L"System", L"miracast wireless display", L"ms-settings:project", 0xEBC6},
    {L"For developers", L"System", L"developer mode sudo terminal dev drive", L"ms-settings:developers", 0xEC7A},
    // Bluetooth & devices
    {L"Bluetooth & devices", L"Devices", L"bluetooth pair add device wireless headphones", L"ms-settings:bluetooth", 0xE702},
    {L"Printers & scanners", L"Devices", L"printer scanner print", L"ms-settings:printers", 0xE749},
    {L"Mouse", L"Devices", L"pointer speed scroll buttons", L"ms-settings:mousetouchpad", 0xE962},
    {L"Touchpad", L"Devices", L"trackpad gestures", L"ms-settings:devices-touchpad", 0xEFA5},
    {L"Typing", L"Devices", L"keyboard autocorrect text suggestions", L"ms-settings:typing", 0xE765},
    {L"Pen & Windows Ink", L"Devices", L"stylus pen ink", L"ms-settings:pen", 0xEDC6},
    {L"AutoPlay", L"Devices", L"autoplay usb removable", L"ms-settings:autoplay", 0xEC57},
    {L"USB", L"Devices", L"usb notifications power", L"ms-settings:usb", 0xECF0},
    {L"Cameras", L"Devices", L"webcam camera", L"ms-settings:camera", 0xE722},
    {L"Game controllers", L"Devices", L"gamepad joystick controller", L"joy.cpl", 0xE7FC},
    // Network
    {L"Network & internet", L"Network", L"internet connection ethernet status", L"ms-settings:network-status", 0xE774},
    {L"Wi-Fi", L"Network", L"wifi wireless network hotspot ssid", L"ms-settings:network-wifi", 0xE701},
    {L"Ethernet", L"Network", L"wired lan cable", L"ms-settings:network-ethernet", 0xE839},
    {L"VPN", L"Network", L"vpn virtual private network", L"ms-settings:network-vpn", 0xE705},
    {L"Mobile hotspot", L"Network", L"hotspot tethering share internet", L"ms-settings:network-mobilehotspot", 0xE704},
    {L"Airplane mode", L"Network", L"flight mode radios", L"ms-settings:network-airplanemode", 0xE709},
    {L"Proxy", L"Network", L"proxy server", L"ms-settings:network-proxy", 0xE774},
    {L"Advanced network settings", L"Network", L"adapters reset network dns ip", L"ms-settings:network-advancedsettings", 0xE968},
    {L"Network connections", L"Control Panel", L"adapters ncpa ethernet properties ip address dns", L"ncpa.cpl", 0xE839},
    // Personalization
    {L"Personalization", L"Personalization", L"customize look theme", L"ms-settings:personalization", 0xE771},
    {L"Background", L"Personalization", L"wallpaper desktop background picture slideshow", L"ms-settings:personalization-background", 0xEB9F},
    {L"Colors", L"Personalization", L"accent color dark mode light mode transparency", L"ms-settings:colors", 0xE790},
    {L"Themes", L"Personalization", L"theme cursor desktop icons", L"ms-settings:themes", 0xE771},
    {L"Lock screen", L"Personalization", L"lock screen picture spotlight", L"ms-settings:lockscreen", 0xEE3F},
    {L"Taskbar", L"Personalization", L"taskbar alignment widgets search icons system tray", L"ms-settings:taskbar", 0xE90E},
    {L"Start", L"Personalization", L"start menu layout recommended", L"ms-settings:personalization-start", 0xE80F},
    {L"Fonts", L"Personalization", L"font install typeface", L"ms-settings:fonts", 0xE8D2},
    {L"Desktop icons", L"Personalization", L"this pc recycle bin desktop icon settings", L"desk.cpl,,0", 0xE7F8},
    // Apps
    {L"Installed apps", L"Apps", L"uninstall programs apps features remove", L"ms-settings:appsfeatures", 0xE71D},
    {L"Default apps", L"Apps", L"default browser file associations open with", L"ms-settings:defaultapps", 0xE7AC},
    {L"Startup apps", L"Apps", L"startup run at login boot", L"ms-settings:startupapps", 0xE7B5},
    {L"Optional features", L"Apps", L"optional features add feature", L"ms-settings:optionalfeatures", 0xE71D},
    {L"Advanced app settings", L"Apps", L"app execution aliases where to get apps", L"ms-settings:advanced-apps", 0xE71D},
    {L"Programs and Features", L"Control Panel", L"uninstall program appwiz classic", L"appwiz.cpl", 0xE71D},
    {L"Windows features", L"Control Panel", L"turn windows features on off hyper-v wsl sandbox", L"optionalfeatures.exe", 0xE71D},
    // Accounts
    {L"Your info", L"Accounts", L"account picture profile microsoft account", L"ms-settings:yourinfo", 0xE77B},
    {L"Sign-in options", L"Accounts", L"password pin windows hello fingerprint face", L"ms-settings:signinoptions", 0xE8D7},
    {L"Email & accounts", L"Accounts", L"email accounts used by other apps", L"ms-settings:emailandaccounts", 0xE715},
    {L"Other users", L"Accounts", L"family add user account", L"ms-settings:otherusers", 0xE716},
    {L"User accounts", L"Control Panel", L"user account control uac netplwiz", L"netplwiz", 0xE77B},
    // Time & language
    {L"Date & time", L"Time & language", L"clock time zone sync date format", L"ms-settings:dateandtime", 0xEC92},
    {L"Language & region", L"Time & language", L"language region format keyboard layout display language", L"ms-settings:regionlanguage", 0xF2B7},
    {L"Speech", L"Time & language", L"voice speech recognition", L"ms-settings:speech", 0xE720},
    // Gaming
    {L"Game Mode", L"Gaming", L"game mode performance", L"ms-settings:gaming-gamemode", 0xE7FC},
    {L"Game Bar", L"Gaming", L"xbox game bar captures", L"ms-settings:gaming-gamebar", 0xE7FC},
    {L"Captures", L"Gaming", L"record clips screenshots", L"ms-settings:gaming-gamedvr", 0xE714},
    {L"Graphics", L"System", L"gpu preference hardware accelerated gpu scheduling", L"ms-settings:display-advancedgraphics", 0xF211},
    // Accessibility
    {L"Accessibility", L"Accessibility", L"ease of access vision hearing", L"ms-settings:easeofaccess", 0xE776},
    {L"Text size", L"Accessibility", L"bigger text scaling font size", L"ms-settings:easeofaccess-display", 0xE8D2},
    {L"Contrast themes", L"Accessibility", L"high contrast", L"ms-settings:easeofaccess-highcontrast", 0xE706},
    {L"Narrator", L"Accessibility", L"screen reader", L"ms-settings:easeofaccess-narrator", 0xE7F5},
    {L"Magnifier", L"Accessibility", L"zoom magnify", L"ms-settings:easeofaccess-magnifier", 0xE71E},
    {L"Mouse pointer", L"Accessibility", L"cursor size color", L"ms-settings:easeofaccess-mousepointer", 0xE962},
    // Privacy & security
    {L"Privacy & security", L"Privacy", L"permissions privacy", L"ms-settings:privacy", 0xE72E},
    {L"Windows Security", L"Privacy", L"defender antivirus virus threat protection firewall", L"windowsdefender:", 0xE83D},
    {L"Location", L"Privacy", L"location services gps", L"ms-settings:privacy-location", 0xE81D},
    {L"Camera permissions", L"Privacy", L"camera access apps", L"ms-settings:privacy-webcam", 0xE722},
    {L"Microphone permissions", L"Privacy", L"microphone access apps", L"ms-settings:privacy-microphone", 0xE720},
    {L"Find my device", L"Privacy", L"lost device", L"ms-settings:findmydevice", 0xE81D},
    {L"Windows Firewall", L"Control Panel", L"firewall defender inbound outbound", L"firewall.cpl", 0xE83D},
    // Update
    {L"Windows Update", L"Windows Update", L"updates check for updates patch install", L"ms-settings:windowsupdate", 0xE895},
    {L"Update history", L"Windows Update", L"installed updates uninstall updates", L"ms-settings:windowsupdate-history", 0xE81C},
    // Classic tools
    {L"Control Panel", L"Control Panel", L"classic control panel", L"control.exe", 0xE80F},
    {L"Device Manager", L"Control Panel", L"drivers hardware devices", L"devmgmt.msc", 0xE7F8},
    {L"Disk Management", L"Control Panel", L"partitions format drive letter", L"diskmgmt.msc", 0xEDA2},
    {L"Computer Management", L"Control Panel", L"management event viewer", L"compmgmt.msc", 0xE912},
    {L"Services", L"Control Panel", L"background services start stop", L"services.msc", 0xE912},
    {L"Event Viewer", L"Control Panel", L"logs errors events", L"eventvwr.msc", 0xE9D9},
    {L"Task Scheduler", L"Control Panel", L"scheduled tasks", L"taskschd.msc", 0xE823},
    {L"Task Manager", L"Control Panel", L"processes performance end task", L"taskmgr.exe", 0xE9D9},
    {L"System properties", L"Control Panel", L"advanced system settings environment variables performance virtual memory", L"SystemPropertiesAdvanced.exe", 0xE7EF},
    {L"Environment variables", L"Control Panel", L"path variables env", L"rundll32.exe sysdm.cpl,EditEnvironmentVariables", 0xE7EF},
    {L"Power options", L"Control Panel", L"power plan high performance lid close", L"powercfg.cpl", 0xE7E8},
    {L"Sound control panel", L"Control Panel", L"playback recording devices mmsys classic sound", L"mmsys.cpl", 0xE767},
    {L"Mouse properties", L"Control Panel", L"classic mouse pointer double click", L"main.cpl", 0xE962},
    {L"Region (classic)", L"Control Panel", L"intl date format number format", L"intl.cpl", 0xF2B7},
    {L"Internet options", L"Control Panel", L"inetcpl internet properties", L"inetcpl.cpl", 0xE774},
    {L"Credential Manager", L"Control Panel", L"passwords saved credentials", L"control.exe /name Microsoft.CredentialManager", 0xE8D7},
    {L"File Explorer options", L"Control Panel", L"folder options hidden files extensions", L"control.exe folders", 0xEC50},
    {L"Indexing options", L"Control Panel", L"search index", L"control.exe /name Microsoft.IndexingOptions", 0xE721},
    {L"Registry Editor", L"Control Panel", L"regedit registry", L"regedit.exe", 0xE912},
    {L"System Information", L"Control Panel", L"msinfo hardware specs", L"msinfo32.exe", 0xE946},
    {L"System Configuration", L"Control Panel", L"msconfig boot", L"msconfig.exe", 0xE912},
    {L"Disk Cleanup", L"Control Panel", L"cleanmgr free space temp files", L"cleanmgr.exe", 0xEA99},
    {L"Defragment and optimize drives", L"Control Panel", L"defrag trim optimize ssd", L"dfrgui.exe", 0xEDA2},
    {L"Resource Monitor", L"Control Panel", L"resmon cpu disk network memory", L"resmon.exe", 0xE9D9},
    {L"Performance Monitor", L"Control Panel", L"perfmon counters", L"perfmon.msc", 0xE9D9},
    {L"Group Policy", L"Control Panel", L"gpedit policy", L"gpedit.msc", 0xE912},
    {L"Color management", L"Control Panel", L"icc color profile calibrate", L"colorcpl.exe", 0xE790},
};

void SearchSettings(const std::wstring& q, std::vector<SettingHit>& out, size_t max) {
    for (const SettingDef& d : kSettings) {
        std::wstring title = ToLower(d.title);
        int s = MatchScore(title, q);
        if (s < 0) {
            int k = MatchScore(ToLower(d.keywords), q);
            if (k >= 0) s = std::min(k, 700) - 150;   // keyword matches rank below title matches
        }
        if (s < 0) continue;
        s += UsageBoost(std::wstring(L"setting:") + d.target);
        out.push_back({d.title, d.section, d.target, d.glyph, s});
    }
    std::stable_sort(out.begin(), out.end(), [](const SettingHit& a, const SettingHit& b) { return a.score > b.score; });
    if (out.size() > max) out.resize(max);
}

void LaunchSetting(const std::wstring& target) {
    UsageRecord(L"setting:" + target);
    if (target.find(L':') != std::wstring::npos && target.find(L'\\') == std::wstring::npos && target.find(L' ') == std::wstring::npos &&
        target.rfind(L".cpl") == std::wstring::npos) {
        ShellOpen(target.c_str());   // ms-settings:..., windowsdefender:
        return;
    }
    if (target.size() > 4 && _wcsicmp(target.c_str() + target.size() - 4, L".cpl") == 0) { ShellOpen(L"control.exe", target.c_str()); return; }
    if (target.find(L",,") != std::wstring::npos) { ShellOpen(L"control.exe", target.c_str()); return; }   // desk.cpl,,0
    size_t sp = target.find(L' ');
    if (sp == std::wstring::npos) ShellOpen(target.c_str());
    else ShellOpen(target.substr(0, sp).c_str(), target.substr(sp + 1).c_str());
}

// ---------------------------------------------------------------- usage
static std::mutex s_usageLock;
static std::map<std::wstring, std::pair<double, double>> s_usage;   // key -> (count, last unix time)
static bool s_usageLoaded = false;

static std::wstring UsagePath() { return DataDir() + L"\\usage.json"; }

void UsageLoad() {
    std::lock_guard<std::mutex> g(s_usageLock);
    if (s_usageLoaded) return;
    s_usageLoaded = true;
    HANDLE h = CreateFileW(UsagePath().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD size = GetFileSize(h, nullptr), read = 0;
    std::string buf(size < 4 * 1024 * 1024 ? size : 0, '\0');
    if (!buf.empty()) ReadFile(h, &buf[0], (DWORD)buf.size(), &read, nullptr);
    CloseHandle(h);
    int n = MultiByteToWideChar(CP_UTF8, 0, buf.data(), (int)read, nullptr, 0);
    std::wstring text(n, L'\0');
    if (n) MultiByteToWideChar(CP_UTF8, 0, buf.data(), (int)read, &text[0], n);
    json::Value root;
    json::Parser p(text);
    if (!p.Parse(root) || root.type != json::Value::Object) return;
    for (auto& kv : root.obj)
        if (kv.second.type == json::Value::Array && kv.second.arr.size() == 2)
            s_usage[kv.first] = {kv.second.arr[0].n, kv.second.arr[1].n};
}

static void UsageSaveLocked() {
    json::Value root = json::Value::MakeObject();
    // keep the file small: drop the oldest entries beyond 500
    std::vector<std::pair<double, std::wstring>> byAge;
    for (auto& kv : s_usage) byAge.push_back({kv.second.second, kv.first});
    std::sort(byAge.begin(), byAge.end(), std::greater<>());
    if (byAge.size() > 500) for (size_t i = 500; i < byAge.size(); ++i) s_usage.erase(byAge[i].second);
    for (auto& kv : s_usage) {
        json::Value a = json::Value::MakeArray();
        a.arr.push_back(json::Value::Num(kv.second.first));
        a.arr.push_back(json::Value::Num(kv.second.second));
        root.Set(kv.first, a);
    }
    std::wstring text;
    json::Write(root, text);
    int n = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0, nullptr, nullptr);
    std::string buf(n, '\0');
    if (n) WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), &buf[0], n, nullptr, nullptr);
    std::wstring tmp = UsagePath() + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD w = 0;
    WriteFile(h, buf.data(), (DWORD)buf.size(), &w, nullptr);
    CloseHandle(h);
    MoveFileExW(tmp.c_str(), UsagePath().c_str(), MOVEFILE_REPLACE_EXISTING);
}

void UsageRecord(const std::wstring& key) {
    if (key.empty()) return;
    UsageLoad();
    std::lock_guard<std::mutex> g(s_usageLock);
    auto& e = s_usage[ToLower(key)];
    e.first += 1;
    e.second = (double)time(nullptr);
    UsageSaveLocked();
}

int UsageBoost(const std::wstring& key) {
    UsageLoad();
    std::lock_guard<std::mutex> g(s_usageLock);
    auto it = s_usage.find(ToLower(key));
    if (it == s_usage.end()) return 0;
    double days = std::max(0.0, ((double)time(nullptr) - it->second.second) / 86400.0);
    double decay = std::pow(0.5, days / 14.0);   // half-life of two weeks
    double boost = 140.0 * std::log2(1.0 + it->second.first) * (0.35 + 0.65 * decay);
    return (int)std::min(400.0, boost);
}

// ---------------------------------------------------------------- file index
// A compact index of the user's own folders: one shared name pool + parent links, so even
// 150,000 entries fit in a few MB. Built at background priority and paused while a game or
// fullscreen app is running.
struct Entry { uint32_t parent; uint32_t nameOff; uint16_t nameLen; uint8_t dir; uint8_t root; uint32_t days; };

struct Index {
    std::vector<Entry> entries;
    std::wstring pool;
    std::vector<std::wstring> roots;
    DWORD builtAt = 0;
};

static SRWLOCK s_idxLock = SRWLOCK_INIT;
static std::shared_ptr<Index> s_index;
static std::atomic<bool> s_building{false};
static HWND s_notify = nullptr;
static UINT s_doneMsg = 0;
constexpr size_t kMaxEntries = 150000;

static std::shared_ptr<Index> CurrentIndex() {
    AcquireSRWLockShared(&s_idxLock);
    auto i = s_index;
    ReleaseSRWLockShared(&s_idxLock);
    return i;
}

size_t FilesIndexed() { auto i = CurrentIndex(); return i ? i->entries.size() : 0; }

static std::wstring KnownFolderPath(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::wstring r;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DONT_VERIFY, nullptr, &p)) && p) r = p;
    CoTaskMemFree(p);
    return r;
}

static bool SkipDir(const wchar_t* name) {
    if (name[0] == L'.' || name[0] == L'$') return true;
    static const wchar_t* skip[] = {L"node_modules", L"__pycache__", L"AppData", L"bin", L"obj", L"target", L"venv", L".venv", L"site-packages", L"cache", L"Cache", L"Temp"};
    for (auto s : skip) if (_wcsicmp(name, s) == 0) return true;
    return false;
}

static uint32_t DaysFromFiletime(const FILETIME& ft) {
    ULARGE_INTEGER u; u.LowPart = ft.dwLowDateTime; u.HighPart = ft.dwHighDateTime;
    return (uint32_t)(u.QuadPart / (10000000ULL * 86400ULL));
}

static uint32_t TodayDays() {
    FILETIME ft; GetSystemTimeAsFileTime(&ft);
    return DaysFromFiletime(ft);
}

static void Walk(Index& idx, const std::wstring& dir, uint32_t parent, int depth) {
    if (depth > 9 || idx.entries.size() >= kMaxEntries) return;
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (f == INVALID_HANDLE_VALUE) return;
    std::vector<std::pair<std::wstring, uint32_t>> subdirs;
    do {
        const wchar_t* n = fd.cFileName;
        if (n[0] == L'.' && (n[1] == 0 || (n[1] == L'.' && n[2] == 0))) continue;
        if (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) continue;
        bool isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (isDir && ((fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) || SkipDir(n))) continue;
        size_t len = wcslen(n);
        if (!isDir && len > 4 && (_wcsicmp(n + len - 4, L".tmp") == 0 || _wcsicmp(n + len - 4, L".ini") == 0)) continue;
        Entry e{parent, (uint32_t)idx.pool.size(), (uint16_t)std::min<size_t>(len, 65535), (uint8_t)isDir, 0, DaysFromFiletime(fd.ftLastWriteTime)};
        idx.pool.append(n, len);
        idx.entries.push_back(e);
        if (isDir) subdirs.push_back({dir + L"\\" + n, (uint32_t)idx.entries.size() - 1});
        if (idx.entries.size() >= kMaxEntries) break;
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    static thread_local unsigned dirsWalked = 0;
    for (auto& sd : subdirs) {
        if ((++dirsWalked & 63) == 0)
            while (updater::UserIsBusy()) Sleep(5000);   // never compete with a game
        Walk(idx, sd.first, sd.second, depth + 1);
    }
}

std::vector<std::wstring> FilesRoots() {
    const KNOWNFOLDERID* ids[] = {&FOLDERID_Desktop, &FOLDERID_Documents, &FOLDERID_Downloads, &FOLDERID_Pictures,
                                  &FOLDERID_Music, &FOLDERID_Videos, &FOLDERID_SkyDrive};
    std::vector<std::wstring> roots;
    for (auto id : ids) {
        std::wstring p = KnownFolderPath(*id);
        if (p.empty() || GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        // skip roots inside another root (Documents inside OneDrive)
        bool nested = false;
        for (auto& r : roots) if (p.size() > r.size() && _wcsnicmp(p.c_str(), r.c_str(), r.size()) == 0 && p[r.size()] == L'\\') nested = true;
        if (nested) continue;
        roots.erase(std::remove_if(roots.begin(), roots.end(), [&](const std::wstring& r) {
            return r.size() > p.size() && _wcsnicmp(r.c_str(), p.c_str(), p.size()) == 0 && r[p.size()] == L'\\';
        }), roots.end());
        roots.push_back(p);
    }
    return roots;
}

// ---- saved index: the last index is kept on disk so file search works the moment Foyer starts
static std::wstring IndexPath() { return DataDir() + L"\\files.index"; }

static void SaveIndex(const Index& idx) {
    std::wstring tmp = IndexPath() + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD w = 0;
    bool ok = true;
    auto put = [&](const void* p, size_t n) { if (ok) ok = WriteFile(h, p, (DWORD)n, &w, nullptr) && w == n; };
    uint32_t magic = 0x31584946 /* "FIX1" */, nEntries = (uint32_t)idx.entries.size(), nPool = (uint32_t)idx.pool.size(), nRoots = (uint32_t)idx.roots.size();
    put(&magic, 4); put(&nEntries, 4); put(&nPool, 4); put(&nRoots, 4);
    for (auto& r : idx.roots) { uint32_t len = (uint32_t)r.size(); put(&len, 4); put(r.data(), len * 2); }
    put(idx.entries.data(), idx.entries.size() * sizeof(Entry));
    put(idx.pool.data(), idx.pool.size() * 2);
    CloseHandle(h);
    if (ok) MoveFileExW(tmp.c_str(), IndexPath().c_str(), MOVEFILE_REPLACE_EXISTING);
    else DeleteFileW(tmp.c_str());
}

static std::shared_ptr<Index> LoadIndex() {
    HANDLE h = CreateFileW(IndexPath().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return nullptr;
    auto idx = std::make_shared<Index>();
    DWORD r = 0;
    bool ok = true;
    auto get = [&](void* p, size_t n) { if (ok) ok = ReadFile(h, p, (DWORD)n, &r, nullptr) && r == n; };
    uint32_t magic = 0, nEntries = 0, nPool = 0, nRoots = 0;
    get(&magic, 4); get(&nEntries, 4); get(&nPool, 4); get(&nRoots, 4);
    ok = ok && magic == 0x31584946 && nEntries <= kMaxEntries + 64 && nPool < 64u * 1024 * 1024 && nRoots < 64;
    for (uint32_t i = 0; ok && i < nRoots; ++i) {
        uint32_t len = 0; get(&len, 4);
        if (!ok || len > 4096) { ok = false; break; }
        std::wstring root(len, L'\0'); get(root.data(), len * 2);
        idx->roots.push_back(root);
    }
    if (ok) { idx->entries.resize(nEntries); get(idx->entries.data(), nEntries * sizeof(Entry)); }
    if (ok) { idx->pool.resize(nPool); get(idx->pool.data(), nPool * 2); }
    CloseHandle(h);
    if (!ok) return nullptr;
    for (auto& e : idx->entries)   // never trust offsets from disk
        if ((size_t)e.nameOff + e.nameLen > idx->pool.size() || (e.parent != 0xFFFFFFFF && e.parent >= nEntries)) return nullptr;
    return idx;
}

static std::atomic<bool> s_dirty{true};       // indexed folders changed since the last build
static DWORD s_lastBuildStart = 0;

void FilesChanged() { s_dirty = true; }

static void BuildIndex() {
    bool expected = false;
    if (!s_building.compare_exchange_strong(expected, true)) return;
    s_dirty = false;          // changes from here on mark it dirty again
    s_lastBuildStart = GetTickCount();
    std::thread([] {
        SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
        DWORD t0 = GetTickCount();
        auto idx = std::make_shared<Index>();
        std::vector<std::wstring> roots = FilesRoots();
        idx->roots = roots;
        for (size_t r = 0; r < roots.size(); ++r) {
            // a synthetic entry per root so paths can be rebuilt from parent links
            Entry e{0xFFFFFFFF, (uint32_t)idx->pool.size(), (uint16_t)roots[r].size(), 1, 1, 0};
            idx->pool += roots[r];
            idx->entries.push_back(e);
            Walk(*idx, roots[r], (uint32_t)idx->entries.size() - 1, 0);
        }
        idx->builtAt = GetTickCount();
        AcquireSRWLockExclusive(&s_idxLock);
        s_index = idx;
        ReleaseSRWLockExclusive(&s_idxLock);
        SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
        SaveIndex(*idx);
        Log(L"File index: %u items in %lu ms (%u KB)", (unsigned)idx->entries.size(), GetTickCount() - t0,
            (unsigned)((idx->pool.size() * 2 + idx->entries.size() * sizeof(Entry)) / 1024));
        s_building = false;
    }).detach();
}

static std::wstring EntryPath(const Index& idx, uint32_t i) {
    std::vector<uint32_t> chain;
    for (uint32_t k = i; k != 0xFFFFFFFF && chain.size() < 32; k = idx.entries[k].parent) {
        chain.push_back(k);
        if (idx.entries[k].root) break;
    }
    std::wstring p;
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        const Entry& e = idx.entries[*it];
        if (!p.empty()) p += L'\\';
        p.append(idx.pool, e.nameOff, e.nameLen);
    }
    return p;
}

void FilesInit(HWND notify, UINT doneMsg) {
    s_notify = notify;
    s_doneMsg = doneMsg;
    std::thread([] {
        // the saved index makes file search work right away; a fresh one follows once boot has settled
        if (auto saved = LoadIndex()) {
            saved->builtAt = GetTickCount();
            AcquireSRWLockExclusive(&s_idxLock);
            if (!s_index) s_index = saved;
            ReleaseSRWLockExclusive(&s_idxLock);
            Log(L"File index: loaded %u saved items", (unsigned)saved->entries.size());
        }
        Sleep(45000);
        BuildIndex();
    }).detach();
}

// Called when the menu opens. Rebuilds only if the watched folders changed, at most every
// few minutes (a build takes about a second at background priority).
void FilesTouch() {
    auto idx = CurrentIndex();
    if (!idx) return;
    if (s_dirty && GetTickCount() - s_lastBuildStart > 3 * 60 * 1000) BuildIndex();
}

// ---------------------------------------------------------------- Everything (voidtools) IPC
#define EVERYTHING_IPC_WNDCLASS L"EVERYTHING_TASKBAR_NOTIFICATION"
#define EVERYTHING_IPC_COPYDATAQUERYW 2
#define EVERYTHING_IPC_FOLDER 0x00000001
#define EVERYTHING_IPC_DRIVE 0x00000002
#pragma pack(push, 1)
struct EV_QUERYW { DWORD reply_hwnd; DWORD reply_copydata_message; DWORD search_flags; DWORD offset; DWORD max_results; WCHAR search_string[1]; };
struct EV_ITEMW { DWORD flags; DWORD filename_offset; DWORD path_offset; };
struct EV_LISTW { DWORD totfolders; DWORD totfiles; DWORD totitems; DWORD numfolders; DWORD numfiles; DWORD numitems; DWORD offset; EV_ITEMW items[1]; };
#pragma pack(pop)

bool EverythingRunning() { return FindWindowW(EVERYTHING_IPC_WNDCLASS, nullptr) != nullptr; }

struct EvReply { bool got = false; std::vector<FileHit> hits; };
static thread_local EvReply* t_reply = nullptr;
constexpr DWORD kEvReplyId = 0xF0E7;

static LRESULT CALLBACK EvWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_COPYDATA && t_reply) {
        auto* cds = (COPYDATASTRUCT*)l;
        if (cds->dwData == kEvReplyId && cds->cbData >= sizeof(EV_LISTW) - sizeof(EV_ITEMW)) {
            auto* list = (const EV_LISTW*)cds->lpData;
            const BYTE* base = (const BYTE*)cds->lpData;
            for (DWORD i = 0; i < list->numitems; ++i) {
                const EV_ITEMW& it = list->items[i];
                if ((size_t)((const BYTE*)&it - base) + sizeof(EV_ITEMW) > cds->cbData) break;
                if (it.filename_offset >= cds->cbData || it.path_offset >= cds->cbData) continue;
                FileHit hit;
                hit.name = (const wchar_t*)(base + it.filename_offset);
                hit.folder = (const wchar_t*)(base + it.path_offset);
                hit.dir = (it.flags & (EVERYTHING_IPC_FOLDER | EVERYTHING_IPC_DRIVE)) != 0;
                hit.path = hit.folder.empty() ? hit.name : hit.folder + L"\\" + hit.name;
                t_reply->hits.push_back(std::move(hit));
            }
            t_reply->got = true;
        }
        return TRUE;
    }
    return DefWindowProcW(h, m, w, l);
}

static bool EverythingQuery(const std::wstring& q, bool scoped, std::vector<FileHit>& out) {
    HWND ev = FindWindowW(EVERYTHING_IPC_WNDCLASS, nullptr);
    if (!ev) return false;
    static thread_local HWND s_win = nullptr;
    if (!s_win) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = EvWndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"FoyerEverythingReply";
        RegisterClassW(&wc);
        s_win = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
        if (!s_win) return false;
    }
    // Leave out system and app-data noise unless the user asked for files specifically.
    std::wstring s = q;
    if (!scoped) s += L" !path:\\appdata\\ !path:\\windows\\ !path:$recycle.bin !path:\\node_modules\\ !path:\\.git\\ !path:\\program files";
    size_t bytes = sizeof(EV_QUERYW) + s.size() * sizeof(wchar_t);
    std::vector<BYTE> buf(bytes, 0);
    auto* query = (EV_QUERYW*)buf.data();
    query->reply_hwnd = (DWORD)(DWORD_PTR)s_win;
    query->reply_copydata_message = kEvReplyId;
    query->search_flags = 0;
    query->offset = 0;
    query->max_results = scoped ? 400 : 200;
    memcpy(query->search_string, s.c_str(), (s.size() + 1) * sizeof(wchar_t));
    COPYDATASTRUCT cds{EVERYTHING_IPC_COPYDATAQUERYW, (DWORD)bytes, buf.data()};
    EvReply reply;
    t_reply = &reply;
    DWORD_PTR res = 0;
    if (!SendMessageTimeoutW(ev, WM_COPYDATA, (WPARAM)s_win, (LPARAM)&cds, SMTO_ABORTIFHUNG | SMTO_BLOCK, 400, &res) || !res) {
        t_reply = nullptr;
        return false;
    }
    DWORD until = GetTickCount() + 700;
    MSG msg;
    while (!reply.got && GetTickCount() < until) {
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 20, QS_ALLINPUT | QS_SENDMESSAGE);
        while (PeekMessageW(&msg, s_win, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
    }
    t_reply = nullptr;
    if (!reply.got) return false;
    out = std::move(reply.hits);
    return true;
}

// ---------------------------------------------------------------- file search worker
static std::mutex s_reqLock;
static std::condition_variable s_reqCv;
static struct { bool pending = false; unsigned gen = 0; std::wstring q; size_t max = 10; bool scoped = false; } s_req;
static bool s_workerStarted = false;

static void ScoreAndTrim(std::vector<FileHit>& hits, const std::wstring& q, size_t max, uint32_t today, const std::vector<uint32_t>* days) {
    for (size_t i = 0; i < hits.size(); ++i) {
        FileHit& h = hits[i];
        int s = MatchScore(ToLower(h.name), q);
        if (s < 0) s = 200;   // matched on the path only
        if (h.dir) s += 25;
        size_t depth = std::count(h.path.begin(), h.path.end(), L'\\');
        s -= (int)std::min<size_t>(depth, 15) * 6;
        if (days && i < days->size()) {
            uint32_t age = today > (*days)[i] ? today - (*days)[i] : 0;
            if (age < 2) s += 90; else if (age < 7) s += 60; else if (age < 30) s += 25;
        }
        s += UsageBoost(L"file:" + h.path);
        h.score = s;
    }
    std::stable_sort(hits.begin(), hits.end(), [](const FileHit& a, const FileHit& b) { return a.score > b.score; });
    if (hits.size() > max) hits.resize(max);
    for (auto& h : hits) if (h.folder.empty()) { size_t sl = h.path.find_last_of(L'\\'); h.folder = sl == std::wstring::npos ? L"" : h.path.substr(0, sl); }
}

static void SearchIndex(const std::wstring& q, size_t max, std::vector<FileHit>& out) {
    auto idx = CurrentIndex();
    if (!idx) return;
    // every word must appear in the name
    std::vector<std::wstring> words;
    for (size_t i = 0; i < q.size();) {
        size_t j = q.find(L' ', i);
        std::wstring w = q.substr(i, j == std::wstring::npos ? std::wstring::npos : j - i);
        if (!w.empty()) words.push_back(w);
        i = j == std::wstring::npos ? q.size() : j + 1;
    }
    if (words.empty()) return;
    std::vector<std::pair<int, uint32_t>> cand;
    for (uint32_t i = 0; i < idx->entries.size(); ++i) {
        const Entry& e = idx->entries[i];
        if (e.root) continue;
        const wchar_t* name = idx->pool.c_str() + e.nameOff;
        bool all = true;
        int quick = 0;
        for (auto& w : words) {
            int pos = FindStringOrdinal(FIND_FROMSTART, name, e.nameLen, w.c_str(), (int)w.size(), TRUE);
            if (pos < 0) { all = false; break; }
            quick += pos == 0 ? 3 : (IsSep(name[pos - 1]) ? 2 : 1);
        }
        if (!all) continue;
        cand.push_back({quick * 100 - (int)e.nameLen, i});
    }
    // keep a generous shortlist, then score properly
    std::sort(cand.begin(), cand.end(), [](auto& a, auto& b) { return a.first > b.first; });
    if (cand.size() > max * 8) cand.resize(max * 8);
    std::vector<uint32_t> days;
    for (auto& c : cand) {
        const Entry& e = idx->entries[c.second];
        FileHit h;
        h.path = EntryPath(*idx, c.second);
        h.name.assign(idx->pool, e.nameOff, e.nameLen);
        h.dir = e.dir != 0;
        out.push_back(std::move(h));
        days.push_back(e.days);
    }
    ScoreAndTrim(out, q, max, TodayDays(), &days);
}

static void Worker() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    for (;;) {
        unsigned gen; std::wstring q; size_t max; bool scoped;
        {
            std::unique_lock<std::mutex> lk(s_reqLock);
            s_reqCv.wait(lk, [] { return s_req.pending; });
            s_req.pending = false;
            gen = s_req.gen; q = s_req.q; max = s_req.max; scoped = s_req.scoped;
        }
        auto* res = new FileResults();
        res->gen = gen;
        std::vector<FileHit> hits;
        if (EverythingQuery(q, scoped, hits)) {
            res->everything = true;
            // Everything matches paths too; keep name matches first
            ScoreAndTrim(hits, ToLower(q), max, 0, nullptr);
            res->hits = std::move(hits);
        } else {
            SearchIndex(ToLower(q), max, res->hits);
            res->indexing = !CurrentIndex();
        }
        // a newer request arrived while we worked: drop this answer
        bool stale;
        { std::lock_guard<std::mutex> lk(s_reqLock); stale = s_req.pending; }
        if (stale || !PostMessageW(s_notify, s_doneMsg, 0, (LPARAM)res)) delete res;
    }
}

void FilesSearchAsync(unsigned gen, const std::wstring& q, size_t max, bool scoped) {
    {
        std::lock_guard<std::mutex> lk(s_reqLock);
        s_req.pending = true;
        s_req.gen = gen;
        s_req.q = q;
        s_req.max = max;
        s_req.scoped = scoped;
        if (!s_workerStarted) { s_workerStarted = true; std::thread(Worker).detach(); }
    }
    s_reqCv.notify_one();
    if (!CurrentIndex() && !EverythingRunning()) BuildIndex();   // first search before the timed build
}

// ---------------------------------------------------------------- run
bool RunCommand(const std::wstring& raw, bool admin) {
    std::wstring cmd = Trim(raw);
    if (cmd.empty()) return false;
    std::wstring file, args;
    if (cmd[0] == L'"') {
        size_t end = cmd.find(L'"', 1);
        file = cmd.substr(1, end == std::wstring::npos ? std::wstring::npos : end - 1);
        args = end == std::wstring::npos ? L"" : Trim(cmd.substr(end + 1));
    } else if (GetFileAttributesW(cmd.c_str()) != INVALID_FILE_ATTRIBUTES) {
        file = cmd;   // a path with spaces, unquoted
    } else {
        size_t sp = cmd.find(L' ');
        file = cmd.substr(0, sp);
        args = sp == std::wstring::npos ? L"" : Trim(cmd.substr(sp + 1));
    }
    // expand %VARS%
    wchar_t exp[4096];
    if (ExpandEnvironmentStringsW(file.c_str(), exp, 4096)) file = exp;
    SHELLEXECUTEINFOW sei{sizeof(sei)};
    sei.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    sei.lpVerb = admin ? L"runas" : nullptr;
    sei.lpFile = file.c_str();
    sei.lpParameters = args.empty() ? nullptr : args.c_str();
    wchar_t home[MAX_PATH];
    DWORD hn = GetEnvironmentVariableW(L"USERPROFILE", home, MAX_PATH);
    sei.lpDirectory = hn ? home : nullptr;
    sei.nShow = SW_SHOWNORMAL;
    bool ran = (!admin && ProcessIsElevated()) ? ShellExecAsUser(file, args, sei.lpDirectory ? sei.lpDirectory : L"") : ShellExecuteExW(&sei) != FALSE;
    if (ran) { Log(L"Run: %ls", cmd.c_str()); UsageRecord(L"run:" + cmd); return true; }
    // fall back to cmd.exe for shell built-ins ("dir", "echo hi")
    if (GetLastError() == ERROR_FILE_NOT_FOUND && !admin) {
        std::wstring c = L"/k " + cmd;
        if (ShellExecAsUser(L"cmd.exe", c, sei.lpDirectory ? sei.lpDirectory : L"")) return true;
    }
    Log(L"Run failed: %ls (error %lu)", cmd.c_str(), GetLastError());
    return false;
}

} // namespace search
