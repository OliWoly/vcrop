#include "Cuts.h"

#include <algorithm>

void CutList::setDuration(qint64 ms)
{
    m_duration = qMax<qint64>(0, ms);
    for (auto it = m_cuts.begin(); it != m_cuts.end();) {
        it->end = qMin(it->end, m_duration);
        if (it->end - it->start < kMinLength)
            it = m_cuts.erase(it);
        else
            ++it;
    }
}

int CutList::indexOf(int id) const
{
    for (int i = 0; i < m_cuts.size(); ++i) {
        if (m_cuts[i].id == id)
            return i;
    }
    return -1;
}

const Cut *CutList::find(int id) const
{
    const int i = indexOf(id);
    return i < 0 ? nullptr : &m_cuts[i];
}

int CutList::add(qint64 start, qint64 end)
{
    start = qBound<qint64>(0, start, m_duration);
    end = qBound<qint64>(0, end, m_duration);
    if (end - start < kMinLength)
        return 0;

    // Whatever the new trim overlaps becomes part of it, keeping the identity
    // (and so the colour) of the earliest one.
    int id = 0;
    for (auto it = m_cuts.begin(); it != m_cuts.end();) {
        if (it->start < end && it->end > start) {
            start = qMin(start, it->start);
            end = qMax(end, it->end);
            if (!id)
                id = it->id;
            it = m_cuts.erase(it);
        } else {
            ++it;
        }
    }
    if (!id)
        id = ++m_lastId;
    const auto after = std::find_if(m_cuts.begin(), m_cuts.end(), [start](const Cut &c) { return c.start > start; });
    m_cuts.insert(after, Cut{id, start, end});
    return id;
}

void CutList::setStart(int id, qint64 start)
{
    const int i = indexOf(id);
    if (i < 0)
        return;
    const qint64 low = i > 0 ? m_cuts[i - 1].end : 0;
    m_cuts[i].start = qBound(low, start, m_cuts[i].end - kMinLength);
}

void CutList::setEnd(int id, qint64 end)
{
    const int i = indexOf(id);
    if (i < 0)
        return;
    const qint64 high = i + 1 < m_cuts.size() ? m_cuts[i + 1].start : m_duration;
    m_cuts[i].end = qBound(m_cuts[i].start + kMinLength, end, high);
}

void CutList::move(int id, qint64 start)
{
    const int i = indexOf(id);
    if (i < 0)
        return;
    const qint64 length = m_cuts[i].end - m_cuts[i].start;
    const qint64 low = i > 0 ? m_cuts[i - 1].end : 0;
    const qint64 high = i + 1 < m_cuts.size() ? m_cuts[i + 1].start : m_duration;
    m_cuts[i].start = qBound(low, start, high - length);
    m_cuts[i].end = m_cuts[i].start + length;
}

void CutList::remove(int id)
{
    const int i = indexOf(id);
    if (i >= 0)
        m_cuts.removeAt(i);
}

const Cut *CutList::entered(qint64 from, qint64 to) const
{
    for (const Cut &c : m_cuts) {
        if (from < c.start && to >= c.start && to < c.end)
            return &c;
    }
    return nullptr;
}

qint64 CutList::removed() const
{
    qint64 total = 0;
    for (const Cut &c : m_cuts)
        total += c.end - c.start;
    return total;
}

QList<KeepRange> CutList::kept() const
{
    QList<KeepRange> ranges;
    qint64 position = 0;
    int number = 0;
    for (const Cut &c : m_cuts) {
        if (c.start > position)
            ranges.append(KeepRange{position, c.start, number});
        position = c.end;
        ++number;
    }
    if (position < m_duration)
        ranges.append(KeepRange{position, -1, number});
    return ranges;
}

QColor CutList::color(int id)
{
    static const QColor palette[] = {
        QColor(0xe5, 0x48, 0x4d), // red
        QColor(0x00, 0x90, 0xff), // blue
        QColor(0xff, 0xb2, 0x24), // amber
        QColor(0x46, 0xa7, 0x58), // green
        QColor(0x8e, 0x4e, 0xc6), // purple
        QColor(0xf7, 0x6b, 0x15), // orange
        QColor(0x12, 0xa5, 0x94), // teal
        QColor(0xe9, 0x3d, 0x82), // pink
    };
    return palette[(qMax(id, 1) - 1) % int(std::size(palette))];
}
