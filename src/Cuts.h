#pragma once

#include "Exporter.h"

#include <QColor>
#include <QList>

// A trim: one section of the video that is cut out, in milliseconds.
struct Cut {
    int id = 0; // stable for the life of the trim; its colour follows it
    qint64 start = 0;
    qint64 end = 0;
};

// The trims of one video, kept sorted by time and never overlapping.
class CutList
{
public:
    // The shortest trim that can be made.
    static constexpr qint64 kMinLength = 100;

    const QList<Cut> &cuts() const { return m_cuts; }
    bool isEmpty() const { return m_cuts.isEmpty(); }

    // Trims reaching past the new duration are shortened or dropped.
    void setDuration(qint64 ms);
    void clear() { m_cuts.clear(); }

    // Adds a trim, absorbing any it overlaps. Returns its id, or 0 when the
    // range is too short to be one.
    int add(qint64 start, qint64 end);
    // These keep the trim between its neighbours and at least kMinLength long.
    void setStart(int id, qint64 start);
    void setEnd(int id, qint64 end);
    void move(int id, qint64 start);
    void remove(int id);

    const Cut *find(int id) const;
    // The trim that playback advancing from `from` to `to` has just run into.
    const Cut *entered(qint64 from, qint64 to) const;
    qint64 removed() const;
    // What is left of the video, for the exporter. Empty when nothing is.
    QList<KeepRange> kept() const;

    static QColor color(int id);

private:
    int indexOf(int id) const;

    QList<Cut> m_cuts;
    qint64 m_duration = 0;
    int m_lastId = 0;
};
