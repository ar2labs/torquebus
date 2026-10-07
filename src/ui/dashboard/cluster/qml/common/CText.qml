// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// Text placed by its centre (cx, cy), which is how the layout was drawn and saves working out
// a baseline for every label.
import QtQuick
import TorqueBus.Cluster

Text {
    id: root

    required property ClusterTheme theme
    property string face: "body"      // display | mono | body
    property string align: "center"   // center | left | right: the side of cx the text grows towards
    property real cx: 0
    property real cy: 0

    color: root.theme.text
    font.family: root.face === "display" ? root.theme.displayFamily
                 : root.face === "mono" ? root.theme.monoFamily : root.theme.bodyFamily
    textFormat: Text.PlainText
    x: root.align === "center" ? root.cx - root.width / 2 : root.align === "left" ? root.cx : root.cx - root.width
    y: root.cy - root.height / 2
}
