#include "Exporter.h"

#include "TimeFormat.h"

#include <QFile>
#include <QFileInfo>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavformat/avformat.h>
#include <libavutil/display.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
}

namespace {

struct ExportError {
    QString message;
};
struct Cancelled {
};

QString tr(const char *text)
{
    return Exporter::tr(text);
}

[[noreturn]] void fail(const QString &message)
{
    throw ExportError{message};
}

void check(int err, const QString &what)
{
    if (err >= 0)
        return;
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(err, buf, sizeof(buf));
    fail(QStringLiteral("%1: %2").arg(what, QString::fromUtf8(buf)));
}

struct InputCloser {
    void operator()(AVFormatContext *c) const { avformat_close_input(&c); }
};
struct OutputCloser {
    void operator()(AVFormatContext *c) const
    {
        if (!(c->oformat->flags & AVFMT_NOFILE))
            avio_closep(&c->pb);
        avformat_free_context(c);
    }
};
struct CodecFree {
    void operator()(AVCodecContext *c) const { avcodec_free_context(&c); }
};
struct GraphFree {
    void operator()(AVFilterGraph *g) const { avfilter_graph_free(&g); }
};
struct FrameFree {
    void operator()(AVFrame *f) const { av_frame_free(&f); }
};
struct PacketFree {
    void operator()(AVPacket *p) const { av_packet_free(&p); }
};
struct BufferUnref {
    void operator()(AVBufferRef *b) const { av_buffer_unref(&b); }
};

constexpr int64_t kNoEnd = std::numeric_limits<int64_t>::max();

struct EncoderChoice {
    enum Kind { Software, VideoToolbox, Nvenc, Vaapi };
    const char *name;
    Kind kind;
};

// The filters ffmpeg's own autorotate inserts for a stream's display matrix,
// so the exported picture matches what the player showed. Each returned
// filter is followed by a comma.
QByteArray rotationFilters(const AVStream *st, bool *swapsAxes)
{
    *swapsAxes = false;
    const AVPacketSideData *sd = av_packet_side_data_get(st->codecpar->coded_side_data,
                                                         st->codecpar->nb_coded_side_data,
                                                         AV_PKT_DATA_DISPLAYMATRIX);
    if (!sd || sd->size < 9 * sizeof(int32_t))
        return {};
    const auto *matrix = reinterpret_cast<const int32_t *>(sd->data);
    double theta = -std::round(av_display_rotation_get(matrix));
    if (std::isnan(theta))
        return {};
    theta -= 360 * std::floor(theta / 360 + 0.9 / 360);

    if (std::fabs(theta - 90) < 1.0) {
        *swapsAxes = true;
        return matrix[3] > 0 ? "transpose=cclock_flip," : "transpose=clock,";
    }
    if (std::fabs(theta - 180) < 1.0) {
        QByteArray filters;
        if (matrix[0] < 0)
            filters += "hflip,";
        if (matrix[4] < 0)
            filters += "vflip,";
        return filters;
    }
    if (std::fabs(theta - 270) < 1.0) {
        *swapsAxes = true;
        return matrix[3] < 0 ? "transpose=clock_flip," : "transpose=cclock,";
    }
    if (std::fabs(theta) < 1.0 && matrix[4] < 0)
        return "vflip,";
    return {};
}

class Session
{
public:
    Session(const ExportJob &job, const QString &partPath, const std::atomic<bool> &cancel,
            std::function<void(double)> progress)
        : m_job(job), m_partPath(partPath), m_cancel(cancel), m_progress(std::move(progress)),
          m_pkt(av_packet_alloc()), m_encPkt(av_packet_alloc()), m_frame(av_frame_alloc()),
          m_filtered(av_frame_alloc()), m_probe(av_frame_alloc())
    {
    }

    // Returns a short description of how the file was produced.
    QString run()
    {
        openInput();
        openOutput();

        const bool copy = !m_job.reencode && m_job.crop.isEmpty();
        if (copy) {
            addCopyStream(m_videoIn);
        } else {
            openDecoder();
            probeFirstFrame();
            openEncoder();
        }
        for (unsigned i = 0; i < m_in->nb_streams; ++i) {
            const AVCodecParameters *par = m_in->streams[i]->codecpar;
            if (par->codec_type != AVMEDIA_TYPE_AUDIO)
                continue;
            if (m_job.preview && !containerSupports(par->codec_id))
                continue;
            addCopyStream(int(i));
        }
        writeHeader();

        runSegments(copy);

        check(av_write_trailer(m_out.get()), tr("Could not finish the output file"));
        m_out.reset();
        return m_details;
    }

private:
    // One stretch of the source that is kept, and where it lands in the
    // output. The *Pts fields are in the video stream's time base and are
    // settled by plan() before anything is written.
    struct Segment {
        int64_t wantStartUs = 0;             // requested range, absolute source time
        int64_t wantEndUs = kNoEnd;
        int64_t startPts = 0;                // first video frame kept
        int64_t endPts = AV_NOPTS_VALUE;     // first video frame not kept; unset when it runs to the end
        int64_t outPts = 0;                  // where startPts lands on the output's timeline
        bool head = false;                   // starts where the source itself starts
        int cutBefore = 0;                   // number of the trim in front of it, for messages
    };

    // Per-stream bookkeeping for joining the segments end to end. Times are
    // in the input stream's time base.
    struct OutStream {
        int out = -1;     // output stream index, -1 when the stream is not copied
        int64_t next = 0; // output time just after the last packet or frame written
        bool wrote = false;
        int64_t lastDts = std::numeric_limits<int64_t>::min();
        // Reset at the start of every segment:
        bool started = false;
        bool done = false;
        int64_t shift = 0; // how far ahead of its ideal place this segment's audio sits
        int64_t lastShown = std::numeric_limits<int64_t>::min(); // highest pts copied so far
    };

    // What the video packets around one point in the source look like.
    struct Scan {
        int64_t nearest = AV_NOPTS_VALUE; // timestamp of the frame closest to the point
        int64_t key = AV_NOPTS_VALUE;     // the keyframe a decoder has to start from to reach it
        int64_t copyEnd = AV_NOPTS_VALUE; // where a stream copy that must stop by the point really stops
    };

    enum class Place { Skip, Keep, Finished };

    void checkCancel() const
    {
        if (m_cancel.load())
            throw Cancelled{};
    }

    // `ts` is a video timestamp inside the current segment.
    void report(int64_t ts)
    {
        if (m_totalPts <= 0)
            return;
        const double f = std::clamp(double(m_seg->outPts + ts - m_seg->startPts) / double(m_totalPts), 0.0, 1.0);
        if (f - m_lastProgress >= 0.005) {
            m_lastProgress = f;
            m_progress(f);
        }
    }

    void openInput()
    {
        AVFormatContext *ic = nullptr;
        check(avformat_open_input(&ic, m_job.input.toUtf8().constData(), nullptr, nullptr),
              tr("Could not open the video"));
        m_in.reset(ic);
        check(avformat_find_stream_info(ic, nullptr), tr("Could not read the video's streams"));

        m_videoIn = av_find_best_stream(ic, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (m_videoIn < 0)
            fail(tr("The file has no video stream."));
        m_streams.resize(ic->nb_streams);

        const AVStream *vst = ic->streams[m_videoIn];
        const AVRational rate = av_guess_frame_rate(ic, ic->streams[m_videoIn], nullptr);
        if (rate.num > 0 && rate.den > 0)
            m_frameDuration = std::max<int64_t>(1, av_rescale_q(1, av_inv_q(rate), vst->time_base));

        // Player positions are relative to the start of the media, which is
        // not necessarily timestamp zero.
        m_originUs = ic->start_time != AV_NOPTS_VALUE ? ic->start_time : 0;
        m_fileEndUs = ic->duration > 0 ? m_originUs + ic->duration : kNoEnd;

        QList<KeepRange> keep = m_job.keep;
        if (keep.isEmpty())
            keep.append(KeepRange{});
        int64_t previousEndUs = m_originUs;
        for (const KeepRange &range : keep) {
            Segment seg;
            seg.wantStartUs = std::max(previousEndUs, m_originUs + std::max<qint64>(0, range.startMs) * 1000);
            if (range.endMs >= 0 && m_originUs + range.endMs * 1000 < m_fileEndUs)
                seg.wantEndUs = m_originUs + range.endMs * 1000;
            seg.cutBefore = range.cutBefore;
            seg.head = seg.wantStartUs <= m_originUs;
            if (seg.wantStartUs >= std::min(seg.wantEndUs, m_fileEndUs))
                continue; // empty, or beyond the end of the file
            m_segments.push_back(seg);
            if (seg.wantEndUs == kNoEnd)
                break;
            previousEndUs = seg.wantEndUs;
        }
        if (m_segments.empty())
            fail(tr("The trims leave nothing to export."));
    }

    void openOutput()
    {
        const QByteArray finalName = m_job.output.toUtf8();
        const AVOutputFormat *format = av_guess_format(nullptr, finalName.constData(), nullptr);
        if (!format)
            fail(tr("Unknown output file type \".%1\". Use .mp4, .mov or .mkv.")
                     .arg(QFileInfo(m_job.output).suffix()));
        AVFormatContext *oc = nullptr;
        check(avformat_alloc_output_context2(&oc, format, nullptr, m_partPath.toUtf8().constData()),
              tr("Could not set up the output file"));
        m_out.reset(oc);
        av_dict_copy(&oc->metadata, m_in->metadata, 0);
    }

    bool containerSupports(AVCodecID id) const
    {
        return avformat_query_codec(m_out->oformat, id, FF_COMPLIANCE_NORMAL) != 0;
    }

    // MP4, MOV and their relatives share one muxer and its conventions.
    bool movFamily() const
    {
        const QByteArray muxer = m_out->oformat->name;
        return muxer == "mp4" || muxer == "mov" || muxer == "ipod";
    }

    void requireContainerSupport(AVCodecID id) const
    {
        if (!containerSupports(id))
            fail(tr("A .%1 file can't hold %2. Export to .mkv instead.")
                     .arg(QFileInfo(m_job.output).suffix(), QString::fromUtf8(avcodec_get_name(id))));
    }

    void addCopyStream(int index)
    {
        const AVStream *ist = m_in->streams[index];
        const AVCodecParameters *par = ist->codecpar;
        requireContainerSupport(par->codec_id);

        AVStream *ost = avformat_new_stream(m_out.get(), nullptr);
        if (!ost)
            fail(tr("Out of memory."));
        check(avcodec_parameters_copy(ost->codecpar, par), tr("Could not copy the stream parameters"));

        // Keep the source's codec tag only where the target container knows it.
        const AVCodecTag *const *tags = m_out->oformat->codec_tag;
        unsigned int unused = 0;
        const bool keepTag = !tags || av_codec_get_id(tags, par->codec_tag) == par->codec_id
                             || !av_codec_get_tag2(tags, par->codec_id, &unused);
        if (!keepTag)
            ost->codecpar->codec_tag = 0;
        // Left to itself the muxer picks hev1, which Apple's players reject.
        if (!ost->codecpar->codec_tag && par->codec_id == AV_CODEC_ID_HEVC && movFamily())
            ost->codecpar->codec_tag = MKTAG('h', 'v', 'c', '1');

        // The decoder delay a container may record describes the source's
        // priming packet, which is only there if the output starts where the
        // source does.
        if (!m_segments.front().head)
            ost->codecpar->initial_padding = 0;
        ost->time_base = ist->time_base;
        ost->avg_frame_rate = ist->avg_frame_rate;
        ost->sample_aspect_ratio = ist->sample_aspect_ratio;
        ost->disposition = ist->disposition;
        av_dict_copy(&ost->metadata, ist->metadata, 0);
        m_streams[index].out = ost->index;
    }

    void openDecoder()
    {
        const AVStream *st = m_in->streams[m_videoIn];
        const AVCodec *codec = avcodec_find_decoder(st->codecpar->codec_id);
        if (!codec)
            fail(tr("FFmpeg has no decoder for %1.").arg(QString::fromUtf8(avcodec_get_name(st->codecpar->codec_id))));
        m_dec.reset(avcodec_alloc_context3(codec));
        check(avcodec_parameters_to_context(m_dec.get(), st->codecpar), tr("Could not set up the decoder"));
        m_dec->pkt_timebase = st->time_base;
        m_dec->framerate = av_guess_frame_rate(m_in.get(), m_in->streams[m_videoIn], nullptr);
        m_dec->thread_count = 0;
        check(avcodec_open2(m_dec.get(), codec, nullptr), tr("Could not open the decoder"));
    }

    // Positions the demuxer on the keyframe at or before a video timestamp.
    bool seekVideo(int64_t pts)
    {
        const AVStream *st = m_in->streams[m_videoIn];
        int err = av_seek_frame(m_in.get(), m_videoIn, pts, AVSEEK_FLAG_BACKWARD);
        if (err < 0 && av_rescale_q(pts, st->time_base, AV_TIME_BASE_Q) <= m_originUs)
            err = av_seek_frame(m_in.get(), -1, 0, AVSEEK_FLAG_BYTE);
        return err >= 0;
    }

    int64_t videoPts(int64_t us) const
    {
        return av_rescale_q(us, AV_TIME_BASE_Q, m_in->streams[m_videoIn]->time_base);
    }

    // Decodes one frame at the start position so the filter graph and encoder
    // are configured from real frame properties rather than container hints.
    void probeFirstFrame()
    {
        if (!seekVideo(videoPts(m_segments.front().wantStartUs)))
            fail(tr("Could not seek in the video."));
        bool got = false;
        while (!got) {
            checkCancel();
            if (av_read_frame(m_in.get(), m_pkt.get()) < 0)
                break;
            if (m_pkt->stream_index == m_videoIn) {
                avcodec_send_packet(m_dec.get(), m_pkt.get());
                got = avcodec_receive_frame(m_dec.get(), m_frame.get()) == 0;
            }
            av_packet_unref(m_pkt.get());
        }
        if (!got) {
            avcodec_send_packet(m_dec.get(), nullptr);
            got = avcodec_receive_frame(m_dec.get(), m_frame.get()) == 0;
        }
        if (!got)
            fail(tr("No video frames could be decoded from the part that is kept."));

        m_srcFormat = AVPixelFormat(m_frame->format);
        m_srcWidth = m_frame->width;
        m_srcHeight = m_frame->height;
        m_srcAspect = m_frame->sample_aspect_ratio;
        m_srcColorSpace = m_frame->colorspace;
        m_srcColorRange = m_frame->color_range;
        av_frame_copy_props(m_probe.get(), m_frame.get());
        av_frame_unref(m_frame.get());
        avcodec_flush_buffers(m_dec.get());
    }

    // The crop rectangle in the pixels of a w x h frame, aligned to even
    // numbers as 4:2:0 chroma subsampling requires.
    QRect cropFor(int w, int h) const
    {
        QRectF r(0, 0, w, h);
        if (!m_job.crop.isEmpty()) {
            r = m_job.crop;
            if (!m_job.displaySize.isEmpty() && m_job.displaySize != QSize(w, h)) {
                const double sx = double(w) / m_job.displaySize.width();
                const double sy = double(h) / m_job.displaySize.height();
                r = QRectF(r.x() * sx, r.y() * sy, r.width() * sx, r.height() * sy);
            }
        }
        const int x = std::clamp(int(std::lround(r.x())) & ~1, 0, std::max(0, w - 2));
        const int y = std::clamp(int(std::lround(r.y())) & ~1, 0, std::max(0, h - 2));
        const int cw = std::min(int(std::lround(r.width())), w - x) & ~1;
        const int ch = std::min(int(std::lround(r.height())), h - y) & ~1;
        if (cw < 2 || ch < 2)
            fail(tr("The crop rectangle is too small."));
        return QRect(x, y, cw, ch);
    }

    AVPixelFormat softwareFormatFor(const AVCodec *codec, EncoderChoice::Kind kind) const
    {
        if (kind == EncoderChoice::Vaapi)
            return AV_PIX_FMT_NV12;
        // The quality preset keeps high bit depth sources at 10 bit; the
        // compatible one is always 8 bit.
        const AVPixFmtDescriptor *source = av_pix_fmt_desc_get(m_srcFormat);
        const bool deep = m_job.preset == ExportPreset::HevcQuality && source && source->comp[0].depth > 8;
        const void *configs = nullptr;
        int count = 0;
        if (avcodec_get_supported_config(nullptr, codec, AV_CODEC_CONFIG_PIX_FORMAT, 0, &configs, &count) < 0
            || !configs)
            return AV_PIX_FMT_YUV420P;
        const auto *formats = static_cast<const AVPixelFormat *>(configs);
        const auto *end = formats + count;
        if (deep && std::find(formats, end, AV_PIX_FMT_YUV420P10LE) != end)
            return AV_PIX_FMT_YUV420P10LE;
        for (AVPixelFormat wanted : {AV_PIX_FMT_YUV420P, AV_PIX_FMT_NV12}) {
            if (std::find(formats, end, wanted) != end)
                return wanted;
        }
        for (const auto *f = formats; f != end; ++f) {
            const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(*f);
            if (desc && !(desc->flags & AV_PIX_FMT_FLAG_HWACCEL))
                return *f;
        }
        return AV_PIX_FMT_YUV420P;
    }

    void buildGraph(const EncoderChoice &choice, AVPixelFormat swFormat)
    {
        const AVStream *st = m_in->streams[m_videoIn];
        bool swapsAxes = false;
        QByteArray chain = rotationFilters(st, &swapsAxes);
        const int w = swapsAxes ? m_srcHeight : m_srcWidth;
        const int h = swapsAxes ? m_srcWidth : m_srcHeight;
        m_crop = cropFor(w, h);
        if (m_crop != QRect(0, 0, w, h)) {
            chain += QByteArray("crop=") + QByteArray::number(m_crop.width()) + ':'
                     + QByteArray::number(m_crop.height()) + ':' + QByteArray::number(m_crop.x()) + ':'
                     + QByteArray::number(m_crop.y()) + ":exact=1,";
        }
        chain += QByteArray("format=") + av_get_pix_fmt_name(swFormat);
        if (choice.kind == EncoderChoice::Vaapi)
            chain += ",hwupload";

        m_graph.reset(avfilter_graph_alloc());
        m_src = avfilter_graph_alloc_filter(m_graph.get(), avfilter_get_by_name("buffer"), "in");
        if (!m_src)
            fail(tr("Out of memory."));
        AVBufferSrcParameters *params = av_buffersrc_parameters_alloc();
        params->format = m_srcFormat;
        params->width = m_srcWidth;
        params->height = m_srcHeight;
        params->time_base = st->time_base;
        params->sample_aspect_ratio = m_srcAspect;
        params->frame_rate = m_dec->framerate;
        params->color_space = m_srcColorSpace;
        params->color_range = m_srcColorRange;
        int err = av_buffersrc_parameters_set(m_src, params);
        av_free(params);
        check(err, tr("Could not configure the filter input"));
        check(avfilter_init_dict(m_src, nullptr), tr("Could not configure the filter input"));
        check(avfilter_graph_create_filter(&m_sink, avfilter_get_by_name("buffersink"), "out", nullptr, nullptr,
                                           m_graph.get()),
              tr("Could not configure the filter output"));

        AVFilterInOut *outputs = avfilter_inout_alloc();
        AVFilterInOut *inputs = avfilter_inout_alloc();
        outputs->name = av_strdup("in");
        outputs->filter_ctx = m_src;
        outputs->pad_idx = 0;
        outputs->next = nullptr;
        inputs->name = av_strdup("out");
        inputs->filter_ctx = m_sink;
        inputs->pad_idx = 0;
        inputs->next = nullptr;
        err = avfilter_graph_parse_ptr(m_graph.get(), chain.constData(), &inputs, &outputs, nullptr);
        avfilter_inout_free(&inputs);
        avfilter_inout_free(&outputs);
        check(err, tr("Could not build the crop filter"));

        if (choice.kind == EncoderChoice::Vaapi) {
            AVBufferRef *device = nullptr;
            check(av_hwdevice_ctx_create(&device, AV_HWDEVICE_TYPE_VAAPI, nullptr, nullptr, 0),
                  tr("Could not open a VA-API device"));
            m_hwDevice.reset(device);
            for (unsigned i = 0; i < m_graph->nb_filters; ++i)
                m_graph->filters[i]->hw_device_ctx = av_buffer_ref(device);
        }
        check(avfilter_graph_config(m_graph.get(), nullptr), tr("Could not configure the crop filter"));
    }

    // A bitrate for encoders without a constant-quality mode: the source's
    // bitrate scaled to the cropped area, kept within sane bits-per-pixel bounds.
    int64_t targetBitrate(double fps) const
    {
        const AVStream *st = m_in->streams[m_videoIn];
        const double pixels = double(m_crop.width()) * m_crop.height();
        const double byQuality = pixels * fps * 0.12;
        int64_t source = st->codecpar->bit_rate;
        if (source <= 0)
            source = m_in->bit_rate;
        const double bySource = double(source) * pixels / (double(m_srcWidth) * m_srcHeight) * 1.5;
        return int64_t(std::clamp(bySource, byQuality, byQuality * 4));
    }

    void buildPipeline(const EncoderChoice &choice)
    {
        const AVCodec *codec = avcodec_find_encoder_by_name(choice.name);
        if (!codec)
            fail(tr("%1 is not part of this FFmpeg build").arg(QLatin1String(choice.name)));
        buildGraph(choice, softwareFormatFor(codec, choice.kind));

        const AVCodecContext *dec = m_dec.get();
        m_enc.reset(avcodec_alloc_context3(codec));
        AVCodecContext *enc = m_enc.get();
        enc->width = av_buffersink_get_w(m_sink);
        enc->height = av_buffersink_get_h(m_sink);
        enc->sample_aspect_ratio = av_buffersink_get_sample_aspect_ratio(m_sink);
        enc->pix_fmt = AVPixelFormat(av_buffersink_get_format(m_sink));
        enc->time_base = av_buffersink_get_time_base(m_sink);
        const AVRational rate = av_buffersink_get_frame_rate(m_sink);
        if (rate.num > 0 && rate.den > 0)
            enc->framerate = rate;
        enc->colorspace = av_buffersink_get_colorspace(m_sink);
        enc->color_range = av_buffersink_get_color_range(m_sink);
        enc->color_primaries = dec->color_primaries;
        enc->color_trc = dec->color_trc;
        enc->chroma_sample_location = dec->chroma_sample_location;
        if (m_out->oformat->flags & AVFMT_GLOBALHEADER)
            enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

        const double fps = enc->framerate.num > 0 ? av_q2d(enc->framerate) : 30.0;
        AVDictionary *options = nullptr;
        if (std::strcmp(choice.name, "libx265") == 0) {
            av_dict_set(&options, "preset", "slow", 0);
            av_dict_set(&options, "crf", "20", 0);
            // Carry HDR mastering metadata over to the new stream.
            for (AVFrameSideDataType type :
                 {AV_FRAME_DATA_MASTERING_DISPLAY_METADATA, AV_FRAME_DATA_CONTENT_LIGHT_LEVEL}) {
                if (const AVFrameSideData *sd = av_frame_get_side_data(m_probe.get(), type))
                    av_frame_side_data_clone(&enc->decoded_side_data, &enc->nb_decoded_side_data, sd, 0);
            }
        } else if (std::strcmp(choice.name, "libx264") == 0) {
            av_dict_set(&options, "preset", "veryfast", 0);
            av_dict_set(&options, "crf", "18", 0);
        } else {
            enc->gop_size = std::max(1, int(std::lround(fps * 2)));
            switch (choice.kind) {
            case EncoderChoice::Nvenc:
                av_dict_set(&options, "preset", "p4", 0);
                av_dict_set(&options, "rc", "vbr", 0);
                av_dict_set(&options, "cq", "19", 0);
                break;
            case EncoderChoice::Vaapi:
                enc->hw_frames_ctx = av_buffer_ref(av_buffersink_get_hw_frames_ctx(m_sink));
                av_dict_set(&options, "rc_mode", "CQP", 0);
                av_dict_set(&options, "qp", "20", 0);
                break;
            case EncoderChoice::VideoToolbox:
            case EncoderChoice::Software:
                enc->bit_rate = targetBitrate(fps);
                break;
            }
        }
        const int err = avcodec_open2(enc, codec, &options);
        av_dict_free(&options);
        check(err, tr("Could not open %1").arg(QLatin1String(choice.name)));
    }

    void openEncoder()
    {
        const bool hevc = m_job.preset == ExportPreset::HevcQuality;
        const bool hardware = m_job.hardware && !hevc;
        requireContainerSupport(hevc ? AV_CODEC_ID_HEVC : AV_CODEC_ID_H264);

        std::vector<EncoderChoice> choices;
        if (hevc) {
            // Only x265's CRF mode gives the quality this preset promises, so
            // there is deliberately no fallback to another encoder.
            choices.push_back({"libx265", EncoderChoice::Software});
        } else {
            if (hardware) {
#ifdef Q_OS_MACOS
                choices.push_back({"h264_videotoolbox", EncoderChoice::VideoToolbox});
#else
                choices.push_back({"h264_nvenc", EncoderChoice::Nvenc});
                choices.push_back({"h264_vaapi", EncoderChoice::Vaapi});
#endif
            }
            choices.push_back({"libx264", EncoderChoice::Software});
            choices.push_back({"libopenh264", EncoderChoice::Software});
        }

        QString lastError;
        const EncoderChoice *used = nullptr;
        for (const EncoderChoice &choice : choices) {
            try {
                buildPipeline(choice);
                used = &choice;
                break;
            } catch (const ExportError &e) {
                lastError = e.message;
                m_enc.reset();
                m_graph.reset();
                m_src = m_sink = nullptr;
                m_hwDevice.reset();
            }
        }
        if (!used)
            fail(tr("No usable %1 encoder was found (%2).").arg(hevc ? tr("H.265") : tr("H.264"), lastError));

        const AVStream *ist = m_in->streams[m_videoIn];
        AVStream *ost = avformat_new_stream(m_out.get(), nullptr);
        if (!ost)
            fail(tr("Out of memory."));
        check(avcodec_parameters_from_context(ost->codecpar, m_enc.get()),
              tr("Could not set up the video stream"));
        // Apple's players only accept H.265 in MP4/MOV under the hvc1 tag.
        if (hevc && movFamily())
            ost->codecpar->codec_tag = MKTAG('h', 'v', 'c', '1');
        ost->time_base = m_enc->time_base;
        ost->avg_frame_rate = m_enc->framerate;
        ost->disposition = ist->disposition;
        av_dict_copy(&ost->metadata, ist->metadata, 0);
        m_videoOut = ost->index;

        m_details = hevc ? tr("re-encoded with x265 (CRF 20, slow)")
                         : tr("re-encoded with %1").arg(QLatin1String(used->name));
        if (hardware && used->kind == EncoderChoice::Software)
            m_details += tr(" (no hardware encoder was usable)");
    }

    void writeHeader()
    {
        AVFormatContext *oc = m_out.get();
        if (!(oc->oformat->flags & AVFMT_NOFILE))
            check(avio_open(&oc->pb, m_partPath.toUtf8().constData(), AVIO_FLAG_WRITE),
                  tr("Could not create the output file"));
        AVDictionary *options = nullptr;
        // Moving the index to the front costs a second pass over the file,
        // which a local preview copy has no use for.
        if (movFamily() && !m_job.preview)
            av_dict_set(&options, "movflags", "+faststart", 0);
        const int err = avformat_write_header(oc, &options);
        av_dict_free(&options);
        check(err, tr("Could not start the output file"));
    }

    bool allFinished() const
    {
        return m_video.done && std::all_of(m_streams.begin(), m_streams.end(), [](const OutStream &s) {
            return s.out < 0 || s.done;
        });
    }

    // Reads the video packets around source time `us`, without decoding them.
    Scan scan(int64_t us)
    {
        const AVStream *st = m_in->streams[m_videoIn];
        const int64_t point = videoPts(us);
        // Packets come in decode order, so one that is shown near the point
        // can turn up a little after later ones; read a second past it.
        const int64_t reach = point + av_rescale_q(AV_TIME_BASE, AV_TIME_BASE_Q, st->time_base);
        Scan result;
        if (!seekVideo(point))
            return result;

        int64_t lastShown = std::numeric_limits<int64_t>::min();
        int64_t lastDuration = m_frameDuration;
        bool pastPoint = false;
        while (av_read_frame(m_in.get(), m_pkt.get()) >= 0) {
            const bool video = m_pkt->stream_index == m_videoIn;
            const int64_t pts = m_pkt->pts != AV_NOPTS_VALUE ? m_pkt->pts : m_pkt->dts;
            const int64_t duration = m_pkt->duration > 0 ? m_pkt->duration : m_frameDuration;
            const bool key = m_pkt->flags & AV_PKT_FLAG_KEY;
            if (video && m_pkt->dts == AV_NOPTS_VALUE)
                m_makeDts = true;
            av_packet_unref(m_pkt.get());
            if (!video || pts == AV_NOPTS_VALUE)
                continue;

            if (result.key == AV_NOPTS_VALUE) {
                if (!key)
                    continue;
                result.key = pts;
            }
            if (result.nearest == AV_NOPTS_VALUE || std::llabs(pts - point) < std::llabs(result.nearest - point))
                result.nearest = pts;
            // The same walk copyVideoPacket() does, to learn where it stops.
            if (!pastPoint && pts > lastShown) {
                if (pts >= point) {
                    pastPoint = true;
                } else {
                    lastShown = pts;
                    lastDuration = duration;
                }
            }
            if (pts > reach)
                break;
        }
        if (lastShown != std::numeric_limits<int64_t>::min())
            result.copyEnd = lastShown + lastDuration;
        else if (result.key != AV_NOPTS_VALUE)
            result.copyEnd = result.key;
        return result;
    }

    // Pins every segment to actual video frames.
    //
    // Re-encoding can cut anywhere, so each requested cut point moves to the
    // nearest frame; the segments then hold a whole number of frames and
    // join without a gap or a repeat. A stream copy can only start on a
    // keyframe and stop in front of a reference frame, so there the segment
    // grows backwards to the keyframe and shrinks to the last clean stop.
    void plan(bool copy)
    {
        std::vector<Segment> planned;
        int64_t previousEnd = std::numeric_limits<int64_t>::min();
        int64_t out = 0;
        for (Segment seg : m_segments) {
            checkCancel();
            const Scan start = scan(seg.wantStartUs);
            if (copy && start.key == AV_NOPTS_VALUE)
                fail(tr("No keyframe was found to start from at %1.")
                         .arg(formatTime((seg.wantStartUs - m_originUs) / 1000)));
            seg.startPts = copy ? start.key
                                : start.nearest != AV_NOPTS_VALUE ? start.nearest : videoPts(seg.wantStartUs);
            if (seg.wantEndUs != kNoEnd) {
                const Scan end = scan(seg.wantEndUs);
                const int64_t found = copy ? end.copyEnd : end.nearest;
                seg.endPts = found != AV_NOPTS_VALUE ? found : videoPts(seg.wantEndUs);
            }

            if (copy && seg.startPts < previousEnd) {
                // The keyframe to resume from lies before the trim, so copying
                // from it would bring the trimmed section straight back.
                fail(tr("Trim %1 has no keyframe after it to resume from, so it can't be cut out without "
                        "re-encoding. Tick Re-encode, or extend that trim.")
                         .arg(seg.cutBefore));
            }
            if (seg.endPts != AV_NOPTS_VALUE && seg.endPts <= seg.startPts)
                continue; // shorter than a frame
            seg.outPts = out;
            m_maxEarlyUs = std::max(m_maxEarlyUs, seg.wantStartUs
                                                      - av_rescale_q(seg.startPts,
                                                                     m_in->streams[m_videoIn]->time_base,
                                                                     AV_TIME_BASE_Q));
            const int64_t stop = seg.endPts != AV_NOPTS_VALUE ? seg.endPts
                                 : m_fileEndUs != kNoEnd      ? videoPts(m_fileEndUs)
                                                              : seg.startPts;
            out += std::max<int64_t>(0, stop - seg.startPts);
            previousEnd = seg.endPts;
            planned.push_back(seg);
            if (seg.endPts == AV_NOPTS_VALUE)
                break;
        }
        if (planned.empty())
            fail(tr("The trims leave nothing to export."));
        m_segments = std::move(planned);
        m_totalPts = m_fileEndUs != kNoEnd || m_segments.back().endPts != AV_NOPTS_VALUE ? out : 0;
    }

    // Decides whether an audio packet at source time `ts`, lasting `dur`,
    // belongs to the current segment, and where it goes in the output.
    //
    // Audio packets do not line up with video frames, so a segment's audio
    // starts and ends on the packets nearest to the cut, and its first packet
    // is butted up against the previous segment's last one. That keeps the
    // track gapless (some players ignore gaps and would drift) and never more
    // than half a packet away from the picture, however many cuts there are.
    Place place(OutStream &s, AVRational tb, int64_t ts, int64_t dur, int64_t *out) const
    {
        const AVRational videoTb = m_in->streams[m_videoIn]->time_base;
        const int64_t half = dur / 2;
        const int64_t srcStart = av_rescale_q(m_seg->startPts, videoTb, tb);
        const int64_t outStart = av_rescale_q(m_seg->outPts, videoTb, tb);
        const bool bounded = m_seg->endPts != AV_NOPTS_VALUE;
        const int64_t length = bounded ? av_rescale_q(m_seg->endPts - m_seg->startPts, videoTb, tb) : 0;
        if (bounded && ts >= srcStart + length)
            return Place::Finished;

        const int64_t ideal = outStart + ts - srcStart;
        // An output that begins where the source does keeps the source's own
        // lead-in, including the priming packet codecs such as AAC start
        // with at a negative time.
        const bool leadIn = m_seg->head && !s.wrote;
        if (!s.started) {
            if (!leadIn && ideal + half < std::max(s.next, outStart - half))
                return Place::Skip;
            s.started = true;
            // A larger distance is a real hole in the source (a track that
            // starts late, say) and is left as it is.
            s.shift = !leadIn && ideal - s.next <= std::max<int64_t>(dur, 1) ? ideal - s.next : 0;
        }
        const int64_t at = leadIn ? ideal : std::max(ideal - s.shift, s.next);
        if (bounded && at + half >= outStart + length)
            return Place::Finished;
        *out = at;
        s.next = at + std::max<int64_t>(dur, 1);
        s.wrote = true;
        return Place::Keep;
    }

    void write(AVPacket *pkt, const OutStream &s, AVRational tb)
    {
        av_packet_rescale_ts(pkt, tb, m_out->streams[s.out]->time_base);
        pkt->stream_index = s.out;
        pkt->pos = -1;
        check(av_interleaved_write_frame(m_out.get(), pkt), tr("Could not write to the output file"));
    }

    void copyAudioPacket(AVPacket *pkt)
    {
        OutStream &s = m_streams[pkt->stream_index];
        if (s.done)
            return;
        const AVStream *ist = m_in->streams[pkt->stream_index];
        const int64_t ts = pkt->pts != AV_NOPTS_VALUE ? pkt->pts : pkt->dts;
        if (ts == AV_NOPTS_VALUE)
            return;
        int64_t dur = pkt->duration;
        if (dur <= 0 && ist->codecpar->frame_size > 0 && ist->codecpar->sample_rate > 0)
            dur = av_rescale_q(ist->codecpar->frame_size, AVRational{1, ist->codecpar->sample_rate}, ist->time_base);

        int64_t out = 0;
        switch (place(s, ist->time_base, ts, std::max<int64_t>(dur, 0), &out)) {
        case Place::Skip:
            return;
        case Place::Finished:
            s.done = true;
            return;
        case Place::Keep:
            break;
        }
        pkt->pts = pkt->dts = out;
        write(pkt, s, ist->time_base);
    }

    // A decode timestamp for a copied video packet whose source has none
    // (Matroska stores only display times): the earliest display time among
    // the packets that can still be waiting to be shown.
    int64_t makeDts(int64_t pts)
    {
        const size_t depth = size_t(std::max(m_in->streams[m_videoIn]->codecpar->video_delay, 0));
        m_reorder.insert(std::upper_bound(m_reorder.begin(), m_reorder.end(), pts), pts);
        if (m_reorder.size() > depth) {
            const int64_t dts = m_reorder.front();
            m_reorder.erase(m_reorder.begin());
            return dts;
        }
        // Still filling up at the very start: count back from the first.
        return m_reorder.front() - int64_t(depth + 1 - m_reorder.size()) * m_frameDuration;
    }

    // Stream copy of the video. Packets arrive in decode order, which with
    // B-frames is not the order they are shown in.
    void copyVideoPacket(AVPacket *pkt)
    {
        OutStream &s = m_streams[m_videoIn];
        if (s.done)
            return;
        const AVStream *ist = m_in->streams[m_videoIn];
        const int64_t pts = pkt->pts != AV_NOPTS_VALUE ? pkt->pts : pkt->dts;
        if (pts == AV_NOPTS_VALUE)
            return;

        // Frames can only be decoded from the segment's keyframe on, and
        // those shown before it reference pictures that are not copied.
        if (!s.started && (!(pkt->flags & AV_PKT_FLAG_KEY) || pts < m_seg->startPts))
            return;
        s.started = true;
        if (pts < m_seg->startPts)
            return;
        // A packet shown later than everything before it is a reference
        // frame; the B-frames that follow it are shown earlier and depend on
        // it. Ending right before such a frame is the only cut that leaves
        // no gaps.
        if (pts > s.lastShown) {
            if (m_seg->endPts != AV_NOPTS_VALUE && pts >= m_seg->endPts) {
                s.done = true;
                return;
            }
            s.lastShown = pts;
        }
        report(pts);

        const int64_t shift = m_seg->startPts - m_seg->outPts;
        const int64_t sourceDts = pkt->dts;
        pkt->pts = pts - shift;
        // Decode times run a little ahead of display times, so at a join the
        // new segment's first ones can overlap the old segment's last. All
        // that is required of them is to keep increasing.
        const int64_t dts = m_makeDts || sourceDts == AV_NOPTS_VALUE ? makeDts(pkt->pts) : sourceDts - shift;
        pkt->dts = std::min(std::max(dts, s.lastDts + 1), pkt->pts);
        s.lastDts = pkt->dts;
        write(pkt, s, ist->time_base);
    }

    void encode(AVFrame *frame)
    {
        check(avcodec_send_frame(m_enc.get(), frame), tr("Encoding failed"));
        for (;;) {
            const int err = avcodec_receive_packet(m_enc.get(), m_encPkt.get());
            if (err == AVERROR(EAGAIN) || err == AVERROR_EOF)
                return;
            check(err, tr("Encoding failed"));
            m_encPkt->stream_index = m_videoOut;
            av_packet_rescale_ts(m_encPkt.get(), m_enc->time_base, m_out->streams[m_videoOut]->time_base);
            check(av_interleaved_write_frame(m_out.get(), m_encPkt.get()),
                  tr("Could not write to the output file"));
        }
    }

    void drainFilter()
    {
        for (;;) {
            const int err = av_buffersink_get_frame(m_sink, m_filtered.get());
            if (err == AVERROR(EAGAIN) || err == AVERROR_EOF)
                return;
            check(err, tr("Cropping failed"));
            m_filtered->pict_type = AV_PICTURE_TYPE_NONE;
            encode(m_filtered.get());
            av_frame_unref(m_filtered.get());
        }
    }

    // Feeds one packet (or a flush, when null) to the decoder and pushes the
    // frames that belong to the current segment on to the encoder.
    void decode(AVPacket *pkt)
    {
        const int sent = avcodec_send_packet(m_dec.get(), pkt);
        if (sent < 0 && sent != AVERROR(EAGAIN) && sent != AVERROR_EOF && sent != AVERROR_INVALIDDATA)
            check(sent, tr("Decoding failed"));

        while (!m_video.done) {
            const int err = avcodec_receive_frame(m_dec.get(), m_frame.get());
            if (err == AVERROR(EAGAIN) || err == AVERROR_EOF)
                return;
            check(err, tr("Decoding failed"));

            int64_t ts = m_frame->best_effort_timestamp;
            if (ts == AV_NOPTS_VALUE)
                ts = m_frame->pts != AV_NOPTS_VALUE ? m_frame->pts : m_nextTs;
            m_nextTs = ts + (m_frame->duration > 0 ? m_frame->duration : m_frameDuration);

            if (m_seg->endPts != AV_NOPTS_VALUE && ts >= m_seg->endPts) {
                av_frame_unref(m_frame.get());
                m_video.done = true;
                return;
            }
            if (ts < m_seg->startPts) {
                av_frame_unref(m_frame.get());
                continue;
            }
            // Strictly increasing even if the source repeats a timestamp.
            m_frame->pts = std::max(ts - m_seg->startPts + m_seg->outPts, m_video.next);
            m_video.next = m_frame->pts + 1;
            m_frame->pict_type = AV_PICTURE_TYPE_NONE;
            // The rotation is baked in by the filter graph.
            av_frame_remove_side_data(m_frame.get(), AV_FRAME_DATA_DISPLAYMATRIX);
            check(av_buffersrc_add_frame_flags(m_src, m_frame.get(), 0), tr("Cropping failed"));
            drainFilter();
            report(ts);
        }
    }

    // Reads the kept segments one after the other and joins them into one
    // continuous output. `copy` passes the video through untouched;
    // otherwise it is decoded, cropped and encoded. Audio is always copied.
    void runSegments(bool copy)
    {
        plan(copy);
        const AVRational videoTb = m_in->streams[m_videoIn]->time_base;

        for (const Segment &seg : m_segments) {
            m_seg = &seg;
            for (OutStream &s : m_streams) {
                s.started = s.done = false;
                s.lastShown = std::numeric_limits<int64_t>::min();
            }
            m_video.done = copy; // when copying, the video is one of m_streams
            if (!seekVideo(seg.startPts))
                break; // nothing left in the file to read
            if (!copy)
                avcodec_flush_buffers(m_dec.get());

            bool eof = false;
            while (!allFinished()) {
                checkCancel();
                const int err = av_read_frame(m_in.get(), m_pkt.get());
                if (err == AVERROR_EOF) {
                    eof = true;
                    break;
                }
                check(err, tr("Could not read the video"));
                const int index = m_pkt->stream_index;
                if (index != m_videoIn) {
                    if (m_streams[index].out >= 0)
                        copyAudioPacket(m_pkt.get());
                } else if (copy) {
                    copyVideoPacket(m_pkt.get());
                } else if (!m_video.done) {
                    decode(m_pkt.get());
                }

                // A track that stopped early never reports that it is past
                // the segment's end; do not read the rest of the file for it.
                const bool videoDone = copy ? m_streams[m_videoIn].done : m_video.done;
                const int64_t ts = m_pkt->dts != AV_NOPTS_VALUE ? m_pkt->dts : m_pkt->pts;
                const bool farPast = videoDone && seg.endPts != AV_NOPTS_VALUE && ts != AV_NOPTS_VALUE
                                     && av_compare_ts(ts, m_in->streams[index]->time_base,
                                                      seg.endPts + 2 * videoPts(AV_TIME_BASE), videoTb) > 0;
                av_packet_unref(m_pkt.get());
                if (farPast)
                    break;
            }
            if (eof && !copy && !m_video.done)
                decode(nullptr);
        }

        if (copy) {
            m_details = tr("copied without re-encoding");
            if (m_maxEarlyUs > 1000)
                m_details += tr(", kept parts start on keyframes up to %1 s before the chosen points")
                                 .arg(QString::number(m_maxEarlyUs / 1e6, 'f', 2));
        } else {
            check(av_buffersrc_add_frame_flags(m_src, nullptr, 0), tr("Cropping failed"));
            drainFilter();
            encode(nullptr);
        }
    }

    const ExportJob &m_job;
    const QString m_partPath;
    const std::atomic<bool> &m_cancel;
    const std::function<void(double)> m_progress;

    std::unique_ptr<AVPacket, PacketFree> m_pkt, m_encPkt;
    std::unique_ptr<AVFrame, FrameFree> m_frame, m_filtered;
    std::unique_ptr<AVFrame, FrameFree> m_probe; // properties of the first decoded frame, no pixels
    std::unique_ptr<AVFormatContext, InputCloser> m_in;
    std::unique_ptr<AVFormatContext, OutputCloser> m_out;
    std::unique_ptr<AVCodecContext, CodecFree> m_dec, m_enc;
    std::unique_ptr<AVBufferRef, BufferUnref> m_hwDevice;
    std::unique_ptr<AVFilterGraph, GraphFree> m_graph;
    AVFilterContext *m_src = nullptr;
    AVFilterContext *m_sink = nullptr;

    int m_videoIn = -1;
    int m_videoOut = -1;
    std::vector<OutStream> m_streams; // copied streams, indexed by input stream
    OutStream m_video;                // the re-encoded video
    std::vector<Segment> m_segments;
    const Segment *m_seg = nullptr;   // the segment being read
    int64_t m_originUs = 0;           // source timestamp of position zero
    int64_t m_fileEndUs = kNoEnd;
    int64_t m_totalPts = 0;           // length of the output in the video time base, 0 when unknown
    int64_t m_maxEarlyUs = 0;         // copying: how far a segment starts before its requested point
    int64_t m_frameDuration = 1;      // video time base
    bool m_makeDts = false;           // copying: the source has no decode timestamps
    std::vector<int64_t> m_reorder;   // copying: display times awaiting a decode time, sorted
    int64_t m_nextTs = 0;
    double m_lastProgress = 0;

    AVPixelFormat m_srcFormat = AV_PIX_FMT_NONE;
    int m_srcWidth = 0;
    int m_srcHeight = 0;
    AVRational m_srcAspect{0, 1};
    AVColorSpace m_srcColorSpace = AVCOL_SPC_UNSPECIFIED;
    AVColorRange m_srcColorRange = AVCOL_RANGE_UNSPECIFIED;
    QRect m_crop;
    QString m_details;
};

} // namespace

Exporter::Exporter(ExportJob job, QObject *parent) : QThread(parent), m_job(std::move(job))
{
}

void Exporter::run()
{
    av_log_set_level(AV_LOG_WARNING);
    const QString partPath = m_job.output + QStringLiteral(".part");
    try {
        QString details;
        {
            Session session(m_job, partPath, m_cancel, [this](double f) { emit progress(f); });
            details = session.run();
        }
        QFile::remove(m_job.output);
        if (!QFile::rename(partPath, m_job.output))
            fail(tr("Could not move the finished file to %1.").arg(m_job.output));
        emit progress(1.0);
        emit succeeded(details);
    } catch (const ExportError &e) {
        QFile::remove(partPath);
        emit failed(e.message);
    } catch (const Cancelled &) {
        QFile::remove(partPath);
        emit cancelled();
    }
}
