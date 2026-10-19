// SPDX-FileCopyrightText: 2026 DeckardDetribine and the ReSkateMusicPacker contributors
// SPDX-License-Identifier: GPL-3.0-only
// platform.h for Windows.
#include "platform.h"

#include <Windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <tlhelp32.h>

#include <algorithm>
#include <array>

namespace platform {
namespace fs = std::filesystem;
namespace {

// One argument as CommandLineToArgvW (and the C runtime) will read it back: quoted when it has
// spaces or quotes, with backslashes doubled only where they precede a quote.
std::wstring quote(const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) return arg;
    std::wstring out = L"\"";
    for (auto it = arg.begin();; ++it) {
        std::size_t backslashes = 0;
        while (it != arg.end() && *it == L'\\') { ++it; ++backslashes; }
        if (it == arg.end()) { out.append(backslashes * 2, L'\\'); break; }
        if (*it == L'"') out.append(backslashes * 2 + 1, L'\\');
        else out.append(backslashes, L'\\');
        out += *it;
    }
    return out + L"\"";
}

fs::path known_folder(REFKNOWNFOLDERID id) {
    PWSTR folder{};
    fs::path result;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &folder))) result = folder;
    CoTaskMemFree(folder);
    return result;
}

} // namespace

const char executable_suffix[] = ".exe";

std::wstring widen(std::string_view text) {
    std::wstring wide(MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), static_cast<int>(wide.size()));
    return wide;
}

std::string narrow(std::wstring_view text) {
    std::string utf8(WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), utf8.data(), static_cast<int>(utf8.size()), nullptr, nullptr);
    return utf8;
}

// No shell and no console window, so the GUI never flashes a terminal.
int run_process(const std::vector<std::string>& args, std::string* output) {
    if (args.empty()) return -1;
    std::wstring line;
    for (const auto& arg : args) {
        if (!line.empty()) line += L' ';
        line += quote(widen(arg));
    }
    const bool capture = output != nullptr;
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), nullptr, TRUE};
    HANDLE read_pipe = nullptr, write_pipe = nullptr;
    if (capture && !CreatePipe(&read_pipe, &write_pipe, &attributes, 0)) return -1;
    if (read_pipe) SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW startup{sizeof(startup)};
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    if (capture) {
        startup.dwFlags |= STARTF_USESTDHANDLES;
        startup.hStdOutput = write_pipe;
        startup.hStdError = write_pipe;
        startup.hStdInput = nullptr;
    }
    PROCESS_INFORMATION process{};
    const BOOL started = CreateProcessW(nullptr, line.data(), nullptr, nullptr, capture ? TRUE : FALSE,
                                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
    if (write_pipe) CloseHandle(write_pipe);
    if (!started) {
        if (read_pipe) CloseHandle(read_pipe);
        return -1;
    }
    if (capture && read_pipe) { // drain while the child runs, so a large output cannot deadlock
        std::array<char, 4096> buffer{};
        for (DWORD read{}; ReadFile(read_pipe, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) && read;)
            output->append(buffer.data(), read);
        CloseHandle(read_pipe);
    }
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exit_code{};
    GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return static_cast<int>(exit_code);
}

std::string command_line(const std::vector<std::string>& args) {
    std::wstring line;
    for (const auto& arg : args) {
        if (!line.empty()) line += L' ';
        line += quote(widen(arg));
    }
    return narrow(line);
}

std::string environment(const char* name) {
    const auto wide_name = widen(name);
    const DWORD needed = GetEnvironmentVariableW(wide_name.c_str(), nullptr, 0);
    if (needed == 0) return {};
    std::wstring value(needed, L'\0');
    value.resize(GetEnvironmentVariableW(wide_name.c_str(), value.data(), needed));
    return narrow(value);
}

void set_environment(const char* name, std::string_view value) {
    const auto wide_name = widen(name);
    if (value.empty()) { SetEnvironmentVariableW(wide_name.c_str(), nullptr); return; }
    const auto wide_value = widen(value);
    SetEnvironmentVariableW(wide_name.c_str(), wide_value.c_str());
}

void add_to_path(const fs::path& folder) {
    if (folder.empty()) return;
    std::wstring path(GetEnvironmentVariableW(L"PATH", nullptr, 0), L'\0');
    path.resize(GetEnvironmentVariableW(L"PATH", path.data(), static_cast<DWORD>(path.size())));
    // Whole entries only: C:\ffmpeg is not already there just because C:\ffmpeg\bin is.
    for (std::size_t start = 0; start <= path.size();) {
        const auto end = std::min(path.find(L';', start), path.size());
        if (_wcsicmp(path.substr(start, end - start).c_str(), folder.c_str()) == 0) return;
        start = end + 1;
    }
    SetEnvironmentVariableW(L"PATH", (folder.wstring() + L";" + path).c_str());
}

bool on_path(const std::string& name) {
    wchar_t found[MAX_PATH];
    return SearchPathW(nullptr, widen(name + executable_suffix).c_str(), nullptr, MAX_PATH, found, nullptr) != 0;
}

void add_common_program_folders() {}

fs::path executable_directory() {
    std::wstring buffer(MAX_PATH, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) return {};
    buffer.resize(length);
    return fs::path(buffer).parent_path();
}

fs::path config_directory() { return known_folder(FOLDERID_RoamingAppData); }
fs::path data_directory() { return known_folder(FOLDERID_LocalAppData); }
fs::path cache_directory() { return known_folder(FOLDERID_LocalAppData); }

std::vector<fs::path> reskate_cache_directories(const fs::path&) {
    const auto local = known_folder(FOLDERID_LocalAppData);
    if (local.empty()) return {};
    return {local / L"ReSkate" / L"cache"};
}

bool process_running(const std::string& executable) {
    const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return false;
    const auto wanted = widen(executable);
    PROCESSENTRY32W entry{sizeof(entry)};
    bool running = false;
    for (auto ok = Process32FirstW(snapshot, &entry); ok && !running; ok = Process32NextW(snapshot, &entry))
        running = _wcsicmp(entry.szExeFile, wanted.c_str()) == 0;
    CloseHandle(snapshot);
    return running;
}

void open_url(const std::string& url) {
    ShellExecuteW(nullptr, L"open", widen(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void reveal(const fs::path& file) {
    if (PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(file.c_str())) {
        SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
        ILFree(pidl);
        return;
    }
    ShellExecuteW(nullptr, L"open", L"explorer.exe", (L"/select,\"" + file.wstring() + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
}

} // namespace platform
