// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick

// Under the power button: shut down, restart, or restart into the PC's firmware settings.
Rectangle {
    id: menu
    objectName: "powerMenu"
    property var comp
    visible: comp.powerOpen
    anchors.right: parent.right; anchors.rightMargin: 10; y: 42
    width: 260; height: column.height + 20; radius: 12
    color: comp.pal.surface; border.color: comp.pal.line
    Column {
        id: column; x: 10; y: 10; width: parent.width - 20; spacing: 4
        Repeater {
            model: [{ how: "poweroff", icon: "system-shutdown", text: "Shut Down" }, { how: "reboot", icon: "system-reboot", text: "Restart" }]
                .concat(shell.firmwareSetupPossible() ? [{ how: "firmware", icon: "preferences-system", text: "Restart into Firmware Settings" }] : [])
            Rectangle {
                width: parent.width; height: 40; radius: 8; color: choice.containsMouse ? comp.pal.raised : "transparent"
                Image { x: 10; anchors.verticalCenter: parent.verticalCenter; width: 20; height: 20; sourceSize: Qt.size(40, 40); source: "image://icon/" + modelData.icon }
                Text { x: 40; anchors.verticalCenter: parent.verticalCenter; text: modelData.text; color: comp.pal.text; font.family: comp.sans; font.pixelSize: 13 }
                MouseArea { id: choice; anchors.fill: parent; hoverEnabled: true; onClicked: { comp.powerOpen = false; shell.power(modelData.how) } }
            }
        }
        Text { width: parent.width; topPadding: 6; text: "Anything still copying or wiping stops. Files you didn't save to a drive are lost."; color: comp.pal.dim
            font.family: comp.sans; font.pixelSize: 11; wrapMode: Text.WordWrap }
    }
}
