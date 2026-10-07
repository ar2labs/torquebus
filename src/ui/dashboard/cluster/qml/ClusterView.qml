// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// The J1939/FMS instrument cluster of the Dashboard, "binocular" layout.
//
// The cluster does not know where its data comes from. Whoever hosts it hands over a `vehicle` object
// with roles (speed, engine speed, lamps...). A role with no data - missing, NaN, "not available" -
// shows as dashes and its lamp stays ready: nothing disappears and nothing is greyed out. That is what
// lets the same cluster sit on a J1939/FMS profile, on the 11-bit example or on a real bus.
//
// The colours come from `theme` (ClusterTheme), which the host fills from the application's Theme.
//
// The `vehicle` contract (every role is optional):
//   numbers   speed (km/h)  rpm  gear (-1 R, 0 N, 1..n)  coolant (°C)  oil (kPa)  fuel (%)  def (%)
//             battery (V)  airPressure (bar)  ambient (°C)  odometer (km)  hours (h)
//             aiRegime (0..4)  aiAnomaly (%)  aiConfidence (%)  aiHealth (%)
//   booleans  ignition (default: on)  cruise
//             lampHigh lampLow lampPosition lampPark lampBelt lampEngine lampOil lampAbs
//             lampLeft lampRight lampHazard
//             dmStop dmWarn dmMil dmProtect          (the DM1 lamps)
import QtQuick
import TorqueBus.Cluster
pragma ComponentBehavior: Bound

Item {
    id: root
    required property ClusterTheme theme
    property var vehicle: null

    implicitWidth: 1280
    implicitHeight: 560

    // ---- reading the roles --------------------------------------------------------------------
    function num(name) {
        var v = root.vehicle ? root.vehicle[name] : undefined
        return Number.isFinite(v) ? v : NaN
    }
    function flag(name) {
        return root.vehicle ? root.vehicle[name] === true : false
    }
    function fmt(v, decimals) {
        return Number(v).toLocaleString(Qt.locale(), "f", decimals)
    }

    readonly property bool ign: root.vehicle && root.vehicle.ignition === false ? false : true
    readonly property real speed: root.num("speed")
    readonly property real rpm: root.num("rpm")
    readonly property real gear: root.num("gear")
    readonly property real coolant: root.num("coolant")
    readonly property real oil: root.num("oil")
    readonly property real fuel: root.num("fuel")
    readonly property real def: root.num("def")
    readonly property real battery: root.num("battery")
    readonly property real airPressure: root.num("airPressure")
    readonly property real ambientTemp: root.num("ambient")
    readonly property real odometer: root.num("odometer")
    readonly property real hours: root.num("hours")
    readonly property real aiRegime: root.num("aiRegime")

    // ---- lamp test at start-up, as on a real panel -------------------------------------------------
    property real bulbPhase: 0
    readonly property bool bulb: bulbAnim.running
    readonly property real sweep: Math.sin(Math.PI * root.bulbPhase)

    NumberAnimation {
        id: bulbAnim
        target: root
        property: "bulbPhase"
        from: 0
        to: 1
        duration: 1600
    }
    onIgnChanged: if (root.ign) bulbAnim.restart()
    Component.onCompleted: if (root.ign) bulbAnim.restart()

    // ---- clock and blink ---------------------------------------------------------------------------
    property bool blinkOn: true
    property string clockText: Qt.formatTime(new Date(), "hh:mm")

    Timer {
        interval: 357
        running: root.visible
        repeat: true
        onTriggered: root.blinkOn = !root.blinkOn
    }
    Timer {
        interval: 1000
        running: root.visible
        repeat: true
        onTriggered: root.clockText = Qt.formatTime(new Date(), "hh:mm")
    }

    // ---- the cluster's rules for the lamps -----------------------------------------------------------
    // Some lamps come from a value (low oil, low battery); the rest arrive lit from the vehicle.
    function lampOn(id) {
        switch (id) {
        case "high": return root.flag("lampHigh")
        case "low": return root.flag("lampLow")
        case "position": return root.flag("lampPosition")
        case "park": return root.flag("lampPark")
        case "belt": return root.flag("lampBelt")
        case "engine": return root.flag("lampEngine") || root.flag("dmMil")
        case "stop": return root.flag("dmStop")
        case "warn": return root.flag("dmWarn") || root.flag("dmProtect")
        case "oil": return root.flag("lampOil") || (Number.isFinite(root.oil) && root.oil < 100)
        case "battery": return Number.isFinite(root.battery) && root.battery < 23
        case "abs": return root.flag("lampAbs")
        }
        return false
    }

    // Lamps in an arc over each lens, split between the two; the hazard lamp has its own module.
    readonly property var arcLamps: [
        { id: "high", icon: "high-beam", tone: root.theme.lampBlue },
        { id: "low", icon: "low-beam", tone: root.theme.lampGreen },
        { id: "position", icon: "position-lights", tone: root.theme.lampGreen },
        { id: "park", icon: "parking-brake", tone: root.theme.lampRed },
        { id: "belt", icon: "seat-belt", tone: root.theme.lampRed },
        { id: "engine", icon: "engine", tone: root.theme.lampAmber },
        { id: "stop", icon: "stop", tone: root.theme.lampRed },
        { id: "warn", icon: "warning", tone: root.theme.lampAmber },
        { id: "oil", icon: "oil-pressure", tone: root.theme.lampRed },
        { id: "battery", icon: "battery", tone: root.theme.lampRed },
        { id: "abs", icon: "abs", tone: root.theme.lampAmber }
    ]

    // The cluster draws on a fixed 1280 x 560 stage and scales it to the space of the widget.
    Item {
        id: stage
        width: 1280
        height: 560
        scale: Math.min(root.width / 1280, root.height / 560)
        transformOrigin: Item.TopLeft
        x: (root.width - 1280 * stage.scale) / 2
        y: (root.height - 560 * stage.scale) / 2

        // The light on the floor and in the core follows the state of the AI.
        readonly property bool alarm: root.ign && root.aiRegime === 4
        readonly property color ambientColor: !root.ign ? root.theme.accent
                                              : stage.alarm ? root.theme.lampRed
                                              : root.aiRegime >= 2 ? root.theme.lampAmber : root.theme.accent
        property real pulse: 0.45

        SequentialAnimation on pulse {
            running: stage.alarm && root.visible
            loops: Animation.Infinite
            NumberAnimation { from: 0.35; to: 0.7; duration: 600; easing.type: Easing.InOutSine }
            NumberAnimation { from: 0.7; to: 0.35; duration: 600; easing.type: Easing.InOutSine }
        }

        RadialGlow {
            // a flattened ellipse on the floor: a 960 px disc squashed vertically around its centre
            x: 160; y: 550 - 480; width: 960; height: 960
            transform: Scale { yScale: 0.025; origin.y: 480 }
            color0: Qt.alpha(stage.ambientColor, stage.alarm ? stage.pulse : 0.45)
            stop0: 0
            color1: Qt.alpha(stage.ambientColor, 0.12)
            stop1: 0.5
            color2: Qt.alpha(stage.ambientColor, 0)
        }

        ClusterFrame {
            theme: root.theme
        }

        // Backdrop of the gauges: a dark disc that softens the edge of the ring.
        Repeater {
            model: [290, 990]
            RadialGlow {
                required property int modelData
                x: modelData - 236; y: 300 - 236; width: 472; height: 472
                color0: Qt.alpha(root.theme.glassTop, 0.95)
                stop0: 0.0
                color1: Qt.alpha(root.theme.glassBottom, 0.8)
                stop1: 0.85
                color2: Qt.alpha(root.theme.glassBottom, 0)
            }
        }

        // ---- lamps in an arc over each lens ------------------------------------------------------
        Repeater {
            model: root.arcLamps

            Lamp {
                id: arcLamp
                required property var modelData
                required property int index

                readonly property int nLeft: Math.floor(root.arcLamps.length / 2)
                readonly property bool onLeft: index < nLeft
                readonly property int k: onLeft ? index : index - nLeft
                readonly property int n: onLeft ? nLeft : root.arcLamps.length - nLeft
                readonly property real a: (90 + (n - 1) * 11 / 2 - k * 11) * Math.PI / 180

                theme: root.theme
                icon: modelData.icon
                tone: modelData.tone
                lit: root.ign && (root.bulb || root.lampOn(modelData.id))
                x: (onLeft ? 290 : 990) + 222 * Math.cos(a) - width / 2
                y: 300 - 222 * Math.sin(a) - height / 2
            }
        }

        // ---- speedometer (left) · CCVS1, SPN 84 -------------------------------------------------
        RadialGauge {
            id: speedo
            x: 290 - width / 2
            y: 300 - height / 2
            theme: root.theme
            minimum: 0; maximum: 240; majorStep: 20; minorStep: 10
            segments: 48
            opacity: root.ign ? 1 : 0.5
            value: !root.ign ? 0 : root.bulb ? 240 * root.sweep : root.speed
            valid: root.ign && (root.bulb || Number.isFinite(root.speed))

            CText {
                theme: root.theme; face: "display"; cx: speedo.width / 2; cy: speedo.height / 2 - 84
                text: qsTr("Speed")
                color: root.theme.textMuted
                font.pixelSize: 16
                font.capitalization: Font.AllUppercase
                font.letterSpacing: 1.9
            }
            CText {
                theme: root.theme; face: "display"; cx: speedo.width / 2; cy: speedo.height / 2 - 21
                text: root.ign && Number.isFinite(root.speed) ? root.fmt(root.speed, 0) : "– –"
                font.pixelSize: 112
                font.weight: Font.DemiBold
                color: root.ign && Number.isFinite(root.speed) ? root.theme.text : root.theme.textFaint
            }
            CText {
                theme: root.theme; face: "display"; cx: speedo.width / 2; cy: speedo.height / 2 + 42
                text: qsTr("km/h")
                color: root.theme.textMuted
                font.pixelSize: 16
                font.capitalization: Font.AllUppercase
                font.letterSpacing: 1.9
            }
            // Cruise control: the pill is the only indication (no repeated lamp in the arc).
            Rectangle {
                x: speedo.width / 2 - 48
                y: speedo.height / 2 + 68
                width: 96; height: 26; radius: 13
                opacity: root.ign && root.flag("cruise") ? 1 : 0
                Behavior on opacity { NumberAnimation { duration: 180 } }
                color: Qt.alpha(root.theme.lampGreen, 0.1)
                border.width: 1.5
                border.color: root.theme.lampGreen

                TelltaleIcon {
                    x: 8; y: 4
                    name: "cruise-control"
                    size: 18
                    color: root.theme.lampGreen
                }
                CText {
                    theme: root.theme; face: "mono"; cx: 58; cy: 13
                    text: qsTr("%1 km/h").arg(Number.isFinite(root.speed) ? root.fmt(Math.round(root.speed / 5) * 5, 0) : "–")
                    color: root.theme.lampGreen
                    font.pixelSize: 13
                }
            }
        }

        // ---- tachometer and gear (right) · EEC1 SPN 190 + ETC2 SPN 523 -------------------------------
        RadialGauge {
            id: tach
            x: 990 - width / 2
            y: 300 - height / 2
            theme: root.theme
            minimum: 0; maximum: 8000; majorStep: 1000; minorStep: 250
            redFrom: 6500
            labelDivisor: 1000
            segments: 64
            opacity: root.ign ? 1 : 0.5
            value: !root.ign ? 0 : root.bulb ? 8000 * root.sweep : root.rpm
            valid: root.ign && (root.bulb || Number.isFinite(root.rpm))

            readonly property bool hasGear: root.ign && Number.isFinite(root.gear)
            readonly property bool driving: tach.hasGear && root.gear > 0

            CText {
                theme: root.theme; face: "display"; cx: tach.width / 2; cy: tach.height / 2 - 84
                text: qsTr("rpm × 1000")
                color: root.theme.textMuted
                font.pixelSize: 16
                font.capitalization: Font.AllUppercase
                font.letterSpacing: 1.9
            }
            // Gear: "D" and the gear number side by side; "N" and "R" alone in the middle.
            CText {
                theme: root.theme; face: "display"
                cx: tach.width / 2 + (tach.driving ? -30 : 0); cy: tach.height / 2 - 21
                text: !tach.hasGear ? "–" : root.gear < 0 ? "R" : root.gear === 0 ? "N" : "D"
                font.pixelSize: 112
                font.weight: Font.DemiBold
                color: tach.hasGear ? root.theme.accent : root.theme.textFaint
            }
            CText {
                theme: root.theme; face: "display"
                cx: tach.width / 2 + 40; cy: tach.height / 2 - 21
                visible: tach.driving
                text: tach.driving ? Math.round(root.gear).toString() : ""
                font.pixelSize: 112
                font.weight: Font.DemiBold
            }
            CText {
                theme: root.theme; face: "mono"; cx: tach.width / 2; cy: tach.height / 2 + 44
                text: root.ign && Number.isFinite(root.rpm) ? qsTr("%1 rpm").arg(root.fmt(Math.round(root.rpm / 10) * 10, 0)) : qsTr("– – – rpm")
                font.pixelSize: 16
                color: root.ign && Number.isFinite(root.rpm) ? root.theme.text : root.theme.textFaint
            }
        }

        // ---- AI core, between the lenses --------------------------------------------------------------
        AiCore {
            x: 500; y: 95
            theme: root.theme
            opacity: root.ign ? 1 : 0.5
            live: root.ign && Number.isFinite(root.aiRegime)
            regime: root.aiRegime
            anomaly: root.num("aiAnomaly")
            confidence: root.num("aiConfidence")
            health: root.num("aiHealth")
            ambient: stage.ambientColor
        }

        // ---- turn indicators and hazard lamp ----------------------------------------------------------
        TurnArrow {
            x: 475; y: 113
            theme: root.theme
            lit: root.ign && (root.bulb || ((root.flag("lampLeft") || root.flag("lampHazard")) && root.blinkOn))
        }
        TurnArrow {
            x: 745; y: 113          // mirror of the left one about the middle of the stage (1280 - 475 - 60)
            mirrored: true
            theme: root.theme
            lit: root.ign && (root.bulb || ((root.flag("lampRight") || root.flag("lampHazard")) && root.blinkOn))
        }
        Lamp {
            x: 627; y: 62
            theme: root.theme
            icon: "hazard"
            tone: root.theme.lampRed
            lit: root.ign && (root.bulb || (root.flag("lampHazard") && root.blinkOn))
        }

        // ---- fluid bars ---------------------------------------------------------------------------------
        Column {
            x: 520; y: 328
            spacing: 8
            opacity: root.ign ? 1 : 0.5

            BarGauge {
                theme: root.theme
                icon: "coolant-temperature"; label: qsTr("Temperature")
                minimum: 40; maximum: 125; warnAt: 105; alarmAt: 115
                value: root.coolant
                valid: root.ign
                valueText: qsTr("%1 °C").arg(root.fmt(root.coolant, 0))
            }
            BarGauge {
                theme: root.theme
                icon: "fuel"; label: qsTr("Fuel")
                minimum: 0; maximum: 100; warnAt: 12; lowIsBad: true
                value: root.fuel
                valid: root.ign
                valueText: qsTr("%1 %").arg(root.fmt(root.fuel, 0))
            }
            BarGauge {
                theme: root.theme
                icon: "def"; label: qsTr("AdBlue")
                minimum: 0; maximum: 100; warnAt: 10; lowIsBad: true
                value: root.def
                valid: root.ign
                valueText: qsTr("%1 %").arg(root.fmt(root.def, 0))
            }
        }

        // ---- vehicle values (oil, battery, air, outside temperature) ------------------------------------
        Repeater {
            model: [
                { label: qsTr("Oil"), value: root.oil, text: qsTr("%1 bar").arg(root.fmt(root.oil / 100, 1)) },
                { label: qsTr("Battery"), value: root.battery, text: qsTr("%1 V").arg(root.fmt(root.battery, 1)) },
                { label: qsTr("Air"), value: root.airPressure, text: qsTr("%1 bar").arg(root.fmt(root.airPressure, 1)) },
                { label: qsTr("Outside"), value: root.ambientTemp, text: qsTr("%1 °C").arg(root.fmt(root.ambientTemp, 0)) }
            ]

            Item {
                id: cell
                required property var modelData
                required property int index

                readonly property bool has: root.ign && Number.isFinite(modelData.value)

                x: 520 + 60 * index
                width: 60
                CText {
                    theme: root.theme; face: "display"; cx: 30; cy: 451
                    text: cell.modelData.label
                    color: root.theme.textMuted
                    font.pixelSize: 10
                    font.capitalization: Font.AllUppercase
                    font.letterSpacing: 1.2
                }
                CText {
                    theme: root.theme; face: "mono"; cx: 30; cy: 468
                    text: cell.has ? cell.modelData.text : "–"
                    font.pixelSize: 12
                    color: cell.has ? root.theme.text : root.theme.textFaint
                }
                Rectangle {
                    visible: cell.index > 0
                    y: 441; width: 1; height: 32
                    color: root.theme.hairline
                }
            }
        }

        // ---- status bar: odometer · clock · engine hours --------------------------------------------------
        CText {
            theme: root.theme; face: "mono"; cx: 470; cy: 502
            text: Number.isFinite(root.odometer) ? qsTr("%1 km").arg(root.fmt(root.odometer, 1)) : qsTr("– km")
            color: Number.isFinite(root.odometer) ? root.theme.text : root.theme.textFaint
        }
        CText {
            theme: root.theme; face: "display"; cx: 640; cy: 502
            text: root.clockText
            font.pixelSize: 20
            font.weight: Font.DemiBold
        }
        CText {
            theme: root.theme; face: "mono"; cx: 810; cy: 502
            text: Number.isFinite(root.hours) ? qsTr("%1 h").arg(root.fmt(root.hours, 1)) : qsTr("– h")
            color: Number.isFinite(root.hours) ? root.theme.text : root.theme.textFaint
        }
        Rectangle { x: 560; y: 492; width: 1; height: 20; color: root.theme.hairline }
        Rectangle { x: 720; y: 492; width: 1; height: 20; color: root.theme.hairline }
    }
}
