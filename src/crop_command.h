#pragma once

#include <string>
#include <vector>

#include "coords.h"

// Everything needed to build the ffmpeg invocation. Trim times are in
// seconds; negative means unset (start falls back to 0, end to the video's
// end). Either crop, trim, or both must be present.
struct CropJob {
    std::string inputPath;
    bool hasCrop = false;
    CropRect crop;
    double trimStart = -1.0;
    double trimEnd = -1.0;

    bool hasTrim() const { return trimStart >= 0.0 || trimEnd >= 0.0; }
};

// "/dir/video.mp4" -> "/dir/video_cropped.mp4" (suffix follows the action:
// _cropped, _trimmed, or _cropped_trimmed).
std::string buildOutputPath(const CropJob& job);

// The ffmpeg invocation as an argv array (no shell involved when spawning).
std::vector<std::string> buildFfmpegArgv(const CropJob& job,
                                         const std::string& outputPath);

// POSIX single-quoting; safe for spaces, quotes, $, etc.
std::string shellQuote(const std::string& s);

// Quoted, space-joined command line suitable for pasting into a shell.
std::string buildShellCommand(const std::vector<std::string>& argv);

// Starts the command detached (new session, stdio on /dev/null) so it keeps
// running after this process exits. Returns false with err on spawn failure.
bool spawnDetached(const std::vector<std::string>& argv, std::string& err);
