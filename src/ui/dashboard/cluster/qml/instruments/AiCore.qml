// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// The AI health core: no frame of its own, in the middle of the cluster. Thermal health in %
// sits inside a risk ring (the same language as the gauges), with the regime the model
// recognised and a short sentence under it. With no data it stays here, showing dashes.
import QtQuick
import TorqueBus.Cluster
pragma ComponentBehavior: Bound

Item {
    id: root
    required property ClusterTheme theme
    property bool live: false            // there is AI data and the ignition is on
    property real regime: NaN            // 0 idle, 1 cruise, 2 high load, 3 thermal stress, 4 anomaly
    property real anomaly: NaN           // risk, 0 to 100 %
    property real confidence: NaN
    property real health: NaN
    property color ambient: root.theme.accent   // follows the state: accent when normal, amber on caution, red on anomaly

    // The names and sentences for each RegimeClass of TinyML_Telemetry.
    readonly property var regimes: [
        { name: qsTr("Idle"), color: root.theme.textMuted, message: qsTr("Engine idling") },
        { name: qsTr("Cruise"), color: root.theme.accent, message: qsTr("Normal driving") },
        { name: qsTr("High load"), color: root.theme.warning, message: qsTr("High engine load") },
        { name: qsTr("Thermal stress"), color: root.theme.warning, message: qsTr("Temperature above expected") },
        { name: qsTr("Anomaly"), color: root.theme.error, message: qsTr("Anomaly · check the engine") }
    ]
    readonly property var current: root.live && Number.isFinite(root.regime) ? root.regimes[Math.max(0, Math.min(4, Math.round(root.regime)))] : null
    readonly property real risk: root.live && Number.isFinite(root.anomaly) ? Math.max(0, Math.min(100, root.anomaly)) : 0
    readonly property color regimeColor: root.current ? root.current.color : root.theme.textFaint

    // The origin of the group: the centre of the core is at (140, 85).
    width: 280
    height: 220

    function pct(v) {
        return Number(Math.round(v)).toLocaleString(Qt.locale(), "f", 0) + "%"
    }

    RadialGlow {
        x: 84; y: 29; width: 112; height: 112
        color0: Qt.alpha(root.ambient, 0.28)
        stop0: 0
        color1: Qt.alpha(root.theme.glassBottom, 0.9)
        stop1: 0.7
        color2: root.theme.instrumentScreen
    }

    SegmentRing {
        x: 60; y: 5; width: 160; height: 160
        theme: root.theme
        radius: 70
        ringWidth: 6
        segments: 36
        startAngle: 225
        sweep: 270
        gapDegrees: 1.8
        litCount: Math.round(root.risk / 100 * 36)
        colors: {
            var out = []
            for (var i = 0; i < 36; ++i) {
                var f = (i + 0.5) / 36
                out.push(f < 0.35 ? root.theme.lampGreen : f < 0.65 ? root.theme.lampAmber : root.theme.lampRed)
            }
            return out
        }
    }
    // The alert threshold (65 %).
    Rectangle {
        readonly property real a: (225 - 0.65 * 270) * Math.PI / 180
        width: 17; height: 2; radius: 1
        color: root.theme.text
        x: 140 + 70.5 * Math.cos(a) - width / 2
        y: 85 - 70.5 * Math.sin(a) - height / 2
        rotation: -a * 180 / Math.PI
    }

    CText {
        theme: root.theme; face: "display"; cx: 140; cy: 81
        text: root.live && Number.isFinite(root.health) ? root.pct(root.health) : "– –"
        font.pixelSize: 36
        font.weight: Font.DemiBold
        color: !root.live || !Number.isFinite(root.health) ? root.theme.textFaint
               : root.health < 40 ? root.theme.error : root.health < 70 ? root.theme.warning : root.theme.text
    }
    CText {
        theme: root.theme; face: "display"; cx: 140; cy: 107.5
        text: qsTr("Thermal health")
        color: root.theme.textMuted
        font.pixelSize: 9
        font.capitalization: Font.AllUppercase
        font.letterSpacing: 1
    }
    CText {
        theme: root.theme; face: "mono"; cx: 140; cy: 123
        text: root.live && Number.isFinite(root.confidence) ? qsTr("conf. %1").arg(root.pct(root.confidence)) : qsTr("conf. – –")
        color: root.theme.textMuted
        font.pixelSize: 11
    }
    CText {
        theme: root.theme; face: "mono"; cx: 140; cy: 150
        text: root.live && Number.isFinite(root.anomaly) ? qsTr("risk %1").arg(root.pct(root.risk)) : qsTr("risk – –")
        font.pixelSize: 11
        color: !root.live || !Number.isFinite(root.anomaly) ? root.theme.textFaint
               : root.risk >= 65 ? root.theme.error : root.risk >= 35 ? root.theme.warning : root.theme.success
    }

    // The regime: icon and name centred together.
    Item {
        id: regimeRow
        readonly property real contentWidth: 18 + 6 + regimeName.width
        x: 140 - contentWidth / 2
        y: 168
        height: 18

        TelltaleIcon {
            name: "ai"
            size: 18
            color: root.regimeColor
        }
        CText {
            id: regimeName
            theme: root.theme; face: "display"; align: "left"; cx: 24; cy: 9
            text: root.current ? root.current.name : qsTr("Waiting")
            color: root.regimeColor
            font.pixelSize: 15
            font.weight: Font.DemiBold
            font.letterSpacing: 0.6
        }
    }
    CText {
        theme: root.theme; face: "body"; cx: 140; cy: 199
        text: root.current ? root.current.message : qsTr("AI waiting for bus data")
        font.pixelSize: 13
    }
}
