// Reusable GitHub Releases updater - see updater.h
#ifndef UNICODE
#define UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "updater.h"
#include "json.h"
#include <winhttp.h>
#include <bcrypt.h>
#include <shellapi.h>
#include <shlobj.h>
#include <vector>
#include <algorithm>
#include <cwctype>

namespace updater {

static Settings s_cfg;
static HWND s_notify = nullptr;
static UINT s_msg = 0;
static SRWLOCK s_lock = SRWLOCK_INIT;
static Status s_status;
static volatile LONG s_busy = 0;

static void Log(const std::wstring& m) { if (s_cfg.log) s_cfg.log(L"Updater: " + m); }

static std::wstring ExePathU() {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, buf, (DWORD)(sizeof(buf) / sizeof(buf[0])));
    return std::wstring(buf, n);
}

static void SetStatus(const Status& st) {
    AcquireSRWLockExclusive(&s_lock);
    s_status = st;
    ReleaseSRWLockExclusive(&s_lock);
    if (s_notify) PostMessageW(s_notify, s_msg, (WPARAM)st.state, 0);
}

Status Get() {
    AcquireSRWLockShared(&s_lock);
    Status st = s_status;
    ReleaseSRWLockShared(&s_lock);
    return st;
}

bool Busy() { return s_busy != 0; }

void Configure(const Settings& s, HWND notify, UINT msg) {
    s_cfg = s;
    s_notify = notify;
    s_msg = msg;
    if (!s_cfg.downloadDir.empty()) SHCreateDirectoryExW(nullptr, s_cfg.downloadDir.c_str(), nullptr);
}

void SetBeta(bool beta) { s_cfg.beta = beta; }
void SetSkipVersion(const std::wstring& v) { s_cfg.skipVersion = v; }

// ---------------------------------------------------------------- versions
static std::vector<int> VersionParts(const std::wstring& v, std::wstring* pre) {
    std::wstring s = v;
    if (!s.empty() && (s[0] == L'v' || s[0] == L'V')) s = s.substr(1);
    size_t dash = s.find_first_of(L"-+");
    if (pre) *pre = dash == std::wstring::npos ? L"" : s.substr(dash + 1);
    if (dash != std::wstring::npos) s = s.substr(0, dash);
    std::vector<int> parts;
    size_t i = 0;
    while (i <= s.size()) {
        size_t dot = s.find(L'.', i);
        std::wstring p = s.substr(i, dot == std::wstring::npos ? std::wstring::npos : dot - i);
        parts.push_back(_wtoi(p.c_str()));
        if (dot == std::wstring::npos) break;
        i = dot + 1;
    }
    while (parts.size() < 3) parts.push_back(0);
    return parts;
}

int CompareVersions(const std::wstring& a, const std::wstring& b) {
    std::wstring pa, pb;
    auto x = VersionParts(a, &pa), y = VersionParts(b, &pb);
    size_t n = std::max(x.size(), y.size());
    x.resize(n, 0); y.resize(n, 0);
    for (size_t i = 0; i < n; ++i) if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1;
    // 1.0.0-beta.1 < 1.0.0
    if (pa.empty() != pb.empty()) return pa.empty() ? 1 : -1;
    return pa.compare(pb) < 0 ? -1 : (pa == pb ? 0 : 1);
}

static std::wstring StripV(const std::wstring& tag) {
    return (!tag.empty() && (tag[0] == L'v' || tag[0] == L'V')) ? tag.substr(1) : tag;
}

// ---------------------------------------------------------------- HTTP (WinHTTP)
struct Url { std::wstring host, path; INTERNET_PORT port = 443; bool https = true; };

static bool CrackUrl(const std::wstring& url, Url& out) {
    URL_COMPONENTS uc{sizeof(uc)};
    wchar_t host[256], path[2048], extra[2048];
    uc.lpszHostName = host; uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path; uc.dwUrlPathLength = 2048;
    uc.lpszExtraInfo = extra; uc.dwExtraInfoLength = 2048;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) return false;
    out.host.assign(host, uc.dwHostNameLength);
    out.path.assign(path, uc.dwUrlPathLength);
    out.path.append(extra, uc.dwExtraInfoLength);
    out.port = uc.nPort;
    out.https = uc.nScheme == INTERNET_SCHEME_HTTPS;
    return true;
}

// GET a URL. Follows redirects (GitHub asset downloads redirect to a CDN).
static bool HttpGet(const std::wstring& url, const wchar_t* accept, std::string& body, DWORD maxBytes,
                    const std::function<void(DWORD, DWORD)>& progress, std::wstring* err) {
    Url u;
    if (!CrackUrl(url, u) || !u.https) { if (err) *err = L"Invalid download address."; return false; }
    std::wstring agent = (s_cfg.appName.empty() ? L"larosa023-app" : s_cfg.appName) + L"-Updater/" + s_cfg.currentVersion;
    HINTERNET ses = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) ses = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) { if (err) *err = L"Couldn't start a network session."; return false; }
    WinHttpSetTimeouts(ses, 10000, 10000, 15000, 30000);
    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | 0x00002000 /*TLS1_3*/;
    if (!WinHttpSetOption(ses, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols))) {
        protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
        WinHttpSetOption(ses, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
    }
    bool ok = false;
    HINTERNET con = WinHttpConnect(ses, u.host.c_str(), u.port, 0);
    HINTERNET req = con ? WinHttpOpenRequest(con, L"GET", u.path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                              WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
    if (req) {
        std::wstring headers = std::wstring(L"Accept: ") + (accept ? accept : L"*/*") + L"\r\nX-GitHub-Api-Version: 2022-11-28\r\n";
        if (WinHttpSendRequest(req, headers.c_str(), (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
            WinHttpReceiveResponse(req, nullptr)) {
            DWORD status = 0, sz = sizeof(status);
            WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &sz, nullptr);
            DWORD total = 0; sz = sizeof(total);
            WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &total, &sz, nullptr);
            if (status == 200) {
                ok = true;
                for (;;) {
                    DWORD avail = 0;
                    if (!WinHttpQueryDataAvailable(req, &avail)) { ok = false; if (err) *err = L"The download was interrupted."; break; }
                    if (!avail) break;
                    size_t old = body.size();
                    if (old + avail > maxBytes) { ok = false; if (err) *err = L"The download is larger than expected."; break; }
                    body.resize(old + avail);
                    DWORD read = 0;
                    if (!WinHttpReadData(req, &body[old], avail, &read)) { ok = false; if (err) *err = L"The download was interrupted."; break; }
                    body.resize(old + read);
                    if (progress) progress((DWORD)body.size(), total);
                }
            } else if (err) {
                if (status == 403 || status == 429) *err = L"GitHub is rate-limiting update checks. Try again later.";
                else if (status == 404) *err = L"No releases found.";
                else *err = L"GitHub returned an error (HTTP " + std::to_wstring(status) + L").";
            }
        } else if (err) {
            *err = L"Couldn't reach GitHub. Check your internet connection.";
        }
        WinHttpCloseHandle(req);
    } else if (err) {
        *err = L"Couldn't reach GitHub.";
    }
    if (con) WinHttpCloseHandle(con);
    WinHttpCloseHandle(ses);
    return ok;
}

static std::wstring Utf8ToWide(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    if (n) MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

// ---------------------------------------------------------------- check
static DWORD WINAPI CheckThread(LPVOID p) {
    bool manual = p != nullptr;
    Status st; st.state = State::Checking; st.message = L"Checking for updates\x2026";
    SetStatus(st);

    std::wstring url = L"https://api.github.com/repos/" + s_cfg.owner + L"/" + s_cfg.repo + L"/releases?per_page=15";
    std::string body;
    std::wstring err;
    if (!HttpGet(url, L"application/vnd.github+json", body, 4 * 1024 * 1024, nullptr, &err)) {
        Log(L"check failed: " + err);
        st = {}; st.state = State::Error; st.message = err;
        SetStatus(st);
        InterlockedExchange(&s_busy, 0);
        return 0;
    }
    std::wstring text = Utf8ToWide(body);
    json::Value root;
    json::Parser parser(text);
    if (!parser.Parse(root) || root.type != json::Value::Array) {
        st = {}; st.state = State::Error; st.message = L"Couldn't read the release list from GitHub.";
        SetStatus(st);
        InterlockedExchange(&s_busy, 0);
        return 0;
    }

    Release best;
    bool found = false;
    for (const json::Value& r : root.arr) {
        if (r.GetBool(L"draft", false)) continue;
        bool pre = r.GetBool(L"prerelease", false);
        if (pre && !s_cfg.beta) continue;
        Release rel;
        rel.tag = r.GetStr(L"tag_name");
        rel.version = StripV(rel.tag);
        rel.title = r.GetStr(L"name", rel.tag);
        rel.notes = r.GetStr(L"body");
        rel.page = r.GetStr(L"html_url");
        rel.prerelease = pre;
        if (const json::Value* assets = r.Get(L"assets"); assets && assets->type == json::Value::Array) {
            for (const json::Value& a : assets->arr) {
                std::wstring name = a.GetStr(L"name");
                if (_wcsicmp(name.c_str(), s_cfg.exeAsset.c_str()) == 0) rel.exeUrl = a.GetStr(L"browser_download_url");
                if (_wcsicmp(name.c_str(), s_cfg.sumsAsset.c_str()) == 0) rel.sumsUrl = a.GetStr(L"browser_download_url");
            }
        }
        if (rel.version.empty() || rel.exeUrl.empty()) continue;
        if (!found || CompareVersions(rel.version, best.version) > 0) { best = rel; found = true; }
    }

    st = {};
    if (found && CompareVersions(best.version, s_cfg.currentVersion) > 0 &&
        (manual || s_cfg.skipVersion.empty() || CompareVersions(best.version, s_cfg.skipVersion) != 0)) {
        st.state = State::Available;
        st.release = best;
        st.message = s_cfg.appName + L" " + best.version + L" is available";
        Log(L"update available: " + best.version);
    } else {
        st.state = State::UpToDate;
        st.message = L"You're up to date";
        Log(L"up to date (" + s_cfg.currentVersion + L")");
    }
    SetStatus(st);
    InterlockedExchange(&s_busy, 0);
    return 0;
}

void CheckAsync(bool manual) {
    if (InterlockedCompareExchange(&s_busy, 1, 0) != 0) return;
    HANDLE h = CreateThread(nullptr, 0, CheckThread, manual ? (LPVOID)1 : nullptr, 0, nullptr);
    if (h) CloseHandle(h); else InterlockedExchange(&s_busy, 0);
}

// ---------------------------------------------------------------- download + verify
static std::wstring Sha256Hex(const std::string& data) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    unsigned char digest[32];
    std::wstring hex;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0) {
        if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
            if (BCryptHashData(hash, (PUCHAR)data.data(), (ULONG)data.size(), 0) == 0 &&
                BCryptFinishHash(hash, digest, sizeof(digest), 0) == 0) {
                wchar_t b[3];
                for (unsigned char c : digest) { swprintf(b, 3, L"%02x", c); hex += b; }
            }
            BCryptDestroyHash(hash);
        }
        BCryptCloseAlgorithmProvider(alg, 0);
    }
    return hex;
}

static std::wstring ExpectedHash(const std::string& sums, const std::wstring& asset) {
    std::wstring text = Utf8ToWide(sums);
    size_t i = 0;
    while (i < text.size()) {
        size_t nl = text.find(L'\n', i);
        std::wstring line = text.substr(i, nl == std::wstring::npos ? std::wstring::npos : nl - i);
        i = nl == std::wstring::npos ? text.size() : nl + 1;
        while (!line.empty() && (line.back() == L'\r' || line.back() == L' ')) line.pop_back();
        size_t sp = line.find_first_of(L" \t");
        if (sp == std::wstring::npos) continue;
        std::wstring hash = line.substr(0, sp);
        std::wstring name = line.substr(line.find_first_not_of(L" \t*", sp));
        if (_wcsicmp(name.c_str(), asset.c_str()) == 0 && hash.size() == 64) {
            for (auto& c : hash) c = (wchar_t)towlower(c);
            return hash;
        }
    }
    return L"";
}

static DWORD WINAPI DownloadThread(LPVOID) {
    Status st = Get();
    Release rel = st.release;
    auto fail = [&](const std::wstring& m) {
        Log(L"download failed: " + m);
        Status e; e.state = State::Error; e.message = m; e.release = rel;
        SetStatus(e);
        InterlockedExchange(&s_busy, 0);
        return (DWORD)0;
    };
    if (rel.sumsUrl.empty()) return fail(L"This release has no checksum file, so it can't be verified. Update skipped.");

    std::string sums, exe;
    std::wstring err;
    if (!HttpGet(rel.sumsUrl, nullptr, sums, 64 * 1024, nullptr, &err)) return fail(err);
    std::wstring expected = ExpectedHash(sums, s_cfg.exeAsset);
    if (expected.empty()) return fail(L"The checksum file doesn't list " + s_cfg.exeAsset + L".");

    st = {}; st.state = State::Downloading; st.release = rel; st.message = L"Downloading " + rel.version + L"\x2026";
    SetStatus(st);
    DWORD lastPct = (DWORD)-1;
    auto progress = [&](DWORD got, DWORD total) {
        if (!total) return;
        DWORD pct = (DWORD)((unsigned long long)got * 100 / total);
        if (pct != lastPct) {
            lastPct = pct;
            Status p; p.state = State::Downloading; p.release = rel; p.percent = (int)pct;
            p.message = L"Downloading " + rel.version + L"\x2026 " + std::to_wstring(pct) + L"%";
            SetStatus(p);
        }
    };
    if (!HttpGet(rel.exeUrl, L"application/octet-stream", exe, 64 * 1024 * 1024, progress, &err)) return fail(err);

    st = {}; st.state = State::Verifying; st.release = rel; st.message = L"Verifying download\x2026";
    SetStatus(st);
    if (exe.size() < 2 || exe[0] != 'M' || exe[1] != 'Z') return fail(L"The download isn't a valid program file.");
    std::wstring actual = Sha256Hex(exe);
    if (actual != expected) return fail(L"The download failed verification (checksum mismatch). Update skipped.");

    std::wstring path = s_cfg.downloadDir + L"\\" + s_cfg.appName + L"-" + rel.version + L".exe";
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return fail(L"Couldn't save the download.");
    DWORD written = 0;
    bool ok = WriteFile(h, exe.data(), (DWORD)exe.size(), &written, nullptr) && written == exe.size();
    CloseHandle(h);
    if (!ok) return fail(L"Couldn't save the download.");

    Log(L"downloaded and verified " + rel.version + L" (sha256 " + actual + L")");
    st = {}; st.state = State::Ready; st.release = rel; st.downloadedPath = path;
    st.message = s_cfg.appName + L" " + rel.version + L" is ready to install";
    SetStatus(st);
    InterlockedExchange(&s_busy, 0);
    return 0;
}

void DownloadAsync() {
    if (Get().release.exeUrl.empty()) return;
    if (InterlockedCompareExchange(&s_busy, 1, 0) != 0) return;
    HANDLE h = CreateThread(nullptr, 0, DownloadThread, nullptr, 0, nullptr);
    if (h) CloseHandle(h); else InterlockedExchange(&s_busy, 0);
}

// ---------------------------------------------------------------- install
static std::wstring HealthyEventName(DWORD pid) {
    return L"Local\\" + (s_cfg.appName.empty() ? std::wstring(L"App") : s_cfg.appName) + L".UpdateHealthy." + std::to_wstring(pid);
}

static bool Launch(const std::wstring& exe, const std::wstring& args, PROCESS_INFORMATION* pi) {
    std::wstring cmd = L"\"" + exe + L"\" " + args;
    STARTUPINFOW si{sizeof(si)};
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    return CreateProcessW(exe.c_str(), buf.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, pi) != FALSE;
}

bool InstallAndRestart(const std::function<void()>& releaseApp, std::wstring* error) {
    Status st = Get();
    if (st.state != State::Ready || st.downloadedPath.empty()) { if (error) *error = L"No verified update is ready."; return false; }
    std::wstring exe = ExePathU();
    std::wstring old = exe + L".old";
    std::wstring version = st.release.version;
    Log(L"installing " + version);

    // 1. Move the running exe aside (allowed while running) and put the new one in place.
    if (!MoveFileExW(exe.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        if (error) *error = L"Couldn't replace the program file. If it's in a protected folder (like Program Files), move it to a folder you own.";
        Log(L"move to .old failed: " + std::to_wstring(GetLastError()));
        return false;
    }
    if (!CopyFileW(st.downloadedPath.c_str(), exe.c_str(), FALSE)) {
        DWORD e = GetLastError();
        MoveFileExW(old.c_str(), exe.c_str(), MOVEFILE_REPLACE_EXISTING);
        if (error) *error = L"Couldn't write the new version (error " + std::to_wstring(e) + L").";
        return false;
    }

    Status inst; inst.state = State::Installing; inst.release = st.release; inst.message = L"Restarting\x2026";
    SetStatus(inst);

    // 2. Let go of everything the new process needs (hooks, tray, single-instance mutex).
    releaseApp();

    // 3. Start the new version and wait for it to say it's healthy.
    DWORD me = GetCurrentProcessId();
    HANDLE healthy = CreateEventW(nullptr, TRUE, FALSE, HealthyEventName(me).c_str());
    PROCESS_INFORMATION pi{};
    bool started = Launch(exe, L"--updated " + std::to_wstring(me), &pi);
    bool ok = false;
    if (started) {
        HANDLE waits[2] = {healthy, pi.hProcess};
        DWORD r = WaitForMultipleObjects(2, waits, FALSE, 30000);
        ok = (r == WAIT_OBJECT_0);
        if (!ok) {
            Log(L"new version did not report healthy (wait result " + std::to_wstring(r) + L"); rolling back");
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 5000);
        }
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    } else {
        Log(L"couldn't start the new version; rolling back");
    }
    if (healthy) CloseHandle(healthy);

    if (ok) {
        Log(L"update to " + version + L" succeeded");
        ExitProcess(0);
    }

    // 4. Rollback: restore the previous exe and start it again.
    MoveFileExW(exe.c_str(), (exe + L".bad").c_str(), MOVEFILE_REPLACE_EXISTING);
    MoveFileExW(old.c_str(), exe.c_str(), MOVEFILE_REPLACE_EXISTING);
    PROCESS_INFORMATION rp{};
    if (Launch(exe, L"--rolled-back " + version, &rp)) {
        CloseHandle(rp.hThread);
        CloseHandle(rp.hProcess);
    }
    ExitProcess(0);
}

static bool ArgAfter(const wchar_t* cmdLine, const wchar_t* flag, std::wstring* value) {
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

bool StartedAfterUpdate(const wchar_t* cmdLine, DWORD* oldPid) {
    std::wstring v;
    if (!ArgAfter(cmdLine, L"--updated", &v)) return false;
    if (oldPid) *oldPid = (DWORD)_wtol(v.c_str());
    return true;
}

bool StartedAfterRollback(const wchar_t* cmdLine, std::wstring* failedVersion) {
    return ArgAfter(cmdLine, L"--rolled-back", failedVersion);
}

void ReportHealthy(DWORD oldPid) {
    if (!oldPid) return;
    HANDLE e = OpenEventW(EVENT_MODIFY_STATE, FALSE, HealthyEventName(oldPid).c_str());
    if (e) { SetEvent(e); CloseHandle(e); Log(L"reported healthy to previous process"); }
}

void CleanupLeftovers() {
    std::wstring exe = ExePathU();
    DeleteFileW((exe + L".old").c_str());
    DeleteFileW((exe + L".bad").c_str());
    if (s_cfg.downloadDir.empty()) return;
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((s_cfg.downloadDir + L"\\*.exe").c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE) return;
    do { DeleteFileW((s_cfg.downloadDir + L"\\" + fd.cFileName).c_str()); } while (FindNextFileW(f, &fd));
    FindClose(f);
}

bool UserIsBusy() {
    QUERY_USER_NOTIFICATION_STATE q;
    if (SUCCEEDED(SHQueryUserNotificationState(&q))) {
        if (q == QUNS_BUSY || q == QUNS_RUNNING_D3D_FULL_SCREEN || q == QUNS_PRESENTATION_MODE) return true;
    }
    // Borderless fullscreen games and videos: the foreground window covers its whole monitor.
    HWND fg = GetForegroundWindow();
    if (fg && fg != GetDesktopWindow() && fg != GetShellWindow()) {
        wchar_t cls[64] = {};
        GetClassNameW(fg, cls, 64);
        if (wcscmp(cls, L"WorkerW") != 0 && wcscmp(cls, L"Progman") != 0) {
            RECT wr; MONITORINFO mi{sizeof(mi)};
            if (GetWindowRect(fg, &wr) && GetMonitorInfoW(MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST), &mi) &&
                wr.left <= mi.rcMonitor.left && wr.top <= mi.rcMonitor.top &&
                wr.right >= mi.rcMonitor.right && wr.bottom >= mi.rcMonitor.bottom) return true;
        }
    }
    return false;
}

} // namespace updater
