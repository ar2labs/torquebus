// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// Where a signal's history lives while you plot it.
//
// The counterpart to TraceStore, and shaped by a different number. The trace
// keeps a million *frames* because a person scrolls through them one at a time.
// A plot is about eight hundred pixels wide, so what it needs is not every
// sample ever decoded - it is enough recent samples that the line through them
// is the line the bus actually drew.
//
// That difference drives everything here:
//
//   * **One ring per signal, not one stream of everything.** A plot draws
//     `EngineSpeed` as a line; finding its samples in a mixed stream would mean
//     a filtered walk per repaint, over the samples of forty other signals.
//
//   * **Bounded per signal, and the oldest goes first.** A measurement left
//     running overnight must not decide how much memory the application uses.
//     Discards are counted, so "the plot starts at 09:14" is a number the panel
//     can show rather than a mystery.
//
//   * **Names are interned once.** A DecodedSignal points at its definition and
//     carries no string (see DecodedSignal.h), and the definition's lifetime is
//     the database's. A series that stored the pointer would dangle the moment
//     a .dbc was reloaded mid-measurement - which is an ordinary thing to do.
//     So a signal is identified by an integer here, and the string that integer
//     stands for is copied once, when the signal is first seen.
//
// Not thread safe, by the same arrangement as TraceStore: written from the
// pipeline executor thread, read from the GUI thread, with the ui layer
// mediating - it copies the window it needs rather than reaching in while the
// executor appends.

#pragma once

#include "core/database/DecodedSignal.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace torquebus {

/// One sample: when, and what.
///
/// 16 bytes, and deliberately no more. At the default capacity a signal costs
/// 128 KB, and a project watching forty of them costs 5 MB - a number worth
/// being able to state, which is why the raw value and the truncation flag that
/// DecodedSignal carries are not here. A plot draws the physical value; the
/// trace is where the bits are.
struct SignalSample final {
    std::uint64_t timestampNs{};
    double value{};
};

static_assert(sizeof(SignalSample) == 16,
              "A sample is two words. Adding a field here multiplies by the "
              "capacity of every series in the project.");

/// Identifies one signal within a series store. Stable for the store's life.
using SeriesId = std::size_t;

/// Not a signal. Returned by find() when nothing of that name has been seen.
inline constexpr SeriesId kNoSeries = static_cast<SeriesId>(-1);

/// One signal's recent history.
class SignalSeries final {
public:
    explicit SignalSeries(std::string name, std::string unit, std::size_t capacity);

    [[nodiscard]] const std::string& name() const noexcept { return m_name; }
    [[nodiscard]] const std::string& unit() const noexcept { return m_unit; }

    void append(std::uint64_t timestampNs, double value);

    [[nodiscard]] std::size_t size() const noexcept { return m_size; }
    [[nodiscard]] std::size_t capacity() const noexcept { return m_samples.size(); }
    [[nodiscard]] bool empty() const noexcept { return m_size == 0; }

    /// Samples dropped because the ring wrapped. The panel says so rather than
    /// letting a plot silently begin part-way through a measurement.
    [[nodiscard]] std::uint64_t discarded() const noexcept { return m_discarded; }

    /// Sample `index`, oldest first. Undefined past size().
    [[nodiscard]] const SignalSample& at(std::size_t index) const noexcept
    {
        return m_samples[(m_first + index) % m_samples.size()];
    }

    [[nodiscard]] const SignalSample& oldest() const noexcept { return at(0); }
    [[nodiscard]] const SignalSample& newest() const noexcept { return at(m_size - 1); }

    /// Copies the samples at or after `sinceNs`, oldest first, into `out`.
    ///
    /// Returns how many were copied, capped at `out.size()`. A span rather than
    /// a vector because this runs on every repaint and a plot owns its own
    /// buffer: at 60 Hz, a vector per repaint per signal is an allocation the
    /// GUI thread does not need to make.
    ///
    /// When there are more samples in range than `out` can hold, the *newest*
    /// are kept - a plot that fell behind should show what is happening now,
    /// not where the window began.
    [[nodiscard]] std::size_t copySince(std::uint64_t sinceNs,
                                        std::span<SignalSample> out) const;

    /// The lowest and highest value in the whole series.
    ///
    /// Tracked as samples arrive rather than scanned on demand: an axis is
    /// recomputed every repaint, and a scan would be O(capacity) per signal per
    /// frame drawn.
    ///
    /// A wrapped ring makes these an over-estimate - a discarded peak still
    /// counts - and that is the right way to be wrong. An axis that shrank as
    /// history fell off the back would make a steady signal look like it was
    /// growing.
    [[nodiscard]] double minimum() const noexcept { return m_minimum; }
    [[nodiscard]] double maximum() const noexcept { return m_maximum; }

    void clear();

private:
    std::string m_name;
    std::string m_unit;

    std::vector<SignalSample> m_samples;
    std::size_t m_first{0};
    std::size_t m_size{0};
    std::uint64_t m_discarded{0};

    double m_minimum{0.0};
    double m_maximum{0.0};
};

/// Every signal a measurement has decoded, each with its own history.
class SignalSeriesStore final {
public:
    /// 8192 samples each: about 80 seconds of a 100 Hz signal, or thirteen
    /// minutes of a 10 Hz one. Enough that a plot scrolled back a little still
    /// has data, small enough that forty signals cost a few megabytes.
    static constexpr std::size_t kDefaultCapacity = 8192;

    explicit SignalSeriesStore(std::size_t capacityPerSignal = kDefaultCapacity);

    /// Records a batch. Signals not seen before get a series.
    ///
    /// Truncated samples are dropped rather than plotted as zero. A frame too
    /// short to hold the signal did not report zero, it reported nothing, and a
    /// line through those zeroes is a measurement that never happened - which
    /// is the distinction DecodedSignal::truncated exists to preserve.
    void append(std::span<const DecodedSignal> batch);

    /// The series for "MessageName.SignalName", or kNoSeries.
    [[nodiscard]] SeriesId find(std::string_view qualifiedName) const;

    [[nodiscard]] std::size_t seriesCount() const noexcept { return m_series.size(); }

    [[nodiscard]] const SignalSeries& series(SeriesId id) const { return m_series[id]; }

    /// Every series, in the order the signals were first seen - so a legend's
    /// order is the order the bus introduced them rather than a hash.
    [[nodiscard]] std::span<const SignalSeries> all() const noexcept { return m_series; }

    /// Samples dropped because a signal's ring wrapped, across every series.
    [[nodiscard]] std::uint64_t discarded() const;

    /// Samples refused because the frame was too short to hold the signal.
    [[nodiscard]] std::uint64_t truncated() const noexcept { return m_truncated; }

    /// Empties every series but keeps them, so a plot's selection and colours
    /// survive a restart of the measurement.
    void clearSamples();

    /// Forgets the signals as well. For a new project rather than a new run.
    void reset();

private:
    /// "EngineData.EngineSpeed" - the message qualifies the signal, because two
    /// databases can use one name for two different things and a plot legend
    /// showing "Speed" twice would be unreadable.
    [[nodiscard]] static std::string qualify(const DecodedSignal& signal);

    std::size_t m_capacity;
    std::vector<SignalSeries> m_series;

    /// Name to index. The hot path looks up once per signal per frame, so this
    /// is the one map that matters here.
    std::unordered_map<std::string, SeriesId> m_index;

    std::uint64_t m_truncated{0};
};

} // namespace torquebus
