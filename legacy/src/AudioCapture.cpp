#include "AudioCapture.h"

#include <fstream>
#include <iostream>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {
// A 100ns-unit buffer duration; 200ms is generous slack for a polling
// consumer writing to disk, not chasing low latency.
constexpr REFERENCE_TIME kBufferDuration = 200 * 10000;

// WASAPI hands back either a plain WAVEFORMATEX (WAVE_FORMAT_IEEE_FLOAT
// or WAVE_FORMAT_PCM) or a WAVEFORMATEXTENSIBLE wrapping one of those two
// subtypes in SubFormat — both are seen in the wild depending on driver,
// so both must be handled rather than assuming the extensible form.
bool MixFormatIsFloat(const WAVEFORMATEX* wfx) {
    if (wfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return true;
    if (wfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE && wfx->cbSize >= 22) {
        const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wfx);
        return ext->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    }
    return false;
}

// Writes a minimal 44-byte canonical PCM/IEEE-float RIFF header, with the
// two size fields zeroed — patched later by PatchWavHeader once the real
// byte count is known. Letting the capture thread start streaming samples
// right after this avoids buffering anything in memory up front.
void WriteWavHeaderPlaceholder(std::ofstream& out, const WAVEFORMATEX* fmt, bool isFloat) {
    uint16_t audioFormat = isFloat ? 3 : 1; // 3 = IEEE float, 1 = PCM
    uint16_t numChannels = fmt->nChannels;
    uint32_t sampleRate = fmt->nSamplesPerSec;
    uint32_t byteRate = fmt->nAvgBytesPerSec;
    uint16_t blockAlign = fmt->nBlockAlign;
    uint16_t bitsPerSample = fmt->wBitsPerSample;
    uint32_t zero32 = 0;
    uint32_t subchunk1Size = 16;

    out.write("RIFF", 4);
    out.write(reinterpret_cast<const char*>(&zero32), 4); // ChunkSize, patched later
    out.write("WAVE", 4);
    out.write("fmt ", 4);
    out.write(reinterpret_cast<const char*>(&subchunk1Size), 4);
    out.write(reinterpret_cast<const char*>(&audioFormat), 2);
    out.write(reinterpret_cast<const char*>(&numChannels), 2);
    out.write(reinterpret_cast<const char*>(&sampleRate), 4);
    out.write(reinterpret_cast<const char*>(&byteRate), 4);
    out.write(reinterpret_cast<const char*>(&blockAlign), 2);
    out.write(reinterpret_cast<const char*>(&bitsPerSample), 2);
    out.write("data", 4);
    out.write(reinterpret_cast<const char*>(&zero32), 4); // Subchunk2Size, patched later
}

// Reopens the file to overwrite just the two size fields (RIFF chunk size
// at offset 4, data chunk size at offset 40) now that the real byte count
// is known — cheaper and simpler than rewriting the whole file, and valid
// since these fields are fixed-offset in a standard 44-byte header with
// no extension chunks.
void PatchWavHeader(const std::wstring& path, uint32_t dataBytes) {
    std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary);
    if (!file.is_open()) {
        std::wcerr << L"[AudioCapture] Failed to reopen for header patch: " << path << L"\n";
        return;
    }
    uint32_t riffChunkSize = 36 + dataBytes;
    file.seekp(4);
    file.write(reinterpret_cast<const char*>(&riffChunkSize), 4);
    file.seekp(40);
    file.write(reinterpret_cast<const char*>(&dataBytes), 4);
}
}

AudioCapture::AudioCapture(AudioSource source) : source_(source) {}

AudioCapture::~AudioCapture() {
    Stop();
}

bool AudioCapture::Start(const std::wstring& wavPath) {
    running_ = true;
    thread_ = std::thread(&AudioCapture::CaptureThreadMain, this, wavPath);

    // Wait specifically for WASAPI init to finish (success or failure),
    // not for the first audio buffer — silence at recording start (common
    // on a mic) could legitimately delay the first buffer well beyond any
    // reasonable init timeout, and that must not be mistaken for a failed
    // Start(). initDone_ is set by the capture thread right after
    // InitWasapi() returns, well before it waits on any real audio data.
    constexpr int kMaxWaitIterations = 100; // 100 * 10ms = 1s cap on WASAPI init itself
    for (int i = 0; i < kMaxWaitIterations && !initDone_.load(std::memory_order_relaxed); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    if (!initDone_.load(std::memory_order_relaxed) || !initSucceeded_.load(std::memory_order_relaxed)) {
        running_ = false;
        if (thread_.joinable()) thread_.join();
        return false;
    }
    return true;
}

void AudioCapture::Stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
}

bool AudioCapture::InitWasapi() {
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                   IID_PPV_ARGS(enumerator_.GetAddressOf()));
    if (FAILED(hr)) {
        std::cerr << "[AudioCapture] CoCreateInstance(MMDeviceEnumerator) failed, hr=0x"
                  << std::hex << hr << std::dec << "\n";
        return false;
    }

    // Loopback taps the render (speaker/output) endpoint — capturing what
    // is being played, not listening through a capture endpoint.
    EDataFlow flow = (source_ == AudioSource::SystemLoopback) ? eRender : eCapture;
    hr = enumerator_->GetDefaultAudioEndpoint(flow, eConsole, device_.GetAddressOf());
    if (FAILED(hr)) {
        std::cerr << "[AudioCapture] GetDefaultAudioEndpoint failed, hr=0x"
                  << std::hex << hr << std::dec << "\n";
        return false;
    }

    hr = device_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                            reinterpret_cast<void**>(audioClient_.GetAddressOf()));
    if (FAILED(hr)) {
        std::cerr << "[AudioCapture] IMMDevice::Activate failed, hr=0x" << std::hex << hr << std::dec << "\n";
        return false;
    }

    hr = audioClient_->GetMixFormat(&mixFormat_);
    if (FAILED(hr) || !mixFormat_) {
        std::cerr << "[AudioCapture] GetMixFormat failed, hr=0x" << std::hex << hr << std::dec << "\n";
        return false;
    }

    DWORD streamFlags = (source_ == AudioSource::SystemLoopback) ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0;
    hr = audioClient_->Initialize(AUDCLNT_SHAREMODE_SHARED, streamFlags,
                                   kBufferDuration, 0, mixFormat_, nullptr);
    if (FAILED(hr)) {
        std::cerr << "[AudioCapture] IAudioClient::Initialize failed, hr=0x" << std::hex << hr << std::dec << "\n";
        return false;
    }

    hr = audioClient_->GetService(IID_PPV_ARGS(captureClient_.GetAddressOf()));
    if (FAILED(hr)) {
        std::cerr << "[AudioCapture] GetService(IAudioCaptureClient) failed, hr=0x" << std::hex << hr << std::dec << "\n";
        return false;
    }

    hr = audioClient_->Start();
    if (FAILED(hr)) {
        std::cerr << "[AudioCapture] IAudioClient::Start failed, hr=0x" << std::hex << hr << std::dec << "\n";
        return false;
    }

    return true;
}

void AudioCapture::CaptureThreadMain(std::wstring wavPath) {
    // WASAPI capture threads should use the multithreaded apartment, per
    // Microsoft's own guidance for audio capture (distinct from the STA
    // used for the WIC screenshot path elsewhere in this codebase).
    HRESULT comHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool weInitializedCom = SUCCEEDED(comHr);

    bool wasapiOk = InitWasapi();
    if (!wasapiOk) {
        initSucceeded_ = false;
        initDone_ = true;
        running_ = false;
        if (weInitializedCom) CoUninitialize();
        return;
    }

    std::ofstream wavFile(wavPath, std::ios::binary);
    if (!wavFile.is_open()) {
        std::wcerr << L"[AudioCapture] Failed to open WAV file for writing: " << wavPath << L"\n";
        initSucceeded_ = false;
        initDone_ = true;
        running_ = false;
        audioClient_->Stop();
        CoTaskMemFree(mixFormat_);
        mixFormat_ = nullptr;
        if (weInitializedCom) CoUninitialize();
        return;
    }

    initSucceeded_ = true;
    initDone_ = true;

    bool isFloat = MixFormatIsFloat(mixFormat_);
    WriteWavHeaderPlaceholder(wavFile, mixFormat_, isFloat);

    uint64_t bytesWritten = 0;

    while (running_.load(std::memory_order_relaxed)) {
        UINT32 packetLength = 0;
        HRESULT hr = captureClient_->GetNextPacketSize(&packetLength);
        if (FAILED(hr)) break;

        while (packetLength != 0) {
            BYTE* data = nullptr;
            UINT32 numFrames = 0;
            DWORD flags = 0;
            hr = captureClient_->GetBuffer(&data, &numFrames, &flags, nullptr, nullptr);
            if (FAILED(hr)) break;

            if (numFrames > 0) {
                size_t bytes = static_cast<size_t>(numFrames) * mixFormat_->nBlockAlign;
                if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                    // Buffer content is undefined per WASAPI docs when
                    // this flag is set — write real zeroed silence rather
                    // than skip, so the file's own internal duration
                    // stays self-consistent with real elapsed time.
                    static thread_local std::vector<uint8_t> silence;
                    if (silence.size() < bytes) silence.assign(bytes, 0);
                    wavFile.write(reinterpret_cast<const char*>(silence.data()), static_cast<std::streamsize>(bytes));
                } else {
                    wavFile.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(bytes));
                }
                bytesWritten += bytes;

                bool expected = false;
                if (haveFirstBuffer_.compare_exchange_strong(expected, true)) {
                    firstBufferTime_ = std::chrono::steady_clock::now();
                }
            }

            captureClient_->ReleaseBuffer(numFrames);
            hr = captureClient_->GetNextPacketSize(&packetLength);
            if (FAILED(hr)) break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    wavFile.close();
    audioClient_->Stop();
    CoTaskMemFree(mixFormat_);
    mixFormat_ = nullptr;

    if (bytesWritten > 0) {
        PatchWavHeader(wavPath, static_cast<uint32_t>(bytesWritten));
    }

    if (weInitializedCom) CoUninitialize();
}
