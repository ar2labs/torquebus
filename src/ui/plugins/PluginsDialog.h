// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// What loaded, what did not, and why.
//
// The Output panel already carries every one of these lines as they happen, and
// that is where somebody sees them the first time. This is where they look the
// second time - after a backend they expected is missing from Hardware
// Configuration and the startup messages have scrolled away behind an hour of
// bus traffic.
//
// It also answers the question PLAN.md section 31 raised, which the Output
// panel answers badly because it is a scroll of text: is the vendor driver not
// installed, or installed and failing? Those are two problems with two
// different fixes, and the difference is in the reason string.
//
// Nothing here is actionable in the dialog itself. There is no enable, no
// disable and no reload: a loaded plugin has left function pointers inside the
// registries and is never unloaded, so a switch here would be a lie about what
// the program can do. See PluginLoader.h.

#pragma once

#include <QDialog>

namespace torquebus::plugins {
class PluginLoader;
}

namespace torquebus::ui {

class PluginsDialog final : public QDialog {
    Q_OBJECT

public:
    /// `loader` is read once, here, and not kept: loading has already finished
    /// by the time any window exists, and nothing adds to it afterwards.
    explicit PluginsDialog(const plugins::PluginLoader& loader, QWidget* parent = nullptr);
};

} // namespace torquebus::ui
