#include <cstdio>
#include <cstring>
#include <string>

#include "app.h"

namespace {

void printUsage(std::FILE* out)
{
    std::fputs("usage: vcrop <video-file>\n"
               "\n"
               "Opens the video in a window. Drag a rectangle to choose a crop\n"
               "region and/or set trim start/end points, then click the action\n"
               "button (Crop / Trim / Crop + Trim) to write the result next to\n"
               "the input, or \"Copy command\" to put the equivalent ffmpeg\n"
               "command on the clipboard. Either action closes the app.\n"
               "\n"
               "keys: space = play/pause, i = trim start, o = trim end, esc = quit\n",
               out);
}

} // namespace

int main(int argc, char** argv)
{
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "-h") == 0 || std::strcmp(argv[i], "--help") == 0) {
            printUsage(stdout);
            return 0;
        }
    }
    if (argc != 2) {
        printUsage(stderr);
        return 2;
    }
    App app;
    return app.run(argv[1]);
}
