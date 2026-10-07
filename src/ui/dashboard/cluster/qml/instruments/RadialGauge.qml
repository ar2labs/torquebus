// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// A 240-degree gauge: segmented ring, dotted scale and a bright head at the value. It serves the
// speedometer and the tachometer; whatever sits in the middle goes in as a child.
// With no data (valid = false) the ring is unlit and the head is hidden; the dashes are the
// owner's to draw.
import QtQuick
import TorqueBus.Cluster
pragma ComponentBehavior: Bound

Item {
    id: root
    required property ClusterTheme theme
    property real minimum: 0
    property real maximum: 120
    property real majorStep: 20
    property real minorStep: 5
    property real redFrom: -1          // from here on the scale and the segments are red (-1: never)
    property real labelDivisor: 1      // the tachometer shows rpm / 1000
    property real radius: 186
    property real ringWidth: 16
    property int segments: 48
    property real value: 0
    property bool valid: true

    default property alias content: contentLayer.data

    readonly property real startAngle: 210
    readonly property real sweep: 240

    // The head and the segments follow with a short smoothing: data arrives at 20 Hz, and a step
    // of 1 km/h should not look like a tick.
    //
    // What is smoothed is always a number. A value that is not one - no data yet - sits at the
    // start of the scale instead: an animation between a number and NaN passes through NaN, and
    // NaN in the head's position and rotation reaches the scene graph (the software renderer
    // rounds it to an integer, which Qt asserts against in a debug build).
    readonly property real finiteValue: Number.isFinite(root.value) ? root.value : root.minimum
    property real shown: root.finiteValue
    Behavior on shown {
        NumberAnimation { duration: 220; easing.type: Easing.OutCubic }
    }

    readonly property real fraction: root.valid ? Math.max(0, Math.min(1, (root.shown - root.minimum) / (root.maximum - root.minimum))) : 0
    readonly property real headAngle: (root.startAngle - root.fraction * root.sweep) * Math.PI / 180

    width: 2 * (root.radius + 34)
    height: width

    function litColors() {
        var out = []
        for (var i = 0; i < root.segments; ++i) {
            var v = root.minimum + ((i + 0.5) / root.segments) * (root.maximum - root.minimum)
            out.push(root.redFrom >= 0 && v >= root.redFrom ? root.theme.lampRed : root.theme.ramp(i / (root.segments - 1)))
        }
        return out
    }

    SegmentRing {
        anchors.fill: parent
        theme: root.theme
        radius: root.radius
        ringWidth: root.ringWidth
        segments: root.segments
        startAngle: root.startAngle
        sweep: root.sweep
        litCount: Math.round(root.fraction * root.segments)
        colors: root.litColors()
    }

    // The scale: a dot every minor step, a number every major step.
    Repeater {
        model: Math.round((root.maximum - root.minimum) / root.minorStep) + 1

        Item {
            id: tick
            required property int index

            readonly property real v: root.minimum + index * root.minorStep
            readonly property bool major: Math.abs(v / root.majorStep - Math.round(v / root.majorStep)) < 1e-6
            readonly property bool red: root.redFrom >= 0 && v >= root.redFrom
            readonly property real a: (root.startAngle - (v - root.minimum) / (root.maximum - root.minimum) * root.sweep) * Math.PI / 180

            Rectangle {
                width: tick.major ? 4.4 : 2.2
                height: width
                radius: width / 2
                x: root.width / 2 + (root.radius + 17) * Math.cos(tick.a) - width / 2
                y: root.height / 2 - (root.radius + 17) * Math.sin(tick.a) - height / 2
                color: tick.red ? root.theme.error : tick.major ? root.theme.text : root.theme.textFaint
            }
            CText {
                visible: tick.major
                theme: root.theme
                face: "display"
                font.pixelSize: 17
                font.weight: Font.Medium
                text: Math.round(tick.v / root.labelDivisor).toString()
                color: tick.red ? root.theme.error : root.theme.textMuted
                cx: root.width / 2 + (root.radius - 30) * Math.cos(tick.a)
                cy: root.height / 2 - (root.radius - 30) * Math.sin(tick.a)
            }
        }
    }

    // The bright head at the current value.
    Rectangle {
        visible: root.valid
        readonly property real rMid: root.radius - 1
        width: root.ringWidth + 10
        height: 3
        radius: 1.5
        color: root.theme.text
        x: root.width / 2 + rMid * Math.cos(root.headAngle) - width / 2
        y: root.height / 2 - rMid * Math.sin(root.headAngle) - height / 2
        rotation: -root.headAngle * 180 / Math.PI
    }

    Item {
        id: contentLayer
        anchors.fill: parent
    }
}
