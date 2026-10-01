// Well-known locations and string helpers for the host (Windows only).
#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace rec {

// %LOCALAPPDATA%\rec (config and logs; per machine, because cached disk benchmarks are).
std::filesystem::path app_data_dir();
std::filesystem::path default_config_path();  // app_data_dir()\rec.toml
std::filesystem::path app_log_dir();          // app_data_dir()\logs

// Expands %VAR% references (e.g. "%USERPROFILE%\Videos\rec"); unknown variables are left as is.
std::filesystem::path expand_env(std::string_view utf8);

std::string to_utf8(std::wstring_view wide);
std::wstring from_utf8(std::string_view utf8);

// Text for a Win32 error code, e.g. "Access is denied. (5)".
std::string win32_error_text(unsigned long code);

}  // namespace rec
