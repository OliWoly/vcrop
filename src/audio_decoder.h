#pragma once

#include <string>
#include <vector>

struct AVFormatContext;
struct AVCodecContext;
struct SwrContext;
struct AVFrame;
struct AVPacket;

// Decodes the best audio stream of a file into interleaved float32 stereo
// samples (any source channel layout is up/downmixed to stereo). It owns its
// own demux context, so it can run alongside VideoDecoder without sharing
// state. All FFmpeg usage is confined to this class. Timestamps are
// normalized so the first audio of the stream is at ~0 seconds regardless of
// the container's start_time.
class AudioDecoder {
public:
    enum class Result { Chunk, Eof, Error };

    AudioDecoder() = default;
    ~AudioDecoder();
    AudioDecoder(const AudioDecoder&) = delete;
    AudioDecoder& operator=(const AudioDecoder&) = delete;

    // Opens the audio stream. Returns false when the file has no playable
    // audio or decoding it is not possible; err describes why.
    bool open(const std::string& path, std::string& err);

    // Decodes the next chunk of interleaved float32 stereo samples into out.
    Result nextChunk(std::vector<float>& out);

    // Keyframe-backward seek, then decode forward until the target time is
    // reached (or EOF, in which case there is nothing more to decode). The
    // following nextChunk() returns samples at or after the target.
    bool seek(double seconds);

    double chunkPtsSec() const { return chunkPtsSec_; }
    double sampleRate() const { return sampleRate_; }
    int channels() const { return channels_; }

private:
    AVFormatContext* fmt_ = nullptr;
    AVCodecContext* codec_ = nullptr;
    SwrContext* swr_ = nullptr;
    AVFrame* native_ = nullptr;
    AVPacket* pkt_ = nullptr;
    int streamIndex_ = -1;
    bool sentEof_ = false;
    double chunkPtsSec_ = 0.0;
    double startOffsetSec_ = 0.0;
    double sampleRate_ = 0.0;
    int channels_ = 2;
};
