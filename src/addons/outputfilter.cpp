// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "outputfilter.h"

QStringList OutputFilter::feed(const QByteArray &bytes)
{
    QStringList done;
    const QString text = m_decoder.decode(bytes);
    for (const char32_t c : text.toUcs4())
        put(c, &done);
    return done;
}

void OutputFilter::put(char32_t c, QStringList *done)
{
    switch (m_state) {
    case State::Escape:
        // ESC [ starts a control sequence; ESC ] ESC P ESC X ESC ^ ESC _ start a string that
        // runs to BEL or ESC \. Anything else is a two-character code.
        m_state = c == U'[' ? State::Csi
            : (c == U']' || c == U'P' || c == U'X' || c == U'^' || c == U'_') ? State::String
                                                                                : State::Text;
        m_stringLength = 0;
        return;
    case State::Csi:
        // Parameters and intermediate bytes, then one final byte.
        if (c >= 0x20 && c <= 0x3f)
            return;
        m_state = State::Text;
        if (c >= 0x40 && c <= 0x7e)
            return;
        break; // something odd inside the sequence: treat it as text
    case State::String:
        if (c == 0x07 || ++m_stringLength > 4096) // BEL ends it; don't swallow everything forever
            m_state = State::Text;
        else if (c == 0x1b)
            m_state = State::StringEscape;
        return;
    case State::StringEscape:
        m_state = c == U'\\' ? State::Text : State::String;
        return;
    case State::Text:
        break;
    }

    switch (c) {
    case 0x1b:
        m_state = State::Escape;
        return;
    case U'\n':
        *done << m_line;
        m_line.clear();
        m_restart = false;
        m_cut = false;
        return;
    case U'\r':
        m_restart = true;
        return;
    case U'\b':
        m_line.chop(!m_line.isEmpty() && m_line.back().isLowSurrogate() ? 2 : 1);
        return;
    case U'\t':
        // To the next column of 8, like a terminal, so columns stay lined up.
        if (m_restart) {
            m_line.clear();
            m_restart = false;
        }
        do
            m_line += QLatin1Char(' ');
        while (m_line.size() % 8 != 0 && m_line.size() < kMaxLine);
        return;
    default:
        break;
    }
    switch (QChar::category(c)) {
    case QChar::Other_Control:
    case QChar::Other_Format: // zero-width characters, text direction overrides
    case QChar::Other_Surrogate:
    case QChar::Separator_Line:
    case QChar::Separator_Paragraph:
        return;
    default:
        break;
    }
    if (m_restart) {
        m_line.clear();
        m_restart = false;
        m_cut = false;
    }
    if (m_line.size() < kMaxLine) {
        m_line += QString::fromUcs4(&c, 1);
    } else if (!m_cut) {
        m_line += QChar(0x2026); // …
        m_cut = true;
    }
}
