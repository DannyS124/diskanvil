// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "input.h"

#include <QGuiApplication>
#include <QPointingDevice>
#include <QScreen>
#include <QSocketNotifier>
#include <QWindow>
#include <qpa/qwindowsysteminterface.h>
#include <QtGui/private/qxkbcommon_p.h>

#include <fcntl.h>
#include <libinput.h>
#include <libudev.h>
#include <linux/input-event-codes.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon.h>

#include <cmath>

namespace {

int openRestricted(const char *path, int flags, void *)
{
    const int fd = ::open(path, flags | O_CLOEXEC);
    return fd < 0 ? -errno : fd;
}

void closeRestricted(int fd, void *)
{
    ::close(fd);
}

const libinput_interface kInterface = {openRestricted, closeRestricted};

Qt::MouseButton button(uint32_t code)
{
    switch (code) {
    case BTN_LEFT: return Qt::LeftButton;
    case BTN_RIGHT: return Qt::RightButton;
    case BTN_MIDDLE: return Qt::MiddleButton;
    case BTN_SIDE: return Qt::BackButton;
    case BTN_EXTRA: return Qt::ForwardButton;
    default: return Qt::NoButton;
    }
}

} // namespace

Input::Input(QWindow *window, QObject *parent)
    : QObject(parent)
    , m_window(window)
{
    udev *u = udev_new();
    if (!u)
        return;
    m_li = libinput_udev_create_context(&kInterface, this, u);
    udev_unref(u);
    if (!m_li || libinput_udev_assign_seat(m_li, "seat0") != 0) {
        if (m_li)
            libinput_unref(m_li);
        m_li = nullptr;
        return;
    }
    m_xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    setLayout(qEnvironmentVariable("XKB_DEFAULT_LAYOUT"), qEnvironmentVariable("XKB_DEFAULT_VARIANT"));
    const QRect screen = window->screen() ? window->screen()->geometry() : QRect(0, 0, 1024, 768);
    m_pos = screen.center();
    m_notifier = new QSocketNotifier(libinput_get_fd(m_li), QSocketNotifier::Read, this);
    connect(m_notifier, &QSocketNotifier::activated, this, &Input::read);
    read();
}

Input::~Input()
{
    if (m_state)
        xkb_state_unref(m_state);
    if (m_keymap)
        xkb_keymap_unref(m_keymap);
    if (m_xkb)
        xkb_context_unref(m_xkb);
    if (m_li)
        libinput_unref(m_li);
}

void Input::setLayout(const QString &layout, const QString &variant)
{
    if (!m_xkb)
        return;
    const QByteArray l = layout.isEmpty() ? QByteArrayLiteral("us") : layout.toUtf8(), v = variant.toUtf8();
    const xkb_rule_names names = {nullptr, nullptr, l.constData(), v.isEmpty() ? nullptr : v.constData(), nullptr};
    xkb_keymap *keymap = xkb_keymap_new_from_names(m_xkb, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (!keymap)
        return;
    if (m_state)
        xkb_state_unref(m_state);
    if (m_keymap)
        xkb_keymap_unref(m_keymap);
    m_keymap = keymap;
    m_state = xkb_state_new(m_keymap);
}

void Input::read()
{
    libinput_dispatch(m_li);
    while (libinput_event *event = libinput_get_event(m_li)) {
        handle(event);
        libinput_event_destroy(event);
    }
}

void Input::deviceAdded(libinput_event *event)
{
    libinput_device *device = libinput_event_get_device(event);
    // A touchpad: a tap is a click (two fingers a right click), two fingers scroll, and it's
    // paused while typing so a palm doesn't move the pointer.
    if (libinput_device_config_tap_get_finger_count(device) > 0) {
        libinput_device_config_tap_set_enabled(device, LIBINPUT_CONFIG_TAP_ENABLED);
        libinput_device_config_tap_set_drag_enabled(device, LIBINPUT_CONFIG_DRAG_ENABLED);
    }
    if (libinput_device_config_scroll_get_methods(device) & LIBINPUT_CONFIG_SCROLL_2FG)
        libinput_device_config_scroll_set_method(device, LIBINPUT_CONFIG_SCROLL_2FG);
    if (libinput_device_config_dwt_is_available(device))
        libinput_device_config_dwt_set_enabled(device, LIBINPUT_CONFIG_DWT_ENABLED);
    if (libinput_device_config_middle_emulation_is_available(device))
        libinput_device_config_middle_emulation_set_enabled(device, LIBINPUT_CONFIG_MIDDLE_EMULATION_ENABLED);
}

void Input::movedTo(QPointF at)
{
    const QRect screen = m_window->geometry();
    m_pos = QPointF(std::clamp(at.x(), qreal(screen.left()), qreal(screen.right())), std::clamp(at.y(), qreal(screen.top()), qreal(screen.bottom())));
    QWindowSystemInterface::handleMouseEvent(m_window, m_pos - screen.topLeft(), m_pos, m_buttons, Qt::NoButton, QEvent::MouseMove,
                                             QXkbCommon::modifiers(m_state));
}

void Input::handle(libinput_event *event)
{
    const QRect screen = m_window->geometry();
    const Qt::KeyboardModifiers mods = m_state ? QXkbCommon::modifiers(m_state) : Qt::NoModifier;
    switch (libinput_event_get_type(event)) {
    case LIBINPUT_EVENT_DEVICE_ADDED:
        deviceAdded(event);
        break;
    case LIBINPUT_EVENT_POINTER_MOTION: {
        libinput_event_pointer *p = libinput_event_get_pointer_event(event);
        movedTo(m_pos + QPointF(libinput_event_pointer_get_dx(p), libinput_event_pointer_get_dy(p)));
        break;
    }
    case LIBINPUT_EVENT_POINTER_MOTION_ABSOLUTE: { // a tablet, or a virtual machine's pointer
        libinput_event_pointer *p = libinput_event_get_pointer_event(event);
        movedTo(QPointF(screen.left() + libinput_event_pointer_get_absolute_x_transformed(p, uint32_t(screen.width())),
                        screen.top() + libinput_event_pointer_get_absolute_y_transformed(p, uint32_t(screen.height()))));
        break;
    }
    case LIBINPUT_EVENT_POINTER_BUTTON: {
        libinput_event_pointer *p = libinput_event_get_pointer_event(event);
        const Qt::MouseButton b = button(libinput_event_pointer_get_button(p));
        if (b == Qt::NoButton)
            break;
        const bool down = libinput_event_pointer_get_button_state(p) == LIBINPUT_BUTTON_STATE_PRESSED;
        m_buttons.setFlag(b, down);
        QWindowSystemInterface::handleMouseEvent(m_window, m_pos - screen.topLeft(), m_pos, m_buttons, b,
                                                 down ? QEvent::MouseButtonPress : QEvent::MouseButtonRelease, mods);
        break;
    }
    case LIBINPUT_EVENT_POINTER_SCROLL_WHEEL:
    case LIBINPUT_EVENT_POINTER_SCROLL_FINGER:
    case LIBINPUT_EVENT_POINTER_SCROLL_CONTINUOUS: {
        libinput_event_pointer *p = libinput_event_get_pointer_event(event);
        const bool wheel = libinput_event_get_type(event) == LIBINPUT_EVENT_POINTER_SCROLL_WHEEL;
        QPointF pixels, steps; // libinput: down and right are positive; Qt's wheel: up and left are
        for (const auto axis : {LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL, LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL}) {
            if (!libinput_event_pointer_has_axis(p, axis))
                continue;
            const double value = wheel ? libinput_event_pointer_get_scroll_value_v120(p, axis) : libinput_event_pointer_get_scroll_value(p, axis);
            // A wheel's notch is 120; fingers move in pixels, about 20 of them to a notch's worth.
            const double angle = wheel ? -value : -value * 6;
            (axis == LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL ? steps.ry() : steps.rx()) = angle;
            if (!wheel)
                (axis == LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL ? pixels.ry() : pixels.rx()) = -value;
        }
        if (steps.isNull())
            break;
        QWindowSystemInterface::handleWheelEvent(m_window, m_pos - screen.topLeft(), m_pos, pixels.toPoint(), steps.toPoint(), mods);
        break;
    }
    case LIBINPUT_EVENT_KEYBOARD_KEY: {
        if (!m_state)
            break;
        libinput_event_keyboard *k = libinput_event_get_keyboard_event(event);
        const xkb_keycode_t code = libinput_event_keyboard_get_key(k) + 8;
        const bool down = libinput_event_keyboard_get_key_state(k) == LIBINPUT_KEY_STATE_PRESSED;
        const xkb_keysym_t sym = xkb_state_key_get_one_sym(m_state, code);
        const Qt::KeyboardModifiers before = QXkbCommon::modifiers(m_state, sym);
        const int key = QXkbCommon::keysymToQtKey(sym, before, m_state, code);
        const QString text = down ? QXkbCommon::lookupString(m_state, code) : QString();
        xkb_state_update_key(m_state, code, down ? XKB_KEY_DOWN : XKB_KEY_UP);
        // The keyboard's own code goes with it (+8, as Wayland has it): DiskAnvil Live's
        // programs get their keys through it.
        QWindowSystemInterface::handleExtendedKeyEvent(m_window, down ? QEvent::KeyPress : QEvent::KeyRelease, key, before, code, sym,
                                                       quint32(QXkbCommon::modifiers(m_state).toInt()), text);
        break;
    }
    default:
        break;
    }
}
