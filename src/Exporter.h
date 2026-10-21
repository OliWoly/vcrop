#pragma once

#include <QList>
#include <QRect>
#include <QSize>
#include <QString>
#include <QThread>

#include <atomic>

// How the video is encoded when an export has to re-encode it.
enum class ExportPreset {
    HevcQuality, // 10-bit H.265: x265 at CRF 14, or VA-API at QP 16
    Compatible,  // 8-bit H.264, which plays everywhere
};

// A stretch of the video that is kept, in milliseconds from the start of the media.
struct KeepRange {
    qint64 startMs = 0;
    qint64 endMs = -1; // negative means "to the end"
    int cutBefore = 0; // number of the trim that sits in front of it, 0 for none
};

struct ExportJob {
    QString input;
    QString output;
    QList<KeepRange> keep; // in order and not overlapping; empty keeps everything
    QRect crop;            // in displayed (rotation applied) video pixels; empty keeps the full frame
    QSize displaySize;     // the frame size the crop rectangle refers to
    bool reencode = false; // without a crop the streams are copied; this re-encodes instead
    ExportPreset preset = ExportPreset::HevcQuality;
    bool hardware = false; // prefer a hardware encoder, falling back to software
    bool preview = false;  // a throwaway copy for the player: audio the container can't hold is dropped
};

// Runs one export on a worker thread using the FFmpeg libraries directly.
// The result is written next to the destination as "<output>.part" and moved
// into place only once it is complete.
class Exporter : public QThread
{
    Q_OBJECT

public:
    explicit Exporter(ExportJob job, QObject *parent = nullptr);

    void cancel() { m_cancel.store(true); }

signals:
    void progress(double fraction);
    void succeeded(const QString &details);
    void failed(const QString &error);
    void cancelled();

protected:
    void run() override;

private:
    const ExportJob m_job;
    std::atomic<bool> m_cancel{false};
};
