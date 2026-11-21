// Foyer - search providers: settings catalog, file index / Everything, usage ranking
#pragma once
#include "common.h"

namespace search {

enum class Scope { All, Apps, Files, Settings, Run };

struct Query {
    Scope scope = Scope::All;
    std::wstring text;      // without the prefix, trimmed
    std::wstring lower;
};

// "f: report" -> Files, "s: bluetooth" -> Settings, "a: chrome" -> Apps, "> cmd" -> Run
Query Parse(const std::wstring& raw);

// Relevance of `name` for `q` (lower-case). -1 = no match. Handles prefixes, word starts,
// initials ("vsc" -> Visual Studio Code) and multi-word queries in any order.
int MatchScore(const std::wstring& nameLower, const std::wstring& q);

// ---------------------------------------------------------------- settings & Control Panel
struct SettingHit { std::wstring title, section, target; wchar_t glyph; int score; };
void SearchSettings(const std::wstring& q, std::vector<SettingHit>& out, size_t max);
void LaunchSetting(const std::wstring& target);

// ---------------------------------------------------------------- files
struct FileHit { std::wstring path, name, folder; bool dir = false; int score = 0; };
struct FileResults { unsigned gen = 0; std::vector<FileHit> hits; bool everything = false; bool indexing = false; };

void FilesInit(HWND notify, UINT doneMsg);   // starts the background index (low priority)
void FilesTouch();                            // the menu opened: refresh the index if files changed
void FilesChanged();                          // the watcher saw a change in an indexed folder
std::vector<std::wstring> FilesRoots();       // the folders the index covers
// Searches on a worker thread; posts doneMsg with a FileResults* (receiver deletes). Only the
// latest request is answered, so typing fast never queues work.
void FilesSearchAsync(unsigned gen, const std::wstring& q, size_t max, bool scoped);
bool EverythingRunning();
size_t FilesIndexed();

// ---------------------------------------------------------------- usage (frecency)
void UsageLoad();
void UsageRecord(const std::wstring& key);
int UsageBoost(const std::wstring& key);     // 0..400, decays over a few weeks

// ---------------------------------------------------------------- run command
bool RunCommand(const std::wstring& cmd, bool admin);

} // namespace search
