#pragma once

#include <QString>
#include <QtGlobal>

// Formats a millisecond position as HH:MM:SS.mmm.
inline QString formatTime(qint64 ms)
{
    ms = qMax<qint64>(0, ms);
    return QStringLiteral("%1:%2:%3.%4")
        .arg(ms / 3600000, 2, 10, QLatin1Char('0'))
        .arg(ms / 60000 % 60, 2, 10, QLatin1Char('0'))
        .arg(ms / 1000 % 60, 2, 10, QLatin1Char('0'))
        .arg(ms % 1000, 3, 10, QLatin1Char('0'));
}
