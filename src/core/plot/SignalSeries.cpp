// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "core/plot/SignalSeries.h"

#include <algorithm>
#include <cmath>
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
{ }

void SignalSeries::append(std::uint64_t timestampNs, double value)
{
    if (m_size > 0 && timestampNs < newest().timestampNs) {
        // Time went backwards (e.g. a replay looped or a measurement restarted
        // at t = 0 without an explicit clear). Clear the previous epoch's
        // samples so copySince()'s binary search always walks a monotonically
        // non-decreasing ring.
        clear();
    }

    const std::size_t capacity = m_samples.size();

    if (m_size == capacity) {
        // Full: the oldest goes, and is counted. Overwriting silently is how a
        // plot ends up starting at 09:14 with nothing saying why.
        m_samples[m_first] = SignalSample{timestampNs, value};
        m_first = (m_first + 1) % capacity;
        ++m_discarded;
    } else {
        m_samples[(m_first + m_size) % capacity] = SignalSample{timestampNs, value};
        ++m_size;
    }

    // The range follows the values that are numbers. A J1939 signal reports "not available" as NaN,
    // and a NaN let into the range stays there: std::min and std::max with a NaN on the left return
    // it, so a series whose first sample was a missing one would have no axis for the rest of the
    // measurement - every later number compared against it and lost.
    if (!std::isfinite(value)) {
        return;
    }

    if (!m_hasRange) {
        // The first number *is* the range. Starting from ±infinity and letting
        // the comparisons below settle it would work and would leave an empty
        // series claiming a range it never had.
        m_minimum = value;
        m_maximum = value;
        m_hasRange = true;
        return;
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
    m_hasRange = false;
}

// ---------------------------------------------------------------------------
// SignalSeriesStore
// ---------------------------------------------------------------------------

SignalSeriesStore::SignalSeriesStore(std::size_t capacityPerSignal)
    : m_capacity{std::max<std::size_t>(capacityPerSignal, 1)}
{ }

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
    // One acquisition for the whole batch. A pass hands over every signal it
    // decoded in one call, so this is the amortisation that makes a lock
    // affordable on the frame path at all.
    const std::lock_guard lock{m_mutex};

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
    const std::lock_guard lock{m_mutex};

    // A string from the view: the map is keyed by std::string with no
    // transparent comparator, and this is a panel-side lookup rather than a
    // per-sample one.
    const auto found = m_index.find(std::string{qualifiedName});
    return found != m_index.end() ? found->second : kNoSeries;
}

std::size_t SignalSeriesStore::seriesCount() const
{
    const std::lock_guard lock{m_mutex};
    return m_series.size();
}

std::vector<SeriesInfo> SignalSeriesStore::listSeries() const
{
    const std::lock_guard lock{m_mutex};

    std::vector<SeriesInfo> infos;
    infos.reserve(m_series.size());

    for (SeriesId id = 0; id < m_series.size(); ++id) {
        infos.push_back(
            SeriesInfo{id, m_series[id].name(), m_series[id].unit(), m_series[id].size()});
    }

    return infos;
}

void SignalSeriesStore::readWindows(std::span<const SeriesId> ids,
                                    std::uint64_t sinceNs,
                                    std::size_t maximumSamples,
                                    std::vector<SeriesWindow>& out) const
{
    const std::lock_guard lock{m_mutex};

    // Grown, not rebuilt: the caller hands the same buffer back every repaint,
    // so after the first few frames this allocates nothing.
    out.resize(ids.size());

    for (std::size_t index = 0; index < ids.size(); ++index) {
        SeriesWindow& window = out[index];

        if (ids[index] >= m_series.size()) {
            window = SeriesWindow{};
            continue;
        }

        const SignalSeries& series = m_series[ids[index]];

        window.id = ids[index];
        window.name = series.name();
        window.unit = series.unit();
        window.minimum = series.minimum();
        window.maximum = series.maximum();
        window.discarded = series.discarded();

        window.samples.resize(std::min(maximumSamples, series.size()));

        const std::size_t copied = series.copySince(sinceNs, window.samples);
        window.samples.resize(copied);
    }
}

bool SignalSeriesStore::latest(SeriesId id, double& value, std::uint64_t& timestampNs) const
{
    const std::lock_guard lock{m_mutex};

    if (id == kNoSeries || id >= m_series.size()) {
        return false;
    }

    const SignalSeries& series = m_series[id];

    if (series.empty()) {
        return false;
    }

    const SignalSample& sample = series.newest();

    value = sample.value;
    timestampNs = sample.timestampNs;

    return true;
}

std::uint64_t SignalSeriesStore::newestTimestampNs() const
{
    const std::lock_guard lock{m_mutex};

    std::uint64_t newest = 0;
    for (const SignalSeries& series : m_series) {
        if (!series.empty()) {
            newest = std::max(newest, series.newest().timestampNs);
        }
    }

    return newest;
}

std::uint64_t SignalSeriesStore::discarded() const
{
    const std::lock_guard lock{m_mutex};

    std::uint64_t total = 0;
    for (const SignalSeries& series : m_series) {
        total += series.discarded();
    }
    return total;
}

std::uint64_t SignalSeriesStore::truncated() const
{
    const std::lock_guard lock{m_mutex};
    return m_truncated;
}

void SignalSeriesStore::clearSamples()
{
    const std::lock_guard lock{m_mutex};

    for (SignalSeries& series : m_series) {
        series.clear();
    }

    m_truncated = 0;
}

void SignalSeriesStore::reset()
{
    const std::lock_guard lock{m_mutex};

    m_series.clear();
    m_index.clear();
    m_truncated = 0;
}

} // namespace torquebus
