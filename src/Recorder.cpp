#include "Recorder.h"
#include "CaptureDXGI.h"
#include "CaptureGDI.h"
#include "CaptureHook.h"

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <algorithm>

namespace fs = std::filesystem;
using Microsoft::WRL::ComPtr;

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

// Encodes a top-down BGRA frame to PNG via WIC (Windows Imaging
// Component) — a built-in Windows API, so this needs no external image
// library. Used for screenshots specifically, so they're viewable in any
// normal image viewer instead of the raw BGRA dump used previously.
bool WriteFrameAsPng(const std::wstring& path, const uint8_t* bgraData, int width, int height) {
    // WIC needs COM initialized on the calling thread. Screenshots are
    // infrequent (a manual F10 press), so scoping init/uninit to just
    // this call is simpler than threading COM setup through Hotkeys'
    // message-loop thread for a single consumer. CoInitializeEx is
    // reference-counted per thread, so this is safe even if the caller
    // (or something else on the same thread) already initialized COM.
    HRESULT coHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool weInitializedCom = SUCCEEDED(coHr);
    struct ComGuard {
        bool active;
        ~ComGuard() { if (active) CoUninitialize(); }
    } comGuard{weInitializedCom};

    ComPtr<IWICImagingFactory> factory;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(factory.GetAddressOf()));
    if (FAILED(hr)) return false;

    ComPtr<IWICStream> stream;
    hr = factory->CreateStream(stream.GetAddressOf());
    if (FAILED(hr)) return false;

    hr = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
    if (FAILED(hr)) return false;

    ComPtr<IWICBitmapEncoder> encoder;
    hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.GetAddressOf());
    if (FAILED(hr)) return false;

    hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    if (FAILED(hr)) return false;

    ComPtr<IWICBitmapFrameEncode> frame;
    hr = encoder->CreateNewFrame(frame.GetAddressOf(), nullptr);
    if (FAILED(hr)) return false;

    hr = frame->Initialize(nullptr);
    if (FAILED(hr)) return false;

    hr = frame->SetSize(width, height);
    if (FAILED(hr)) return false;

    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    hr = frame->SetPixelFormat(&format);
    if (FAILED(hr)) return false;

    const UINT rowBytes = static_cast<UINT>(width) * 4;
    hr = frame->WritePixels(height, rowBytes, rowBytes * height,
                             const_cast<BYTE*>(reinterpret_cast<const BYTE*>(bgraData)));
    if (FAILED(hr)) return false;

    hr = frame->Commit();
    if (FAILED(hr)) return false;

    hr = encoder->Commit();
    return SUCCEEDED(hr);
}
}

Recorder::Recorder(int fpsTarget, bool micEnabled)
    : ringBuffer_(static_cast<size_t>(fpsTarget) * kRingBufferSecondsOfHeadroom),
      fpsTarget_(fpsTarget),
      micEnabled_(micEnabled) {
    outputDir_ = L"C:\\SavedRecordings";
    ffmpegPath_ = L"ffmpeg.exe"; // bundled next to the executable by CMake
    fs::create_directories(outputDir_);
}

Recorder::~Recorder() {
    RequestExit();
    if (captureThread_.joinable()) captureThread_.join();
    if (encoderFeedThread_.joinable()) encoderFeedThread_.join();
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
    encoderFeedThread_ = std::thread(&Recorder::EncoderFeedLoop, this);

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
                // First genuinely recorded frame of this session — anchor
                // for the audio/video sync-offset computation in
                // StopRecordingAndEncode. Deliberately NOT set from the
                // DXGI-timeout backfill push below (repeating the last
                // cached frame): only a real, freshly captured frame
                // should establish video's t=0, and the backfill path
                // can't fire before a real push has happened anyway since
                // lastFrameData_ doesn't exist yet at session start.
                bool expectedFirstFrame = false;
                if (haveFirstFrameTime_.compare_exchange_strong(expectedFirstFrame, true)) {
                    videoFirstFrameTime_ = std::chrono::steady_clock::now();
                }
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
        // Push-driven backends (the D3D11 hook) have a different meaning
        // for "no frame this call" — it can mean a real frame arrived but
        // was deliberately throttled to fpsTarget_ (see CaptureHook), not
        // "nothing changed." Backfilling a duplicate there would push an
        // extra frame at the exact moment the throttle meant to suppress
        // one, defeating the throttle and adding irregular fake frames on
        // top — so only poll-based backends get this behavior.
        if (!gotFrame && recordingThisFrame && !capture_->IsPushDriven()) {
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

void Recorder::EncoderFeedLoop() {
    int diagWriteIters = 0;
    double diagWriteMsSum = 0, diagWriteMsMax = 0;
    auto diagWriteWindowStart = std::chrono::steady_clock::now();

    while (running_) {
        FrameRingBuffer::Frame frame;
        // Keep draining regardless of isRecording_: StopRecordingAndEncode
        // flips that flag the instant it's called, on a different thread,
        // while frames captured just before the stop may still be sitting
        // in the ring buffer. Gating the pop on isRecording_ here would
        // strand them — FinishStreamingEncode() (called after this loop
        // is told to stop feeding, see StopRecordingAndEncode) needs every
        // already-queued frame to have actually reached ffmpeg's stdin
        // first, or the video would be missing its last moments.
        //
        // encoderReady_ (rather than isRecording_) gates whether we
        // actually try to write to encoder_: isRecording_ flips to false
        // the instant StopRecordingAndEncode is called, but the encoder_
        // object itself (and its pipe) stays alive and valid until
        // FinishStreamingEncode() closes it — so frames queued right at
        // the stop boundary still get written, they just don't get
        // pushed to a null/finished encoder.
        if (ringBuffer_.TryPop(frame)) {
            auto writeStart = std::chrono::steady_clock::now();
            bool ok;
            {
                std::lock_guard<std::mutex> lock(encoderMutex_);
                if (!encoder_) {
                    continue; // dropped: no active encoder session to write into
                }
                ok = encoder_->WriteFrame(frame.data.data(), frame.data.size());
            }
            double writeMs = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - writeStart).count();

            if (!ok) {
                std::cerr << "[Recorder] WriteFrame to encoder failed — ffmpeg may have exited early.\n";
            }

            if (writeMs > 20.0) {
                std::cout << "[EncoderFeed] SLOW WRITE: " << writeMs << "ms at ts=" << frame.timestampMs << "\n";
            }
            diagWriteMsSum += writeMs;
            diagWriteMsMax = std::max(diagWriteMsMax, writeMs);
            ++diagWriteIters;
            if (std::chrono::steady_clock::now() - diagWriteWindowStart >= std::chrono::seconds(1) && diagWriteIters > 0) {
                std::cout << "[EncoderFeed] avg=" << (diagWriteMsSum / diagWriteIters)
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
    syncAnchor_ = std::chrono::steady_clock::now();
    ringBuffer_.Clear();
    recordedFrameCount_ = 0;
    droppedFrameCount_ = 0;
    haveFirstFrameTime_ = false;
    recordingStart_ = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(frameLogMutex_);
        frameTimestampsMs_.clear();
    }

    std::wstring stamp = Timestamp();

    {
        std::lock_guard<std::mutex> lock(encoderMutex_);
        encoder_ = std::make_unique<Encoder>(ffmpegPath_, outputDir_);
        std::wstring outName = L"gameplay_" + stamp + L".mp4";
        if (!encoder_->StartStreamingEncode(frameWidth_, frameHeight_, outName)) {
            std::cerr << "[Recorder] Failed to start streaming encode — recording will not produce a video.\n";
            encoder_.reset();
            return;
        }
    }

    // Audio: system loopback is always attempted; mic only if requested.
    // Each degrades independently to "proceed without this track" on
    // failure (no default device, WASAPI unavailable, etc.) — matching
    // the existing DXGI-falls-back-to-GDI philosophy of never aborting a
    // recording because an enhancement isn't available. No live ffmpeg
    // involvement here at all: each AudioCapture writes straight to its
    // own WAV file, muxed in only after both it and the video encode have
    // already finished (see StopRecordingAndEncode / Encoder::RemuxWithAudio).
    sysAudioWavPath_ = outputDir_ + L"\\sysaudio_" + stamp + L".wav";
    sysAudioCapture_ = std::make_unique<AudioCapture>(AudioSource::SystemLoopback);
    if (!sysAudioCapture_->Start(sysAudioWavPath_)) {
        std::cerr << "[Recorder] System audio capture unavailable — recording will have no system-audio track.\n";
        sysAudioCapture_.reset();
    }

    micCapture_.reset();
    if (micEnabled_) {
        micWavPath_ = outputDir_ + L"\\mic_" + stamp + L".wav";
        auto mic = std::make_unique<AudioCapture>(AudioSource::Microphone);
        if (mic->Start(micWavPath_)) {
            micCapture_ = std::move(mic);
        } else {
            std::cerr << "[Recorder] --mic was passed but no microphone capture is available — continuing without it.\n";
        }
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
               << " dropped from a full ring buffer). Finishing encode...\n";

    std::wstring timingLogPath = outputDir_ + L"\\frame_timing_" + Timestamp() + L".csv";
    WriteFrameTimingLog(timingLogPath);
    std::wcout << L"[Recorder] Frame timing log: " << timingLogPath << L"\n";

    // Stop audio capture threads now (not on the background finish
    // thread below) so both WAV files' content ends at essentially the
    // same moment video capture stops — Stop() joins the thread and
    // patches the WAV header in place, so by the time this returns each
    // file (if it produced any data) is already a complete, valid,
    // playable WAV ready for the remux step.
    bool haveSysAudio = false;
    std::chrono::steady_clock::time_point sysAudioFirstBufferTime;
    if (sysAudioCapture_) {
        haveSysAudio = sysAudioCapture_->HasFirstBuffer();
        if (haveSysAudio) sysAudioFirstBufferTime = sysAudioCapture_->FirstBufferTime();
        sysAudioCapture_->Stop();
    }
    bool haveMic = false;
    std::chrono::steady_clock::time_point micFirstBufferTime;
    if (micCapture_) {
        haveMic = micCapture_->HasFirstBuffer();
        if (haveMic) micFirstBufferTime = micCapture_->FirstBufferTime();
        micCapture_->Stop();
    }

    bool haveVideoFirstFrame = haveFirstFrameTime_.load(std::memory_order_relaxed);
    std::chrono::steady_clock::time_point videoFirstFrameTime = videoFirstFrameTime_;
    std::wstring sysWavPath = sysAudioWavPath_;
    std::wstring micWavPath = micWavPath_;
    std::wstring ffmpegPath = ffmpegPath_;
    std::wstring outputDir = outputDir_;

    // Finish on a background thread so the hotkey thread (which called
    // this) never blocks on ffmpeg finalizing the MP4's container. encoder_
    // itself stays alive (it's a Recorder member, not a local) until this
    // thread is done with it — EncoderFeedLoop still checks it against
    // nullptr on every pop, and won't race with FinishStreamingEncode()
    // because we drain the ring buffer to empty first, below.
    std::thread([this, haveSysAudio, sysAudioFirstBufferTime, sysWavPath,
                 haveMic, micFirstBufferTime, micWavPath,
                 haveVideoFirstFrame, videoFirstFrameTime, ffmpegPath, outputDir]() {
        // The encoder feed thread may still be writing queued frames into
        // ffmpeg's stdin; wait for the ring buffer to fully drain before
        // closing the pipe, otherwise we'd cut off the last moments of
        // the recording.
        while (!ringBuffer_.Empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        std::wstring finishedVideoPath;
        bool videoOk = false;
        {
            std::lock_guard<std::mutex> lock(encoderMutex_);
            if (encoder_) {
                finishedVideoPath = encoder_->OutputPath();
                videoOk = encoder_->FinishStreamingEncode();
                encoder_.reset();
            }
        }

        if (!videoOk || finishedVideoPath.empty()) {
            std::cerr << "[Recorder] Video encode failed; skipping audio remux.\n";
            return;
        }

        // No audio track produced any data at all (e.g. WASAPI
        // unavailable and no mic requested/found) — today's plain video
        // file is already the final output, unchanged. Skip the remux
        // pass entirely rather than running ffmpeg for no reason.
        if (!haveSysAudio && !haveMic) {
            return;
        }

        // Sync-anchor offset computation: normalize so the earliest of
        // {video, all audio streams} sits at t=0 and everything else is
        // delayed by its measured real gap relative to that earliest
        // starter. Never apply a negative -itsoffset (ffmpeg doesn't
        // reliably support shifting a stream earlier that way) — instead
        // shift whichever stream(s) started first later, relative to
        // whichever started last overall.
        struct Track { std::wstring wavPath; double rawOffset; };
        std::vector<Track> tracks;
        if (haveSysAudio && haveVideoFirstFrame) {
            tracks.push_back({sysWavPath, std::chrono::duration<double>(
                sysAudioFirstBufferTime - videoFirstFrameTime).count()});
        }
        if (haveMic && haveVideoFirstFrame) {
            tracks.push_back({micWavPath, std::chrono::duration<double>(
                micFirstBufferTime - videoFirstFrameTime).count()});
        }

        double minRawOffset = 0.0;
        for (const auto& t : tracks) minRawOffset = std::min(minRawOffset, t.rawOffset);
        double videoItsOffset = -minRawOffset; // always >= 0

        std::vector<AudioTrackInput> audioTracks;
        for (const auto& t : tracks) {
            audioTracks.push_back({t.wavPath, t.rawOffset - minRawOffset}); // always >= 0
        }

        std::wstring finalOutputPath = finishedVideoPath;
        std::wstring remuxOutputPath =
            finishedVideoPath.substr(0, finishedVideoPath.size() - 4) + L"_remux.mp4"; // strip ".mp4"

        bool remuxOk = RemuxWithAudio(ffmpegPath, outputDir, finishedVideoPath, videoItsOffset,
                                       audioTracks, remuxOutputPath);
        if (remuxOk) {
            DeleteFileW(finishedVideoPath.c_str());
            if (haveSysAudio) DeleteFileW(sysWavPath.c_str());
            if (haveMic) DeleteFileW(micWavPath.c_str());
            MoveFileExW(remuxOutputPath.c_str(), finalOutputPath.c_str(), MOVEFILE_REPLACE_EXISTING);
        } else {
            std::wcerr << L"[Recorder] Audio remux failed; keeping intermediate files ("
                       << finishedVideoPath << L", audio WAV(s)) for recovery.\n";
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
        std::wstring path = outputDir_ + L"\\screenshot_" + Timestamp() + L".png";
        if (WriteFrameAsPng(path, f.data, f.width, f.height)) {
            std::wcout << L"[Screenshot] Saved " << path << L"\n";
        } else {
            std::wcerr << L"[Screenshot] Failed to encode PNG: " << path << L"\n";
        }
    });
}
