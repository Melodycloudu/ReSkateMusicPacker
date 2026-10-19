// SPDX-FileCopyrightText: 2026 DeckardDetribine and the ReSkateMusicPacker contributors
// SPDX-License-Identifier: GPL-3.0-only
// What differs between Windows, macOS and Linux, behind one interface: running ffmpeg, where
// settings and caches live, PATH, and handing a URL or file to the desktop. platform_win32.cpp
// and platform_posix.cpp implement it; text crossing this boundary is UTF-8.
#pragma once
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace platform {

// UTF-8 <-> the platform's wchar_t (UTF-16 on Windows, UTF-32 elsewhere).
std::wstring widen(std::string_view utf8);
std::string narrow(std::wstring_view wide);

// ".exe" on Windows, empty elsewhere.
extern const char executable_suffix[];

// Runs args[0] (looked up on PATH) with the rest as its arguments. There is no shell, so nothing in
// an argument is ever interpreted. With `output`, stdout and stderr are captured into it; without,
// the program's output is discarded on Windows (no console window) and inherited elsewhere. stdin
// is empty. Returns the exit code, or -1 when the program could not be started.
int run_process(const std::vector<std::string>& args, std::string* output = nullptr);
// The arguments as one readable line, for error messages.
std::string command_line(const std::vector<std::string>& args);

// An environment variable as UTF-8; empty when unset.
std::string environment(const char* name);
// Sets it for this process (empty value unsets it), so a setting read at runtime can be exercised.
void set_environment(const char* name, std::string_view value);
// Puts `folder` first on this process's PATH (once), so run_process finds programs in it.
void add_to_path(const std::filesystem::path& folder);
// Whether `name` (without executable_suffix) is a file in a PATH folder.
bool on_path(const std::string& name);
// Adds the usual package-manager folders (Homebrew, MacPorts, /usr/local/bin) to the end of PATH.
// An app started from Finder or a desktop launcher gets a minimal PATH that misses them, and that
// is where ffmpeg usually is. Does nothing on Windows.
void add_common_program_folders();

// The folder of the running executable; empty if unknown.
std::filesystem::path executable_directory();
// Per-user folders, under which the app keeps a ReSkateMusicPacker folder:
//   config: %APPDATA%, ~/Library/Application Support, $XDG_CONFIG_HOME or ~/.config
//   data:   %LOCALAPPDATA%, ~/Library/Application Support, $XDG_DATA_HOME or ~/.local/share
//   cache:  %LOCALAPPDATA%, ~/Library/Caches, $XDG_CACHE_HOME or ~/.cache
// Empty when the home folder is unknown.
std::filesystem::path config_directory();
std::filesystem::path data_directory();
std::filesystem::path cache_directory();

// Folders ReSkate may keep its content cache in (%LOCALAPPDATA%\ReSkate\cache). Off Windows the
// game runs under Proton or Wine, so this looks inside the prefixes beside the game install.
std::vector<std::filesystem::path> reskate_cache_directories(const std::filesystem::path& game);

// Whether a process with this executable name (e.g. "Skate.exe") is running, case-insensitively.
// Off Windows this also matches Wine/Proton processes running that executable.
bool process_running(const std::string& executable);

// Opens a web page in the default browser.
void open_url(const std::string& url);
// Shows a file in the file manager (Explorer, Finder, or the folder that holds it).
void reveal(const std::filesystem::path& file);

} // namespace platform
