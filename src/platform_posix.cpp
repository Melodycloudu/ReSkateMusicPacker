// SPDX-FileCopyrightText: 2026 DeckardDetribine and the ReSkateMusicPacker contributors
// SPDX-License-Identifier: GPL-3.0-only
// platform.h for macOS and Linux.
#include "platform.h"

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <thread>

#include <fcntl.h>
#include <pwd.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef __APPLE__
#include <libproc.h>
#include <mach-o/dyld.h>
#include <sys/sysctl.h>
#endif

extern char** environ;

namespace platform {
namespace fs = std::filesystem;
namespace {

std::string fold(std::string text) {
    for (auto& c : text) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return text;
}

// The last component of a Windows or POSIX path, as Wine shows "C:\...\Skate.exe".
std::string leaf(const std::string& path) {
    const auto slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

fs::path home() {
    if (const char* value = std::getenv("HOME"); value && *value) return value;
    if (const auto* entry = getpwuid(getuid()); entry && entry->pw_dir) return entry->pw_dir;
    return {};
}

#ifndef __APPLE__
// $XDG_<name> when it is set to an absolute path, else ~/<fallback>.
fs::path xdg(const char* variable, const char* fallback) {
    if (const char* value = std::getenv(variable); value && *value == '/') return value;
    const auto base = home();
    return base.empty() ? fs::path{} : base / fallback;
}
#endif

std::vector<std::string> path_folders() {
    std::vector<std::string> folders;
    const std::string path = environment("PATH");
    std::size_t start = 0;
    while (start <= path.size()) {
        const auto end = std::min(path.find(':', start), path.size());
        if (end > start) folders.push_back(path.substr(start, end - start));
        start = end + 1;
    }
    return folders;
}

void append_to_path(const fs::path& folder) {
    std::error_code error;
    if (!fs::is_directory(folder, error)) return;
    for (const auto& existing : path_folders()) if (existing == folder.string()) return;
    const auto path = environment("PATH");
    setenv("PATH", (path.empty() ? folder.string() : path + ":" + folder.string()).c_str(), 1);
}

// Runs a desktop helper (open, xdg-open) without waiting on it.
void launch(std::vector<std::string> args) {
    std::thread([args = std::move(args)] { run_process(args); }).detach();
}

} // namespace

const char executable_suffix[] = "";

std::wstring widen(std::string_view utf8) {
    std::wstring wide;
    wide.reserve(utf8.size());
    for (std::size_t i = 0; i < utf8.size();) {
        const auto lead = static_cast<unsigned char>(utf8[i]);
        const int length = lead < 0x80 ? 1 : (lead >> 5) == 0x6 ? 2 : (lead >> 4) == 0xE ? 3 : (lead >> 3) == 0x1E ? 4 : 0;
        char32_t code = length == 1 ? lead : length == 2 ? lead & 0x1F : length == 3 ? lead & 0x0F : lead & 0x07;
        bool valid = length != 0 && i + length <= utf8.size();
        for (int k = 1; valid && k < length; ++k) {
            const auto next = static_cast<unsigned char>(utf8[i + k]);
            valid = (next & 0xC0) == 0x80;
            code = (code << 6) | (next & 0x3F);
        }
        if (!valid || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) {
            wide += static_cast<wchar_t>(0xFFFD);
            ++i;
            continue;
        }
        wide += static_cast<wchar_t>(code);
        i += static_cast<std::size_t>(length);
    }
    return wide;
}

std::string narrow(std::wstring_view wide) {
    std::string utf8;
    utf8.reserve(wide.size());
    for (const auto character : wide) {
        auto code = static_cast<std::uint32_t>(character);
        if (code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) code = 0xFFFD;
        if (code < 0x80) utf8 += static_cast<char>(code);
        else if (code < 0x800) {
            utf8 += static_cast<char>(0xC0 | (code >> 6));
            utf8 += static_cast<char>(0x80 | (code & 0x3F));
        } else if (code < 0x10000) {
            utf8 += static_cast<char>(0xE0 | (code >> 12));
            utf8 += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            utf8 += static_cast<char>(0x80 | (code & 0x3F));
        } else {
            utf8 += static_cast<char>(0xF0 | (code >> 18));
            utf8 += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
            utf8 += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            utf8 += static_cast<char>(0x80 | (code & 0x3F));
        }
    }
    return utf8;
}

int run_process(const std::vector<std::string>& args, std::string* output) {
    if (args.empty()) return -1;
    int pipe_ends[2] = {-1, -1};
    if (output) {
        if (pipe(pipe_ends) != 0) return -1;
        // Close-on-exec, so a pipe never leaks into another child; dup2 below clears it on 1 and 2.
        fcntl(pipe_ends[0], F_SETFD, FD_CLOEXEC);
        fcntl(pipe_ends[1], F_SETFD, FD_CLOEXEC);
    }
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    // ffmpeg reads stdin for interactive keys; give it nothing to read.
    posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
    if (output) {
        posix_spawn_file_actions_adddup2(&actions, pipe_ends[1], 1);
        posix_spawn_file_actions_adddup2(&actions, pipe_ends[1], 2);
    }
    std::vector<char*> argv;
    for (const auto& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);
    pid_t child{};
    const int failed = posix_spawnp(&child, argv[0], &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    if (output) close(pipe_ends[1]);
    if (failed) {
        if (output) close(pipe_ends[0]);
        return -1;
    }
    if (output) { // drain while the child runs, so a large output cannot deadlock
        char buffer[4096];
        for (;;) {
            const auto count = read(pipe_ends[0], buffer, sizeof(buffer));
            if (count > 0) output->append(buffer, static_cast<std::size_t>(count));
            else if (count == 0 || errno != EINTR) break;
        }
        close(pipe_ends[0]);
    }
    int status{};
    while (waitpid(child, &status, 0) < 0)
        if (errno != EINTR) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

std::string command_line(const std::vector<std::string>& args) {
    std::string line;
    for (const auto& arg : args) {
        if (!line.empty()) line += ' ';
        if (!arg.empty() && arg.find_first_of(" \t\n'\"\\$`") == std::string::npos) { line += arg; continue; }
        line += '\'';
        for (const auto c : arg) line += c == '\'' ? std::string("'\\''") : std::string(1, c);
        line += '\'';
    }
    return line;
}

std::string environment(const char* name) {
    const char* value = std::getenv(name);
    return value ? value : "";
}

void set_environment(const char* name, std::string_view value) {
    if (value.empty()) { unsetenv(name); return; }
    const std::string text(value);
    setenv(name, text.c_str(), 1);
}

void add_to_path(const fs::path& folder) {
    if (folder.empty()) return;
    for (const auto& existing : path_folders()) if (existing == folder.string()) return;
    const auto path = environment("PATH");
    setenv("PATH", (path.empty() ? folder.string() : folder.string() + ":" + path).c_str(), 1);
}

bool on_path(const std::string& name) {
    for (const auto& folder : path_folders()) {
        const auto file = fs::path(folder) / name;
        std::error_code error;
        if (fs::is_regular_file(file, error) && access(file.c_str(), X_OK) == 0) return true;
    }
    return false;
}

void add_common_program_folders() {
    for (const char* folder : {"/opt/homebrew/bin", "/usr/local/bin", "/opt/local/bin", "/usr/bin", "/snap/bin"})
        append_to_path(folder);
    if (const auto base = home(); !base.empty()) append_to_path(base / ".local" / "bin");
}

fs::path executable_directory() {
#ifdef __APPLE__
    std::uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string buffer(size, '\0');
    if (_NSGetExecutablePath(buffer.data(), &size) != 0) return {};
    std::error_code error;
    const auto path = fs::canonical(buffer.c_str(), error);
    return error ? fs::path(buffer.c_str()).parent_path() : path.parent_path();
#else
    std::error_code error;
    const auto path = fs::read_symlink("/proc/self/exe", error);
    return error ? fs::path{} : path.parent_path();
#endif
}

fs::path config_directory() {
#ifdef __APPLE__
    const auto base = home();
    return base.empty() ? base : base / "Library" / "Application Support";
#else
    return xdg("XDG_CONFIG_HOME", ".config");
#endif
}

fs::path data_directory() {
#ifdef __APPLE__
    return config_directory();
#else
    return xdg("XDG_DATA_HOME", ".local/share");
#endif
}

fs::path cache_directory() {
#ifdef __APPLE__
    const auto base = home();
    return base.empty() ? base : base / "Library" / "Caches";
#else
    return xdg("XDG_CACHE_HOME", ".cache");
#endif
}

std::vector<fs::path> reskate_cache_directories(const fs::path& game) {
    std::vector<fs::path> found;
    std::error_code error;
    const auto add_users = [&](const fs::path& drive_c) {
        for (const auto& user : fs::directory_iterator(drive_c / "users", error)) {
            const auto cache = user.path() / "AppData" / "Local" / "ReSkate" / "cache";
            if (fs::is_directory(cache, error)) found.push_back(cache);
        }
    };
    // A Wine prefix (CrossOver, Whisky, Lutris, Bottles): the game sits somewhere under drive_c.
    for (auto folder = game; !folder.empty() && folder != folder.parent_path(); folder = folder.parent_path())
        if (fold(folder.filename().string()) == "drive_c") { add_users(folder); break; }
    // Steam under Proton: steamapps/common/<game> beside steamapps/compatdata/<app id>/pfx.
    const auto steamapps = game.parent_path().parent_path();
    if (fold(game.parent_path().filename().string()) == "common")
        for (const auto& app : fs::directory_iterator(steamapps / "compatdata", error))
            add_users(app.path() / "pfx" / "drive_c");
    return found;
}

bool process_running(const std::string& executable) {
    const auto wanted = fold(executable);
#ifdef __APPLE__
    std::vector<pid_t> pids(4096);
    const int count = proc_listallpids(pids.data(), static_cast<int>(pids.size() * sizeof(pid_t)));
    for (int i = 0; i < count; ++i) {
        char name[2 * MAXCOMLEN + 1]{};
        if (proc_name(pids[i], name, sizeof(name)) > 0 && fold(name) == wanted) return true;
        // Wine runs Windows programs as wine64-preloader with the .exe as an argument.
        int mib[3]{CTL_KERN, KERN_PROCARGS2, pids[i]};
        std::size_t size = 0;
        if (sysctl(mib, 3, nullptr, &size, nullptr, 0) != 0 || size <= sizeof(int)) continue;
        std::string args(size, '\0');
        if (sysctl(mib, 3, args.data(), &size, nullptr, 0) != 0) continue;
        args.resize(size);
        // argc, then the executable path, padding, then the NUL-separated arguments.
        int argc{};
        std::memcpy(&argc, args.data(), sizeof(argc));
        std::size_t at = args.find('\0', sizeof(int));
        while (at < args.size() && args[at] == '\0') ++at;
        for (int arg = 0; arg < argc && at < args.size(); ++arg) {
            const auto end = std::min(args.find('\0', at), args.size());
            if (fold(leaf(args.substr(at, end - at))) == wanted) return true;
            at = end + 1;
        }
    }
    return false;
#else
    std::error_code error;
    for (const auto& entry : fs::directory_iterator("/proc", error)) {
        const auto pid = entry.path().filename().string();
        if (pid.empty() || pid.find_first_not_of("0123456789") != std::string::npos) continue;
        std::ifstream comm(entry.path() / "comm");
        std::string name;
        if (std::getline(comm, name) && fold(name) == wanted) return true;
        // Wine and Proton: the .exe is one of the first arguments.
        std::ifstream cmdline(entry.path() / "cmdline", std::ios::binary);
        const std::string args((std::istreambuf_iterator<char>(cmdline)), {});
        for (std::size_t at = 0, arg = 0; at < args.size() && arg < 4; ++arg) {
            const auto end = std::min(args.find('\0', at), args.size());
            if (fold(leaf(args.substr(at, end - at))) == wanted) return true;
            at = end + 1;
        }
    }
    return false;
#endif
}

void open_url(const std::string& url) {
#ifdef __APPLE__
    launch({"open", url});
#else
    launch({"xdg-open", url});
#endif
}

void reveal(const fs::path& file) {
#ifdef __APPLE__
    launch({"open", "-R", file.string()});
#else
    // There is no portable "select this file"; open the folder that holds it.
    launch({"xdg-open", file.parent_path().string()});
#endif
}

} // namespace platform
