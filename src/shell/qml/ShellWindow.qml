// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtWayland.Compositor
import QtWayland.Compositor.XdgShell

// A window: our frame round what the program draws. Dragged by its title, sized from its edges,
// maximised with a double-click or its button.
Item {
    id: win
    property var shellSurface
    property var toplevel
    property var comp
    property Item area
    readonly property int bar: 34
    readonly property string title: toplevel ? toplevel.title : ""
    readonly property string appId: toplevel ? toplevel.appId : ""
    readonly property var parentWindow: toplevel && toplevel.parentToplevel ? comp.windows.find(w => w.toplevel === toplevel.parentToplevel) : null
    property real pid: shellSurface && shellSurface.surface && shellSurface.surface.client ? shellSurface.surface.client.processId : 0
    property bool maximized: false
    property rect before
    readonly property bool focused: comp.active === win
    readonly property string icon: appId.indexOf("terminal") >= 0 ? "utilities-terminal"
                                 : appId.indexOf("firefox") >= 0 ? "firefox-esr"
                                 : appId.indexOf("diskanvil") >= 0 || title.indexOf("DiskAnvil") >= 0 ? "diskanvil-live-drives" : "application-x-executable"
    width: surf.width; height: surf.height + bar
    visible: false

    function takeFocus() { surf.takeFocus() }
    function close() { if (toplevel) toplevel.sendClose() }
    function minimise() { comp.hide(win) }
    function toggleMaximise() {
        if (!toplevel) return
        if (maximized) {
            maximized = false
            x = before.x; y = before.y
            toplevel.sendUnmaximized(Qt.size(before.width, before.height - bar))
        } else {
            before = Qt.rect(x, y, width, height)
            maximized = true
            x = area.x; y = area.y
            toplevel.sendMaximized(Qt.size(area.width, area.height - bar))
        }
    }
    // A new window: over the one it belongs to, or in the middle; never bigger than the room.
    function placeFirst() {
        const w = surf.width, h = surf.height + bar
        if (w > area.width || h > area.height) {
            before = Qt.rect(area.x + 40, area.y + 30, Math.min(w, area.width - 80), Math.min(h, area.height - 60))
            maximized = true
            x = area.x; y = area.y
            toplevel.sendMaximized(Qt.size(area.width, area.height - bar))
        } else if (parentWindow) {
            x = parentWindow.x + (parentWindow.width - w) / 2
            y = Math.max(area.y, parentWindow.y + Math.min(60, (parentWindow.height - h) / 2))
        } else {
            const n = comp.windows.filter(o => o !== win && o.visible && !o.parentWindow).length
            x = area.x + (area.width - w) / 2 + n * 28
            y = area.y + Math.max(0, (area.height - h) / 2) + n * 28
        }
        x = Math.max(area.x, Math.min(x, area.x + area.width - w))
        y = Math.max(area.y, Math.min(y, area.y + area.height - h))
        visible = true
        comp.track(win)
    }

    Connections {
        target: win.toplevel
        function onMaximizedChanged() { if (win.toplevel.maximized !== win.maximized) win.toggleMaximise() }
        function onSetMinimized() { win.minimise() }
        function onSetMaximized() { if (!win.maximized) win.toggleMaximise() }
        function onUnsetMaximized() { if (win.maximized) win.toggleMaximise() }
    }

    // The edge and its shadow.
    Rectangle { anchors.fill: parent; anchors.margins: -1; radius: 9; color: "transparent"; border.width: 1
        border.color: win.focused ? comp.pal.accent : comp.pal.line; visible: !win.maximized }

    Rectangle {
        id: titlebar
        width: parent.width; height: win.bar
        radius: win.maximized ? 0 : 8
        color: comp.pal.raised
        Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 8; color: parent.color }
        Rectangle { visible: win.focused; anchors.bottom: parent.bottom; width: parent.width; height: 2; color: comp.pal.accent; opacity: 0.8 }
        MouseArea {
            anchors.fill: parent
            drag.target: win.maximized ? null : win
            drag.minimumY: win.area.y; drag.maximumY: win.area.y + win.area.height - win.bar
            drag.minimumX: win.area.x - win.width + 120; drag.maximumX: win.area.x + win.area.width - 120
            onPressed: comp.raise(win)
            onDoubleClicked: win.toggleMaximise()
        }
        Image { id: badge; x: 10; anchors.verticalCenter: parent.verticalCenter; width: 18; height: 18; sourceSize: Qt.size(36, 36); source: "image://icon/" + win.icon }
        Text { anchors.left: badge.right; anchors.leftMargin: 8; anchors.right: buttons.left; anchors.verticalCenter: parent.verticalCenter
            text: win.title; color: comp.pal.text; font.family: comp.sans; font.pixelSize: 13; font.weight: Font.DemiBold; elide: Text.ElideRight }
        Row {
            id: buttons; anchors.right: parent.right; anchors.rightMargin: 6; anchors.verticalCenter: parent.verticalCenter; spacing: 2
            Repeater {
                model: [{ i: "window-minimize", tip: "minimise" }, { i: win.maximized ? "window-restore" : "window-maximize", tip: "maximise" }, { i: "window-close", tip: "close" }]
                Rectangle {
                    width: 30; height: 24; radius: 5
                    color: hover.containsMouse ? (index === 2 ? comp.pal.danger : comp.pal.line) : "transparent"
                    Image { anchors.centerIn: parent; width: 16; height: 16; sourceSize: Qt.size(32, 32); source: "image://icon/" + modelData.i }
                    MouseArea { id: hover; anchors.fill: parent; hoverEnabled: true
                        onClicked: index === 0 ? win.minimise() : index === 1 ? win.toggleMaximise() : win.close() }
                }
            }
        }
    }

    ShellSurfaceItem {
        id: surf
        y: win.bar
        shellSurface: win.shellSurface
        autoCreatePopupItems: true
        // The shell places windows itself (Qt's compositor would put a maximised one back at 0,0).
        moveItem: placeholder
        onSurfaceDestroyed: { comp.untrack(win); win.destroy() }
        Component.onCompleted: placeTimer.start()
        TapHandler { acceptedButtons: Qt.AllButtons; gesturePolicy: TapHandler.DragThreshold; grabPermissions: PointerHandler.TakeOverForbidden
            onPressedChanged: if (pressed) comp.raise(win) }
    }
    Item { id: placeholder; visible: false }
    // Placed once the program has said how big it wants to be.
    Timer { id: placeTimer; interval: 60; repeat: true; onTriggered: { if (surf.width > 0 && surf.height > 0) { stop(); win.placeFirst() } } }

    // Sizing from the edges and corners.
    component Edge: MouseArea {
        property int ex: 0; property int ey: 0
        property real startW; property real startH; property real startX; property real startY; property point at; property size asked
        visible: !win.maximized && win.visible
        cursorShape: ex !== 0 && ey !== 0 ? (ex * ey > 0 ? Qt.SizeFDiagCursor : Qt.SizeBDiagCursor) : ex !== 0 ? Qt.SizeHorCursor : Qt.SizeVerCursor
        onPressed: (m) => { comp.raise(win); at = mapToItem(win.parent, m.x, m.y); startW = surf.width; startH = surf.height; startX = win.x; startY = win.y }
        onPositionChanged: (m) => {
            const p = mapToItem(win.parent, m.x, m.y), dx = p.x - at.x, dy = p.y - at.y
            const w = Math.max(240, startW + ex * dx), h = Math.max(120, startH + ey * dy)
            if (ex < 0) win.x = startX + (startW - w)
            if (ey < 0) win.y = startY + (startH - h)
            asked = Qt.size(Math.round(w), Math.round(h))
            win.toplevel.sendResizing(asked)
        }
        // The last size asked for, now no longer being dragged (not the size it had a moment ago).
        onReleased: if (asked.width > 0) win.toplevel.sendConfigure(asked, [])
    }
    Edge { ex: 1; x: parent.width - 4; y: 8; width: 8; height: parent.height - 16 }
    Edge { ex: -1; x: -4; y: 8; width: 8; height: parent.height - 16 }
    Edge { ey: 1; x: 8; y: parent.height - 4; width: parent.width - 16; height: 8 }
    Edge { ex: 1; ey: 1; x: parent.width - 10; y: parent.height - 10; width: 14; height: 14 }
    Edge { ex: -1; ey: 1; x: -4; y: parent.height - 10; width: 14; height: 14 }
}
