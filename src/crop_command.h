#pragma once

#include <string>
#include <vector>

#include "coords.h"

// "/dir/video.mp4" -> "/dir/video_cropped.mp4"
std::string buildOutputPath(const std::string& inputPath);

// The ffmpeg invocation as an argv array (no shell involved when spawning).
std::vector<std::string> buildFfmpegArgv(const std::string& inputPath,
                                         const std::string& outputPath,
                                         const CropRect& r);

// POSIX single-quoting; safe for spaces, quotes, $, etc.
std::string shellQuote(const std::string& s);

// Quoted, space-joined command line suitable for pasting into a shell.
std::string buildShellCommand(const std::vector<std::string>& argv);

// Starts the command detached (new session, stdio on /dev/null) so it keeps
// running after this process exits. Returns false with err on spawn failure.
bool spawnDetached(const std::vector<std::string>& argv, std::string& err);
