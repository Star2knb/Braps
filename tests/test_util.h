// Helpers for the recorder tests.
#pragma once

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace rt {

// A fresh, empty folder under %TEMP%, removed when the object goes away.
class TempDir {
public:
    explicit TempDir(const char* tag) {
        path_ = std::filesystem::temp_directory_path() /
                (std::string("rec_test_") + tag + "_" + std::to_string(GetCurrentProcessId()));
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
        std::filesystem::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

inline std::string read_file(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream os;
    os << in.rdbuf();
    return os.str();
}

inline void write_file(const std::filesystem::path& p, const std::string& text) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << text;
}

}  // namespace rt
