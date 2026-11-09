// Reusable GitHub Releases updater for larosa023 apps (Foyer, Faderly, Ember, Tonely, ...)
//
// Drop updater.h/.cpp + json.h into an app, then:
//   1. updater::Configure({owner, repo, "App.exe", "SHA256SUMS.txt", version, ...}, hwnd, WM_APP_UPDATE);
//   2. updater::CheckAsync();               -> hwnd receives WM_APP_UPDATE whenever Get() changes
//   3. updater::DownloadAsync();            -> downloads the exe and verifies its SHA-256
//   4. updater::InstallAndRestart(release); -> swaps the exe, relaunches, rolls back on failure
//   On start-up the app calls updater::StartedAfterUpdate(cmdLine, &pid) and, once it is
//   running properly, updater::ReportHealthy(pid). If that never happens the old process
//   restores the previous exe and starts it again with "--rolled-back <version>".
//
// Release requirements: a GitHub Release whose tag is the version ("v1.2.3"), with the exe
// and a SHA256SUMS.txt ("<sha256>  App.exe" per line) attached. Pre-releases are the Beta channel.
#pragma once
#include <windows.h>
#include <string>
#include <functional>

namespace updater {

struct Settings {
    std::wstring owner;            // "larosa023"
    std::wstring repo;             // "Foyer"
    std::wstring exeAsset;         // "Foyer.exe"
    std::wstring sumsAsset;        // "SHA256SUMS.txt"
    std::wstring currentVersion;   // "0.2.0"
    std::wstring appName;          // "Foyer" (user agent, messages)
    std::wstring downloadDir;      // where downloads are kept
    bool beta = false;             // include pre-releases
    std::wstring skipVersion;      // a version that failed to start; ignored by automatic checks
    std::function<void(const std::wstring&)> log;
};

enum class State { Idle, Checking, UpToDate, Available, Downloading, Verifying, Ready, Installing, Error };

struct Release {
    std::wstring tag, version, title, notes, page, exeUrl, sumsUrl;
    bool prerelease = false;
};

struct Status {
    State state = State::Idle;
    std::wstring message;          // human-readable status line
    Release release;               // valid for Available / Downloading / Ready
    int percent = 0;               // download progress
    std::wstring downloadedPath;   // valid for Ready
};

void Configure(const Settings& s, HWND notify, UINT msg);
void SetBeta(bool beta);
void SetSkipVersion(const std::wstring& v);
Status Get();
bool Busy();

void CheckAsync(bool manual);      // manual checks also offer a previously skipped version
void DownloadAsync();

// Replaces the running exe with the downloaded one and starts it. `releaseApp` must stop
// hooks, remove tray icons and release any single-instance mutex. On success this does not
// return (the process exits). On failure it restores everything it changed and returns false.
bool InstallAndRestart(const std::function<void()>& releaseApp, std::wstring* error);

// New-process side
bool StartedAfterUpdate(const wchar_t* cmdLine, DWORD* oldPid);
bool StartedAfterRollback(const wchar_t* cmdLine, std::wstring* failedVersion);
void ReportHealthy(DWORD oldPid);
void CleanupLeftovers();           // removes App.exe.old / .bad and stale downloads

// Helpers
int CompareVersions(const std::wstring& a, const std::wstring& b);   // <0, 0, >0
bool UserIsBusy();                 // fullscreen game/video, presentation mode, quiet hours

} // namespace updater
