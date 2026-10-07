// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// A pretend vehicle for previewing the cluster without TorqueBus: a driving cycle with an
// 8-speed gearbox, temperature, fuel and an AI that reacts to thermal drift (the same idea as the
// tinyml.ecu node). The property names are the ClusterView contract. It only serves the preview;
// in the application the ClusterDataSource fills the contract.
import QtQuick

QtObject {
    id: root

    // preview controls
    property bool cycle: true
    property bool overheat: false
    property bool forceAnomaly: false
    property string profile: "full"     // full: everything arrives; example11: only speed, temperature and the AI

    // the contract
    property bool ignition: true
    property real speed: 72
    property real rpm: 1210
    property real gear: 8
    property real coolant: 82
    property real oil: 380
    property real fuel: 64
    property real def: 48
    property real battery: 27.6
    property real airPressure: 8.4
    property real ambient: 24
    property real odometer: 123456.7
    property real hours: 8412.3
    property real aiRegime: 1
    property real aiAnomaly: 4
    property real aiConfidence: 96
    property real aiHealth: 96
    property bool cruise: true
    property bool lampHigh: false
    property bool lampLow: true
    property bool lampPosition: true
    property bool lampPark: false
    property bool lampBelt: false
    property bool lampEngine: false
    property bool lampOil: false
    property bool lampAbs: false
    property bool lampLeft: false
    property bool lampRight: false
    property bool lampHazard: false
    property bool dmStop: false
    property bool dmWarn: false
    property bool dmMil: false
    property bool dmProtect: false

    // ---- driving cycle ---------------------------------------------------------------------------
    readonly property var ratios: [0, 4.7, 3.1, 2.1, 1.7, 1.3, 1.0, 0.82, 0.67]   // rpm = km/h x ratio x 25
    readonly property var phases: [
        { t: 5, throttle: 0, brake: 0, park: true },
        { t: 30, throttle: 0.7, brake: 0 },
        { t: 14, throttle: 0.28, brake: 0, cruise: true },
        { t: 6, throttle: 0.28, brake: 0, turn: "left", cruise: true },
        { t: 10, throttle: 0.85, brake: 0 },
        { t: 12, throttle: 0, brake: 0.5, turn: "right" },
        { t: 4, throttle: 0, brake: 0.2 }
    ]
    property int phaseIdx: 2            // starts in cruise, consistent with 72 km/h and 82 °C
    property real phaseT: 0

    function rpmFor(kmh, g) {
        return g <= 0 ? 800 : Math.max(800, kmh * ratios[g] * 25)
    }

    function stepCycle(dt) {
        var p = phases[phaseIdx]
        phaseT += dt
        if (phaseT > p.t) {
            phaseT = 0
            phaseIdx = (phaseIdx + 1) % phases.length
        }
        var accel = p.throttle * 2.2 - p.brake * 5.5 - 0.0009 * speed * speed
        speed = Math.max(0, Math.min(190, speed + accel * dt))
        if (p.park)
            speed = 0
        if (speed < 1) {
            gear = p.park ? 0 : 1
        } else {
            while (gear < 8 && rpmFor(speed, gear) > 3000) gear++
            while (gear > 1 && rpmFor(speed, gear) < 1300) gear--
        }
        cruise = !!p.cruise
        lampLeft = p.turn === "left"
        lampRight = p.turn === "right"
        lampPark = !!p.park
    }

    // Temperature and the AI, which every profile has.
    function stepThermalAndAi(dt) {
        var coolTarget = overheat ? 116 : 70 + 0.12 * speed + (cycle ? 4 : 0)
        if (cycle || overheat)
            coolant += (coolTarget - coolant) * Math.min(1, dt / 8)
        // AI: drift from the nominal curve 70 + 0.12 * v
        var thermalDelta = coolant - (70 + 0.12 * speed)
        var anomaly = Math.max(2, Math.min(100, 4 + Math.max(0, thermalDelta) * 4))
        if (forceAnomaly)
            anomaly = Math.max(anomaly, 88)
        aiAnomaly += (anomaly - aiAnomaly) * Math.min(1, dt * 2)
        aiHealth = Math.max(0, Math.min(100, 100 - Math.max(0, thermalDelta) * 3.2))
        aiConfidence = 92 + 6 * Math.sin(Date.now() / 2300)
        aiRegime = aiAnomaly >= 65 ? 4 : thermalDelta > 12 ? 3 : speed < 1 ? 0
                   : (profile === "full" && cycle && phases[phaseIdx].throttle > 0.6) ? 2 : 1
    }

    // Everything else, only on the full profile.
    function stepRest(dt) {
        rpm = ignition ? rpmFor(speed, gear) : 0
        if (cycle && speed > 0) {
            odometer += speed * dt / 3600
            fuel = Math.max(0, fuel - dt * 0.08)
            def = Math.max(0, def - dt * 0.02)
        }
        if (ignition)
            hours += dt / 3600
    }

    // The data a profile does not carry becomes "no data" (NaN).
    function applyProfile() {
        if (profile === "example11") {
            rpm = NaN; gear = NaN; fuel = NaN; def = NaN; oil = NaN; battery = NaN; airPressure = NaN
            ambient = NaN; odometer = NaN; hours = NaN
        } else {
            rpm = 1210; gear = 8; fuel = 64; def = 48; oil = 380; battery = 27.6; airPressure = 8.4
            ambient = 24; odometer = 123456.7; hours = 8412.3
        }
    }
    onProfileChanged: applyProfile()

    property Timer clock: Timer {
        interval: 50
        running: true
        repeat: true
        onTriggered: {
            if (root.ignition && root.cycle)
                root.stepCycle(0.05)
            root.stepThermalAndAi(0.05)
            if (root.profile === "full")
                root.stepRest(0.05)
        }
    }
}
