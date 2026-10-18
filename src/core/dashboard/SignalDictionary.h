// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// What the loaded databases say, in the shape a dashboard wants it.
//
// A widget is configured by saying what it reads. By hand that is two names typed exactly as the
// database spells them, and then a unit, a range, a number of decimals and - for a gear selector or
// a risk level - the words for the values, all of which the database already has. This file is the
// part of the Dashboard that knows how to read it back out:
//
//   * **A dictionary.** Every signal of every loaded .dbc as one flat, searchable list, with what a
//     person choosing one needs to see: unit, range, how often its message is sent, and its value
//     table. Copied out of the databases, so it does not depend on them staying alive.
//
//   * **A suggestion.** Which widget a signal wants (a two-state signal is a lamp, a signal with a
//     table of names is a state, a percentage is a bar, a speed is a gauge), and the settings to
//     give it. Rules, not guesses: each is written down below and pinned by a test, so that "why
//     did it make a numeric of that" has an answer in one place.
//
//   * **An audit.** Whether a widget's binding names something that exists, and when it does not,
//     the nearest thing that does. The description itself never asks - see
//     DashboardDescription::validate - because refusing to open a project over a database that is
//     on another machine would be the wrong trade. This asks, for a person to read.
//
// No Qt, like the rest of the core, so the same rules answer the form that configures a widget, the
// drop of a signal onto the Dashboard, and a test.

#pragma once

#include "core/dashboard/DashboardDescription.h"
#include "core/database/CanMessage.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace torquebus {

/// One signal, as the dictionary lists it.
struct DictionaryEntry final {
    std::string message;
    std::string signal;

    std::string unit;

    /// From the signal's `CM_`, and from its message's.
    std::string comment;
    std::string messageComment;

    /// The node that sends the message.
    std::string transmitter;

    /// The file the definition came from, without its folder.
    std::string database;

    std::uint32_t identifier{0};
    CanFrameFormat format{CanFrameFormat::Standard};

    /// How often the message is sent, from its `GenMsgCycleTime`. Zero: the database does not say.
    std::uint32_t cycleTimeMs{0};

    std::uint16_t bitLength{1};
    bool isSigned{false};

    double factor{1.0};
    double offset{0.0};

    /// The physical range. What the database declares when it declares one, and what the bits can
    /// hold otherwise - a .dbc that says [0|0] is not saying the signal is always zero.
    double minimum{0.0};
    double maximum{0.0};

    /// Whether the range above was written in the database, and not worked out from the bit length.
    /// A declared range is a statement about the signal; a derived one is arithmetic.
    bool declaredRange{false};

    /// The value table, as the database holds it: by raw value.
    std::vector<SignalValueName> values;

    /// "Message.Signal" - the name a signal has in the plot store, and so the one a widget reads.
    [[nodiscard]] std::string qualified() const { return message + "." + signal; }

    /// What a raw value of this signal reads as: raw * factor + offset, with the decoder's own
    /// arithmetic so that a state's value matches the number the widget is given, bit for bit.
    [[nodiscard]] double physical(std::int64_t raw) const noexcept
    {
        return static_cast<double>(raw) * factor + offset;
    }
};

// --- Rules ---------------------------------------------------------------------------------

/// A name for a person: "EngineCoolantTemperature" is "Engine Coolant Temperature" and
/// "ABSAmberWarning" is "ABS Amber Warning". Words split where the case turns, at underscores and
/// dots; an acronym stays whole, and a digit stays with the word it follows.
[[nodiscard]] std::string prettyName(std::string_view identifier);

/// A widget id for a signal: "EngineSpeed" is "engine_speed". Lowercase words joined by
/// underscores, and never empty.
[[nodiscard]] std::string idFor(std::string_view signalName);

/// What a name for a value suggests about it. "Error", "Fault" and "Critical" are danger; "Warning"
/// and "High" are attention; "Running" and "Engaged" are good; "Off", "Idle" and "NotAvailable" are
/// quiet; everything else has no opinion. A starting point to edit, not a judgement: the same word
/// means different things on different signals, so only the words that rarely do are listed.
[[nodiscard]] DashboardTone inferTone(std::string_view name);

/// Decimals to show for a signal that changes in steps of `factor`: enough to tell one step from
/// the next, and no more than two. A step of 0.125 shows one decimal and a step of 0.05 two, and a
/// step of 1 or more none; nobody reads an engine speed to the third place.
[[nodiscard]] int decimalsFor(double factor);

/// How long a signal sent every `cycleMs` may stay silent before it is called stale: three of its
/// periods, since a message that missed one has probably only been late, but never less than half a
/// second - a 20 ms message is not stale at 60 ms, it is a bus having a busy moment - and never
/// more than a minute.
[[nodiscard]] std::uint32_t staleAfterMs(std::uint32_t cycleMs);

/// The words for a signal's values, as states of a widget: physical value, name, and the tone the
/// name suggests.
[[nodiscard]] std::vector<DashboardState> statesOf(const DictionaryEntry& entry);

/// Which widget a signal wants.
///
///   1. A value table that names exactly 0 and 1 is a **Lamp**: two states, and a lamp knows them.
///      Any other value table is a **State**, which shows the names.
///   2. A signal of one or two bits is a **Lamp**.
///   3. A percentage is a **Bar**.
///   4. A unit that counts time, distance or turns is a **Numeric**: a dial of an odometer is a
///   dial
///      that never moves.
///   5. A signal whose range was declared, whose span is under a hundred thousand and which has a
///      unit is a **Gauge**.
///   6. Everything else is a **Numeric**, which is correct for anything.
[[nodiscard]] DashboardWidgetKind suggestKind(const DictionaryEntry& entry);

/// The kinds a signal can be shown as, in the order a person would want to be offered them. The
/// first is what suggestKind says, if it is among them.
[[nodiscard]] std::vector<DashboardWidgetKind> readerKinds();

/// A widget for a signal: the kind it wants, or `kind` when a person chose one (a kind that cannot
/// show a signal is ignored in favour of the suggestion), bound to it and configured from what the
/// database says. Its id is only a base - uniqueId() makes it unique - and its position is the
/// caller's.
[[nodiscard]] DashboardWidget suggestWidget(const DictionaryEntry& entry,
                                            std::optional<DashboardWidgetKind> kind = std::nullopt);

/// A table of `message`, tall enough for `signalCount` rows (up to a screenful).
[[nodiscard]] DashboardWidget suggestTable(std::string_view message, std::size_t signalCount);

/// Binds an existing widget to a signal and fills in what the database knows that the kind uses:
/// unit, range, decimals, the names of the values, and how long the signal may be silent. Its
/// title is replaced only when it is empty or is the one the previous binding would have given it -
/// a title somebody typed is theirs. The widget's size, picture and limits are left alone.
void applyEntry(DashboardWidget& widget, const DictionaryEntry& entry);

// --- Audit ---------------------------------------------------------------------------------

/// Whether a binding names something the loaded databases have.
struct BindingFinding final {
    enum class Status : std::uint8_t {
        /// Not asked: no database is loaded, or the binding is not a signal or a message.
        NotChecked,

        /// There.
        Found,

        /// The message is in no database.
        UnknownMessage,

        /// The message is there and has no such signal.
        UnknownSignal,
    };

    Status status{Status::NotChecked};

    /// For Found on a signal.
    const DictionaryEntry* entry{nullptr};

    /// The nearest thing that does exist, when there is one close enough to be a typing mistake or
    /// a case difference: a message, with the signal when it was the signal that gave it away.
    std::string suggestedMessage;
    std::string suggestedSignal;
};

/// A widget whose binding the databases do not bear out.
struct WidgetFinding final {
    std::string widgetId;
    BindingFinding finding;
};

/// The signals of the loaded databases.
class SignalDictionary final {
public:
    SignalDictionary() = default;

    explicit SignalDictionary(std::span<const std::shared_ptr<const CanDatabase>> databases)
    {
        rebuild(databases);
    }

    /// Replaces everything with what `databases` hold now. Databases that are null are skipped.
    void rebuild(std::span<const std::shared_ptr<const CanDatabase>> databases);

    [[nodiscard]] bool empty() const noexcept { return m_entries.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return m_entries.size(); }

    /// In the order the databases and messages list them.
    [[nodiscard]] const std::vector<DictionaryEntry>& entries() const noexcept { return m_entries; }

    /// The first signal of that name in that message, or nullptr. Case matters, as it does to the
    /// plot store the widget reads from.
    [[nodiscard]] const DictionaryEntry* find(std::string_view message,
                                              std::string_view signal) const noexcept;

    [[nodiscard]] bool hasMessage(std::string_view message) const noexcept;

    /// The signals of a message, in the database's order.
    [[nodiscard]] std::vector<const DictionaryEntry*> signalsOf(std::string_view message) const;

    /// Message names, sorted and without repeats.
    [[nodiscard]] std::vector<std::string> messageNames() const;

    /// The entries that match `text`, best first.
    ///
    /// Words are separated by spaces or dots and every one has to match. A word matches a signal
    /// name best when it is the name, then when the name starts with it, then when it starts the
    /// message name, a word inside the name, the initials of the name's words ("ect" finds
    /// EngineCoolantTemperature), anywhere in the name, in the message name, the unit, and last the
    /// comment. Ties keep the databases' order. Empty text lists the first `limit`.
    [[nodiscard]] std::vector<const DictionaryEntry*> search(std::string_view text,
                                                             std::size_t limit = 200) const;

    /// Whether the binding names something here, and what the nearest thing is when it does not.
    [[nodiscard]] BindingFinding check(const DashboardBinding& binding) const;

    /// The widgets of `dashboard` whose binding is not found. Empty when nothing is loaded - an
    /// empty dictionary cannot say a widget is wrong.
    [[nodiscard]] std::vector<WidgetFinding> audit(const DashboardDescription& dashboard) const;

private:
    /// What search() compares against, lowercased once.
    struct Prepared final {
        std::string signal;
        std::string message;
        std::string unit;
        std::string comment;
        std::string initials;

        /// The words of the signal's name, lowercased.
        std::vector<std::string> words;
    };

    std::vector<DictionaryEntry> m_entries;
    std::vector<Prepared> m_prepared;

    /// "Message.Signal" to the first entry of that name.
    std::unordered_map<std::string, std::size_t> m_byName;

    /// Message to its entries, in order.
    std::unordered_map<std::string, std::vector<std::size_t>> m_byMessage;
};

} // namespace torquebus
