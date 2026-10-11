// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Turns a command's raw output into plain text for the add-on output window. UTF-8 is
// decoded across reads (a character can be split between two), terminal escape codes and
// invisible characters are dropped, "\r" starts the line over like in a terminal (that's how
// progress bars redraw), and very long lines are cut short.

#include <QByteArray>
#include <QString>
#include <QStringDecoder>
#include <QStringList>

class OutputFilter
{
public:
    static constexpr int kMaxLine = 1000;

    // Feeds more output; returns the lines it finished.
    QStringList feed(const QByteArray &bytes);
    // The line still being written, like a progress bar.
    const QString &current() const { return m_line; }

private:
    void put(char32_t c, QStringList *done);

    enum class State { Text, Escape, Csi, String, StringEscape };
    QStringDecoder m_decoder{QStringDecoder::Utf8};
    State m_state = State::Text;
    int m_stringLength = 0;
    QString m_line;
    bool m_restart = false; // after "\r": the next character starts the line over
    bool m_cut = false;
};
