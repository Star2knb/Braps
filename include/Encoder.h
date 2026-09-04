#pragma once

#include <windows.h>
#include <string>
#include <cstdint>
#include <vector>

// Spawns the bundled ffmpeg.exe once at recording start and streams raw
// BGRA frames into its stdin continuously, rather than dumping frames to
// disk and running a one-shot batch encode after the fact. This mirrors
// Fraps' own verified behavior (Process Monitor showed it writing a
// continuously-growing, already-compressed file from frame 1 — never a
// folder of raw per-frame files stitched together afterward) and avoids
// the whole class of disk-write stalls that came from writing hundreds of
// individual multi-megabyte BMP files per recording.
class Encoder {
public:
    Encoder(std::wstring ffmpegPath, std::wstring outputDir);
    ~Encoder();

    // Spawns ffmpeg with stdin piped from this process, ready to accept
    // raw BGRA frames of exactly width x height for the rest of the
    // recording. Returns false if the process or pipe couldn't be set up.
    //
    // The input's declared framerate is fixed internally at a high value
    // (1000) rather than taking fpsTarget_ as a parameter — it only sets
    // the timebase -use_wallclock_as_timestamps quantizes real arrival
    // times into, not the displayed/played-back rate. Using the real
    // target (e.g. 60) there was actively wrong: two frames arriving less
    // than 1/60s apart (routine jitter from a sub-60fps source) would
    // quantize into the same timebase tick and get silently merged/
    // dropped by -fps_mode vfr afterward. A fixed high value avoids that
    // regardless of fpsTarget_, and also covers the original bug where
    // omitting -framerate entirely left the container stuck at ffmpeg's
    // 25fps default. The container's actual r_frame_rate/avg_frame_rate
    // are derived from real per-frame PTS deltas instead (via vfr mode),
    // not from this value.
    bool StartStreamingEncode(int width, int height, const std::wstring& outputFileName);

    // Blocking write of one raw BGRA frame into ffmpeg's stdin. If ffmpeg
    // is momentarily behind, this call blocks until pipe space frees up —
    // intentional back-pressure, absorbed by the caller's own worker
    // thread rather than the capture thread.
    bool WriteFrame(const uint8_t* bgraData, size_t byteCount);

    // Closes the write end of the pipe (signals EOF, so ffmpeg finishes
    // encoding and finalizes the MP4's container) and waits for the
    // process to exit. Returns true if ffmpeg reported success.
    bool FinishStreamingEncode();

    // The path StartStreamingEncode wrote (or will write) to — needed by
    // the caller after FinishStreamingEncode/encoder teardown, to hand
    // the finished video file to RemuxWithAudio below.
    const std::wstring& OutputPath() const { return outputPath_; }

private:
    std::wstring ffmpegPath_;
    std::wstring outputDir_;

    HANDLE stdinWriteHandle_ = nullptr;
    HANDLE processHandle_ = nullptr;
    HANDLE logHandle_ = nullptr;
    std::wstring logPath_;
    std::wstring outputPath_;
};

// One audio track to be muxed in by RemuxWithAudio: a completed WAV file
// plus the non-negative -itsoffset (in seconds) needed to align it with
// video's own t=0, per the sync-anchor algorithm in Recorder.cpp.
struct AudioTrackInput {
    std::wstring wavPath;
    double itsOffsetSeconds = 0.0;
};

// Post-hoc remux: copies the already-encoded video stream from videoPath
// without re-encoding it (-c:v copy, fast, I/O-bound) while encoding one
// or more already-finished WAV files to AAC, muxing everything into
// outputPath as separate (never mixed) audio tracks. Runs once, after
// both the video encode and all audio capture have already finished —
// there is no live ffmpeg multi-input demuxing here, which is what makes
// this safe: a prior attempt at live-piping audio into the same ffmpeg
// process as video repeatedly stalled ffmpeg's interleaver (see
// Recorder.cpp's sync-anchor comment for the history). Applies each
// track's videoItsOffsetSeconds/itsOffsetSeconds to correct for the two
// pipelines' independent capture-startup latency. Returns false (logging
// to ffmpeg_remux_last_run.log in outputDir) if ffmpeg exits non-zero.
bool RemuxWithAudio(const std::wstring& ffmpegPath,
                     const std::wstring& outputDir,
                     const std::wstring& videoPath,
                     double videoItsOffsetSeconds,
                     const std::vector<AudioTrackInput>& audioTracks,
                     const std::wstring& outputPath);
