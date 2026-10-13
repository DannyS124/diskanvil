// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick

// Everything, in a grid over the screen: the dock's tools and the rest.
Rectangle {
    id: grid
    objectName: "grid"
    property var comp
    visible: comp.gridOpen
    anchors.fill: parent
    color: "#d9060a16"
    MouseArea { anchors.fill: parent; onClicked: comp.gridOpen = false }
    Keys.onEscapePressed: comp.gridOpen = false
    onVisibleChanged: if (visible) forceActiveFocus()

    Column {
        anchors.horizontalCenter: parent.horizontalCenter; y: Math.max(60, (parent.height - height) / 2 - 40); spacing: 28
        Text { anchors.horizontalCenter: parent.horizontalCenter; text: "Everything on DiskAnvil Live"; color: comp.pal.text; font.family: comp.sans; font.pixelSize: 26; font.weight: Font.Bold }
        Grid {
            anchors.horizontalCenter: parent.horizontalCenter
            columns: Math.max(3, Math.min(7, Math.floor((grid.width - 120) / 150))); spacing: 18
            Repeater {
                model: comp.dockItems.concat(comp.programs, comp.more)
                Rectangle {
                    width: 132; height: 120; radius: 14
                    color: cell.containsMouse ? comp.pal.raised : "transparent"; border.color: cell.containsMouse ? comp.pal.line : "transparent"
                    Column { anchors.centerIn: parent; spacing: 8; width: parent.width - 16
                        Image { anchors.horizontalCenter: parent.horizontalCenter; width: 52; height: 52; sourceSize: Qt.size(104, 104); source: "image://icon/" + modelData.icon }
                        Text { width: parent.width; horizontalAlignment: Text.AlignHCenter; text: modelData.title; color: comp.pal.text; font.family: comp.sans; font.pixelSize: 13; wrapMode: Text.WordWrap } }
                    MouseArea { id: cell; anchors.fill: parent; hoverEnabled: true; onClicked: { comp.gridOpen = false; comp.launch(modelData) } }
                }
            }
        }
    }
}
