// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick

// Programs' notifications, in the top right corner for a few seconds. A click puts one away.
Column {
    property var comp
    anchors.right: parent.right; anchors.rightMargin: 12; y: 48
    spacing: 8
    Repeater {
        model: notifications.shown
        Rectangle {
            width: 340; height: text.height + 24; radius: 12
            color: comp.pal.surface; border.color: comp.pal.line
            Image { id: icon; x: 12; y: 12; width: 28; height: 28; sourceSize: Qt.size(56, 56)
                source: "image://icon/" + (modelData.icon.length > 0 && modelData.icon.indexOf("/") < 0 ? modelData.icon : "dialog-information") }
            Column { id: text; x: 52; y: 12; width: parent.width - 64; spacing: 2
                Text { width: parent.width; text: modelData.summary; color: comp.pal.text; font.family: comp.sans; font.pixelSize: 13; font.weight: Font.DemiBold
                    wrapMode: Text.WordWrap; textFormat: Text.PlainText }
                Text { width: parent.width; visible: text.length > 0; text: modelData.body; color: comp.pal.dim; font.family: comp.sans; font.pixelSize: 12
                    wrapMode: Text.WordWrap; textFormat: Text.PlainText; maximumLineCount: 4; elide: Text.ElideRight } }
            MouseArea { anchors.fill: parent; onClicked: notifications.dismiss(modelData.id) }
        }
    }
}
