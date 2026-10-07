// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// A disc with a three-colour radial gradient: the gauges' backdrop, the AI core and the light on the floor.
import QtQuick
import QtQuick.Shapes

Item {
    id: root

    property color color0: "transparent"
    property real stop0: 0
    property color color1: "transparent"
    property real stop1: 0.5
    property color color2: "transparent"

    Shape {
        anchors.fill: parent
        preferredRendererType: Shape.CurveRenderer

        ShapePath {
            strokeColor: "transparent"
            fillGradient: RadialGradient {
                centerX: root.width / 2
                centerY: root.height / 2
                centerRadius: Math.min(root.width, root.height) / 2
                focalX: centerX
                focalY: centerY
                GradientStop { position: root.stop0; color: root.color0 }
                GradientStop { position: root.stop1; color: root.color1 }
                GradientStop { position: 1; color: root.color2 }
            }
            PathAngleArc {
                centerX: root.width / 2
                centerY: root.height / 2
                radiusX: root.width / 2
                radiusY: root.height / 2
                startAngle: 0
                sweepAngle: 360
            }
        }
    }
}
