#pragma once

#include "Cuts.h"

#include <QWidget>

// The timeline. The bar on top scrubs through the video; the lane below it
// holds the trims, one coloured block per section that is cut out. Dragging
// in an empty part of the lane draws a new trim, dragging a block moves it
// and dragging its edges resizes it. All positions are in milliseconds.
class TrimSlider : public QWidget
{
    Q_OBJECT

public:
    explicit TrimSlider(QWidget *parent = nullptr);

    void setDuration(qint64 ms);
    void setPosition(qint64 ms);
    void setCuts(const QList<Cut> &cuts, int selectedId);
    // Where a trim was started with "trim from here"; negative for none.
    void setMark(qint64 ms);

    QSize sizeHint() const override;

signals:
    void seekRequested(qint64 ms);
    // The owner applies these to its list and answers with setCuts().
    void cutDrawn(qint64 start, qint64 end);
    void startDragged(int id, qint64 start);
    void endDragged(int id, qint64 end);
    void cutDragged(int id, qint64 start);
    void cutSelected(int id); // 0 when the selection is dropped

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;

private:
    enum class Drag { None, Playhead, Draw, Start, End, Move };
    struct Hit {
        Drag what = Drag::None;
        int id = 0;
    };

    QRectF trackRect() const;
    QRectF laneRect() const;
    qreal xFor(qint64 ms) const;
    qint64 msFor(qreal x) const;
    Hit hitTest(const QPointF &pos) const;
    const Cut *cut(int id) const;
    void dragTo(qreal x);

    qint64 m_duration = 0;
    qint64 m_position = 0;
    QList<Cut> m_cuts;
    int m_selected = 0;
    qint64 m_mark = -1;

    Hit m_drag;
    qreal m_pressX = 0;
    qint64 m_anchor = 0;  // Draw: where the drag began; Move: offset of the grab into the trim
    qint64 m_drawTo = 0;
};
