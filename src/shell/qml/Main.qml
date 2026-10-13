// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Window
import QtWayland.Compositor
import QtWayland.Compositor.XdgShell

// DiskAnvil Live's screen: the bar along the top, the windows, the dock along the bottom.
WaylandCompositor {
    id: comp
    socketName: "diskanvil-0"

    readonly property var pal: ({ bg: "#060a16", surface: "#0d1426", raised: "#141e36", line: "#233150", text: "#e6f1ff", dim: "#8fa3c4",
                                  accent: "#00e5ff", blue: "#1f6fff", good: "#2bee8a", danger: "#ff3355" })
    readonly property string sans: "Noto Sans"

    // The dock: DiskAnvil's tools, then the programs. `pid` is the one started from here.
    property var dockItems: [
        { id: "drives", icon: "diskanvil-live-drives", title: "Drives and Partitions", program: "diskanvil", args: [] },
        { id: "whathappened", icon: "diskanvil-live-whathappened", title: "What Happened?", program: "diskanvil", args: ["--open", "what-happened"] },
        { id: "savefiles", icon: "diskanvil-live-savefiles", title: "Save My Files", program: "diskanvil", args: ["--open", "save-files"] },
        { id: "recover", icon: "diskanvil-live-recover", title: "Get Files Back", program: "diskanvil", args: ["--open", "lost-files"] },
        { id: "lookcloser", icon: "diskanvil-live-lookcloser", title: "Look Closer", program: "diskanvil", args: ["--open", "look-closer"] },
        { id: "virus", icon: "diskanvil-live-virus", title: "Scan for Viruses", program: "diskanvil", args: ["--open", "virus-scan"] },
        { id: "testdisk", icon: "diskanvil-live-testdisk", title: "Find Lost Partitions", program: "qterminal", args: ["-e", "sudo testdisk"] },
        { id: "windowsusb", icon: "diskanvil-live-windowsusb", title: "Make a Windows USB", program: "diskanvil", args: ["--open", "windows-usb"] }
    ]
    readonly property var programs: [
        { id: "terminal", icon: "utilities-terminal", title: "Terminal", program: "qterminal", args: [] },
        { id: "files", icon: "system-file-manager", title: "Files", program: "diskanvil", args: ["--open", "files"] }
    ]
    // Everything else, in the grid the first dock button opens.
    readonly property var more: [
        { id: "writeimage", icon: "diskanvil-live-writeimage", title: "Write an Image to USB", program: "diskanvil", args: ["--open", "write-image"] },
        { id: "checkstick", icon: "diskanvil-live-checkstick", title: "Check a USB Stick", program: "diskanvil", args: ["--open", "check-stick"] },
        { id: "copystick", icon: "diskanvil-live-copystick", title: "Copy This Stick", program: "diskanvil", args: ["--open", "rescue-usb"] },
        { id: "activity", icon: "diskanvil-live-logs", title: "What This PC Did", program: "diskanvil", args: ["--open", "activity"] },
        { id: "drivers", icon: "diskanvil-live-windowsfix", title: "Windows Drivers", program: "diskanvil", args: ["--open", "drivers"] },
        { id: "bootmenu", icon: "diskanvil-live-firmware", title: "Boot Menu Repair", program: "diskanvil", args: ["--open", "boot-menu"] },
        { id: "remotehelp", icon: "diskanvil-live-remotehelp", title: "Let Someone Help", program: "diskanvil", args: ["--open", "remote-help"] },
        { id: "netboot", icon: "diskanvil-live-netboot", title: "Start Other PCs", program: "diskanvil", args: ["--open", "net-boot"] },
        { id: "web", icon: "firefox-esr", title: "Web Browser", program: "firefox-esr", args: [] },
        { id: "logs", icon: "diskanvil-live-logs", title: "Save Logs", program: "diskanvil-live-logs", args: [] },
        { id: "readme", icon: "diskanvil-live-readme", title: "Read Me", program: "qterminal", args: ["-e", "less /usr/local/share/diskanvil-live/README.txt"] }
    ]
    property var started: ({})   // dock item id → process number
    property bool gridOpen: false   // the grid of everything
    property bool powerOpen: false  // the power menu

    property var windows: []
    property Item active: null
    property int nextZ: 1

    function launch(item) {
        // Its window, if it's already open; otherwise it's started.
        const own = windows.filter(w => w.pid > 0 && w.pid === started[item.id])
        if (own.length) {
            raise(own[own.length - 1])
            return
        }
        const pid = shell.launch(item.program, item.args)
        if (pid > 0) {
            const s = Object.assign({}, started); s[item.id] = pid; started = s
        }
    }
    function itemRunning(item) { return windows.some(w => w.pid > 0 && w.pid === started[item.id]) }
    function track(w) { windows = windows.concat([w]); raise(w) }
    function untrack(w) {
        windows = windows.filter(x => x !== w)
        if (active === w) focusTop()
    }
    // Put out of sight (minimised): the one under it gets the keys.
    function hide(w) {
        w.visible = false
        if (active === w) focusTop()
    }
    function focusTop() {
        const shown = windows.filter(x => x.visible)
        active = null
        if (shown.length) raise(shown.reduce((a, b) => a.z > b.z ? a : b))
    }
    function raise(w) {
        w.visible = true
        w.z = ++nextZ
        active = w
        w.takeFocus()
    }
    function cycle() {
        if (windows.length < 2) return
        const order = windows.slice().sort((a, b) => a.z - b.z)
        raise(order[0])
    }

    WaylandOutput {
        id: output
        sizeFollowsWindow: true
        // What a maximised window fills: between the bar and the dock.
        availableGeometry: Qt.rect(0, 36, screen.width, screen.height - 36 - 116)
        window: Window {
            id: screen
            width: 1600; height: 1000
            visible: true
            // The whole screen on DiskAnvil Live (straight to the screen, or on Xorg); a window while it's worked on.
            visibility: shell.fullScreen ? Window.FullScreen : Window.AutomaticVisibility
            color: pal.bg
            title: "DiskAnvil Live"

            Image { anchors.fill: parent; source: "qrc:/shell/art/desktop.svg"; sourceSize: Qt.size(width, height); fillMode: Image.PreserveAspectCrop; asynchronous: true }

            // The room windows get: between the bar and the dock.
            Item { id: area; x: output.availableGeometry.x; y: output.availableGeometry.y; width: output.availableGeometry.width; height: output.availableGeometry.height }
            Item { id: desk; anchors.fill: parent }

            TopBar { id: topBar; comp: comp; z: 100000 }
            Dock { id: dock; comp: comp; z: 100000 }
            AppGrid { id: grid; comp: comp; z: 100001 }
            PowerMenu { id: powerMenu; comp: comp; z: 100001 }

            Shortcut { sequences: ["Alt+Tab"]; context: Qt.ApplicationShortcut; onActivated: comp.cycle() }

            // The mouse pointer, drawn here when nothing else does: the shape the program under it
            // asks for (an I-beam over text, arrows on an edge), or our arrow.
            WaylandCursorItem {
                id: cursor
                seat: comp.defaultSeat
                inputEventsEnabled: false
                visible: shell.drawsPointer && surface !== null
                x: shell.pointer.x - hotspotX; y: shell.pointer.y - hotspotY
                z: 1000000
            }
            Image {
                visible: shell.drawsPointer && cursor.surface === null
                source: "qrc:/shell/art/pointer.svg"; sourceSize: Qt.size(24, 24)
                x: shell.pointer.x; y: shell.pointer.y; z: 1000000
            }
        }
    }

    XdgShell {
        onToplevelCreated: (toplevel, xdgSurface) => frame.createObject(desk, { shellSurface: xdgSurface, toplevel: toplevel, comp: comp, area: area })
    }
    XdgDecorationManagerV1 { preferredMode: XdgToplevel.ServerSideDecoration }
    Component { id: frame; ShellWindow {} }
    Component.onCompleted: {
        shell.setSocket(socketName)
        if (shell.keyboardLayout.length > 0) {
            defaultSeat.keymap.layout = shell.keyboardLayout
            defaultSeat.keymap.variant = shell.keyboardVariant
        }
    }
}
