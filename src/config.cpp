// Foyer - settings and pins persisted as config.json
#include "config.h"
#include "json.h"
#include <algorithm>
#include <cmath>

Config g_config;

std::wstring ConfigPath() { return DataDir() + L"\\config.json"; }
std::wstring ThemesDir() { return DataDir() + L"\\themes"; }

std::wstring PinDisplayName(const Pin& p) {
    if (!p.label.empty()) return p.label;
    if (!p.name.empty()) return p.name;
    if (p.type == PinType::Url) {
        std::wstring h = p.id;
        size_t s = h.find(L"://");
        if (s != std::wstring::npos) h = h.substr(s + 3);
        if (h.compare(0, 4, L"www.") == 0) h = h.substr(4);
        size_t e = h.find_first_of(L"/?#");
        return e == std::wstring::npos ? h : h.substr(0, e);
    }
    if (p.type == PinType::Group) return L"Folder";
    std::wstring id = p.id;
    while (!id.empty() && (id.back() == L'\\' || id.back() == L'/')) id.pop_back();
    size_t sl = id.find_last_of(L"\\/");
    std::wstring n = sl == std::wstring::npos ? id : id.substr(sl + 1);
    if (p.type == PinType::App && n.size() > 4 && _wcsicmp(n.c_str() + n.size() - 4, L".exe") == 0) n.resize(n.size() - 4);
    return n.empty() ? p.id : n;
}

static const wchar_t* PinTypeName(PinType t) {
    switch (t) { case PinType::File: return L"file"; case PinType::Folder: return L"folder"; case PinType::Url: return L"url";
                 case PinType::Group: return L"group"; default: return L"app"; }
}

static PinType PinTypeFrom(const std::wstring& s) {
    if (s == L"file") return PinType::File;
    if (s == L"folder") return PinType::Folder;
    if (s == L"url") return PinType::Url;
    if (s == L"group") return PinType::Group;
    return PinType::App;
}

static Pin PinFromJson(const json::Value& v, bool allowGroup) {
    Pin p;
    p.id = v.GetStr(L"id");
    p.name = v.GetStr(L"name");
    p.type = PinTypeFrom(v.GetStr(L"type", L"app"));
    p.label = v.GetStr(L"label");
    p.icon = v.GetStr(L"icon");
    if (p.type == PinType::Group) {
        if (!allowGroup) p.type = PinType::App;
        else if (const json::Value* items = v.Get(L"items"); items && items->type == json::Value::Array)
            for (auto& c : items->arr) { Pin child = PinFromJson(c, false); if (!child.id.empty()) p.items.push_back(child); }
    }
    return p;
}

static json::Value PinToJson(const Pin& p) {
    json::Value o = json::Value::MakeObject();
    if (p.type != PinType::Group) o.Set(L"id", json::Value::Str(p.id));
    o.Set(L"name", json::Value::Str(p.name));
    if (p.type != PinType::App) o.Set(L"type", json::Value::Str(PinTypeName(p.type)));
    if (!p.label.empty()) o.Set(L"label", json::Value::Str(p.label));
    if (!p.icon.empty()) o.Set(L"icon", json::Value::Str(p.icon));
    if (p.type == PinType::Group) {
        json::Value items = json::Value::MakeArray();
        for (auto& c : p.items) items.arr.push_back(PinToJson(c));
        o.Set(L"items", items);
    }
    return o;
}

static std::vector<std::wstring> StrList(const json::Value& root, const wchar_t* key, bool* present = nullptr) {
    std::vector<std::wstring> out;
    const json::Value* v = root.Get(key);
    if (present) *present = v && v->type == json::Value::Array;
    if (v && v->type == json::Value::Array) for (auto& e : v->arr) if (e.type == json::Value::String) out.push_back(e.s);
    return out;
}

static json::Value StrArray(const std::vector<std::wstring>& v) {
    json::Value a = json::Value::MakeArray();
    for (auto& s : v) a.arr.push_back(json::Value::Str(s));
    return a;
}

bool Config::IsHidden(const std::wstring& id) const {
    for (auto& h : hidden) if (_wcsicmp(h.c_str(), id.c_str()) == 0) return true;
    return false;
}

bool Config::IsPinned(const std::wstring& id) const {
    for (auto& p : pins) {
        if (p.type != PinType::Group && _wcsicmp(p.id.c_str(), id.c_str()) == 0) return true;
        for (auto& c : p.items) if (_wcsicmp(c.id.c_str(), id.c_str()) == 0) return true;
    }
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
    auto app = [](const wchar_t* id, const wchar_t* name) { Pin p; p.id = id; p.name = name; return p; };
    g_config.pins = {
        app(L"Microsoft.Windows.Explorer", L"File Explorer"),
        app(L"windows.immersivecontrolpanel_cw5n1h2txyewy!microsoft.windows.immersivecontrolpanel", L"Settings"),
        app(L"Microsoft.WindowsTerminal_8wekyb3d8bbwe!App", L"Terminal"),
        app(L"Microsoft.WindowsNotepad_8wekyb3d8bbwe!App", L"Notepad"),
        app(L"Microsoft.WindowsCalculator_8wekyb3d8bbwe!App", L"Calculator"),
        app(L"Microsoft.Windows.Photos_8wekyb3d8bbwe!App", L"Photos"),
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

    c.theme = root.GetStr(L"theme", d.theme);
    if (c.theme.compare(0, 5, L"file:") != 0)
        c.theme = OneOf(c.theme, {L"dark", L"light", L"midnight", L"glass", L"ember", L"ocean", L"system"}, L"dark");
    c.font = root.GetStr(L"font");
    c.fontScale = std::clamp((float)root.GetNum(L"fontScale", d.fontScale), 0.85f, 1.3f);
    c.askedInstall = root.GetBool(L"askedInstall", false);
    c.elevatedMode = root.GetBool(L"elevatedMode", false);
    c.recentEnabled = root.GetBool(L"recentEnabled", d.recentEnabled);
    c.hiddenRecent = StrList(root, L"hiddenRecent");
    c.myAppsGroup = root.GetBool(L"myAppsGroup", d.myAppsGroup);
    bool hasShortcuts = false;
    std::vector<std::wstring> sc = StrList(root, L"shortcuts", &hasShortcuts);
    if (hasShortcuts) c.shortcuts = sc;
    c.searchFiles = root.GetBool(L"searchFiles", d.searchFiles);
    c.searchSettings = root.GetBool(L"searchSettings", d.searchSettings);
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
            Pin pin = PinFromJson(v, true);
            if (!pin.id.empty() || pin.type == PinType::Group) c.pins.push_back(pin);
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
    root.Set(L"askedInstall", json::Value::Boolean(c.askedInstall));
    root.Set(L"elevatedMode", json::Value::Boolean(c.elevatedMode));
    root.Set(L"recentEnabled", json::Value::Boolean(c.recentEnabled));
    root.Set(L"hiddenRecent", StrArray(c.hiddenRecent));
    root.Set(L"myAppsGroup", json::Value::Boolean(c.myAppsGroup));
    root.Set(L"shortcuts", StrArray(c.shortcuts));
    root.Set(L"searchFiles", json::Value::Boolean(c.searchFiles));
    root.Set(L"searchSettings", json::Value::Boolean(c.searchSettings));
    root.Set(L"font", json::Value::Str(c.font));
    root.Set(L"fontScale", json::Value::Num(std::round(c.fontScale * 100) / 100));
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
    for (auto& p : c.pins) pins.arr.push_back(PinToJson(p));
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
