// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// The window ClusterPreviewMain.cpp (src/app) shows: the cluster, a pretend vehicle under it, and
// buttons to drive both. The two themes arrive filled from the application's real Theme, and
// the window's own colours come from the roles of the one in use, so nothing here is a colour
// of its own.
//
// Options of the program, after its name: --light, --scene=hazard|anomaly|nodata|stop,
// --shot=<file.png> (saves a picture of the window and exits).
import QtQuick
import TorqueBus.Cluster
pragma ComponentBehavior: Bound

Rectangle {
    id: root

    required property ClusterTheme darkTheme
    required property ClusterTheme lightTheme

    property bool light: false
    property string shotPath: ""

    readonly property ClusterTheme theme: root.light ? root.lightTheme : root.darkTheme

    width: 1360
    height: 760
    // The ground the cluster sits on in the Dashboard.
    color: root.theme.canvas

    FakeVehicle {
        id: vehicle
    }

    ClusterView {
        x: 20; y: 20
        width: root.width - 40
        height: root.height - 140
        vehicle: vehicle
        theme: root.theme
    }

    // ---- controls ---------------------------------------------------------------------------------
    component PreviewButton: Rectangle {
        id: btn
        property string label
        property bool on: false
        signal clicked

        width: txt.implicitWidth + 28
        height: 32
        radius: 6
        color: btn.on ? Qt.alpha(root.theme.accent, 0.25) : root.theme.tabStrip
        border.color: btn.on ? root.theme.accent : root.theme.border

        Text {
            id: txt
            anchors.centerIn: parent
            text: btn.label
            color: root.theme.text
            font.family: root.theme.bodyFamily
            font.pixelSize: 14
        }
        MouseArea {
            anchors.fill: parent
            onClicked: btn.clicked()
        }
    }

    Flow {
        x: 20
        y: root.height - 108
        width: root.width - 40
        spacing: 8

        PreviewButton {
            label: vehicle.ignition ? "Ignition off" : "Ignition on"
            on: vehicle.ignition
            onClicked: vehicle.ignition = !vehicle.ignition
        }
        PreviewButton {
            label: vehicle.cycle ? "Pause cycle" : "Run cycle"
            on: vehicle.cycle
            onClicked: vehicle.cycle = !vehicle.cycle
        }
        PreviewButton {
            label: "Overheat"
            on: vehicle.overheat
            onClicked: vehicle.overheat = !vehicle.overheat
        }
        PreviewButton {
            label: "Inject anomaly"
            on: vehicle.forceAnomaly
            onClicked: vehicle.forceAnomaly = !vehicle.forceAnomaly
        }
        PreviewButton {
            label: "Hazard"
            on: vehicle.lampHazard
            onClicked: vehicle.lampHazard = !vehicle.lampHazard
        }
        PreviewButton {
            label: "STOP (DM1)"
            on: vehicle.dmStop
            onClicked: vehicle.dmStop = !vehicle.dmStop
        }
        PreviewButton {
            label: "Warning (DM1)"
            on: vehicle.dmWarn
            onClicked: vehicle.dmWarn = !vehicle.dmWarn
        }
        PreviewButton {
            label: "MIL (DM1)"
            on: vehicle.dmMil
            onClicked: vehicle.dmMil = !vehicle.dmMil
        }
        PreviewButton {
            label: "Profile: " + (vehicle.profile === "full" ? "J1939 / FMS" : "11-bit example")
            onClicked: vehicle.profile = vehicle.profile === "full" ? "example11" : "full"
        }
        PreviewButton {
            label: "Theme: " + (root.light ? "light" : "dark")
            onClicked: root.light = !root.light
        }
    }

    // ---- a picture of the window, to check without looking at it ----------------------------------------
    Timer {
        id: shotTimer
        interval: 3200
        onTriggered: root.grabToImage(function (result) {
            result.saveToFile(root.shotPath)
            Qt.quit()
        })
    }
    Component.onCompleted: {
        var args = Qt.application.arguments
        for (var i = 0; i < args.length; ++i) {
            var a = String(args[i])
            if (a === "--light")
                root.light = true
            else if (a.indexOf("--shot=") === 0)
                root.shotPath = a.substring(7)
            else if (a === "--scene=hazard")
                vehicle.lampHazard = true
            else if (a === "--scene=anomaly")
                vehicle.forceAnomaly = true
            else if (a === "--scene=nodata")
                vehicle.profile = "example11"
            else if (a === "--scene=stop") {
                vehicle.dmStop = true
                vehicle.dmWarn = true
            }
        }
        if (root.shotPath !== "")
            shotTimer.start()
    }
}
