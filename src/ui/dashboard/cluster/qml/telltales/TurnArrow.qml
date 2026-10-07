// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// A turn indicator: large, solid, one on each side of the AI core. The right-hand one is the mirror image.
import QtQuick
import TorqueBus.Cluster

Item {
    id: root

    required property ClusterTheme theme
    property bool mirrored: false
    property bool lit: false

    // cluster-turn-arrow.svg is drawn in a 43 x 40 box; this is how much larger it is shown.
    readonly property real zoom: 1.4

    width: 43 * root.zoom
    height: 40 * root.zoom

    RadialGlow {
        visible: root.lit
        anchors.centerIn: parent
        width: root.width * 2
        height: root.width * 2
        color0: Qt.alpha(root.theme.lampGreen, 0.3)
        stop0: 0
        color1: Qt.alpha(root.theme.lampGreen, 0.1)
        stop1: 0.5
        color2: Qt.alpha(root.theme.lampGreen, 0)
    }
    TelltaleIcon {
        anchors.fill: parent
        name: "turn-arrow"
        size: root.width
        aspectRatio: 43 / 40
        mirrored: root.mirrored
        color: root.lit ? root.theme.lampGreen : root.theme.lampOff
    }
}
