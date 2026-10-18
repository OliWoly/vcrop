#include "TrimSlider.h"

#include <QMouseEvent>
#include <QPainter>

#include <algorithm>
#include <cmath>

namespace {

constexpr qreal kPad = 12.0;       // room for the playhead knob at either end
constexpr qreal kTrackTop = 9.0;
constexpr qreal kTrackHeight = 10.0;
constexpr qreal kLaneTop = 30.0;
constexpr qreal kLaneHeight = 26.0;
constexpr qreal kEdgeGrab = 5.0;   // how close to a trim's edge counts as grabbing it
constexpr qreal kDrawSlop = 4.0;   // a shorter drag in the lane is treated as a click

} // namespace

TrimSlider::TrimSlider(QWidget *parent) : QWidget(parent)
{
    setMouseTracking(true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

QSize TrimSlider::sizeHint() const
{
    return QSize(400, int(kLaneTop + kLaneHeight + 6));
}

void TrimSlider::setDuration(qint64 ms)
{
    m_duration = std::max<qint64>(0, ms);
    update();
}

void TrimSlider::setPosition(qint64 ms)
{
    ms = std::clamp<qint64>(ms, 0, m_duration);
    if (ms == m_position)
        return;
    m_position = ms;
    update();
}

void TrimSlider::setCuts(const QList<Cut> &cuts, int selectedId)
{
    m_cuts = cuts;
    m_selected = selectedId;
    update();
}

void TrimSlider::setMark(qint64 ms)
{
    m_mark = ms;
    update();
}

QRectF TrimSlider::trackRect() const
{
    return QRectF(kPad, kTrackTop, width() - 2 * kPad, kTrackHeight);
}

QRectF TrimSlider::laneRect() const
{
    return QRectF(kPad, kLaneTop, width() - 2 * kPad, kLaneHeight);
}

qreal TrimSlider::xFor(qint64 ms) const
{
    if (m_duration <= 0)
        return kPad;
    return kPad + (width() - 2 * kPad) * qreal(ms) / qreal(m_duration);
}

qint64 TrimSlider::msFor(qreal x) const
{
    const qreal span = width() - 2 * kPad;
    if (m_duration <= 0 || span <= 0)
        return 0;
    return std::clamp<qint64>(std::llround((x - kPad) / span * m_duration), 0, m_duration);
}

const Cut *TrimSlider::cut(int id) const
{
    const auto it = std::find_if(m_cuts.begin(), m_cuts.end(), [id](const Cut &c) { return c.id == id; });
    return it == m_cuts.end() ? nullptr : &*it;
}

TrimSlider::Hit TrimSlider::hitTest(const QPointF &pos) const
{
    if (pos.y() < kLaneTop - 4)
        return {Drag::Playhead, 0};

    // The nearest edge within reach wins, so the edges of two trims that
    // touch can still be told apart.
    Hit hit{Drag::Draw, 0};
    qreal best = kEdgeGrab;
    for (const Cut &c : m_cuts) {
        const qreal xs = xFor(c.start), xe = xFor(c.end);
        if (pos.x() > xs && pos.x() < xe && hit.what == Drag::Draw)
            hit = {Drag::Move, c.id};
        // On a narrow block only the outer halves of the edge zones count,
        // leaving its middle free to be grabbed for moving.
        const qreal inner = std::min(kEdgeGrab, (xe - xs) / 3);
        const qreal ds = pos.x() - xs, de = xe - pos.x();
        if (ds >= -kEdgeGrab && ds <= inner && std::abs(ds) <= best) {
            best = std::abs(ds);
            hit = {Drag::Start, c.id};
        }
        if (de >= -kEdgeGrab && de <= inner && std::abs(de) <= best) {
            best = std::abs(de);
            hit = {Drag::End, c.id};
        }
    }
    return hit;
}

void TrimSlider::dragTo(qreal x)
{
    const qint64 ms = msFor(x);
    switch (m_drag.what) {
    case Drag::Playhead:
        emit seekRequested(ms);
        break;
    case Drag::Draw:
        m_drawTo = ms;
        update();
        emit seekRequested(ms);
        break;
    // For the rest the owner decides where the trim may go; follow the
    // result with the playhead so the frame at the edge is on screen.
    case Drag::Start:
        emit startDragged(m_drag.id, ms);
        if (const Cut *c = cut(m_drag.id))
            emit seekRequested(c->start);
        break;
    case Drag::End:
        emit endDragged(m_drag.id, ms);
        if (const Cut *c = cut(m_drag.id))
            emit seekRequested(c->end);
        break;
    case Drag::Move:
        emit cutDragged(m_drag.id, ms - m_anchor);
        if (const Cut *c = cut(m_drag.id))
            emit seekRequested(c->start);
        break;
    case Drag::None:
        break;
    }
}

void TrimSlider::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton || m_duration <= 0)
        return;
    const qreal x = event->position().x();
    m_drag = hitTest(event->position());
    m_pressX = x;
    const Cut *c = cut(m_drag.id);
    switch (m_drag.what) {
    case Drag::Playhead:
        dragTo(x);
        break;
    case Drag::Draw:
        m_anchor = m_drawTo = msFor(x);
        break;
    case Drag::Start:
    case Drag::Move:
        m_anchor = msFor(x) - c->start;
        emit cutSelected(c->id);
        emit seekRequested(c->start);
        break;
    case Drag::End:
        emit cutSelected(c->id);
        emit seekRequested(c->end);
        break;
    case Drag::None:
        break;
    }
}

void TrimSlider::mouseMoveEvent(QMouseEvent *event)
{
    if (m_drag.what != Drag::None) {
        dragTo(event->position().x());
        return;
    }
    if (m_duration <= 0) {
        unsetCursor();
        return;
    }
    switch (hitTest(event->position()).what) {
    case Drag::Start:
    case Drag::End:
        setCursor(Qt::SizeHorCursor);
        break;
    case Drag::Move:
        setCursor(Qt::OpenHandCursor);
        break;
    case Drag::Draw:
        setCursor(Qt::CrossCursor);
        break;
    default:
        unsetCursor();
    }
}

void TrimSlider::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton)
        return;
    const Hit drag = m_drag;
    m_drag = {};
    if (drag.what != Drag::Draw)
        return;
    update();
    if (std::abs(event->position().x() - m_pressX) >= kDrawSlop) {
        emit cutDrawn(std::min(m_anchor, m_drawTo), std::max(m_anchor, m_drawTo));
    } else {
        // A click on an empty part of the lane drops the selection and
        // moves the playhead there.
        emit cutSelected(0);
        emit seekRequested(m_anchor);
    }
}

void TrimSlider::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QPalette &pal = palette();
    const QColor ink = pal.color(QPalette::WindowText);
    const QColor paper = pal.color(QPalette::Window);
    QColor groove = ink;
    groove.setAlphaF(0.22f);
    QColor faint = ink;
    faint.setAlphaF(0.10f);

    const QRectF track = trackRect();
    const QRectF lane = laneRect();
    const bool live = m_duration > 0 && isEnabled();

    p.setPen(Qt::NoPen);
    p.setBrush(live ? pal.color(QPalette::Active, QPalette::Highlight) : groove);
    p.drawRoundedRect(track, kTrackHeight / 2, kTrackHeight / 2);
    p.setBrush(faint);
    p.drawRoundedRect(lane, 4, 4);
    if (!live)
        return;

    // The trims: a block in the lane, and the same stretch knocked out of
    // the bar above so it reads as "not part of the result".
    for (const Cut &c : m_cuts) {
        const qreal xs = xFor(c.start), xe = xFor(c.end);
        const QColor color = CutList::color(c.id);
        p.setPen(Qt::NoPen);
        p.setBrush(paper);
        p.drawRect(QRectF(xs, track.top() - 1, xe - xs, track.height() + 2));
        QColor tint = color;
        tint.setAlphaF(0.45f);
        p.setBrush(tint);
        p.drawRect(QRectF(xs, track.top(), xe - xs, track.height()));

        const QRectF block(xs, lane.top(), xe - xs, lane.height());
        p.setBrush(color);
        p.drawRoundedRect(block, 3, 3);
        if (c.id == m_selected) {
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(ink, 2));
            p.drawRoundedRect(block.adjusted(1, 1, -1, -1), 3, 3);
        }
    }
    // Numbered in time order, matching the list beside the video.
    QFont bold = font();
    bold.setBold(true);
    p.setFont(bold);
    p.setPen(Qt::white);
    for (int i = 0; i < m_cuts.size(); ++i) {
        const QRectF block(xFor(m_cuts[i].start), lane.top(), xFor(m_cuts[i].end) - xFor(m_cuts[i].start),
                           lane.height());
        const QString number = QString::number(i + 1);
        if (block.width() >= p.fontMetrics().horizontalAdvance(number) + 6)
            p.drawText(block, Qt::AlignCenter, number);
    }

    if (m_drag.what == Drag::Draw && m_drawTo != m_anchor) {
        QColor ghost = ink;
        ghost.setAlphaF(0.35f);
        p.setPen(QPen(ink, 1, Qt::DashLine));
        p.setBrush(ghost);
        const qreal a = xFor(std::min(m_anchor, m_drawTo)), b = xFor(std::max(m_anchor, m_drawTo));
        p.drawRoundedRect(QRectF(a, lane.top(), b - a, lane.height()), 3, 3);
    } else if (m_cuts.isEmpty() && m_mark < 0) {
        QColor hint = ink;
        hint.setAlphaF(0.55f);
        p.setFont(font());
        p.setPen(hint);
        p.drawText(lane, Qt::AlignCenter, tr("Drag here to mark a section to cut out"));
    }

    // A trim that has a start but no end yet.
    if (m_mark >= 0) {
        const qreal x = xFor(m_mark);
        p.setPen(QPen(ink, 1.5, Qt::DashLine));
        p.drawLine(QPointF(x, lane.top()), QPointF(x, lane.bottom()));
        p.setPen(Qt::NoPen);
        p.setBrush(ink);
        p.drawPolygon(QPolygonF({QPointF(x, lane.top()), QPointF(x + 7, lane.top() + 5), QPointF(x, lane.top() + 10)}));
    }

    // Playhead, across both rows.
    const qreal xp = xFor(m_position);
    p.setPen(QPen(paper, 1));
    p.setBrush(ink);
    p.drawRect(QRectF(xp - 1.5, 2, 3, height() - 4));
    p.drawEllipse(QPointF(xp, 5), 4.5, 4.5);
}
