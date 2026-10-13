// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QObject>
#include <QPointF>

class QSocketNotifier;
class QWindow;
struct libinput;
struct libinput_event;
struct xkb_context;
struct xkb_keymap;
struct xkb_state;

// The mouse, touchpad and keyboard, read from libinput by the shell itself when it draws straight
// to the screen. Qt's own reading (Debian's Qt 6.8) knows only a mouse wheel: a touchpad's two-
// finger scrolling never arrived, and tapping to click was off. Here a touchpad taps to click,
// scrolls with two fingers, and pauses while typing; the keyboard has the layout DiskAnvil Live
// was set to (Qt's own key names and text).
class Input : public QObject
{
    Q_OBJECT
public:
    explicit Input(QWindow *window, QObject *parent = nullptr);
    ~Input() override;
    bool started() const { return m_li != nullptr; }
    void setLayout(const QString &layout, const QString &variant);

private:
    void read();
    void handle(libinput_event *event);
    void deviceAdded(libinput_event *event);
    void movedTo(QPointF at);

    QWindow *m_window;
    libinput *m_li = nullptr;
    QSocketNotifier *m_notifier = nullptr;
    xkb_context *m_xkb = nullptr;
    xkb_keymap *m_keymap = nullptr;
    xkb_state *m_state = nullptr;
    QPointF m_pos;
    Qt::MouseButtons m_buttons;
    QPointF m_scrollRest; // finger scrolling under a whole step, kept for the next
};
