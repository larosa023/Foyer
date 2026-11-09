// Foyer - settings and pins persisted as config.json
#include "config.h"
#include "json.h"
#include <algorithm>
#include <cmath>

Config g_config;

std::wstring ConfigPath() { return DataDir() + L"\\config.json"; }

bool Config::IsHidden(const std::wstring& id) const {
    for (auto& h : hidden) if (_wcsicmp(h.c_str(), id.c_str()) == 0) return true;
    return false;
}

bool Config::IsPinned(const std::wstring& id) const {
    for (auto& p : pins) if (_wcsicmp(p.id.c_str(), id.c_str()) == 0) return true;
    return false;
}

static bool ReadUtf8File(const std::wstring& path, std::wstring& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD size = GetFileSize(h, nullptr);
    if (size > 8 * 1024 * 1024) { CloseHandle(h); return false; }
    std::string buf(size, '\0');
    DWORD read = 0;
    bool ok = size == 0 || ReadFile(h, &buf[0], size, &read, nullptr);
    CloseHandle(h);
    if (!ok) return false;
    size_t skip = (read >= 3 && (unsigned char)buf[0] == 0xEF && (unsigned char)buf[1] == 0xBB && (unsigned char)buf[2] == 0xBF) ? 3 : 0;
    int n = MultiByteToWideChar(CP_UTF8, 0, buf.data() + skip, (int)(read - skip), nullptr, 0);
    out.assign(n, L'\0');
    if (n) MultiByteToWideChar(CP_UTF8, 0, buf.data() + skip, (int)(read - skip), &out[0], n);
    return true;
}

static bool WriteUtf8FileAtomic(const std::wstring& path, const std::wstring& text) {
    int n = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0, nullptr, nullptr);
    std::string buf(n, '\0');
    if (n) WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), &buf[0], n, nullptr, nullptr);
    std::wstring tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool ok = WriteFile(h, buf.data(), (DWORD)buf.size(), &written, nullptr) && written == buf.size();
    FlushFileBuffers(h);
    CloseHandle(h);
    if (!ok) { DeleteFileW(tmp.c_str()); return false; }
    return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

void ConfigDefaultPins() {
    g_config.pins = {
        {L"Microsoft.Windows.Explorer", L"File Explorer"},
        {L"windows.immersivecontrolpanel_cw5n1h2txyewy!microsoft.windows.immersivecontrolpanel", L"Settings"},
        {L"Microsoft.WindowsTerminal_8wekyb3d8bbwe!App", L"Terminal"},
        {L"Microsoft.WindowsNotepad_8wekyb3d8bbwe!App", L"Notepad"},
        {L"Microsoft.WindowsCalculator_8wekyb3d8bbwe!App", L"Calculator"},
        {L"Microsoft.Windows.Photos_8wekyb3d8bbwe!App", L"Photos"},
    };
}

static uint32_t ParseHexColor(const std::wstring& s, uint32_t def) {
    std::wstring t = s;
    if (!t.empty() && t[0] == L'#') t = t.substr(1);
    if (t.size() != 6) return def;
    wchar_t* end = nullptr;
    unsigned long v = wcstoul(t.c_str(), &end, 16);
    return (end && *end == 0) ? (uint32_t)v : def;
}

static std::wstring HexColor(uint32_t c) {
    wchar_t b[16];
    swprintf(b, 16, L"#%06X", c & 0xFFFFFF);
    return b;
}

static std::wstring OneOf(const std::wstring& v, std::initializer_list<const wchar_t*> allowed, const wchar_t* def) {
    for (auto a : allowed) if (v == a) return v;
    return def;
}

// Fill g_config from a parsed JSON object (missing keys keep their defaults).
static void FromJson(const json::Value& root) {
    Config d;   // defaults
    Config& c = g_config;
    c.interceptStartButton = root.GetBool(L"interceptStartButton", d.interceptStartButton);
    c.interceptWinKey = root.GetBool(L"interceptWinKey", d.interceptWinKey);
    c.showGreeting = root.GetBool(L"showGreeting", d.showGreeting);

    c.theme = OneOf(root.GetStr(L"theme", d.theme), {L"dark", L"light", L"midnight", L"glass", L"ember", L"ocean", L"system"}, L"dark");
    c.useWindowsAccent = root.GetBool(L"useWindowsAccent", d.useWindowsAccent);
    c.accentColor = ParseHexColor(root.GetStr(L"accentColor"), d.accentColor);
    c.backdrop = OneOf(root.GetStr(L"backdrop", d.backdrop), {L"solid", L"mica", L"acrylic"}, L"acrylic");
    c.opacity = std::clamp((float)root.GetNum(L"opacity", d.opacity), 0.4f, 1.0f);
    c.cornerRadius = std::clamp((float)root.GetNum(L"cornerRadius", d.cornerRadius), 0.0f, 16.0f);
    c.iconSize = OneOf(root.GetStr(L"iconSize", d.iconSize), {L"small", L"medium", L"large"}, L"medium");
    c.density = OneOf(root.GetStr(L"density", d.density), {L"compact", L"comfortable"}, L"comfortable");
    c.animation = OneOf(root.GetStr(L"animation", d.animation), {L"none", L"fast", L"smooth"}, L"smooth");
    c.accentGlow = root.GetBool(L"accentGlow", d.accentGlow);

    c.layout = OneOf(root.GetStr(L"layout", d.layout), {L"modern", L"classic", L"fullscreen"}, L"modern");
    c.position = OneOf(root.GetStr(L"position", d.position), {L"taskbar", L"center"}, L"taskbar");

    c.autoCheck = root.GetBool(L"autoCheck", d.autoCheck);
    c.autoInstall = root.GetBool(L"autoInstall", d.autoInstall);
    c.channel = OneOf(root.GetStr(L"channel", d.channel), {L"stable", L"beta"}, L"stable");
    c.checkHours = std::clamp((int)root.GetNum(L"checkHours", d.checkHours), 1, 168);
    c.lastCheck = root.GetNum(L"lastCheck", 0);
    c.notifiedVersion = root.GetStr(L"notifiedVersion");

    c.pins.clear();
    if (const json::Value* pins = root.Get(L"pins"); pins && pins->type == json::Value::Array) {
        for (auto& v : pins->arr) {
            Pin pin{v.GetStr(L"id"), v.GetStr(L"name")};
            if (!pin.id.empty()) c.pins.push_back(pin);
        }
    }
    c.hidden.clear();
    if (const json::Value* hid = root.Get(L"hidden"); hid && hid->type == json::Value::Array) {
        for (auto& v : hid->arr) if (v.type == json::Value::String) c.hidden.push_back(v.s);
    }
}

static std::wstring ToJsonText() {
    const Config& c = g_config;
    json::Value root = json::Value::MakeObject();
    root.Set(L"version", json::Value::Num(2));
    root.Set(L"interceptWinKey", json::Value::Boolean(c.interceptWinKey));
    root.Set(L"interceptStartButton", json::Value::Boolean(c.interceptStartButton));
    root.Set(L"showGreeting", json::Value::Boolean(c.showGreeting));
    root.Set(L"theme", json::Value::Str(c.theme));
    root.Set(L"useWindowsAccent", json::Value::Boolean(c.useWindowsAccent));
    root.Set(L"accentColor", json::Value::Str(HexColor(c.accentColor)));
    root.Set(L"backdrop", json::Value::Str(c.backdrop));
    root.Set(L"opacity", json::Value::Num(std::round(c.opacity * 100) / 100));
    root.Set(L"cornerRadius", json::Value::Num(std::round(c.cornerRadius)));
    root.Set(L"iconSize", json::Value::Str(c.iconSize));
    root.Set(L"density", json::Value::Str(c.density));
    root.Set(L"animation", json::Value::Str(c.animation));
    root.Set(L"accentGlow", json::Value::Boolean(c.accentGlow));
    root.Set(L"layout", json::Value::Str(c.layout));
    root.Set(L"position", json::Value::Str(c.position));
    root.Set(L"autoCheck", json::Value::Boolean(c.autoCheck));
    root.Set(L"autoInstall", json::Value::Boolean(c.autoInstall));
    root.Set(L"channel", json::Value::Str(c.channel));
    root.Set(L"checkHours", json::Value::Num(c.checkHours));
    root.Set(L"lastCheck", json::Value::Num(c.lastCheck));
    root.Set(L"notifiedVersion", json::Value::Str(c.notifiedVersion));
    json::Value pins = json::Value::MakeArray();
    for (auto& p : c.pins) {
        json::Value o = json::Value::MakeObject();
        o.Set(L"id", json::Value::Str(p.id));
        o.Set(L"name", json::Value::Str(p.name));
        pins.arr.push_back(o);
    }
    root.Set(L"pins", pins);
    json::Value hid = json::Value::MakeArray();
    for (auto& h : c.hidden) hid.arr.push_back(json::Value::Str(h));
    root.Set(L"hidden", hid);
    std::wstring text;
    json::Write(root, text);
    text += L"\n";
    return text;
}

void ConfigLoad() {
    std::wstring text;
    json::Value root;
    if (!ReadUtf8File(ConfigPath(), text)) {
        Log(L"No config.json yet; using defaults");
        ConfigDefaultPins();
        ConfigSave();
        return;
    }
    json::Parser p(text);
    if (!p.Parse(root) || root.type != json::Value::Object) {
        Log(L"config.json is invalid; backing it up and using defaults");
        MoveFileExW(ConfigPath().c_str(), (ConfigPath() + L".bad").c_str(), MOVEFILE_REPLACE_EXISTING);
        ConfigDefaultPins();
        ConfigSave();
        return;
    }
    FromJson(root);
    Log(L"Config loaded: %u pins, %u hidden, theme %ls, layout %ls", (unsigned)g_config.pins.size(),
        (unsigned)g_config.hidden.size(), g_config.theme.c_str(), g_config.layout.c_str());
}

void ConfigSave() {
    if (!WriteUtf8FileAtomic(ConfigPath(), ToJsonText())) Log(L"Failed to save config.json (error %lu)", GetLastError());
}

bool ConfigExportTo(const std::wstring& path) {
    return WriteUtf8FileAtomic(path, ToJsonText());
}

bool ConfigImportFrom(const std::wstring& path, std::wstring* error) {
    std::wstring text;
    if (!ReadUtf8File(path, text)) { if (error) *error = L"Couldn't read that file."; return false; }
    json::Value root;
    json::Parser p(text);
    if (!p.Parse(root) || root.type != json::Value::Object || !root.Get(L"pins")) {
        if (error) *error = L"That file isn't a Foyer backup.";
        return false;
    }
    double lastCheck = g_config.lastCheck;
    FromJson(root);
    g_config.lastCheck = lastCheck;
    ConfigSave();
    Log(L"Settings restored from %ls", path.c_str());
    return true;
}
