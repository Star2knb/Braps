#include "Encoder.h"

#include <windows.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <algorithm>

namespace fs = std::filesystem;

namespace {
// ffmpeg's concat demuxer wants duration in seconds with enough precision
// that a single-millisecond frame gap doesn't round away to zero.
constexpr int kDurationPrecision = 6;
}

Encoder::Encoder(std::wstring ffmpegPath, std::wstring tempDir, std::wstring outputDir)
    : ffmpegPath_(std::move(ffmpegPath)), tempDir_(std::move(tempDir)), outputDir_(std::move(outputDir)) {}

bool Encoder::EncodeSequenceToMp4(const std::vector<uint64_t>& frameTimestampsMs, const std::wstring& outputFileName) {
    if (frameTimestampsMs.size() < 2) {
        std::wcerr << L"[Encoder] Not enough frames to encode (" << frameTimestampsMs.size() << L").\n";
        return false;
    }

    fs::create_directories(outputDir_);
    std::wstring outputPath = outputDir_ + L"\\" + outputFileName;
    std::wstring concatListPath = tempDir_ + L"\\frames.ffconcat";

    // Build a variable-frame-rate playlist: each frame gets the exact
    // duration it actually held the screen for (time until the *next*
    // frame's real capture timestamp), instead of every frame receiving an
    // equal slice of one flat averaged fps. The final frame reuses the
    // previous gap since there's no "next" timestamp to measure against.
    // Frames are BMP (self-describing, uncompressed) rather than headerless
    // raw, because ffmpeg's concat demuxer probes each listed file
    // individually and has no way to be told a shared raw pixel format
    // up front.
    {
        std::wofstream concatFile(concatListPath);
        if (!concatFile.is_open()) {
            std::wcerr << L"[Encoder] Failed to open concat list for writing: " << concatListPath << L"\n";
            return false;
        }
        concatFile << L"ffconcat version 1.0\n";
        for (size_t i = 0; i < frameTimestampsMs.size(); ++i) {
            double durationSec;
            if (i + 1 < frameTimestampsMs.size()) {
                uint64_t gapMs = frameTimestampsMs[i + 1] - frameTimestampsMs[i];
                durationSec = std::max<double>(static_cast<double>(gapMs), 1.0) / 1000.0;
            } else {
                uint64_t prevGapMs = frameTimestampsMs[i] - frameTimestampsMs[i - 1];
                durationSec = std::max<double>(static_cast<double>(prevGapMs), 1.0) / 1000.0;
            }
            concatFile << L"file 'frame_" << i << L".bmp'\n";
            concatFile << std::fixed << std::setprecision(kDurationPrecision);
            concatFile << L"duration " << durationSec << L"\n";
        }
        // The concat demuxer requires the last listed file to repeat once
        // more without a duration line, or its duration is otherwise ignored.
        concatFile << L"file 'frame_" << (frameTimestampsMs.size() - 1) << L".bmp'\n";

        if (!concatFile.good()) {
            std::wcerr << L"[Encoder] Error writing concat list: " << concatListPath << L"\n";
            return false;
        }
    }

    if (!fs::exists(concatListPath)) {
        std::wcerr << L"[Encoder] Concat list missing after write: " << concatListPath << L"\n";
        return false;
    }

    std::wstringstream cmd;
    cmd << L"\"" << ffmpegPath_ << L"\""
        << L" -y"
        << L" -f concat -safe 0"
        << L" -i \"" << concatListPath << L"\""
        << L" -fps_mode vfr"
        // H.264 requires even width/height. A resolution change mid-
        // recording (e.g. the source window losing/gaining a title bar on
        // alt-tab) can leave an odd dimension in the frame sequence, which
        // would otherwise fail the encoder outright rather than just
        // cropping a stray pixel off that edge.
        << L" -vf \"scale=trunc(iw/2)*2:trunc(ih/2)*2\""
        << L" -c:v libx264 -preset ultrafast -crf 23"
        << L" -pix_fmt yuv420p"
        << L" \"" << outputPath << L"\"";

    // Redirect ffmpeg's stderr to a log file rather than discarding it —
    // with CREATE_NO_WINDOW there's no console for it to write to, so any
    // error detail beyond the bare exit code would otherwise be lost.
    std::wstring logPath = tempDir_ + L"\\ffmpeg_last_run.log";
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE logHandle = CreateFileW(logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa,
                                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
    si.wShowWindow = SW_HIDE;
    si.hStdError = logHandle;
    si.hStdOutput = logHandle;
    si.hStdInput = nullptr;
    PROCESS_INFORMATION pi{};

    std::wstring mutableCmd = cmd.str();
    std::wcout << L"[Encoder] Running: " << mutableCmd << L"\n";
    BOOL ok = CreateProcessW(
        nullptr, mutableCmd.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);

    if (logHandle != INVALID_HANDLE_VALUE) CloseHandle(logHandle);

    if (!ok) {
        std::wcerr << L"[Encoder] Failed to launch ffmpeg.exe (path: " << ffmpegPath_
                    << L", GetLastError=" << GetLastError() << L")\n";
        return false;
    }

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    if (exitCode == 0) {
        std::wcout << L"[Encoder] Wrote " << outputPath << L"\n";
        return true;
    }

    std::wcerr << L"[Encoder] ffmpeg exited with code " << exitCode
                << L". See log: " << logPath << L"\n";
    return false;
}

void Encoder::CleanupTempFrames() {
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(tempDir_, ec)) {
        if (entry.path().extension() == L".bmp" || entry.path().extension() == L".ffconcat") {
            fs::remove(entry.path(), ec);
        }
    }
}
