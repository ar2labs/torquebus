// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// The colours and fonts of the instrument cluster, as one typed object.
//
// The raw properties mirror the roles of `Theme` (ui/theme/Theme.h), one for one and
// with the same names; the host fills them from the active theme and refills them when
// it changes. Everything below them is derived here, so the cluster has no palette of
// its own to keep in step with the application's, and the accent the user picked reaches
// it. The preview, which cannot see C++, fills the raw properties from fixed values.
import QtQuick

QtObject {
    id: root

    // --- Mirrors of Theme --------------------------------------------------
    property color text
    property color textMuted
    property color accent
    property color accentHover
    property color border
    property color canvas
    property color tabStrip
    property color warning
    property color error
    property color success
    property color lampRed
    property color lampAmber
    property color lampGreen
    property color lampBlue
    property color instrumentScreen
    property color instrumentBezel

    // --- Fonts, from the application ---------------------------------------
    property string displayFamily: Application.font.family   // big numbers and headings
    property string bodyFamily: Application.font.family      // sentences
    property string monoFamily: Application.font.family      // values with units

    // --- Derived -----------------------------------------------------------
    // "No data" dashes: the muted text, one step further back.
    readonly property color textFaint: Qt.alpha(root.textMuted, 0.55)
    // An unlit segment is a recessed surface, the same one the tab strip is.
    readonly property color segmentOff: root.tabStrip
    readonly property color hairline: Qt.alpha(root.border, 0.6)
    readonly property color lampOff: Qt.tint(root.tabStrip, Qt.alpha(root.textMuted, 0.2))

    // The glass and the housing are lit from above, and how that looks depends on the face. On a
    // dark one the glass is nearly black and the housing is shaded almost to black underneath, with
    // only a hint of light on its top edge. On a light one the housing is lit to white on top and
    // only slightly shaded below, and the glass is near white. The amounts below reproduce the
    // approved design on both themes.
    readonly property bool lightFace: root.instrumentScreen.hslLightness > 0.5

    readonly property color glassTop: Qt.tint(root.instrumentScreen, root.lightFace ? "#ffffffff" : "#08ffffff")
    readonly property color glassBottom: Qt.tint(root.instrumentScreen, root.lightFace ? "#08000000" : "#57000000")
    // Glare on the glass: whichever of the text and the glass is brighter, mostly transparent.
    readonly property color glare: Qt.alpha(root.text.hslLightness > root.instrumentScreen.hslLightness
                                            ? root.text : root.instrumentScreen, 0.08)

    // The housing, from its mid-tone: the lit top of the ring, the line of light along the glass,
    // a shaded underside, and a rim around the whole.
    readonly property color bezelHighlight: Qt.tint(root.instrumentBezel, root.lightFace ? "#ffffffff" : "#30ffffff")
    readonly property color bezelEdge: Qt.tint(root.instrumentBezel, root.lightFace ? "#ffffffff" : "#1dffffff")
    readonly property color bezelLow: Qt.tint(root.instrumentBezel, root.lightFace ? "#2b000000" : "#99000000")
    readonly property color bezelRim: Qt.tint(root.instrumentBezel, root.lightFace ? "#52000000" : "#d9000000")

    // What the cluster casts on the dashboard: darker than the canvas, and much fainter on a light
    // one, where a black shadow would look like a stain.
    readonly property color shadow: Qt.darker(root.canvas, 4)
    readonly property real shadowOpacity: root.lightFace ? 0.22 : 0.6

    // The lit part of a gauge runs from soft to strong. "Strong" is whichever of the accent and
    // its hover tone stands further from the glass, which is the hover tone on a dark one and the
    // accent itself on a light one, for any accent the user chooses.
    readonly property color litEnd: Math.abs(root.accent.hslLightness - root.instrumentScreen.hslLightness)
                                    >= Math.abs(root.accentHover.hslLightness - root.instrumentScreen.hslLightness)
                                    ? root.accent : root.accentHover
    readonly property color litStart: Qt.tint(root.litEnd, Qt.alpha(root.instrumentScreen, 0.5))

    // A point on the soft-to-strong ramp, `t` from 0 to 1.
    function ramp(t) {
        return Qt.tint(root.litStart, Qt.alpha(root.litEnd, t))
    }
}
