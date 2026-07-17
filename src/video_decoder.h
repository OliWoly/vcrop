#pragma once

#include <string>

struct AVFormatContext;
struct AVCodecContext;
struct SwsContext;
struct AVFrame;
struct AVPacket;

// Decodes the best video stream of a file into yuv420p frames. All FFmpeg
// usage is confined to this class. Timestamps are normalized so the first
// frame of the stream is at ~0 seconds regardless of the container's
// start_time.
class VideoDecoder {
public:
    enum class Result { Frame, Eof, Error };

    VideoDecoder() = default;
    ~VideoDecoder();
    VideoDecoder(const VideoDecoder&) = delete;
    VideoDecoder& operator=(const VideoDecoder&) = delete;

    bool open(const std::string& path, std::string& err);

    // Decodes the next video frame into the internal yuv420p frame.
    Result nextFrame();

    // Keyframe-backward seek, then decode forward until the target time is
    // reached (or EOF, in which case the last decoded frame is kept).
    bool seek(double seconds);

    const AVFrame* frame() const { return yuv_; }
    double framePtsSec() const { return ptsSec_; }
    double durationSec() const;
    int width() const;
    int height() const;

private:
    AVFormatContext* fmt_ = nullptr;
    AVCodecContext* codec_ = nullptr;
    SwsContext* sws_ = nullptr;
    AVFrame* native_ = nullptr;
    AVFrame* yuv_ = nullptr;
    AVPacket* pkt_ = nullptr;
    int streamIndex_ = -1;
    bool sentEof_ = false;
    double ptsSec_ = 0.0;
    double startOffsetSec_ = 0.0;
    double frameDurSec_ = 0.04;
};
