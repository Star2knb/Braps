#include "Recorder.h"
#include "CaptureDXGI.h"
#include "CaptureGDI.h"
#include "CaptureHook.h"

#include <windows.h>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <algorithm>

namespace fs = std::filesystem;

namespace {
// Ring buffer sized to hold ~5 seconds of frames at the target rate, so a
// higher FPS target doesn't starve the buffer sooner than a lower one would.
constexpr int kRingBufferSecondsOfHeadroom = 5;

uint64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::wstring Timestamp() {
    return std::to_wstring(static_cast<uint64_t>(std::time(nullptr)));
}

#pragma pack(push, 1)
struct BmpFileHeader {
    uint16_t type = 0x4D42; // 'BM'
    uint32_t fileSize;
    uint16_t reserved1 = 0;
    uint16_t reserved2 = 0;
    uint32_t pixelDataOffset;
};
struct BmpInfoHeader {
    uint32_t headerSize = 40;
    int32_t width;
    int32_t height;
    uint16_t planes = 1;
    uint16_t bitsPerPixel = 32;
    uint32_t compression = 0;
    uint32_t imageSize;
    int32_t xPixelsPerMeter = 2835;
    int32_t yPixelsPerMeter = 2835;
    uint32_t colorsUsed = 0;
    uint32_t colorsImportant = 0;
};
#pragma pack(pop)

// Writes a frame as BMP: same top-down BGRA bytes our capture backends
// already produce, just prepended with a fixed 54-byte header and stored
// bottom-up per the BMP spec. Chosen over headerless .raw because ffmpeg's
// concat demuxer (used for variable-frame-rate encoding) requires each
// listed file to be self-describing — it probes every entry individually
// rather than accepting a declared raw pixel format up front. BMP is
// uncompressed, so this keeps the "zero compression during capture" design
// intact while still satisfying that requirement.
void WriteFrameAsBmp(const std::wstring& path, const uint8_t* bgraData, int width, int height) {
    const size_t rowBytes = static_cast<size_t>(width) * 4;
    const size_t imageSize = rowBytes * height;

    BmpFileHeader fileHeader;
    fileHeader.pixelDataOffset = sizeof(BmpFileHeader) + sizeof(BmpInfoHeader);
    fileHeader.fileSize = static_cast<uint32_t>(fileHeader.pixelDataOffset + imageSize);

    BmpInfoHeader infoHeader;
    infoHeader.width = width;
    infoHeader.height = height; // positive = bottom-up row order
    infoHeader.imageSize = static_cast<uint32_t>(imageSize);

    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(&fileHeader), sizeof(fileHeader));
    out.write(reinterpret_cast<const char*>(&infoHeader), sizeof(infoHeader));

    // Our data is top-down; BMP rows are stored bottom-up.
    for (int y = height - 1; y >= 0; --y) {
        out.write(reinterpret_cast<const char*>(bgraData + static_cast<size_t>(y) * rowBytes), rowBytes);
    }
}
}

Recorder::Recorder(int fpsTarget)
    : ringBuffer_(static_cast<size_t>(fpsTarget) * kRingBufferSecondsOfHeadroom),
      fpsTarget_(fpsTarget) {
    tempDir_ = L"C:\\TempRecordings";
    outputDir_ = L"C:\\SavedRecordings";
    ffmpegPath_ = L"ffmpeg.exe"; // bundled next to the executable by CMake
    fs::create_directories(tempDir_);
    fs::create_directories(outputDir_);
}

Recorder::~Recorder() {
    RequestExit();
    if (captureThread_.joinable()) captureThread_.join();
    if (diskWriterThread_.joinable()) diskWriterThread_.join();
    if (capture_) capture_->Shutdown();
}

bool Recorder::Initialize() {
    auto dxgiCapture = std::make_unique<CaptureDXGI>();
    // Keep AcquireNextFrame's wait below our own frame budget so a high
    // FPS target isn't stuck blocking longer than the whole frame period.
    dxgiCapture->SetPollTimeoutMs(static_cast<unsigned int>(std::max(1, (1000 / fpsTarget_) / 2)));
    if (dxgiCapture->Initialize()) {
        std::cout << "[Recorder] Using DXGI Desktop Duplication capture.\n";
        capture_ = std::move(dxgiCapture);
        return true;
    }

    std::cout << "[Recorder] DXGI unavailable, falling back to GDI BitBlt.\n";
    capture_ = std::make_unique<CaptureGDI>();
    return capture_->Initialize();
}

bool Recorder::InitializeWithCapture(std::unique_ptr<ICapture> capture) {
    capture_ = std::move(capture);
    return capture_->Initialize();
}

void Recorder::Run() {
    captureThread_ = std::thread(&Recorder::CaptureLoop, this);
    diskWriterThread_ = std::thread(&Recorder::DiskWriterLoop, this);

    // On a low-core-count machine, the capture thread competes directly
    // with whatever's being recorded (a game, especially) for CPU time.
    // Raising its scheduling priority tells Windows to favor waking it
    // promptly over the game's own worker threads when both are runnable,
    // which matters most for the push-driven hook path where capture
    // timing directly reflects how quickly this thread gets scheduled
    // after the shared-memory event fires.
    SetThreadPriority(captureThread_.native_handle(), THREAD_PRIORITY_ABOVE_NORMAL);
}

void Recorder::RequestExit() {
    running_ = false;
}

void Recorder::CaptureLoop() {
    const auto frameInterval = std::chrono::duration<double, std::milli>(1000.0 / fpsTarget_);

    int diagIters = 0;
    double diagCaptureSum = 0, diagCacheSum = 0, diagPushSum = 0, diagTotalSum = 0;
    double diagRequestedSleepSum = 0, diagActualSleepSum = 0;
    auto diagWindowStart = std::chrono::steady_clock::now();

    // Live FPS counter for the overlay HUD: counts real captured frames
    // regardless of recording state (unlike diagIters above, which only
    // counts while recording), so the overlay can show a live number even
    // when idle as a sign that capture itself is healthy.
    int fpsWindowFrames = 0;
    auto fpsWindowStart = std::chrono::steady_clock::now();

    while (running_) {
        auto loopStart = std::chrono::steady_clock::now();
        const bool recordingThisFrame = isRecording_.load(std::memory_order_relaxed);
        // Screenshots need a fresh frame even while idle, so pixel data is
        // still needed for the first frame or two until width/height are
        // known; beyond that, idle polling only needs the DXGI/GDI backend
        // to acknowledge whether a new frame arrived, not its pixels.
        const bool needPixels = recordingThisFrame || frameWidth_ == 0;

        double cacheMs = 0, pushMs = 0;
        auto captureStart = std::chrono::steady_clock::now();
        bool gotFrame = capture_->CaptureFrame([this, recordingThisFrame, &cacheMs, &pushMs](const CapturedFrame& frame) {
            frameWidth_ = frame.width;
            frameHeight_ = frame.height;

            if (!recordingThisFrame) return;

            // Cache the frame only while actually recording: it exists
            // solely to backfill a DXGI timeout (desktop didn't change)
            // with a repeat of the last real frame, so copying it while
            // idle would be pure waste — a full frame's worth of memcpy
            // every single loop tick for no consumer.
            auto t0 = std::chrono::steady_clock::now();
            {
                std::lock_guard<std::mutex> lock(lastFrameMutex_);
                lastFrameData_.assign(frame.data, frame.data + frame.byteCount);
                haveLastFrame_ = true;
            }
            auto t1 = std::chrono::steady_clock::now();
            cacheMs = std::chrono::duration<double, std::milli>(t1 - t0).count();

            uint64_t ts = NowMs();
            if (ringBuffer_.TryPush(frame.data, frame.byteCount, frame.width, frame.height, ts)) {
                recordedFrameCount_.fetch_add(1, std::memory_order_relaxed);
                std::lock_guard<std::mutex> logLock(frameLogMutex_);
                frameTimestampsMs_.push_back(ts);
            } else {
                // Buffer full: drop the frame rather than stall the capture
                // loop. Counted (not just silently dropped) so a gap in
                // frame_timing_*.csv can be told apart from "the disk
                // writer/encoder is genuinely keeping up but the source
                // itself paused" vs "we dropped frames because downstream
                // couldn't keep up."
                droppedFrameCount_.fetch_add(1, std::memory_order_relaxed);
            }
            auto t2 = std::chrono::steady_clock::now();
            pushMs = std::chrono::duration<double, std::milli>(t2 - t1).count();
        }, needPixels);
        double captureMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - captureStart).count();

        if (gotFrame) {
            ++fpsWindowFrames;
        }

        // DXGI only signals a new frame when the desktop actually changes.
        // While recording, a static screen must still produce output frames
        // at the target FPS, so repeat the last captured frame instead.
        if (!gotFrame && recordingThisFrame) {
            std::lock_guard<std::mutex> lock(lastFrameMutex_);
            if (haveLastFrame_) {
                uint64_t ts = NowMs();
                if (ringBuffer_.TryPush(lastFrameData_.data(), lastFrameData_.size(), frameWidth_, frameHeight_, ts)) {
                    recordedFrameCount_.fetch_add(1, std::memory_order_relaxed);
                    std::lock_guard<std::mutex> logLock(frameLogMutex_);
                    frameTimestampsMs_.push_back(ts);
                }
            }
        }

        auto elapsed = std::chrono::steady_clock::now() - loopStart;
        double requestedSleepMs = 0;
        double actualSleepMs = 0;
        // Push-driven backends (the D3D11 hook) already blocked inside
        // CaptureFrame's WaitForFrame for up to a frame's worth of time,
        // waiting on the producer's own schedule — layering a fixed
        // sleep_for on top fought that wait instead of complementing it
        // (the two timeouts summed, capping throughput well below target).
        // Poll-based backends (DXGI, GDI) have no such wait, so they still
        // need this sleep to pace themselves against fpsTarget_.
        if (!capture_->IsPushDriven() && elapsed < frameInterval) {
            requestedSleepMs = std::chrono::duration<double, std::milli>(frameInterval - elapsed).count();
            auto sleepStart = std::chrono::steady_clock::now();
            std::this_thread::sleep_for(frameInterval - elapsed);
            actualSleepMs = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - sleepStart).count();
        }

        auto sinceFpsWindow = std::chrono::steady_clock::now() - fpsWindowStart;
        if (sinceFpsWindow >= std::chrono::seconds(1)) {
            double windowSeconds = std::chrono::duration<double>(sinceFpsWindow).count();
            currentFps_.store(static_cast<int>(fpsWindowFrames / windowSeconds + 0.5), std::memory_order_relaxed);
            fpsWindowFrames = 0;
            fpsWindowStart = std::chrono::steady_clock::now();
        }

        if (recordingThisFrame) {
            diagCaptureSum += captureMs;
            diagCacheSum += cacheMs;
            diagPushSum += pushMs;
            diagRequestedSleepSum += requestedSleepMs;
            diagActualSleepSum += actualSleepMs;
            diagTotalSum += std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - loopStart).count();
            ++diagIters;
        }
        if (std::chrono::steady_clock::now() - diagWindowStart >= std::chrono::seconds(1) && diagIters > 0) {
            std::cout << "[Diag] capture=" << (diagCaptureSum / diagIters)
                       << "ms cache=" << (diagCacheSum / diagIters)
                       << "ms push=" << (diagPushSum / diagIters)
                       << "ms reqSleep=" << (diagRequestedSleepSum / diagIters)
                       << "ms actSleep=" << (diagActualSleepSum / diagIters)
                       << "ms total=" << (diagTotalSum / diagIters)
                       << "ms/iter over " << diagIters << " iters";
            if (auto* dxgi = dynamic_cast<CaptureDXGI*>(capture_.get())) {
                std::cout << " [acquire=" << dxgi->LastAcquireMs()
                           << "ms copymap=" << dxgi->LastCopyMapMs() << "ms (last sample)]";
            }
            if (auto* hook = dynamic_cast<CaptureHook*>(capture_.get())) {
                std::cout << " [wait=" << hook->LastWaitMs()
                           << "ms drain=" << hook->LastDrainMs()
                           << "ms framesDrained=" << hook->LastFramesDrained() << " (last sample)]";
            }
            std::cout << "\n";
            diagIters = 0;
            diagCaptureSum = diagCacheSum = diagPushSum = diagRequestedSleepSum = diagActualSleepSum = diagTotalSum = 0;
            diagWindowStart = std::chrono::steady_clock::now();
        }
    }
}

void Recorder::DiskWriterLoop() {
    int fileCounter = 0;

    int diagWriteIters = 0;
    double diagWriteMsSum = 0, diagWriteMsMax = 0;
    auto diagWriteWindowStart = std::chrono::steady_clock::now();

    while (running_) {
        if (resetFileCounter_.exchange(false)) {
            fileCounter = 0;
        }

        FrameRingBuffer::Frame frame;
        // Keep draining regardless of isRecording_: StopRecordingAndEncode
        // flips that flag the instant it's called, on a different thread,
        // while frames captured just before the stop may still be sitting
        // in the ring buffer. Gating the pop on isRecording_ here left them
        // stranded forever — the encoder's "wait until the buffer drains"
        // check would then spin waiting for an empty buffer nothing was
        // emptying, and EncodeSequenceToMp4 never got called at all.
        if (ringBuffer_.TryPop(frame)) {
            std::wstring filename = tempDir_ + L"\\frame_" + std::to_wstring(fileCounter++) + L".bmp";

            auto writeStart = std::chrono::steady_clock::now();
            WriteFrameAsBmp(filename, frame.data.data(), frame.width, frame.height);
            double writeMs = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - writeStart).count();

            std::lock_guard<std::mutex> lock(writtenTimestampsMutex_);
            writtenFrameTimestampsMs_.push_back(frame.timestampMs);

            if (writeMs > 20.0) {
                std::cout << "[DiagWrite] SLOW WRITE: " << writeMs << "ms at ts=" << frame.timestampMs << "\n";
            }
            diagWriteMsSum += writeMs;
            diagWriteMsMax = std::max(diagWriteMsMax, writeMs);
            ++diagWriteIters;
            if (std::chrono::steady_clock::now() - diagWriteWindowStart >= std::chrono::seconds(1) && diagWriteIters > 0) {
                std::cout << "[DiagWrite] avg=" << (diagWriteMsSum / diagWriteIters)
                           << "ms max=" << diagWriteMsMax << "ms over " << diagWriteIters << " writes\n";
                diagWriteIters = 0;
                diagWriteMsSum = 0;
                diagWriteMsMax = 0;
                diagWriteWindowStart = std::chrono::steady_clock::now();
            }
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
}

void Recorder::StartRecording() {
    ringBuffer_.Clear();
    recordedFrameCount_ = 0;
    droppedFrameCount_ = 0;
    recordingStart_ = std::chrono::steady_clock::now();
    resetFileCounter_ = true;
    {
        std::lock_guard<std::mutex> lock(frameLogMutex_);
        frameTimestampsMs_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(writtenTimestampsMutex_);
        writtenFrameTimestampsMs_.clear();
    }
    isRecording_ = true;
    std::cout << "\n[REC] Recording started...\n";
}

void Recorder::WriteFrameTimingLog(const std::wstring& path) {
    std::lock_guard<std::mutex> lock(frameLogMutex_);
    std::ofstream out(path);
    out << "frame_index,timestamp_ms,interval_ms\n";
    for (size_t i = 0; i < frameTimestampsMs_.size(); ++i) {
        int64_t interval = (i == 0) ? 0 : static_cast<int64_t>(frameTimestampsMs_[i] - frameTimestampsMs_[i - 1]);
        out << i << "," << frameTimestampsMs_[i] << "," << interval << "\n";
    }
}

void Recorder::StopRecordingAndEncode() {
    isRecording_ = false;
    auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - recordingStart_).count();
    uint64_t frameCount = recordedFrameCount_.load(std::memory_order_relaxed);

    // Measured rate, not the nominal target: even if the capture loop
    // drifted or the source content's own framerate was unstable, the
    // output plays back over the same span of time it took to record.
    double measuredFps = (elapsed > 0.0) ? (static_cast<double>(frameCount) / elapsed) : static_cast<double>(fpsTarget_);

    uint64_t droppedCount = droppedFrameCount_.load(std::memory_order_relaxed);
    std::cout << "\n[REC] Recording stopped (" << frameCount << " frames over "
               << elapsed << "s, ~" << measuredFps << " fps, " << droppedCount
               << " dropped from a full ring buffer). Encoding in background...\n";

    std::wstring timingLogPath = outputDir_ + L"\\frame_timing_" + Timestamp() + L".csv";
    WriteFrameTimingLog(timingLogPath);
    std::wcout << L"[Recorder] Frame timing log: " << timingLogPath << L"\n";

    // Deferred post-processing: encode on its own thread so the app (and
    // any subsequent capture) never blocks on FFmpeg.
    std::thread([this]() {
        // The disk writer thread may still be flushing queued frames from
        // the ring buffer to disk; wait for it to fully drain before
        // invoking ffmpeg, otherwise we'd encode a partial frame set (and
        // grab an incomplete timestamp list below).
        while (!ringBuffer_.Empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        std::vector<uint64_t> timestamps;
        {
            std::lock_guard<std::mutex> lock(writtenTimestampsMutex_);
            timestamps = writtenFrameTimestampsMs_;
        }

        Encoder encoder(ffmpegPath_, tempDir_, outputDir_);
        std::wstring outName = L"gameplay_" + Timestamp() + L".mp4";
        if (encoder.EncodeSequenceToMp4(timestamps, outName)) {
            encoder.CleanupTempFrames();
        } else {
            std::wcerr << L"[Recorder] Keeping raw frames in " << tempDir_
                       << L" for inspection since encoding failed.\n";
        }
    }).detach();
}

void Recorder::ToggleRecording() {
    if (isRecording_) {
        StopRecordingAndEncode();
    } else {
        StartRecording();
    }
}

void Recorder::TakeScreenshot() {
    FrameRingBuffer::Frame frame;
    // Grab a fresh frame directly rather than relying on the recording queue,
    // so screenshots work even when not recording.
    capture_->CaptureFrame([this](const CapturedFrame& f) {
        std::wstring path = outputDir_ + L"\\screenshot_" + Timestamp() + L".raw";
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(f.data), f.byteCount);
        std::wcout << L"[Screenshot] Saved " << path << L"\n";
    });
}
