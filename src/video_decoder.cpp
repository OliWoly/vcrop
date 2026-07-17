#include "video_decoder.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libswscale/swscale.h>
}

namespace {

std::string avErr(const std::string& what, int ret)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(ret, buf, sizeof(buf));
    return what + ": " + buf;
}

} // namespace

VideoDecoder::~VideoDecoder()
{
    sws_freeContext(sws_);
    av_frame_free(&native_);
    av_frame_free(&yuv_);
    av_packet_free(&pkt_);
    avcodec_free_context(&codec_);
    avformat_close_input(&fmt_);
}

bool VideoDecoder::open(const std::string& path, std::string& err)
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
    streamIndex_ = av_find_best_stream(fmt_, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    if (streamIndex_ < 0 || codec == nullptr) {
        err = "no playable video stream in '" + path + "'";
        return false;
    }
    const AVStream* st = fmt_->streams[streamIndex_];

    codec_ = avcodec_alloc_context3(codec);
    if (codec_ == nullptr) {
        err = "cannot allocate decoder context";
        return false;
    }
    ret = avcodec_parameters_to_context(codec_, st->codecpar);
    if (ret >= 0)
        ret = avcodec_open2(codec_, codec, nullptr);
    if (ret < 0) {
        err = avErr("cannot open decoder", ret);
        return false;
    }
    if (codec_->width <= 0 || codec_->height <= 0) {
        err = "video stream has no valid dimensions";
        return false;
    }

    native_ = av_frame_alloc();
    pkt_ = av_packet_alloc();
    yuv_ = av_frame_alloc();
    if (native_ == nullptr || pkt_ == nullptr || yuv_ == nullptr) {
        err = "cannot allocate frame buffers";
        return false;
    }
    yuv_->format = AV_PIX_FMT_YUV420P;
    yuv_->width = codec_->width;
    yuv_->height = codec_->height;
    ret = av_frame_get_buffer(yuv_, 0);
    if (ret < 0) {
        err = avErr("cannot allocate frame buffers", ret);
        return false;
    }

    if (st->start_time != AV_NOPTS_VALUE)
        startOffsetSec_ = st->start_time * av_q2d(st->time_base);
    if (st->avg_frame_rate.num > 0 && st->avg_frame_rate.den > 0)
        frameDurSec_ = av_q2d(av_inv_q(st->avg_frame_rate));
    return true;
}

VideoDecoder::Result VideoDecoder::nextFrame()
{
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

    sws_ = sws_getCachedContext(sws_, native_->width, native_->height,
                                static_cast<AVPixelFormat>(native_->format),
                                yuv_->width, yuv_->height, AV_PIX_FMT_YUV420P,
                                SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (sws_ == nullptr)
        return Result::Error;
    if (av_frame_make_writable(yuv_) < 0)
        return Result::Error;
    sws_scale(sws_, native_->data, native_->linesize, 0, native_->height,
              yuv_->data, yuv_->linesize);

    const int64_t ts = native_->best_effort_timestamp;
    if (ts != AV_NOPTS_VALUE) {
        const AVStream* st = fmt_->streams[streamIndex_];
        ptsSec_ = ts * av_q2d(st->time_base) - startOffsetSec_;
    } else {
        ptsSec_ += frameDurSec_;
    }
    av_frame_unref(native_);
    return Result::Frame;
}

bool VideoDecoder::seek(double seconds)
{
    const AVStream* st = fmt_->streams[streamIndex_];
    const int64_t ts =
        static_cast<int64_t>((seconds + startOffsetSec_) / av_q2d(st->time_base));
    if (av_seek_frame(fmt_, streamIndex_, ts, AVSEEK_FLAG_BACKWARD) < 0)
        return false;
    avcodec_flush_buffers(codec_);
    sentEof_ = false;

    // Decode forward from the keyframe until we reach the target; on EOF the
    // last successfully decoded frame stays in yuv_.
    while (nextFrame() == Result::Frame) {
        if (ptsSec_ >= seconds - 0.001)
            break;
    }
    return true;
}

double VideoDecoder::durationSec() const
{
    if (fmt_->duration != AV_NOPTS_VALUE && fmt_->duration > 0)
        return fmt_->duration / static_cast<double>(AV_TIME_BASE);
    const AVStream* st = fmt_->streams[streamIndex_];
    if (st->duration != AV_NOPTS_VALUE && st->duration > 0)
        return st->duration * av_q2d(st->time_base);
    return 0.0;
}

int VideoDecoder::width() const { return codec_->width; }
int VideoDecoder::height() const { return codec_->height; }
