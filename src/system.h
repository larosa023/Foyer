// Foyer - installation, admin mode and accessibility plumbing
#pragma once
#include "common.h"
#include <functional>

// ---------------------------------------------------------------- elevation
bool ProcessIsElevated();
// Starts something as the signed-in user even when Foyer runs as administrator, by asking
// Explorer to launch it. Falls back to a normal ShellExecute when Foyer isn't elevated.
bool ShellExecAsUser(const std::wstring& file, const std::wstring& args, const std::wstring& dir, const wchar_t* verb = nullptr);

// "Work over admin windows": Foyer starts as administrator at sign-in through a scheduled
// task, so it can open over elevated apps.
bool AdminModeTaskExists();
bool AdminModeEnable(HWND owner);    // one UAC prompt; the elevated copy takes over
bool AdminModeDisable(HWND owner);   // removes the task and restarts Foyer normally

// Called from wWinMain before the single-instance check.
// Returns true if this process should exit right away.
bool HandleStartupSwitches(const wchar_t* cmdLine);
// After ConfigLoad: applies what the switches above asked for (admin mode on, start-up path...).
void ApplyStartupSwitches(const wchar_t* cmdLine);

// ---------------------------------------------------------------- install
std::wstring InstallDir();           // %LOCALAPPDATA%\Programs\Foyer
bool IsInstalledCopy();
bool IsPortableCopy();
bool InstallFoyer(HWND owner, std::wstring* error);   // copies itself, shortcut, Apps & features, then restarts there
void MaybeOfferInstall(HWND owner);  // first run from Downloads etc.
int UninstallFoyer();                // "Foyer.exe --uninstall"
void RestartFoyer(const std::wstring& exe, const std::wstring& extraArgs, bool asUser);

// ---------------------------------------------------------------- accessibility
namespace a11y {
void Attach(HWND hwnd, std::function<void()> activate);
LRESULT OnGetObject(HWND hwnd, WPARAM wParam, LPARAM lParam);
// What Narrator should read for the focused item: e.g. ("Notepad", "App"), and where it is.
void Focus(HWND hwnd, const std::wstring& name, const std::wstring& description, RECT screen, bool isSearchBox, const std::wstring& value);
bool HighContrast();
float TextScale();                   // Windows "Text size" (1.0 - 2.25)
bool ReduceMotion();                 // Windows "Animation effects" off
}
