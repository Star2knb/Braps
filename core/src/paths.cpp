#include "rec/paths.h"

#include <windows.h>
#include <shlobj.h>

namespace rec {

std::filesystem::path app_data_dir() {
    // REC_DATA_DIR replaces %LOCALAPPDATA%ec: tests and scripts that must not touch the user's own config, logs and cache.
    wchar_t override_dir[MAX_PATH] = {};
    const DWORD n = GetEnvironmentVariableW(L"REC_DATA_DIR", override_dir, MAX_PATH);
    if (n > 0 && n < MAX_PATH) return std::filesystem::path(override_dir);
    PWSTR p = nullptr;
    std::filesystem::path dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p)) && p)
        dir = std::filesystem::path(p) / L"rec";
    else
        dir = std::filesystem::temp_directory_path() / L"rec";
    CoTaskMemFree(p);
    return dir;
}

std::filesystem::path default_config_path() { return app_data_dir() / L"rec.toml"; }

std::filesystem::path app_log_dir() { return app_data_dir() / L"logs"; }

std::filesystem::path expand_env(std::string_view utf8) {
    const std::wstring in = from_utf8(utf8);
    const DWORD n = ExpandEnvironmentStringsW(in.c_str(), nullptr, 0);
    if (n == 0) return std::filesystem::path(in);
    std::wstring out(n, L'\0');
    if (ExpandEnvironmentStringsW(in.c_str(), out.data(), n) == 0) return std::filesystem::path(in);
    out.resize(n - 1);  // n counts the terminating null
    return std::filesystem::path(out);
}

std::string to_utf8(std::wstring_view wide) {
    if (wide.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, wide.data(), int(wide.size()), nullptr, 0, nullptr, nullptr);
    std::string out(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), int(wide.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring from_utf8(std::string_view utf8) {
    if (utf8.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()), nullptr, 0);
    std::wstring out(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()), out.data(), n);
    return out;
}

std::string win32_error_text(unsigned long code) {
    wchar_t* buf = nullptr;
    const DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                       FORMAT_MESSAGE_IGNORE_INSERTS,
                                   nullptr, code, 0, reinterpret_cast<wchar_t*>(&buf), 0, nullptr);
    std::wstring text = n && buf ? std::wstring(buf, n) : L"unknown error";
    LocalFree(buf);
    while (!text.empty() && (text.back() == L'\n' || text.back() == L'\r' || text.back() == L' ')) text.pop_back();
    return to_utf8(text) + " (" + std::to_string(code) + ")";
}

}  // namespace rec
