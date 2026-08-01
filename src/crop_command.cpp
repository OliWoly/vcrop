#include "crop_command.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <spawn.h>
#include <unistd.h>

extern char** environ;

namespace {

std::string formatSeconds(double t)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.3f", t);
    return buf;
}

// Resolves "ffmpeg" (or any argv[0] without a directory component) to an
// absolute path. When the app is launched by a file manager or Launch
// Services (Finder "Open With"), PATH is minimal or unset, so the search
// falls back to well-known install directories.
std::string resolveExecutable(const std::string& name)
{
    if (name.empty())
        return {};
    if (name.find('/') != std::string::npos)
        return name;
    if (const char* override = std::getenv("VCROP_FFMPEG")) {
        if (*override != '\0' && std::filesystem::is_regular_file(override))
            return override;
    }
    static const char* extraDirs[] = {
        "/opt/homebrew/bin", // Apple Silicon Homebrew
        "/usr/local/bin",    // Intel Homebrew / macports-style prefixes
        "/usr/bin",
        "/opt/homebrew/opt/ffmpeg/bin",
        "/usr/local/opt/ffmpeg/bin",
        "/opt/local/bin", // MacPorts
    };
    const auto found = [&name](const std::string& dir) -> std::string {
        if (dir.empty())
            return {};
        const std::string path = (std::filesystem::path(dir) / name).string();
        return std::filesystem::is_regular_file(path) &&
                       ::access(path.c_str(), X_OK) == 0
                   ? path
                   : std::string();
    };
    if (const char* pathEnv = std::getenv("PATH")) {
        const std::string path = pathEnv;
        const char* p = path.c_str();
        for (;;) {
            const char* end = std::strchr(p, ':');
            const std::string dir = end
                                        ? path.substr(p - path.c_str(),
                                                      end - p)
                                        : std::string(p);
            if (const std::string hit = found(dir); !hit.empty())
                return hit;
            if (end == nullptr)
                break;
            p = end + 1;
        }
    }
    for (const char* dir : extraDirs)
        if (const std::string hit = found(dir); !hit.empty())
            return hit;
    return {};
}

} // namespace

std::string buildOutputPath(const CropJob& job)
{
    std::string suffix;
    if (job.hasCrop)
        suffix += "_cropped";
    if (job.hasTrim())
        suffix += "_trimmed";
    if (suffix.empty())
        suffix = "_cropped";

    const std::filesystem::path in(job.inputPath);
    const auto out =
        in.parent_path() / (in.stem().string() + suffix + in.extension().string());
    return out.string();
}

std::vector<std::string> buildFfmpegArgv(const CropJob& job,
                                         const std::string& outputPath)
{
    std::vector<std::string> argv{"ffmpeg", "-hide_banner", "-nostdin", "-y"};

    // -ss before -i seeks the demuxer; combined with re-encoding below this
    // is frame-accurate. -t (a duration) avoids the shifted-timestamp
    // semantics -to would have after input seeking.
    if (job.trimStart >= 0.0) {
        argv.push_back("-ss");
        argv.push_back(formatSeconds(job.trimStart));
    }
    argv.push_back("-i");
    argv.push_back(job.inputPath);
    if (job.trimEnd >= 0.0) {
        argv.push_back("-t");
        argv.push_back(formatSeconds(job.trimEnd - std::max(job.trimStart, 0.0)));
    }

    if (job.hasCrop) {
        const CropRect& r = job.crop;
        argv.push_back("-vf");
        argv.push_back("crop=" + std::to_string(r.w) + ":" + std::to_string(r.h) +
                       ":" + std::to_string(r.x) + ":" + std::to_string(r.y));
    }

    // -crf 18 is visually near-lossless; audio is passed through untouched.
    // -y: re-running vcrop is expected to replace a previous output.
    for (const char* arg :
         {"-c:v", "libx264", "-crf", "18", "-preset", "veryfast", "-c:a", "copy"})
        argv.push_back(arg);
    argv.push_back(outputPath);
    return argv;
}

std::string shellQuote(const std::string& s)
{
    static const char* safe =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
        "0123456789_./:=,+-";
    if (!s.empty() && s.find_first_not_of(safe) == std::string::npos)
        return s;
    std::string out = "'";
    for (const char c : s) {
        if (c == '\'')
            out += "'\\''";
        else
            out += c;
    }
    out += "'";
    return out;
}

std::string buildShellCommand(const std::vector<std::string>& argv)
{
    std::string out;
    for (const auto& arg : argv) {
        if (!out.empty())
            out += ' ';
        out += shellQuote(arg);
    }
    return out;
}

bool spawnDetached(const std::vector<std::string>& argv, std::string& err)
{
    if (argv.empty()) {
        err = "empty command";
        return false;
    }
    const std::string exe = resolveExecutable(argv[0]);
    if (exe.empty()) {
        err = argv[0] +
              " not found; install it or add its directory to PATH (or set "
              "VCROP_FFMPEG)";
        return false;
    }

    std::vector<char*> cargv;
    cargv.reserve(argv.size() + 1);
    cargv.push_back(const_cast<char*>(exe.c_str()));
    for (size_t i = 1; i < argv.size(); ++i)
        cargv.push_back(const_cast<char*>(argv[i].c_str()));
    cargv.push_back(nullptr);

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);

    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
#ifdef POSIX_SPAWN_SETSID
    // New session: the child survives the terminal closing.
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSID);
#endif

    pid_t pid = -1;
    const int rc = posix_spawnp(&pid, cargv[0], &fa, &attr, cargv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&attr);

    if (rc != 0) {
        err = std::strerror(rc);
        return false;
    }
    return true;
}
