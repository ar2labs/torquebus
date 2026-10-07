// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// The "binocular" frame: two round lenses joined by a lower bridge, with a module at the top of the
// middle. It is drawn in layers (shadow, outer rim, shaded ring, glass), each the same outline moved
// outwards; the bevel is what gives the impression of volume.
import QtQuick
import QtQuick.Effects
import QtQuick.Shapes
import TorqueBus.Cluster

Item {
    id: root

    required property ClusterTheme theme

    width: 1280
    height: 560

    // The outline, `d` pixels outwards (inwards when negative). The lenses are the circles of radius
    // 248 at (290, 300) and (990, 300); the bridge rises 38 px in the middle to make room for the
    // hazard lamp.
    function outline(d) {
        var R = 248 + d, T = 92 - d, B = 540 + d, bump = 54 - d
        var dxT = Math.sqrt(R * R - Math.pow(300 - T, 2))
        var dxB = Math.sqrt(R * R - Math.pow(B - 300, 2))
        var f = function (v) { return v.toFixed(1) }
        return "M" + f(290 + dxT) + " " + T
             + " L532 " + T + " C552 " + T + " 556 " + bump + " 580 " + bump
             + " L700 " + bump + " C724 " + bump + " 728 " + T + " 748 " + T
             + " L" + f(990 - dxT) + " " + T
             + " A" + R + " " + R + " 0 1 1 " + f(990 - dxB) + " " + B
             + " L" + f(290 + dxB) + " " + B
             + " A" + R + " " + R + " 0 1 1 " + f(290 + dxT) + " " + T + " Z"
    }

    // The shadow on the dashboard: the outline, moved down and blurred. The blur is drawn from a
    // picture of the outline, so that picture is bigger than the outline by the reach of the blur;
    // a layer is clipped to its item, and a shadow cut off at the item's edge would show.
    Item {
        id: shadowSource
        x: -100; y: -80; width: root.width + 200; height: root.height + 200
        visible: false
        layer.enabled: true

        Shape {
            x: 100; y: 80 + 16
            preferredRendererType: Shape.CurveRenderer
            ShapePath {
                strokeColor: "transparent"
                fillColor: root.theme.shadow
                PathSvg { path: root.outline(0) }
            }
        }
    }
    MultiEffect {
        x: shadowSource.x; y: shadowSource.y
        width: shadowSource.width; height: shadowSource.height
        source: shadowSource
        blurEnabled: true
        blurMax: 64
        blur: 1.0
        opacity: root.theme.shadowOpacity
    }

    Shape {
        anchors.fill: parent
        preferredRendererType: Shape.CurveRenderer

        // the outer rim
        ShapePath {
            strokeColor: "transparent"
            fillColor: root.theme.bezelRim
            PathSvg { path: root.outline(12) }
        }
        // the shaded ring: bright on top, dark underneath, like a part lit from above
        ShapePath {
            strokeColor: "transparent"
            fillGradient: LinearGradient {
                x1: 0; y1: 40; x2: 0; y2: 554
                GradientStop { position: 0; color: root.theme.bezelHighlight }
                GradientStop { position: 0.45; color: root.theme.instrumentBezel }
                GradientStop { position: 1; color: root.theme.bezelLow }
            }
            PathSvg { path: root.outline(8) }
        }
        // the line of light along the glass; the glass is drawn over its inner half
        ShapePath {
            strokeColor: root.theme.bezelEdge
            strokeWidth: 3
            joinStyle: ShapePath.RoundJoin
            fillColor: "transparent"
            PathSvg { path: root.outline(0) }
        }
        // the glass
        ShapePath {
            strokeColor: "transparent"
            fillGradient: LinearGradient {
                x1: 0; y1: 54; x2: 0; y2: 540
                GradientStop { position: 0; color: root.theme.glassTop }
                GradientStop { position: 0.55; color: root.theme.instrumentScreen }
                GradientStop { position: 1; color: root.theme.glassBottom }
            }
            PathSvg { path: root.outline(0) }
        }
        // glare on the top of the glass
        ShapePath {
            strokeColor: "transparent"
            fillGradient: LinearGradient {
                x1: 0; y1: 54; x2: 0; y2: 224
                GradientStop { position: 0; color: root.theme.glare }
                GradientStop { position: 1; color: Qt.alpha(root.theme.glare, 0) }
            }
            PathSvg { path: root.outline(0) }
        }
        // A fine line of the accent colour along the glass edge, strongest in the middle and
        // fading to the border colour at the sides. Shapes can fill with a gradient but not stroke
        // with one, so it is a ring 1.2 px wide - the outline twice, filled where they differ.
        ShapePath {
            strokeColor: "transparent"
            fillRule: ShapePath.OddEvenFill
            fillGradient: LinearGradient {
                x1: 40; y1: 0; x2: 1240; y2: 0
                GradientStop { position: 0; color: root.theme.border }
                GradientStop { position: 0.5; color: Qt.alpha(root.theme.accentHover, 0.75) }
                GradientStop { position: 1; color: root.theme.border }
            }
            PathSvg { path: root.outline(0.6) + " " + root.outline(-0.6) }
        }
        // the light line of the hazard module
        ShapePath {
            strokeColor: root.theme.accentHover
            strokeWidth: 2.5
            capStyle: ShapePath.RoundCap
            fillColor: "transparent"
            PathSvg { path: "M598 54 H682" }
        }
    }
}
