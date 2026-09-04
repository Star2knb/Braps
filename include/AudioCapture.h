#pragma once

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <wrl/client.h>
#include <atomic>
#include <thread>
#include <string>
#include <chrono>
#include <cstdint>

enum class AudioSource { SystemLoopback, Microphone };

// Captures one WASAPI audio stream (system-output loopback, or a
// microphone input device) on its own dedicated thread and writes raw
// PCM/float samples straight into a .wav file for the duration of the
// recording. No live ffmpeg process is involved during capture — unlike
// video (which streams into ffmpeg live because raw BGRA is enormous),
// raw PCM is small enough that there's no performance reason to encode it
// live, and this sidesteps entirely the ffmpeg multi-input interleaver
// stall found when audio was previously piped into the same ffmpeg
// process as video (see Encoder.h's RemuxWithAudio for the post-hoc mux
// step this feeds into).
//
// Mirrors the existing capture backends' degrade-gracefully philosophy
// (DXGI falling back to GDI): if WASAPI initialization fails for any
// reason (no default device, device unavailable, etc.), Start() returns
// false and the caller proceeds without this track rather than aborting
// the whole recording.
class AudioCapture {
public:
    explicit AudioCapture(AudioSource source);
    ~AudioCapture();

    // Opens the WASAPI endpoint, writes a placeholder WAV header to
    // wavPath, and spawns the capture thread. Returns false on any
    // failure; safe to destroy without calling Stop() in that case.
    bool Start(const std::wstring& wavPath);

    // Signals the capture thread to stop, joins it, and patches the WAV
    // header's size fields now that the final byte count is known. Safe
    // to call even if Start() failed or was never called. Must complete
    // before the file is handed to the remux step.
    void Stop();

    // True once this stream's first real WASAPI buffer (silent or not)
    // has been captured — used as this stream's "t=0" for sync-offset
    // computation against video's own first-frame timestamp. Silence
    // still counts: it means the stream is live and timestamped
    // correctly, and waiting for non-silent audio would add a content-
    // dependent delay unrelated to actual capture-pipeline startup time.
    bool HasFirstBuffer() const { return haveFirstBuffer_.load(std::memory_order_relaxed); }
    std::chrono::steady_clock::time_point FirstBufferTime() const { return firstBufferTime_; }

private:
    void CaptureThreadMain(std::wstring wavPath);
    bool InitWasapi();

    AudioSource source_;
    std::thread thread_;
    std::atomic<bool> running_{false};

    // Set once by the capture thread immediately after InitWasapi()
    // returns (success or failure) — lets Start() wait for the outcome of
    // WASAPI setup specifically, without conflating it with waiting for
    // the first actual audio buffer (which could be legitimately delayed
    // by silence at recording start, especially on the mic).
    std::atomic<bool> initDone_{false};
    std::atomic<bool> initSucceeded_{false};

    std::atomic<bool> haveFirstBuffer_{false};
    std::chrono::steady_clock::time_point firstBufferTime_;

    Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator_;
    Microsoft::WRL::ComPtr<IMMDevice> device_;
    Microsoft::WRL::ComPtr<IAudioClient> audioClient_;
    Microsoft::WRL::ComPtr<IAudioCaptureClient> captureClient_;

    WAVEFORMATEX* mixFormat_ = nullptr; // from GetMixFormat, freed via CoTaskMemFree
};
