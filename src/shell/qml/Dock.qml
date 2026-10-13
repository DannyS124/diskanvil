// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick

// Along the bottom: everything else, DiskAnvil's tools, then a terminal, files, and any other window
// that's open. A dot under what's running.
Rectangle {
    id: dock
    property var comp
    anchors.horizontalCenter: parent.horizontalCenter
    anchors.bottom: parent.bottom; anchors.bottomMargin: 14
    height: 78; width: row.width + 28; radius: 22
    color: "#e60d1426"; border.color: comp.pal.line

    component Item2: Item {
        property string icon
        property string tip
        property bool running: false
        property bool front: false
        signal clicked
        width: 58; height: 66
        Rectangle { anchors.horizontalCenter: parent.horizontalCenter; y: 2; width: 56; height: 56; radius: 14
            color: mouse.containsMouse ? comp.pal.raised : "transparent"
            Image { anchors.centerIn: parent; width: 44; height: 44; sourceSize: Qt.size(88, 88); source: "image://icon/" + icon } }
        Rectangle { visible: running; anchors.horizontalCenter: parent.horizontalCenter; y: 61; width: front ? 18 : 6; height: 4; radius: 2; color: comp.pal.accent }
        MouseArea { id: mouse; anchors.fill: parent; hoverEnabled: true; onClicked: parent.clicked() }
        // Its name, above it while the mouse is on it.
        Rectangle { visible: mouse.containsMouse && tip.length > 0; anchors.horizontalCenter: parent.horizontalCenter; y: -40
            width: tipText.implicitWidth + 20; height: 28; radius: 6; color: comp.pal.surface; border.color: comp.pal.line
            Text { id: tipText; anchors.centerIn: parent; text: tip; color: comp.pal.text; font.family: comp.sans; font.pixelSize: 12 } }
    }

    Row {
        id: row
        anchors.centerIn: parent; spacing: 4
        Item2 { objectName: "everything"; icon: "applications-all"; tip: "Everything"; onClicked: comp.gridOpen = !comp.gridOpen }
        Rectangle { width: 1; height: 48; color: comp.pal.line; anchors.verticalCenter: parent.verticalCenter }
        Repeater {
            model: comp.dockItems
            Item2 { icon: modelData.icon; tip: modelData.title; running: comp.itemRunning(modelData) && comp.windows.length >= 0
                front: comp.active && comp.active.pid === comp.started[modelData.id]; onClicked: comp.launch(modelData) }
        }
        Rectangle { width: 1; height: 48; color: comp.pal.line; anchors.verticalCenter: parent.verticalCenter }
        Repeater {
            model: comp.programs
            Item2 { icon: modelData.icon; tip: modelData.title; running: comp.itemRunning(modelData) && comp.windows.length >= 0
                front: comp.active && comp.active.pid === comp.started[modelData.id]; onClicked: comp.launch(modelData) }
        }
        // Windows not started from the dock (Firefox, a program a tool opened): there to get back to.
        Repeater {
            model: comp.windows.filter(w => !w.parentWindow && !Object.values(comp.started).includes(w.pid))
            Item2 { icon: modelData.icon; tip: modelData.title; running: true; front: comp.active === modelData
                onClicked: comp.active === modelData && modelData.visible ? comp.hide(modelData) : comp.raise(modelData) }
        }
    }
}
