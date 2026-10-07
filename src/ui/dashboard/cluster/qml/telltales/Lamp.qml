// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// A telltale: grey when off, in its ISO 2575 colour with a halo when lit. It is always on screen -
// with no data it simply stays off.
import QtQuick
import TorqueBus.Cluster

Item {
    id: root

    required property ClusterTheme theme
    property string icon
    property color tone: root.theme.lampGreen   // one of the theme's lamp colours
    property bool lit: false
    property real size: 26

    width: root.size
    height: root.size

    RadialGlow {
        visible: root.lit
        anchors.centerIn: parent
        width: root.size * 2.2
        height: width
        color0: Qt.alpha(root.tone, 0.35)
        stop0: 0
        color1: Qt.alpha(root.tone, 0.12)
        stop1: 0.45
        color2: Qt.alpha(root.tone, 0)
    }
    TelltaleIcon {
        anchors.fill: parent
        name: root.icon
        size: root.size
        color: root.lit ? root.tone : root.theme.lampOff
    }
}
