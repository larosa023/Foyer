# Foyer

A fast, clean, fully customizable replacement for the Windows 11 Start menu.
Part of the larosa023 suite of lightweight Windows apps.

**Status: v0.2.0** — Win key + Start button interception with fallbacks, search, pinned apps,
full All apps list, account and power menus, a full settings window, themes (Dark, Light,
Midnight, Glass, Ember, Ocean, Follow Windows) with acrylic/Mica, three layouts (Modern,
Classic, Full screen), animations, and a built-in updater with automatic rollback.

---

## Run it

1. Put `Foyer.exe` anywhere (e.g. `C:\Apps\Foyer\`) and double-click it.
   A small doorway icon appears in the tray. No installer, no admin rights.
2. Tap the **Windows key** or click the **Start button**: Foyer opens.
3. To quit: tray icon → **Quit Foyer**. The Windows Start menu is back instantly.

Optional portable mode: create an empty file named `portable` next to `Foyer.exe`
and settings go in a `data` folder beside it instead of `%APPDATA%\Foyer`.

Windows may show a SmartScreen warning the first time because the exe isn't
code-signed yet: **More info → Run anyway**.

## Getting to the real Start menu (always works)

| Way | Notes |
|---|---|
| **Ctrl+Win** | Hold Ctrl, tap Win |
| **Ctrl+Esc** | Foyer never touches this |
| **Shift+click** the Start button | |
| Tray → **Open Windows Start menu** | |
| Tray → **Pause Foyer** | Win key and Start button go back to Windows until unpaused |
| Quit or crash | Windows removes Foyer's hooks with the process, so Start works on the next key press |
| Foyer frozen | If the Foyer window stops responding, the hook lets the key through to Windows |
| Admin window focused | Foyer steps aside and Windows opens its own Start menu (see below) |

## Using Foyer

- **Type** to search apps (prefix, word-start, initials like `vsc`, and substring matches)
- **↑ ↓ ← →** move, **Tab** switches between Pinned and All apps, **PgUp/PgDn** page
- **Enter** launches, **Ctrl+Shift+Enter** runs as administrator
- **Esc** clears the search, then closes
- **Right-click** (or the Menu key) an app: run as admin, open file location, pin/unpin,
  move left/right, hide, uninstall
- Click a **letter** in All apps to jump to another letter
- Bottom bar: your name (lock / sign out / switch user), Downloads, File Explorer,
  Settings, Power (sleep, hibernate, shut down, restart, restart to BIOS/UEFI)
- **Ctrl+,**, the sliders icon in the bottom bar, right-clicking empty space, or the tray icon
  opens **Foyer settings**

## Settings

| Page | What's there |
|---|---|
| General | Open with Win key / Start button, pause, start with Windows, greeting, open the original Start menu |
| Appearance | Theme presets, accent color (or follow Windows), accent glow, Solid / Mica / Acrylic, tint, corner radius, animations, **live preview** |
| Layout | Modern (grid + list), Classic (single column), Full screen (launcher overlay); position, density, icon size |
| Apps | Reorder or remove pins, unhide apps, refresh the app list |
| Updates | Status, check now, download and install, automatic checks and installs, Stable / Beta channel, release notes |
| About | Version, GitHub, back up and restore all settings to one file, data folder, log, shortcuts |

Everything is stored in `config.json` in the data folder (Settings → About → Open data folder).

## Updates

Foyer checks GitHub Releases for larosa023/Foyer (every 1, 6 or 24 hours, or on demand).
An update is downloaded, its SHA-256 checked against the release's `SHA256SUMS.txt`, then
Foyer swaps its exe, restarts, and waits for the new version to report that it started
properly. If it doesn't within 30 seconds, the previous version is restored and started
again automatically. Automatic installs never happen while a fullscreen game, video or
presentation is running. The updater lives in `src/updater.h/.cpp` and is written to be
dropped into the other larosa023 apps unchanged.

---

## Tech stack

**Native C++17 + Win32, rendered with Direct2D/DirectWrite.** No framework, no runtime.

| Option | Verdict |
|---|---|
| **C++ / Win32 + Direct2D** ✅ | ~380 KB exe, a few MB of RAM, zero idle CPU, no runtime to install. The hook thread is tiny native code with no garbage collector pausing it — which matters, because Windows drops a low-level hook that is slow to answer. Full control over drawing, so themes, Mica/acrylic and layouts are just data. Cost: more code for UI widgets. |
| C# / .NET 8 (WPF or WinUI 3) | Faster to build settings screens. But 60–150 MB RAM, slower cold start right after boot, and GC pauses on the hook thread are a real risk. WinUI 3 also needs the Windows App SDK runtime. |
| Rust + windows-rs | Same performance as C++, but more ceremony for COM-heavy shell work. |
| Electron / Tauri / web UI | Too heavy and too slow to open for a Start menu. |

Foyer cross-compiles from Linux with Zig (`./build.sh`) or builds on Windows with
Visual Studio 2022 (`CMakeLists.txt`).

## How interception works

Windows opens Start when the Win key is **released** and no other key was pressed while it
was held. Foyer installs low-level keyboard and mouse hooks on a dedicated high-priority
thread that does nothing else:

1. **Every Win key-down passes through untouched**, so Win+E, Win+D, Win+R, Win+Shift+S,
   Win+arrow snapping etc. all work normally.
2. If Win is released **alone**, Foyer blocks that release and replays it behind an unused
   key code (`vkE8`, the same trick AutoHotkey uses). Windows sees a "combo" and doesn't
   open Start. Foyer shows its menu instead.
3. **Start button clicks**: Windows 11's taskbar is XAML, so Foyer asks UI Automation
   where the `StartButton` is on every taskbar (all monitors), caches those rectangles, and
   the mouse hook does a cheap point-in-rectangle test. A click there opens Foyer. The scan
   reruns when Explorer restarts, displays change, or settings change, and after a
   taskbar click if the button moved.

Nothing is injected into Explorer, no system files or policies are changed, and nothing
is written outside Foyer's own data folder and (if you enable it) the HKCU Run key.

**Why the fallbacks are reliable**

- Hooks belong to the Foyer process. When it exits or crashes, Windows removes them.
- Before swallowing a key, the hook checks whether Foyer's window is hung; if so, Windows
  gets the key.
- If replaying the key-up fails, the real key-up passes through, so Win can never get "stuck".
- Windows itself unhooks a low-level hook that keeps timing out — which again just restores
  the normal Start menu.
- Windows blocks normal apps from sending keystrokes to admin windows. When an elevated
  window is focused Foyer doesn't intercept, so Windows' Start menu opens. (Phase 5 can
  remove this limitation by running Foyer with UI access from Program Files.)

## Project structure

```
Foyer/
├─ src/
│  ├─ main.cpp          entry point, single instance, command line (--tray, --quit, --original)
│  ├─ hook.cpp          Win key + Start button interception and fallbacks
│  ├─ startbutton.cpp   finds Start button(s) on all taskbars via UI Automation
│  ├─ menu.cpp          the menu window: layouts, drawing, animation, keyboard/mouse, actions
│  ├─ settings.cpp      the settings window (custom-drawn, Mica, live apply + live preview)
│  ├─ gfx.cpp/.h        shared Direct2D/DirectWrite helpers and theme presets
│  ├─ updater.cpp/.h    reusable GitHub Releases updater (verify, swap, relaunch, rollback)
│  ├─ autoupdate.cpp    Foyer's update policy (schedule, notifications, quiet-time deferral)
│  ├─ apps.cpp/.h       installed apps (shell AppsFolder: desktop + Store), icons, launching
│  ├─ power.cpp         sleep / hibernate / shut down / restart / UEFI, lock / sign out / switch user
│  ├─ tray.cpp          tray icon and quick menu, Start with Windows
│  ├─ config.cpp/.h     config.json (pins, hidden apps, toggles)
│  ├─ json.h            tiny JSON reader/writer (also used for themes later)
│  ├─ log.cpp           foyer.log, data folder, portable mode
│  └─ common.h          shared declarations and message IDs
├─ res/                 icon, manifest (Per-Monitor DPI v2), version info
├─ build.sh             Zig cross-compile
└─ CMakeLists.txt       Visual Studio build
```

Planned additions: `search/` (file index, Everything SDK, settings catalog, calculator,
unit conversions, frecency), `recent.cpp`, `theme.cpp` + `themes/*.json`, `settings/`
(settings window), `updater/` (reusable GitHub Releases updater module shared with
Faderly, Ember, Tonely, etc.), `installer/`.

## Build

**Windows (Visual Studio 2022, "Desktop development with C++"):**
```
cmake -S . -B build -A x64
cmake --build build --config Release
```
**Linux/macOS:** `pip install ziglang && ./build.sh`

Output: `dist/Foyer.exe`

**GitHub Actions:** every push to `main` builds `Foyer.exe` (download it from the run's
*Artifacts*). **To ship a release, bump `FOYER_VERSION` in `src/common.h`, update
`CHANGELOG-latest.md`, and push to `main`** — the workflow publishes `v<version>` with
`Foyer.exe`, a zip and `SHA256SUMS.txt` (the hashes the updater checks). A version like
`0.3.0-beta.1` is published as a pre-release, which only Beta-channel users receive.

## Roadmap

- [x] **Phase 1** — interception + fallbacks, search bar, pinned, all apps, power/account menus, tray
- [ ] **Phase 2** — search: files (own index or Everything), folders, Settings pages, Control Panel,
      calculator, unit conversions, prefix filters (`f:` `s:` `>`), ranking by launch count; Recent
- [x] **Phase 3 (most)** — themes, Mica/acrylic, layout modes, settings window with live preview
- [ ] Phase 3 remainder — drag-to-reorder pins, pin folders, custom fonts, theme JSON import/export
- [x] **Phase 4 (most)** — reusable updater with hash check, rollback and Stable/Beta; backup/restore
- [ ] Phase 4 remainder — installer
- [ ] **Phase 5** — (animations done) accessibility, performance tuning, "My Apps" auto-detection, UI-access build
