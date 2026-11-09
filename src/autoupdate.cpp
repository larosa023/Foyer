// Foyer - wiring between the reusable updater module and the app
#include "common.h"
#include "config.h"
#include "updater.h"
#include <ctime>

static HWND g_owner = nullptr;
static DWORD g_updatedFromPid = 0;
static bool g_installPending = false;   // Ready + auto-install, waiting for a quiet moment

static double UnixNow() { return (double)time(nullptr); }

void AutoUpdateApplySettings() {
    updater::SetBeta(g_config.channel == L"beta");
    if (!g_owner) return;
    KillTimer(g_owner, TIMER_UPDATE_CHECK);
    if (g_config.autoCheck) SetTimer(g_owner, TIMER_UPDATE_CHECK, 30 * 60 * 1000, nullptr);   // re-evaluate every 30 min
}

void AutoUpdateInit(HWND owner, const wchar_t* cmdLine) {
    g_owner = owner;
    updater::Settings s;
    s.owner = L"larosa023";
    s.repo = L"Foyer";
    s.exeAsset = L"Foyer.exe";
    s.sumsAsset = L"SHA256SUMS.txt";
    s.currentVersion = FOYER_VERSION;
    s.appName = L"Foyer";
    s.downloadDir = DataDir() + L"\\updates";
    s.beta = g_config.channel == L"beta";
    s.log = [](const std::wstring& m) { Log(L"%ls", m.c_str()); };

    std::wstring failed;
    if (updater::StartedAfterRollback(cmdLine, &failed)) {
        Log(L"Started after a rollback; version %ls failed to start", failed.c_str());
        s.skipVersion = failed;
    }
    updater::Configure(s, owner, WM_FOYER_UPDATE);

    if (updater::StartedAfterUpdate(cmdLine, &g_updatedFromPid)) {
        Log(L"Started after updating (previous pid %lu)", g_updatedFromPid);
    } else {
        updater::CleanupLeftovers();
    }
    if (!failed.empty()) {
        TrayBalloon(L"Foyer update didn't start", L"Foyer " + failed + L" failed to start, so the previous version was restored.");
    }

    // Report healthy a couple of seconds after start-up (catches early crashes), then
    // run the first update check shortly after.
    SetTimer(owner, TIMER_HEALTHY, 2500, nullptr);
    AutoUpdateApplySettings();
}

void AutoUpdateOnStarted() {
    if (g_updatedFromPid) {
        updater::ReportHealthy(g_updatedFromPid);
        g_updatedFromPid = 0;
        TrayBalloon(L"Foyer updated", L"You're now running Foyer " FOYER_VERSION L".");
    }
    // First check ~20s after start so it never competes with boot.
    if (g_config.autoCheck) SetTimer(g_owner, TIMER_UPDATE_CHECK, 20 * 1000, nullptr);
}

void AutoUpdateOnTimer() {
    // Back to the regular cadence after the first (short) timer.
    SetTimer(g_owner, TIMER_UPDATE_CHECK, 30 * 60 * 1000, nullptr);
    if (g_installPending) {
        AutoUpdateInstallNow();
        return;
    }
    if (!g_config.autoCheck) return;
    double due = g_config.lastCheck + g_config.checkHours * 3600.0;
    if (UnixNow() >= due) updater::CheckAsync(false);
}

void AutoUpdateCheckNow() { updater::CheckAsync(true); }
void AutoUpdateDownloadNow() { updater::DownloadAsync(); }

bool AutoUpdateAvailable() {
    auto st = updater::Get().state;
    return st == updater::State::Available || st == updater::State::Downloading ||
           st == updater::State::Verifying || st == updater::State::Ready;
}

void AutoUpdateInstallNow() {
    if (updater::Get().state != updater::State::Ready) return;
    if (MenuVisible() || updater::UserIsBusy()) {
        // Never interrupt a game, video or presentation; try again later.
        g_installPending = true;
        SetTimer(g_owner, TIMER_UPDATE_CHECK, 5 * 60 * 1000, nullptr);
        Log(L"Update install deferred (menu open or user busy)");
        return;
    }
    g_installPending = false;
    std::wstring error;
    bool ok = updater::InstallAndRestart([] {
        MenuHide();
        if (HWND sw = FindWindowW(L"FoyerSettings", nullptr)) DestroyWindow(sw);
        HookStop();
        TrayRemove();
        ReleaseInstanceMutex();
    }, &error);
    if (!ok) {
        Log(L"Update install failed: %ls", error.c_str());
        MessageBoxW(nullptr, (L"Foyer couldn't install the update.\n\n" + error).c_str(), L"Foyer", MB_ICONWARNING);
    }
}

void AutoUpdateOnStatus() {
    updater::Status st = updater::Get();
    if (st.state == updater::State::UpToDate || st.state == updater::State::Available) {
        g_config.lastCheck = UnixNow();
        ConfigSave();
    }
    if (st.state == updater::State::Available) {
        if (g_config.autoInstall) {
            updater::DownloadAsync();
        } else if (g_config.notifiedVersion != st.release.version) {
            g_config.notifiedVersion = st.release.version;
            ConfigSave();
            TrayBalloon(L"Foyer " + st.release.version + L" is available", L"Click to see what's new and update.");
        }
    } else if (st.state == updater::State::Ready && g_config.autoInstall) {
        AutoUpdateInstallNow();
    }
    SettingsOnUpdateStatus();
    MenuReloadConfig();
}
