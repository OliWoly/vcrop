#include "audio_decoder.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
}

namespace {

std::string avErr(const std::string& what, int ret)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(ret, buf, sizeof(buf));
    return what + ": " + buf;
}

} // namespace

AudioDecoder::~AudioDecoder()
{
    swr_free(&swr_);
    av_frame_free(&native_);
    av_packet_free(&pkt_);
    avcodec_free_context(&codec_);
    avformat_close_input(&fmt_);
}

bool AudioDecoder::open(const std::string& path, std::string& err)
{
    int ret = avformat_open_input(&fmt_, path.c_str(), nullptr, nullptr);
    if (ret < 0) {
        err = avErr("cannot open '" + path + "'", ret);
        return false;
    }
    ret = avformat_find_stream_info(fmt_, nullptr);
    if (ret < 0) {
        err = avErr("cannot read stream info from '" + path + "'", ret);
        return false;
    }

    const AVCodec* codec = nullptr;
    streamIndex_ = av_find_best_stream(fmt_, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
    if (streamIndex_ < 0 || codec == nullptr) {
        err = "no audio stream in '" + path + "'";
        return false;
    }
    const AVStream* st = fmt_->streams[streamIndex_];

    codec_ = avcodec_alloc_context3(codec);
    if (codec_ == nullptr) {
        err = "cannot allocate audio decoder context";
        return false;
    }
    ret = avcodec_parameters_to_context(codec_, st->codecpar);
    if (ret >= 0)
        ret = avcodec_open2(codec_, codec, nullptr);
    if (ret < 0) {
        err = avErr("cannot open audio decoder", ret);
        return false;
    }
    if (codec_->sample_rate <= 0) {
        err = "audio stream has no valid sample rate";
        return false;
    }

    // Everything is converted to interleaved float32 stereo at the stream's
    // native sample rate; SDL handles the final conversion to the hardware.
    AVChannelLayout inLayout;
    bool ownedInLayout = false;
    if (codec_->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC) {
        av_channel_layout_default(&inLayout, codec_->ch_layout.nb_channels);
        ownedInLayout = true;
    } else {
        inLayout = codec_->ch_layout;
    }
    AVChannelLayout outLayout;
    av_channel_layout_default(&outLayout, 2);
    ret = swr_alloc_set_opts2(&swr_, &outLayout, AV_SAMPLE_FMT_FLT,
                              codec_->sample_rate, &inLayout, codec_->sample_fmt,
                              codec_->sample_rate, 0, nullptr);
    av_channel_layout_uninit(&outLayout);
    if (ownedInLayout)
        av_channel_layout_uninit(&inLayout);
    if (ret < 0 || swr_ == nullptr) {
        err = avErr("cannot set up audio resampler", ret);
        return false;
    }
    ret = swr_init(swr_);
    if (ret < 0) {
        err = avErr("cannot init audio resampler", ret);
        return false;
    }

    native_ = av_frame_alloc();
    pkt_ = av_packet_alloc();
    if (native_ == nullptr || pkt_ == nullptr) {
        err = "cannot allocate audio buffers";
        return false;
    }

    if (st->start_time != AV_NOPTS_VALUE)
        startOffsetSec_ = st->start_time * av_q2d(st->time_base);
    sampleRate_ = codec_->sample_rate;
    channels_ = 2;
    return true;
}

AudioDecoder::Result AudioDecoder::nextChunk(std::vector<float>& out)
{
    out.clear();
    for (;;) {
        int ret = avcodec_receive_frame(codec_, native_);
        if (ret == 0)
            break;
        if (ret == AVERROR_EOF)
            return Result::Eof;
        if (ret != AVERROR(EAGAIN))
            return Result::Error;

        // Decoder wants more input: feed it the next packet of our stream.
        for (;;) {
            ret = av_read_frame(fmt_, pkt_);
            if (ret == AVERROR_EOF) {
                if (!sentEof_) {
                    avcodec_send_packet(codec_, nullptr);
                    sentEof_ = true;
                }
                break; // go drain the decoder
            }
            if (ret < 0)
                return Result::Error;
            if (pkt_->stream_index == streamIndex_) {
                ret = avcodec_send_packet(codec_, pkt_);
                av_packet_unref(pkt_);
                if (ret < 0 && ret != AVERROR(EAGAIN))
                    return Result::Error;
                break;
            }
            av_packet_unref(pkt_);
        }
    }

    const int maxOut = swr_get_out_samples(swr_, native_->nb_samples);
    out.resize(static_cast<size_t>(maxOut) * channels_);
    uint8_t* outPtr = reinterpret_cast<uint8_t*>(out.data());
    const int got =
        swr_convert(swr_, &outPtr, maxOut,
                    const_cast<const uint8_t**>(native_->data), native_->nb_samples);
    if (got < 0)
        return Result::Error;
    out.resize(static_cast<size_t>(got) * channels_);

    const int64_t ts = native_->best_effort_timestamp;
    if (ts != AV_NOPTS_VALUE) {
        const AVStream* st = fmt_->streams[streamIndex_];
        chunkPtsSec_ = ts * av_q2d(st->time_base) - startOffsetSec_;
    } else {
        chunkPtsSec_ += native_->nb_samples / sampleRate_;
    }
    av_frame_unref(native_);
    return Result::Chunk;
}

bool AudioDecoder::seek(double seconds)
{
    const AVStream* st = fmt_->streams[streamIndex_];
    const int64_t ts =
        static_cast<int64_t>((seconds + startOffsetSec_) / av_q2d(st->time_base));
    if (av_seek_frame(fmt_, streamIndex_, ts, AVSEEK_FLAG_BACKWARD) < 0)
        return false;
    avcodec_flush_buffers(codec_);
    sentEof_ = false;

    // Decode forward from the keyframe until the chunk reaches the target;
    // on EOF there is simply nothing more to deliver.
    std::vector<float> scratch;
    for (;;) {
        const Result r = nextChunk(scratch);
        if (r == Result::Eof)
            return true;
        if (r == Result::Error)
            return false;
        if (chunkPtsSec_ >= seconds - 0.001)
            return true;
    }
}
