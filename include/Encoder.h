#pragma once

#include <string>
#include <vector>
#include <cstdint>

// Spawns the bundled ffmpeg.exe to stitch a directory of BMP frame dumps
// into a compressed MP4, then deletes the temp frames. Runs on a
// background worker thread after recording stops (deferred post-processing)
// so the heavy encoding math never competes with the game for CPU time
// while capture is active.
class Encoder {
public:
    Encoder(std::wstring ffmpegPath, std::wstring tempDir, std::wstring outputDir);

    // Reads tempDir/frame_<i>.bmp for i = 0 .. frameTimestampsMs.size()-1
    // and stitches them into a variable-frame-rate MP4 at
    // outputDir/outputFileName. frameTimestampsMs[i] is the real wall-clock
    // capture time (ms) of frame i; each frame holds the screen for exactly
    // as long as it really did (via an ffconcat duration list) rather than
    // every frame getting an equal slice of one flat averaged fps. This is
    // what keeps playback matching the real pacing of the recording,
    // jitter included, instead of smoothing or distorting it.
    bool EncodeSequenceToMp4(const std::vector<uint64_t>& frameTimestampsMs, const std::wstring& outputFileName);

    void CleanupTempFrames();

private:
    std::wstring ffmpegPath_;
    std::wstring tempDir_;
    std::wstring outputDir_;
};
