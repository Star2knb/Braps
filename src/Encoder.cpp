#include "Encoder.h"

#include <filesystem>
#include <sstream>
#include <iomanip>
#include <iostream>

namespace fs = std::filesystem;

namespace {
// Large enough to hold several 1080p BGRA frames (~8MB each) so a momentary
// ffmpeg stall doesn't immediately block WriteFile on every single call —
// some slack absorbs brief encoder hiccups without the caller blocking on
// every frame, while genuinely sustained backpressure still blocks (by
// design: that's the caller's worker thread absorbing it, not the capture
// thread).
constexpr DWORD kPipeBufferBytes = 32 * 1024 * 1024;
}

Encoder::Encoder(std::wstring ffmpegPath, std::wstring outputDir)
    : ffmpegPath_(std::move(ffmpegPath)), outputDir_(std::move(outputDir)) {}

Encoder::~Encoder() {
    if (stdinWriteHandle_) CloseHandle(stdinWriteHandle_);
    if (processHandle_) CloseHandle(processHandle_);
}

bool Encoder::StartStreamingEncode(int width, int height, const std::wstring& outputFileName) {
    fs::create_directories(outputDir_);
    outputPath_ = outputDir_ + L"\\" + outputFileName;
    logPath_ = outputDir_ + L"\\ffmpeg_last_run.log";

    // Anonymous pipe: ffmpeg reads raw frames from the read end (wired to
    // its stdin below); we keep the write end to WriteFrame() into.
    SECURITY_ATTRIBUTES pipeSa{};
    pipeSa.nLength = sizeof(pipeSa);
    pipeSa.bInheritHandle = TRUE; // the read end must be inheritable for CreateProcessW to hand it to the child

    HANDLE stdinReadHandle = nullptr;
    if (!CreatePipe(&stdinReadHandle, &stdinWriteHandle_, &pipeSa, kPipeBufferBytes)) {
        std::wcerr << L"[Encoder] CreatePipe failed (GetLastError=" << GetLastError() << L")\n";
        return false;
    }

    // Our own write end must NOT be inherited by the child — only the
    // read end should cross into ffmpeg's process, otherwise ffmpeg would
    // hold a spare write handle open on the pipe, and EOF (from us closing
    // our write end at FinishStreamingEncode) would never actually signal
    // since ffmpeg's own copy would still be keeping the pipe alive.
    SetHandleInformation(stdinWriteHandle_, HANDLE_FLAG_INHERIT, 0);

    // Redirect ffmpeg's stdout/stderr to a log file rather than discarding
    // it — with CREATE_NO_WINDOW there's no console for it to write to,
    // so any error detail beyond the bare exit code would otherwise be lost.
    SECURITY_ATTRIBUTES logSa{};
    logSa.nLength = sizeof(logSa);
    logSa.bInheritHandle = TRUE;
    logHandle_ = CreateFileW(logPath_.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &logSa,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

    std::wstringstream cmd;
    cmd << std::fixed << std::setprecision(3);
    cmd << L"\"" << ffmpegPath_ << L"\""
        << L" -y"
        << L" -f rawvideo -pix_fmt bgra -video_size " << width << L"x" << height
        // The INPUT -framerate here is not the declared display rate —
        // it's the timebase -use_wallclock_as_timestamps quantizes real
        // arrival times into. Setting it to the real target (e.g. 60)
        // was actively wrong: any two frames arriving less than 1/60s
        // apart (routine jitter from a sub-60fps source, e.g. a 50fps
        // game) got quantized into the same or adjacent timebase tick
        // and silently merged/dropped by -fps_mode vfr afterward —
        // confirmed via ffmpeg's own log reporting drop=82 out of 1207
        // real captured frames on a 60fps-target/~50fps-source session.
        // Using a high nominal value here (1000 = millisecond
        // resolution) gives each real wallclock arrival its own distinct
        // timebase tick so genuine frames never collide. This also
        // covers the original 25fps-container-default bug (ffmpeg
        // silently assumed 25fps when -framerate was omitted entirely),
        // since a value is now always declared regardless of the actual
        // capture target.
        << L" -framerate 1000"
        // Timestamps each incoming frame by when it actually arrives at
        // the pipe, giving the same real-pacing/VFR behavior the old
        // ffconcat-duration approach delivered, without building a
        // manifest file after the fact — frames only ever reach this
        // pipe already paced by the capture loop's real timing, never as
        // an artificial burst, so this reflects genuine frame pacing.
        << L" -use_wallclock_as_timestamps 1"
        << L" -i -"
        // NOTE: -r on the output is deliberately NOT used here — it
        // forces CFR resampling and is directly rejected by ffmpeg
        // ("-r/-fpsmax specified together a non-CFR -vsync/-fps_mode")
        // when combined with vfr mode below. The container's
        // r_frame_rate/avg_frame_rate end up derived from real per-frame
        // PTS deltas instead, which is what vfr mode is for in the first
        // place.
        << L" -fps_mode vfr"
        // H.264 requires even width/height. A resolution change mid-
        // recording (e.g. the source window losing/gaining a title bar on
        // alt-tab) can leave an odd dimension, which would otherwise fail
        // the encoder outright rather than just cropping a stray pixel.
        << L" -vf \"scale=trunc(iw/2)*2:trunc(ih/2)*2\""
        << L" -c:v libx264 -preset ultrafast -crf 23"
        << L" -pix_fmt yuv420p"
        << L" \"" << outputPath_ << L"\"";

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
    si.wShowWindow = SW_HIDE;
    si.hStdInput = stdinReadHandle;
    si.hStdError = logHandle_;
    si.hStdOutput = logHandle_;
    PROCESS_INFORMATION pi{};

    std::wstring mutableCmd = cmd.str();
    std::wcout << L"[Encoder] Starting streaming encode: " << mutableCmd << L"\n";
    BOOL ok = CreateProcessW(
        nullptr, mutableCmd.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);

    // The read end's inherited copy now lives in ffmpeg's process; our
    // copy of that handle is no longer needed and must be closed so our
    // process doesn't also keep the read side alive.
    CloseHandle(stdinReadHandle);

    if (!ok) {
        std::wcerr << L"[Encoder] Failed to launch ffmpeg.exe (path: " << ffmpegPath_
                    << L", GetLastError=" << GetLastError() << L")\n";
        CloseHandle(stdinWriteHandle_);
        stdinWriteHandle_ = nullptr;
        if (logHandle_) { CloseHandle(logHandle_); logHandle_ = nullptr; }
        return false;
    }

    CloseHandle(pi.hThread);
    processHandle_ = pi.hProcess;
    return true;
}

bool Encoder::WriteFrame(const uint8_t* bgraData, size_t byteCount) {
    if (!stdinWriteHandle_) return false;

    const uint8_t* data = bgraData;
    size_t remaining = byteCount;
    while (remaining > 0) {
        DWORD written = 0;
        DWORD chunk = static_cast<DWORD>(remaining > (1u << 30) ? (1u << 30) : remaining);
        if (!WriteFile(stdinWriteHandle_, data, chunk, &written, nullptr) || written == 0) {
            std::wcerr << L"[Encoder] WriteFile to ffmpeg stdin failed (GetLastError=" << GetLastError() << L")\n";
            return false;
        }
        data += written;
        remaining -= written;
    }
    return true;
}

bool Encoder::FinishStreamingEncode() {
    if (stdinWriteHandle_) {
        // Closing our write end signals EOF to ffmpeg's stdin, letting it
        // finish encoding whatever's already been sent and finalize the
        // MP4's container (moov atom, etc.) before exiting.
        CloseHandle(stdinWriteHandle_);
        stdinWriteHandle_ = nullptr;
    }

    if (!processHandle_) return false;

    WaitForSingleObject(processHandle_, INFINITE);
    DWORD exitCode = 1;
    GetExitCodeProcess(processHandle_, &exitCode);
    CloseHandle(processHandle_);
    processHandle_ = nullptr;

    if (logHandle_) { CloseHandle(logHandle_); logHandle_ = nullptr; }

    if (exitCode == 0) {
        std::wcout << L"[Encoder] Wrote " << outputPath_ << L"\n";
        return true;
    }

    std::wcerr << L"[Encoder] ffmpeg exited with code " << exitCode
                << L". See log: " << logPath_ << L"\n";
    return false;
}

bool RemuxWithAudio(const std::wstring& ffmpegPath,
                     const std::wstring& outputDir,
                     const std::wstring& videoPath,
                     double videoItsOffsetSeconds,
                     const std::vector<AudioTrackInput>& audioTracks,
                     const std::wstring& outputPath) {
    std::wstring logPath = outputDir + L"\\ffmpeg_remux_last_run.log";

    SECURITY_ATTRIBUTES logSa{};
    logSa.nLength = sizeof(logSa);
    logSa.bInheritHandle = TRUE;
    HANDLE logHandle = CreateFileW(logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &logSa,
                                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

    std::wstringstream cmd;
    cmd << std::fixed << std::setprecision(6);
    cmd << L"\"" << ffmpegPath << L"\" -y";

    // Both inputs here are already-finished files (no live pipe involved
    // at all) — this is what makes this remux safe where live dual-input
    // streaming into one ffmpeg process was not: there is no cross-stream
    // interleaving decision for ffmpeg to make while data is still
    // arriving, since all of it already exists on disk before this
    // process is even spawned.
    if (videoItsOffsetSeconds > 0.0) cmd << L" -itsoffset " << videoItsOffsetSeconds;
    cmd << L" -i \"" << videoPath << L"\"";

    for (const auto& track : audioTracks) {
        if (track.itsOffsetSeconds > 0.0) cmd << L" -itsoffset " << track.itsOffsetSeconds;
        cmd << L" -i \"" << track.wavPath << L"\"";
    }

    cmd << L" -map 0:v -c:v copy";
    for (size_t i = 0; i < audioTracks.size(); ++i) {
        cmd << L" -map " << (i + 1) << L":a -c:a:" << i << L" aac";
    }
    cmd << L" \"" << outputPath << L"\"";

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
    si.wShowWindow = SW_HIDE;
    si.hStdError = logHandle;
    si.hStdOutput = logHandle;
    PROCESS_INFORMATION pi{};

    std::wstring mutableCmd = cmd.str();
    std::wcout << L"[Encoder] Remuxing audio: " << mutableCmd << L"\n";
    BOOL ok = CreateProcessW(
        nullptr, mutableCmd.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);

    if (!ok) {
        std::wcerr << L"[Encoder] Failed to launch ffmpeg.exe for remux (GetLastError=" << GetLastError() << L")\n";
        if (logHandle != INVALID_HANDLE_VALUE) CloseHandle(logHandle);
        return false;
    }

    CloseHandle(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    if (logHandle != INVALID_HANDLE_VALUE) CloseHandle(logHandle);

    if (exitCode == 0) {
        std::wcout << L"[Encoder] Remux wrote " << outputPath << L"\n";
        return true;
    }

    std::wcerr << L"[Encoder] Remux ffmpeg exited with code " << exitCode
                << L". See log: " << logPath << L"\n";
    return false;
}
