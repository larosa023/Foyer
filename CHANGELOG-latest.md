## Foyer 0.3.1 — refinement and performance

Tuned against a week of real usage logs.

### Faster
- **Complete from the first press after boot.** The app list is saved and loads instantly, instead of showing "Loading apps" for about two seconds while Windows enumerates apps.
- **File search works immediately at startup.** The file index is saved to disk and reused, instead of being rebuilt from scratch first.
- **Smoother across monitors.** Moving the menu to a monitor with different scaling no longer rebuilds every icon (that was behind the slowest opens, up to about 60 ms).
- **Typing is lighter.** File search waits for a brief pause in typing instead of searching the disk on every keystroke.

### Much less background work
- Foyer now **watches for changes** instead of re-checking on every open: the app list refreshes only when a Start menu shortcut or a Store app changes, Recent only when you open a file, and the file index only when your folders actually change. (Last week Foyer re-scanned all apps 161 times and rebuilt the file index 47 times. Now it's only when something changed.)
- The taskbar Start button is located again only when a taskbar moves or its alignment changes, instead of on many clicks and setting changes.

### Fixed
- **File search could be unavailable for 30–50 minutes.** Foyer pauses background work while you game, but an ordinary maximized window (with an auto-hiding taskbar) looked like a fullscreen game. Only real borderless fullscreen apps count now. This also makes automatic updates wait less.
- **Typing sometimes went to the app behind Foyer** when Windows refused to give it focus. Foyer now retries until it gets focus.
