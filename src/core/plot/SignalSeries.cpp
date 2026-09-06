// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "core/plot/SignalSeries.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace torquebus {

// ---------------------------------------------------------------------------
// SignalSeries
// ---------------------------------------------------------------------------

SignalSeries::SignalSeries(std::string name, std::string unit, std::size_t capacity)
    : m_name{std::move(name)}
    , m_unit{std::move(unit)}
    , m_samples(std::max<std::size_t>(capacity, 1))
{
}

void SignalSeries::append(std::uint64_t timestampNs, double value)
{
    const std::size_t capacity = m_samples.size();

    if (m_size == capacity) {
        // Full: the oldest goes, and is counted. Overwriting silently is how a
        // plot ends up starting at 09:14 with nothing saying why.
        m_samples[m_first] = SignalSample{timestampNs, value};
        m_first = (m_first + 1) % capacity;
        ++m_discarded;
    } else {
        m_samples[(m_first + m_size) % capacity] = SignalSample{timestampNs, value};

        if (m_size == 0) {
            // The first sample *is* the range. Starting from
            // ±infinity and letting the comparisons below settle it would work
            // and would leave an empty series claiming a range it never had.
            m_minimum = value;
            m_maximum = value;
        }

        ++m_size;
    }

    m_minimum = std::min(m_minimum, value);
    m_maximum = std::max(m_maximum, value);
}

std::size_t SignalSeries::copySince(std::uint64_t sinceNs, std::span<SignalSample> out) const
{
    if (m_size == 0 || out.empty()) {
        return 0;
    }

    // Samples arrive in time order, so the first one in range can be found by
    // bisection rather than by walking. A ring is not contiguous, so the search
    // is over indices and reads through at().
    std::size_t low = 0;
    std::size_t high = m_size;

    while (low < high) {
        const std::size_t middle = low + (high - low) / 2;
        if (at(middle).timestampNs < sinceNs) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }

    const std::size_t available = m_size - low;
    const std::size_t wanted = std::min(available, out.size());

    // The newest when there are too many: a plot that fell behind should show
    // what is happening now, not where the window began.
    const std::size_t start = low + (available - wanted);

    for (std::size_t index = 0; index < wanted; ++index) {
        out[index] = at(start + index);
    }

    return wanted;
}

void SignalSeries::clear()
{
    m_first = 0;
    m_size = 0;
    m_discarded = 0;
    m_minimum = 0.0;
    m_maximum = 0.0;
}

// ---------------------------------------------------------------------------
// SignalSeriesStore
// ---------------------------------------------------------------------------

SignalSeriesStore::SignalSeriesStore(std::size_t capacityPerSignal)
    : m_capacity{std::max<std::size_t>(capacityPerSignal, 1)}
{
}

std::string SignalSeriesStore::qualify(const DecodedSignal& signal)
{
    std::string qualified;
    qualified.reserve(signal.messageName().size() + signal.name().size() + 1);

    qualified.append(signal.messageName());
    qualified.push_back('.');
    qualified.append(signal.name());

    return qualified;
}

void SignalSeriesStore::append(std::span<const DecodedSignal> batch)
{
    for (const DecodedSignal& decoded : batch) {
        if (decoded.truncated) {
            // Not a zero. The frame was too short to hold this signal, so
            // nothing was measured - and a line drawn through those zeroes is a
            // measurement that never happened.
            ++m_truncated;
            continue;
        }

        if (decoded.signal == nullptr) {
            continue;
        }

        const std::string qualified = qualify(decoded);

        const auto existing = m_index.find(qualified);
        if (existing != m_index.end()) {
            m_series[existing->second].append(decoded.timestampNs, decoded.value);
            continue;
        }

        // First sighting. The name and unit are copied here, once, and never
        // read from the definition again - which is what lets a database be
        // reloaded mid-measurement without this store dangling.
        const SeriesId id = m_series.size();
        m_series.emplace_back(qualified, std::string{decoded.unit()}, m_capacity);
        m_index.emplace(qualified, id);

        m_series[id].append(decoded.timestampNs, decoded.value);
    }
}

SeriesId SignalSeriesStore::find(std::string_view qualifiedName) const
{
    // A string from the view: the map is keyed by std::string with no
    // transparent comparator, and this is a panel-side lookup rather than a
    // per-sample one.
    const auto found = m_index.find(std::string{qualifiedName});
    return found != m_index.end() ? found->second : kNoSeries;
}

std::uint64_t SignalSeriesStore::discarded() const
{
    std::uint64_t total = 0;
    for (const SignalSeries& series : m_series) {
        total += series.discarded();
    }
    return total;
}

void SignalSeriesStore::clearSamples()
{
    for (SignalSeries& series : m_series) {
        series.clear();
    }

    m_truncated = 0;
}

void SignalSeriesStore::reset()
{
    m_series.clear();
    m_index.clear();
    m_truncated = 0;
}

} // namespace torquebus
