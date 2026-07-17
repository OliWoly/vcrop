#include "crop_command.h"

#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <spawn.h>

extern char** environ;

std::string buildOutputPath(const std::string& inputPath)
{
    const std::filesystem::path in(inputPath);
    const auto out =
        in.parent_path() / (in.stem().string() + "_cropped" + in.extension().string());
    return out.string();
}

std::vector<std::string> buildFfmpegArgv(const std::string& inputPath,
                                         const std::string& outputPath,
                                         const CropRect& r)
{
    const std::string filter = "crop=" + std::to_string(r.w) + ":" +
                               std::to_string(r.h) + ":" + std::to_string(r.x) +
                               ":" + std::to_string(r.y);
    // -crf 18 is visually near-lossless; audio is passed through untouched.
    // -y: re-running vcrop is expected to replace a previous _cropped output.
    return {"ffmpeg", "-hide_banner", "-nostdin", "-y",
            "-i",     inputPath,
            "-vf",    filter,
            "-c:v",   "libx264", "-crf", "18", "-preset", "veryfast",
            "-c:a",   "copy",
            outputPath};
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
    std::vector<char*> cargv;
    cargv.reserve(argv.size() + 1);
    for (const auto& s : argv)
        cargv.push_back(const_cast<char*>(s.c_str()));
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
