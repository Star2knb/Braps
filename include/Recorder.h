#pragma once

#include "ICapture.h"
#include "RingBuffer.h"
#include "Encoder.h"

#include <memory>
#include <atomic>
#include <thread>
#include <string>
#include <vector>
#include <mutex>
#include <chrono>

// Orchestrates the three-thread pipeline described in the design doc:
//   1. Capture thread: grabs frames at the target FPS via DXGI (or GDI
//      fallback) and pushes them into the ring buffer. Never touches disk.
//   2. Disk writer thread: drains the ring buffer to raw frame files on
//      disk. Runs only while recording so idle preview costs nothing.
//   3. Encoder (spawned on stop): converts the raw frame dump into a
//      compressed MP4 via FFmpeg, off the hot path entirely.
class Recorder {
public:
    explicit Recorder(int fpsTarget = 30);
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
    void DiskWriterLoop();
    void StartRecording();
    void StopRecordingAndEncode();

    std::unique_ptr<ICapture> capture_;
    FrameRingBuffer ringBuffer_;

    std::thread captureThread_;
    std::thread diskWriterThread_;

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
    // human-readable summary on stop; actual encode timing now comes from
    // per-frame timestamps (writtenFrameTimestampsMs_), not this average.
    std::chrono::steady_clock::time_point recordingStart_;
    std::atomic<uint64_t> recordedFrameCount_{0};
    std::atomic<uint64_t> droppedFrameCount_{0};

    // Real capture timestamp of each frame as it's written to disk, indexed
    // to match frame_<i>.raw. Populated by DiskWriterLoop (not CaptureLoop),
    // since that's the thread that assigns each frame its file index — the
    // two must stay in lockstep for the VFR concat file to point at the
    // right duration for the right file. Encoder reads this to build a
    // variable-frame-rate ffconcat file, so each frame holds the screen for
    // exactly as long as it really did, instead of every frame getting an
    // equal slice of one flat averaged fps.
    std::mutex writtenTimestampsMutex_;
    std::vector<uint64_t> writtenFrameTimestampsMs_;

    // Set once by StartRecording (never by DiskWriterLoop itself) so a
    // transient empty-buffer tick while the writer is still draining the
    // tail end of a just-stopped recording can't reset it mid-drain and
    // overwrite frame_0.bmp onward with frames that belong later in the
    // sequence.
    std::atomic<bool> resetFileCounter_{false};

    std::wstring tempDir_;
    std::wstring outputDir_;
    std::wstring ffmpegPath_;

    // Diagnostic only: logs each recorded frame's wall-clock timestamp so
    // frame pacing (real inter-frame intervals) can be inspected after the
    // fact, separate from the flat "measured average fps" the encoder uses.
    std::mutex frameLogMutex_;
    std::vector<uint64_t> frameTimestampsMs_;
    void WriteFrameTimingLog(const std::wstring& path);
};
