#pragma once

#include <QImage>
#include <QVideoFrame>
#include <QWidget>

class QVideoSink;

// Shows the current video frame scaled to fit and lets the user draw, move
// and resize a crop rectangle on top of it. The crop is kept in video pixels.
class VideoView : public QWidget
{
    Q_OBJECT

public:
    explicit VideoView(QWidget *parent = nullptr);

    QVideoSink *sink() const { return m_sink; }
    QSize videoSize() const { return m_image.size(); }
    QRect crop() const { return m_crop; }
    bool hasCrop() const { return !m_crop.isEmpty(); }

    // Forgets the frame and the crop, for when another file is loaded.
    void reset();

public slots:
    void clearCrop();

signals:
    void cropChanged(const QRect &crop);
    void videoSizeChanged(const QSize &size);
    // `videoPos` is clamped to the frame; `overVideo` is false while the
    // pointer is on the letterbox bars and no drag is in progress.
    void pointerMoved(const QPoint &videoPos, bool overVideo, const QPointF &globalPos);
    void pointerLeft();

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    void setFrame(const QVideoFrame &frame);
    void convertPending();
    void setCrop(const QRect &crop);
    QRectF targetRect() const;
    QPointF widgetToVideo(const QPointF &pos) const;
    QRectF videoToWidget(const QRect &rect) const;
    int hitTest(const QPointF &pos) const;
    void updateCursor(int hit);
    void dragTo(const QPointF &videoPos);
    const QImage &blurred();
    void paintCrop(QPainter &painter, const QRectF &target);

    QVideoSink *m_sink;
    // Frames are converted to an image only when one is about to be painted,
    // so those that arrive faster than the view repaints cost nothing.
    QVideoFrame m_pending;
    QSize m_pendingSize;
    QImage m_image;
    QImage m_blurred;
    bool m_blurDirty = true;

    QRect m_crop;        // video pixels, empty when there is no crop
    int m_drag = 0;      // what the current drag edits, see the Hit flags
    QRect m_dragOrigin;  // the crop when the drag started
    QPointF m_anchor;    // press position in video pixels
};
