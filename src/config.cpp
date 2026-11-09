// Foyer - settings and pins persisted as config.json
#include "config.h"
#include "json.h"

Config g_config;

static std::wstring ConfigPath() { return DataDir() + L"\\config.json"; }

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

static void DefaultPins() {
    g_config.pins = {
        {L"Microsoft.Windows.Explorer", L"File Explorer"},
        {L"windows.immersivecontrolpanel_cw5n1h2txyewy!microsoft.windows.immersivecontrolpanel", L"Settings"},
        {L"Microsoft.WindowsTerminal_8wekyb3d8bbwe!App", L"Terminal"},
        {L"Microsoft.WindowsNotepad_8wekyb3d8bbwe!App", L"Notepad"},
        {L"Microsoft.WindowsCalculator_8wekyb3d8bbwe!App", L"Calculator"},
        {L"Microsoft.Windows.Photos_8wekyb3d8bbwe!App", L"Photos"},
    };
}

void ConfigLoad() {
    std::wstring text;
    json::Value root;
    if (!ReadUtf8File(ConfigPath(), text)) {
        Log(L"No config.json yet; using defaults");
        DefaultPins();
        ConfigSave();
        return;
    }
    json::Parser p(text);
    if (!p.Parse(root) || root.type != json::Value::Object) {
        Log(L"config.json is invalid; backing it up and using defaults");
        MoveFileExW(ConfigPath().c_str(), (ConfigPath() + L".bad").c_str(), MOVEFILE_REPLACE_EXISTING);
        DefaultPins();
        ConfigSave();
        return;
    }
    g_config.interceptStartButton = root.GetBool(L"interceptStartButton", true);
    g_config.interceptWinKey = root.GetBool(L"interceptWinKey", true);
    g_config.pins.clear();
    if (const json::Value* pins = root.Get(L"pins"); pins && pins->type == json::Value::Array) {
        for (auto& v : pins->arr) {
            Pin pin{v.GetStr(L"id"), v.GetStr(L"name")};
            if (!pin.id.empty()) g_config.pins.push_back(pin);
        }
    }
    g_config.hidden.clear();
    if (const json::Value* hid = root.Get(L"hidden"); hid && hid->type == json::Value::Array) {
        for (auto& v : hid->arr) if (v.type == json::Value::String) g_config.hidden.push_back(v.s);
    }
    Log(L"Config loaded: %u pins, %u hidden", (unsigned)g_config.pins.size(), (unsigned)g_config.hidden.size());
}

void ConfigSave() {
    json::Value root = json::Value::MakeObject();
    root.Set(L"version", json::Value::Num(1));
    root.Set(L"interceptWinKey", json::Value::Boolean(g_config.interceptWinKey));
    root.Set(L"interceptStartButton", json::Value::Boolean(g_config.interceptStartButton));
    json::Value pins = json::Value::MakeArray();
    for (auto& p : g_config.pins) {
        json::Value o = json::Value::MakeObject();
        o.Set(L"id", json::Value::Str(p.id));
        o.Set(L"name", json::Value::Str(p.name));
        pins.arr.push_back(o);
    }
    root.Set(L"pins", pins);
    json::Value hid = json::Value::MakeArray();
    for (auto& h : g_config.hidden) hid.arr.push_back(json::Value::Str(h));
    root.Set(L"hidden", hid);

    std::wstring text;
    json::Write(root, text);
    text += L"\n";
    if (!WriteUtf8FileAtomic(ConfigPath(), text)) Log(L"Failed to save config.json (error %lu)", GetLastError());
}
