// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// What a signal is when it is being dragged.
//
// The DBC Explorer puts it on the drag and the Dashboard takes it off the drop, and neither knows
// the other: this is the whole of what they agree on. A reference is a message name and a signal
// name - the same two strings a widget's binding holds - and not a pointer into a database, which
// the receiver does not own and which would dangle the moment a .dbc was reloaded between the press
// and the release. An empty signal means the message itself, which a Table is made from.
//
// The same drag also carries the references as plain text, "Message.Signal" a line, so that
// dropping one on a text field or an editor says what it was rather than doing nothing.

#pragma once

#include <QByteArray>
#include <QList>
#include <QString>

class QMimeData;

namespace torquebus::ui {

/// The format of a drag of signals and messages. Versioned in the name, since a format that has to
/// change gets a new one rather than a receiver that guesses.
inline constexpr char kSignalMimeType[] = "application/x-torquebus-signals-v1";

struct SignalReference final {
    QString message;

    /// Empty: the message as a whole.
    QString signal;

    [[nodiscard]] friend bool operator==(const SignalReference&, const SignalReference&) = default;
};

/// The references as the drag carries them: a JSON array of {"message", "signal"}.
[[nodiscard]] QByteArray encodeSignalReferences(const QList<SignalReference>& references);

/// The references in `data`. Whatever is not a well-formed entry is skipped - data from a drag is
/// data from another program, and a malformed one is a drop that does nothing, not one that fails.
[[nodiscard]] QList<SignalReference> decodeSignalReferences(const QByteArray& data);

/// A drag carrying `references`, in the format above and as text. The caller owns it, which in
/// practice means a QDrag does.
[[nodiscard]] QMimeData* makeSignalMimeData(const QList<SignalReference>& references);

[[nodiscard]] bool hasSignalReferences(const QMimeData* data);

/// Empty for a drop that carries none.
[[nodiscard]] QList<SignalReference> signalReferencesOf(const QMimeData* data);

} // namespace torquebus::ui
