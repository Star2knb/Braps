#pragma once

#include "ICapture.h"
#include "RingBuffer.h"
#include "Encoder.h"
#include "AudioCapture.h"

#include <memory>
#include <atomic>
#include <thread>
#include <string>
#include <vector>
#include <mutex>
#include <chrono>

// Orchestrates the capture/encode pipeline:
//   1. Capture thread: grabs frames at the target FPS via DXGI/GDI/hook
//      and pushes them into the ring buffer. Never touches disk or ffmpeg.
//   2. Encoder feed thread: drains the ring buffer and streams each frame
//      directly into an already-running ffmpeg process's stdin — frames
//      are compressed continuously as they arrive, never written to disk
//      raw and never batch-encoded after the fact (see Encoder.h for why:
//      Process Monitor showed Fraps itself writes one continuously-
//      growing, already-compressed file from frame 1, never a folder of
//      raw per-frame files stitched together afterward).
class Recorder {
public:
    // micEnabled controls whether a microphone AudioCapture is started
    // alongside the always-on system-audio loopback capture.
    explicit Recorder(int fpsTarget = 30, bool micEnabled = false);
    ~Recorder();

    // Auto-selects DXGI Desktop Duplication, falling back to GDI BitBlt.
    bool Initialize();
    // Uses a pre-constructed capture backend instead — e.g. CaptureHook,
    // for the Fraps-style injected-hook path, which Recorder itself has no
    // knowledge of (it lives in hook/, a separate module).
    bool InitializeWithCapture(std::unique_ptr<ICapture> capture);
    void Run();
    void RequestExit();

    void ToggleRecording();
    void TakeScreenshot();

    // For the overlay HUD (and anything else that just wants to display
    // current state without touching the capture/encode pipeline itself).
    int CurrentFps() const { return currentFps_.load(std::memory_order_relaxed); }
    bool IsRecording() const { return isRecording_.load(std::memory_order_relaxed); }

private:
    void CaptureLoop();
    void EncoderFeedLoop();
    void StartRecording();
    void StopRecordingAndEncode();

    std::unique_ptr<ICapture> capture_;
    FrameRingBuffer ringBuffer_;

    std::thread captureThread_;
    std::thread encoderFeedThread_;

    std::atomic<bool> running_{true};
    std::atomic<bool> isRecording_{false};

    int frameWidth_ = 0;
    int frameHeight_ = 0;
    std::atomic<int> currentFps_{0};

    // Target capture cadence, configurable at construction (see main.cpp's
    // --fps flag). Also sets the ring buffer capacity (scaled to hold ~5
    // seconds of frames) so a higher target doesn't starve the buffer
    // sooner than a lower one would.
    int fpsTarget_;

    // DXGI only delivers a frame when the desktop actually changes, so a
    // static screen (paused game, idle UI) would otherwise starve the
    // ring buffer entirely. We cache the last captured frame and re-push
    // it on timeout while recording, keeping output at a steady FPS.
    std::mutex lastFrameMutex_;
    std::vector<uint8_t> lastFrameData_;
    bool haveLastFrame_ = false;

    // Wall-clock bounds of the current recording session. Used to log a
    // human-readable summary on stop.
    std::chrono::steady_clock::time_point recordingStart_;
    std::atomic<uint64_t> recordedFrameCount_{0};
    std::atomic<uint64_t> droppedFrameCount_{0};

    // Sync-anchor mechanism for audio/video alignment (see StartRecording/
    // StopRecordingAndEncode): both video's first real frame and each
    // AudioCapture's first real WASAPI buffer are timestamped against the
    // same steady_clock, so the two pipelines' independent startup
    // latency (DXGI+ffmpeg spawn vs. WASAPI init) becomes a computable
    // offset rather than an unknown drift. syncAnchor_ itself is only
    // used for diagnostics (logging how long each pipeline took to
    // produce its first sample) — the actual offset math only needs
    // videoFirstFrameTime_ and each AudioCapture::FirstBufferTime().
    std::chrono::steady_clock::time_point syncAnchor_;
    std::atomic<bool> haveFirstFrameTime_{false};
    std::chrono::steady_clock::time_point videoFirstFrameTime_;

    // Lives for the duration of one recording session: StartRecording()
    // spawns ffmpeg via StartStreamingEncode(), EncoderFeedLoop() streams
    // frames into it via WriteFrame(), StopRecordingAndEncode() finishes
    // it via FinishStreamingEncode() on a background thread. No per-frame
    // timestamp manifest is needed anymore — ffmpeg's own
    // -use_wallclock_as_timestamps derives VFR timing directly from when
    // frames actually arrive at the pipe.
    //
    // encoderMutex_ guards every access to encoder_ itself (not calls into
    // an already-obtained Encoder*, which only ever happens from the one
    // feed thread) — StartRecording (main/hotkey thread), EncoderFeedLoop
    // (feed thread), and StopRecordingAndEncode's background finish thread
    // can all read or replace this pointer, and a rapid F9/F9/F9 could
    // otherwise race a reset() against a fresh make_unique().
    std::mutex encoderMutex_;
    std::unique_ptr<Encoder> encoder_;

    // System audio (WASAPI loopback) is always captured; mic only if
    // micEnabled_ was set at construction. Each degrades independently to
    // "proceed without this track" on failure (no default device, etc.),
    // matching this codebase's existing DXGI-falls-back-to-GDI philosophy
    // — never abort a recording because an enhancement isn't available.
    bool micEnabled_;
    std::unique_ptr<AudioCapture> sysAudioCapture_;
    std::unique_ptr<AudioCapture> micCapture_;
    std::wstring sysAudioWavPath_;
    std::wstring micWavPath_;

    std::wstring outputDir_;
    std::wstring ffmpegPath_;

    // Diagnostic only: logs each recorded frame's wall-clock timestamp so
    // frame pacing (real inter-frame intervals) can be inspected after the
    // fact, separate from the flat "measured average fps" the encoder uses.
    std::mutex frameLogMutex_;
    std::vector<uint64_t> frameTimestampsMs_;
    void WriteFrameTimingLog(const std::wstring& path);
};
