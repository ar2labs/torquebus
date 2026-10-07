// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// One icon of the telltale set, in the colour asked for.
//
// The drawing is a monochrome SVG in resources/icons (cluster-<name>.svg), like the rest of the
// application's icons, and IconImageProvider paints it in `color` - see ui/theme/IconImageProvider.h
// for the address it takes. The provider is registered by whoever owns the QML engine, under the
// name kIconProviderId, "torquebus-icons".
import QtQuick

Item {
    id: root

    property string name                // "hazard" for cluster-hazard.svg
    property color color: "white"
    property real size: 26              // the width, as drawn
    property real aspectRatio: 1        // width / height of the drawing
    property bool mirrored: false

    // The icon is rasterised this many times larger than it is drawn and scaled down with
    // mipmaps. The cluster scales itself to the widget it sits in, and a picture made at the
    // size of the first frame would go soft the first time the widget was made bigger.
    readonly property int oversample: 3

    width: root.size
    height: root.size / root.aspectRatio

    Image {
        anchors.fill: parent
        source: root.name !== "" ? "image://torquebus-icons/cluster-" + root.name + "/" + String(root.color).substring(1) : ""
        sourceSize: Qt.size(root.width * root.oversample, root.height * root.oversample)
        fillMode: Image.PreserveAspectFit
        mirror: root.mirrored
        mipmap: true
        smooth: true
    }
}
