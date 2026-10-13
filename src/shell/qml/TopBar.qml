// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick

// Along the top: DiskAnvil Live and the window in front; on the right Wi-Fi, the time and power.
Rectangle {
    id: bar
    property var comp
    width: parent.width; height: 36
    color: "#e60a1020"
    property date now: new Date()
    Timer { interval: 5000; running: true; repeat: true; onTriggered: bar.now = new Date() }

    Row {
        x: 12; anchors.verticalCenter: parent.verticalCenter; spacing: 10
        Image { width: 20; height: 20; sourceSize: Qt.size(40, 40); source: "qrc:/shell/art/diskanvil-live-icon.svg"; anchors.verticalCenter: parent.verticalCenter }
        Text { text: "DiskAnvil Live"; color: comp.pal.text; font.family: comp.sans; font.pixelSize: 13; font.weight: Font.Bold; anchors.verticalCenter: parent.verticalCenter }
        Text { text: comp.active ? comp.active.title : ""; color: comp.pal.dim; font.family: comp.sans; font.pixelSize: 13; anchors.verticalCenter: parent.verticalCenter
            width: Math.min(implicitWidth, bar.width / 2); elide: Text.ElideRight }
    }
    Row {
        anchors.right: parent.right; anchors.rightMargin: 12; anchors.verticalCenter: parent.verticalCenter; spacing: 6
        component Button: Rectangle {
            property alias icon: img.source
            signal clicked
            width: 30; height: 28; radius: 6; color: area.containsMouse ? comp.pal.raised : "transparent"
            Image { id: img; anchors.centerIn: parent; width: 18; height: 18; sourceSize: Qt.size(36, 36) }
            MouseArea { id: area; anchors.fill: parent; hoverEnabled: true; onClicked: parent.clicked() }
        }
        Button { objectName: "wifiButton"; anchors.verticalCenter: parent.verticalCenter
            // Wi-Fi is joined in the terminal: diskanvil-live-wifi.
            onClicked: { comp.powerOpen = false; comp.launch(comp.more.find(m => m.id === "wifi")) }
            icon: "image://icon/" + (network.wired ? "network-wired"
                : network.joined.length > 0 ? "network-wireless-signal-" + (network.strength > 75 ? "excellent" : network.strength > 50 ? "good" : network.strength > 25 ? "ok" : "weak")
                : network.wifiPresent && network.wifiOn ? "network-wireless-disconnected" : "network-offline") }
        Text { text: Qt.formatDate(bar.now, "ddd d MMM") + "  " + Qt.formatTime(bar.now, "hh:mm"); color: comp.pal.text; font.family: comp.sans; font.pixelSize: 13; anchors.verticalCenter: parent.verticalCenter
            rightPadding: 4; leftPadding: 4 }
        Button { objectName: "powerButton"; icon: "image://icon/system-shutdown"; anchors.verticalCenter: parent.verticalCenter; onClicked: comp.powerOpen = !comp.powerOpen }
    }
}
