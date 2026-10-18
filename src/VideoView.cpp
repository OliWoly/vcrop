#include "VideoView.h"

#include <QFontDatabase>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QVideoSink>

#include <algorithm>
#include <cmath>

namespace {

enum Hit { Left = 1, Top = 2, Right = 4, Bottom = 8, Move = 16, New = 32 };

constexpr int kMinCrop = 16;    // smallest crop edge, in video pixels
constexpr qreal kGrab = 7.0;    // how close to an edge counts as grabbing it
constexpr qreal kHandle = 8.0;  // drawn handle size
constexpr qreal kMargin = 8.0;  // space around the frame so edge handles stay reachable

// 4:2:0 video needs even offsets and sizes, so the crop snaps to them.
int snapEven(qreal v)
{
    return int(std::lround(v / 2.0)) * 2;
}

} // namespace

VideoView::VideoView(QWidget *parent) : QWidget(parent), m_sink(new QVideoSink(this))
{
    setMouseTracking(true);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    connect(m_sink, &QVideoSink::videoFrameChanged, this, &VideoView::setFrame);
}

void VideoView::reset()
{
    m_pending = QVideoFrame();
    m_pendingSize = QSize();
    m_image = QImage();
    m_blurred = QImage();
    m_drag = 0;
    unsetCursor();
    setCrop(QRect());
    update();
}

void VideoView::clearCrop()
{
    m_drag = 0;
    setCrop(QRect());
}

void VideoView::setFrame(const QVideoFrame &frame)
{
    // The player sends an empty frame when it stops; keep showing the last one.
    if (!frame.isValid())
        return;
    m_pending = frame;
    // The first frame of a new size sets the geometry everything else is
    // measured against, so that one cannot wait for the next repaint.
    if (m_image.isNull() || frame.size() != m_pendingSize) {
        m_pendingSize = frame.size();
        convertPending();
    }
    update();
}

void VideoView::convertPending()
{
    if (!m_pending.isValid())
        return;
    QImage image = m_pending.toImage();
    m_pending = QVideoFrame();
    if (image.isNull())
        return;
    const bool resized = image.size() != m_image.size();
    m_image = std::move(image);
    m_blurDirty = true;
    if (resized) {
        m_drag = 0;
        setCrop(QRect());
        emit videoSizeChanged(m_image.size());
    }
}

void VideoView::setCrop(const QRect &crop)
{
    if (crop == m_crop)
        return;
    m_crop = crop;
    emit cropChanged(m_crop);
    update();
}

QRectF VideoView::targetRect() const
{
    if (m_image.isNull())
        return {};
    const QRectF area = QRectF(rect()).adjusted(kMargin, kMargin, -kMargin, -kMargin);
    const QSizeF fitted = QSizeF(m_image.size()).scaled(area.size(), Qt::KeepAspectRatio);
    return QRectF(QPointF(area.center().x() - fitted.width() / 2, area.center().y() - fitted.height() / 2), fitted);
}

QPointF VideoView::widgetToVideo(const QPointF &pos) const
{
    const QRectF t = targetRect();
    if (t.isEmpty())
        return {};
    return QPointF((pos.x() - t.x()) * m_image.width() / t.width(),
                   (pos.y() - t.y()) * m_image.height() / t.height());
}

QRectF VideoView::videoToWidget(const QRect &rect) const
{
    const QRectF t = targetRect();
    const qreal sx = t.width() / m_image.width();
    const qreal sy = t.height() / m_image.height();
    return QRectF(t.x() + rect.x() * sx, t.y() + rect.y() * sy, rect.width() * sx, rect.height() * sy);
}

int VideoView::hitTest(const QPointF &pos) const
{
    if (!hasCrop())
        return 0;
    const QRectF sel = videoToWidget(m_crop);
    if (!sel.adjusted(-kGrab, -kGrab, kGrab, kGrab).contains(pos))
        return 0;

    int hit = 0;
    const qreal dl = std::abs(pos.x() - sel.left()), dr = std::abs(pos.x() - sel.right());
    const qreal dt = std::abs(pos.y() - sel.top()), db = std::abs(pos.y() - sel.bottom());
    if (std::min(dl, dr) <= kGrab)
        hit |= dl < dr ? Left : Right;
    if (std::min(dt, db) <= kGrab)
        hit |= dt < db ? Top : Bottom;
    return hit ? hit : Move;
}

void VideoView::updateCursor(int hit)
{
    switch (hit) {
    case Left | Top:
    case Right | Bottom:
        setCursor(Qt::SizeFDiagCursor);
        break;
    case Right | Top:
    case Left | Bottom:
        setCursor(Qt::SizeBDiagCursor);
        break;
    case Left:
    case Right:
        setCursor(Qt::SizeHorCursor);
        break;
    case Top:
    case Bottom:
        setCursor(Qt::SizeVerCursor);
        break;
    case Move:
        setCursor(Qt::SizeAllCursor);
        break;
    default:
        setCursor(Qt::CrossCursor);
    }
}

void VideoView::dragTo(const QPointF &v)
{
    const int maxW = m_image.width() & ~1;
    const int maxH = m_image.height() & ~1;
    const int minW = std::min(kMinCrop, maxW);
    const int minH = std::min(kMinCrop, maxH);
    const QRect o = m_dragOrigin;

    if (m_drag == New) {
        const int x1 = std::clamp(snapEven(m_anchor.x()), 0, maxW);
        const int y1 = std::clamp(snapEven(m_anchor.y()), 0, maxH);
        const int x2 = std::clamp(snapEven(v.x()), 0, maxW);
        const int y2 = std::clamp(snapEven(v.y()), 0, maxH);
        // Until the drag spans an area, the previous crop stays in place.
        if (x1 != x2 && y1 != y2)
            setCrop(QRect(std::min(x1, x2), std::min(y1, y2), std::abs(x2 - x1), std::abs(y2 - y1)));
    } else if (m_drag == Move) {
        const QPointF d = v - m_anchor;
        setCrop(QRect(std::clamp(snapEven(o.x() + d.x()), 0, maxW - o.width()),
                      std::clamp(snapEven(o.y() + d.y()), 0, maxH - o.height()), o.width(), o.height()));
    } else {
        int l = o.x(), t = o.y(), r = o.x() + o.width(), b = o.y() + o.height();
        if (m_drag & Left)
            l = std::clamp(snapEven(v.x()), 0, r - minW);
        if (m_drag & Right)
            r = std::clamp(snapEven(v.x()), l + minW, maxW);
        if (m_drag & Top)
            t = std::clamp(snapEven(v.y()), 0, b - minH);
        if (m_drag & Bottom)
            b = std::clamp(snapEven(v.y()), t + minH, maxH);
        setCrop(QRect(l, t, r - l, b - t));
    }
}

void VideoView::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton || m_image.isNull())
        return;
    const int hit = hitTest(event->position());
    m_drag = hit ? hit : New;
    m_dragOrigin = m_crop;
    m_anchor = widgetToVideo(event->position());
}

void VideoView::mouseMoveEvent(QMouseEvent *event)
{
    if (m_image.isNull())
        return;
    const QPointF v = widgetToVideo(event->position());
    const QPoint pixel(std::clamp(int(std::floor(v.x())), 0, m_image.width() - 1),
                       std::clamp(int(std::floor(v.y())), 0, m_image.height() - 1));
    const bool overVideo = targetRect().contains(event->position());
    emit pointerMoved(pixel, overVideo || m_drag, event->globalPosition());

    if (m_drag)
        dragTo(v);
    else if (overVideo || hasCrop())
        updateCursor(hitTest(event->position()));
    else
        unsetCursor();
}

void VideoView::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton || !m_drag)
        return;
    // A drag too small to be a usable crop leaves the previous one untouched.
    if (m_drag == New && hasCrop() && (m_crop.width() < kMinCrop || m_crop.height() < kMinCrop))
        setCrop(m_dragOrigin);
    m_drag = 0;
    updateCursor(hitTest(event->position()));
    update();
}

void VideoView::leaveEvent(QEvent *)
{
    emit pointerLeft();
}

// A cheap stand-in for a real blur: shrink the frame to a thumbnail and let
// the smooth upscale smear it back out.
const QImage &VideoView::blurred()
{
    if (m_blurDirty) {
        // Averaging every pixel of a large frame is the expensive part, so
        // the frame is first decimated to a few times the thumbnail size.
        const QSize small = m_image.size().scaled(96, 96, Qt::KeepAspectRatio).expandedTo(QSize(1, 1));
        const QSize rough = m_image.size().boundedTo(small * 6);
        m_blurred = m_image.scaled(rough, Qt::IgnoreAspectRatio, Qt::FastTransformation)
                        .scaled(small, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                        .scaled(small * 4, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        m_blurDirty = false;
    }
    return m_blurred;
}

void VideoView::paintCrop(QPainter &p, const QRectF &target)
{
    const QRectF sel = videoToWidget(m_crop);

    // Everything outside a finished selection is blurred and darkened.
    QPainterPath outside;
    outside.addRect(target);
    outside.addRect(sel);
    p.save();
    p.setClipPath(outside);
    if (m_drag) {
        // While the rectangle is being dragged the surroundings stay sharp,
        // only slightly dimmed, so the picture can still be judged.
        p.fillRect(target, QColor(0, 0, 0, 70));
    } else {
        p.drawImage(target, blurred());
        p.fillRect(target, QColor(0, 0, 0, 115));
    }
    p.restore();

    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(QColor(0, 0, 0, 170), 3));
    p.drawRect(sel);
    p.setPen(QPen(Qt::white, 1));
    p.drawRect(sel);

    p.setPen(QPen(QColor(0, 0, 0, 200), 1));
    p.setBrush(Qt::white);
    const qreal xs[] = {sel.left(), sel.center().x(), sel.right()};
    const qreal ys[] = {sel.top(), sel.center().y(), sel.bottom()};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            if (i == 1 && j == 1)
                continue;
            p.drawRect(QRectF(xs[i] - kHandle / 2, ys[j] - kHandle / 2, kHandle, kHandle));
        }
    }

    // Size and position readout, tucked above the selection where it fits.
    const QString text = QStringLiteral("%1 × %2  at  %3, %4")
                             .arg(m_crop.width())
                             .arg(m_crop.height())
                             .arg(m_crop.x())
                             .arg(m_crop.y());
    QFont font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    font.setPointSizeF(font.pointSizeF() * 0.95);
    p.setFont(font);
    const QSizeF textSize = QSizeF(p.fontMetrics().size(Qt::TextSingleLine, text)) + QSizeF(12, 6);
    qreal x = std::clamp(sel.left(), 2.0, std::max(2.0, width() - textSize.width() - 2));
    qreal y = sel.top() - textSize.height() - 8;
    if (y < 2)
        y = std::min(sel.top() + 8, height() - textSize.height() - 2);
    const QRectF label(QPointF(x, y), textSize);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0, 190));
    p.setRenderHint(QPainter::Antialiasing);
    p.drawRoundedRect(label, 4, 4);
    p.setPen(Qt::white);
    p.drawText(label, Qt::AlignCenter, text);
}

void VideoView::paintEvent(QPaintEvent *)
{
    convertPending();
    QPainter p(this);
    p.fillRect(rect(), Qt::black);
    if (m_image.isNull()) {
        p.setPen(QColor(150, 150, 150));
        p.drawText(rect(), Qt::AlignCenter, tr("Drop a video here, or press %1 to open one")
                                                .arg(QKeySequence(QKeySequence::Open).toString(QKeySequence::NativeText)));
        return;
    }
    const QRectF target = targetRect();
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.drawImage(target, m_image);
    if (hasCrop())
        paintCrop(p, target);
}
