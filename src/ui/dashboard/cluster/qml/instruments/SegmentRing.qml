// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// A ring of segments. Each one is a rectangle rotated until it is tangent to the ring: 36 to 64
// plain items instead of a vector path per segment, which keeps the cost of painting low even
// when the value changes 20 times a second. Angles are in degrees, 0 on the right and growing
// anticlockwise, as in the design.
import QtQuick
import TorqueBus.Cluster
pragma ComponentBehavior: Bound

Item {
    id: root

    required property ClusterTheme theme
    property real radius: 186
    property real ringWidth: 16
    property int segments: 48
    property real startAngle: 210
    property real sweep: 240
    property real gapDegrees: 1.1
    property int litCount: 0
    property var colors: []            // the lit colour of each segment; past litCount they are off

    Repeater {
        model: root.segments

        Rectangle {
            id: seg
            required property int index

            readonly property real step: root.sweep / root.segments
            readonly property real mid: (root.startAngle - (index + 0.5) * step) * Math.PI / 180

            width: root.ringWidth
            height: Math.max(1, (seg.step - root.gapDegrees) * Math.PI / 180 * root.radius)
            radius: 1.5
            x: root.width / 2 + root.radius * Math.cos(seg.mid) - width / 2
            y: root.height / 2 - root.radius * Math.sin(seg.mid) - height / 2
            rotation: -seg.mid * 180 / Math.PI
            color: seg.index < root.litCount && seg.index < root.colors.length ? root.colors[seg.index] : root.theme.segmentOff
        }
    }
}
