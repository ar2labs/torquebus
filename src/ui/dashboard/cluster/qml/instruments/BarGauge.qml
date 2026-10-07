// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// A 20-segment bar with an icon, a name and a value: temperature, fuel, AdBlue. The alert is the
// colour of the bar itself (amber on a warning, red on an alarm), which is why the cluster does
// not repeat the information in a separate lamp.
import QtQuick
import TorqueBus.Cluster
pragma ComponentBehavior: Bound

Item {
    id: root
    required property ClusterTheme theme
    property string icon
    property string label
    property real value: NaN
    property bool valid: true
    property real minimum: 0
    property real maximum: 100
    property real warnAt: NaN          // a warning once the value reaches this...
    property real alarmAt: NaN
    property bool lowIsBad: false      // ...from above (temperature) or from below (fuel)
    property string valueText: ""

    readonly property int segments: 20
    readonly property real gap: 3
    readonly property bool hasValue: root.valid && Number.isFinite(root.value)
    readonly property real fraction: root.hasValue ? Math.max(0, Math.min(1, (root.value - root.minimum) / (root.maximum - root.minimum))) : 0
    readonly property string level: {
        if (!root.hasValue)
            return "none"
        var past = function (limit) { return Number.isFinite(limit) && (root.lowIsBad ? root.value <= limit : root.value >= limit) }
        return past(root.alarmAt) ? "alarm" : past(root.warnAt) ? "warn" : "ok"
    }

    width: 240
    height: 28

    TelltaleIcon {
        name: root.icon
        size: 16
        color: root.theme.textMuted
    }
    CText {
        theme: root.theme
        face: "display"
        align: "left"
        cx: 22
        cy: 8
        text: root.label
        color: root.theme.textMuted
        font.pixelSize: 11
        font.capitalization: Font.AllUppercase
        font.letterSpacing: 1.3
    }
    CText {
        theme: root.theme
        face: "mono"
        align: "right"
        cx: root.width
        cy: 8
        text: root.hasValue ? root.valueText : "– – –"
        font.pixelSize: 13
        color: !root.hasValue ? root.theme.textFaint
               : root.level === "alarm" ? root.theme.error
               : root.level === "warn" ? root.theme.warning : root.theme.text
    }

    Repeater {
        model: root.segments

        Rectangle {
            id: seg
            required property int index

            readonly property real segW: (root.width - (root.segments - 1) * root.gap) / root.segments

            x: index * (segW + root.gap)
            y: 20
            width: segW
            height: 9
            radius: 2
            color: seg.index < Math.round(root.fraction * root.segments)
                   ? (root.level === "alarm" ? root.theme.lampRed
                      : root.level === "warn" ? root.theme.lampAmber
                      : root.theme.ramp(seg.index / (root.segments - 1)))
                   : root.theme.segmentOff
        }
    }
}
