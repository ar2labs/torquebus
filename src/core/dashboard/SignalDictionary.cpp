// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "core/dashboard/SignalDictionary.h"

#include "core/FilePath.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <utility>

namespace torquebus {
namespace {

// --- ASCII, by hand -------------------------------------------------------------------------
//
// Signal names are C identifiers and unit strings are short, and std::isupper and friends depend on
// the locale in ways that make a name sort differently on two machines. A byte that is not ASCII is
// treated as a lowercase letter: it is never a word boundary, and it is left as it was.

[[nodiscard]] constexpr bool isUpper(char c) noexcept
{
    return c >= 'A' && c <= 'Z';
}

[[nodiscard]] constexpr bool isLower(char c) noexcept
{
    return c >= 'a' && c <= 'z';
}

[[nodiscard]] constexpr bool isDigit(char c) noexcept
{
    return c >= '0' && c <= '9';
}

[[nodiscard]] constexpr char lowerOf(char c) noexcept
{
    return isUpper(c) ? static_cast<char>(c - 'A' + 'a') : c;
}

[[nodiscard]] std::string lowered(std::string_view text)
{
    std::string result{text};

    for (char& c : result) {
        c = lowerOf(c);
    }

    return result;
}

/// Splits an identifier into the words a person would read in it.
///
/// A boundary is a separator (underscore, dot, space, hyphen), a lowercase letter or a digit
/// followed by a capital ("engineSpeed", "Intake1Air"), and the last capital of a run of capitals
/// that is followed by a lowercase letter ("ABSAmber" is ABS and Amber). A digit belongs to the
/// word before it.
[[nodiscard]] std::vector<std::string> splitWords(std::string_view text)
{
    std::vector<std::string> words;
    std::string current;

    const auto flush = [&words, &current] {
        if (!current.empty()) {
            words.push_back(std::move(current));
            current.clear();
        }
    };

    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];

        if (c == '_' || c == ' ' || c == '.' || c == '-') {
            flush();
            continue;
        }

        if (!current.empty() && isUpper(c)) {
            // `current` is not empty, so the character before this one is not a separator.
            const char previous = text[i - 1];
            const bool nextIsLower = i + 1 < text.size() && isLower(text[i + 1]);

            if (isLower(previous) || isDigit(previous) || (isUpper(previous) && nextIsLower)) {
                flush();
            }
        }

        current.push_back(c);
    }

    flush();
    return words;
}

[[nodiscard]] std::string fileNameOf(const std::string& path)
{
    return utf8FromPath(pathFromUtf8(path).filename());
}

/// The physical range of a signal: the one the database declares, or else what its bits can hold.
void rangeOf(const CanSignal& signal, double& minimum, double& maximum, bool& declared)
{
    if (std::isfinite(signal.minimum) && std::isfinite(signal.maximum)
        && signal.minimum < signal.maximum) {
        minimum = signal.minimum;
        maximum = signal.maximum;
        declared = true;
        return;
    }

    declared = false;

    // A double holds 53 bits exactly. A wider signal gets a range that is approximate at the top,
    // which is all a range nobody wrote is.
    const double bits = static_cast<double>(std::min<std::uint16_t>(signal.bitLength, 53));

    const double lowRaw = signal.isSigned ? -std::exp2(bits - 1.0) : 0.0;
    const double highRaw = signal.isSigned ? std::exp2(bits - 1.0) - 1.0 : std::exp2(bits) - 1.0;

    const double a = lowRaw * signal.factor + signal.offset;
    const double b = highRaw * signal.factor + signal.offset;

    minimum = std::min(a, b);
    maximum = std::max(a, b);
}

/// Optimal string alignment distance: insertions, deletions, substitutions, and the swap of two
/// neighbours - the mistake fingers make most.
[[nodiscard]] std::size_t editDistance(std::string_view a, std::string_view b)
{
    const std::size_t n = a.size();
    const std::size_t m = b.size();

    std::vector<std::vector<std::size_t>> d(n + 1, std::vector<std::size_t>(m + 1, 0));

    for (std::size_t i = 0; i <= n; ++i) {
        d[i][0] = i;
    }
    for (std::size_t j = 0; j <= m; ++j) {
        d[0][j] = j;
    }

    for (std::size_t i = 1; i <= n; ++i) {
        for (std::size_t j = 1; j <= m; ++j) {
            const std::size_t cost = a[i - 1] == b[j - 1] ? 0U : 1U;

            d[i][j] = std::min({d[i - 1][j] + 1, d[i][j - 1] + 1, d[i - 1][j - 1] + cost});

            if (i > 1 && j > 1 && a[i - 1] == b[j - 2] && a[i - 2] == b[j - 1]) {
                d[i][j] = std::min(d[i][j], d[i - 2][j - 2] + 1);
            }
        }
    }

    return d[n][m];
}

/// The candidate closest to `target`, ignoring case, when it is close enough to be a typing mistake
/// - a third of the length, and at least one letter. Empty otherwise. Earlier candidates win a tie.
[[nodiscard]] std::string nearestName(const std::vector<std::string>& candidates,
                                      std::string_view target)
{
    if (target.empty()) {
        return {};
    }

    const std::string wanted = lowered(target);
    const std::size_t allowed = std::max<std::size_t>(1, wanted.size() / 3);

    std::size_t bestDistance = allowed + 1;
    const std::string* best = nullptr;

    for (const std::string& candidate : candidates) {
        const std::size_t distance = editDistance(wanted, lowered(candidate));

        if (distance < bestDistance) {
            bestDistance = distance;
            best = &candidate;
        }
    }

    return best != nullptr ? *best : std::string{};
}

/// Whether `word` is in `set`, for the handful of fixed vocabularies below.
template<std::size_t N>
[[nodiscard]] bool isOneOf(std::string_view word, const std::array<std::string_view, N>& set)
{
    return std::find(set.begin(), set.end(), word) != set.end();
}

/// The two names of a lamp that say nothing a lamp does not: copying them onto it would only put
/// the word "On" under a bulb that is lit.
[[nodiscard]] bool isGenericOnOff(const std::vector<SignalValueName>& values)
{
    if (values.size() != 2) {
        return false;
    }

    const SignalValueName* zero = nullptr;
    const SignalValueName* one = nullptr;

    for (const SignalValueName& value : values) {
        if (value.value == 0) {
            zero = &value;
        } else if (value.value == 1) {
            one = &value;
        }
    }

    if (zero == nullptr || one == nullptr) {
        return false;
    }

    // Only the pairs that mean what off and on already mean. "Closed" and "Open" say something a
    // bulb does not, and a door is worth naming.
    static constexpr std::array<std::string_view, 6> kPlainOff{
        "off", "false", "inactive", "disabled", "no", "0"};
    static constexpr std::array<std::string_view, 6> kPlainOn{
        "on", "true", "active", "enabled", "yes", "1"};

    return isOneOf(lowered(zero->name), kPlainOff) && isOneOf(lowered(one->name), kPlainOn);
}

[[nodiscard]] bool isCounterUnit(std::string_view unit)
{
    static constexpr std::array<std::string_view, 17> kCounters{"s",
                                                                "ms",
                                                                "us",
                                                                "min",
                                                                "h",
                                                                "hr",
                                                                "hrs",
                                                                "day",
                                                                "days",
                                                                "month",
                                                                "year",
                                                                "km",
                                                                "m",
                                                                "mi",
                                                                "rev",
                                                                "revs",
                                                                "count"};

    return isOneOf(lowered(unit), kCounters);
}

} // namespace

// ---------------------------------------------------------------------------
// Rules
// ---------------------------------------------------------------------------

std::string prettyName(std::string_view identifier)
{
    const std::vector<std::string> words = splitWords(identifier);

    if (words.empty()) {
        return std::string{identifier};
    }

    std::string result;

    for (const std::string& word : words) {
        if (!result.empty()) {
            result.push_back(' ');
        }

        result += word;
    }

    return result;
}

std::string idFor(std::string_view signalName)
{
    std::string result;

    for (const std::string& word : splitWords(signalName)) {
        std::string clean;

        for (const char c : word) {
            if (isLower(c) || isUpper(c) || isDigit(c)) {
                clean.push_back(lowerOf(c));
            }
        }

        if (clean.empty()) {
            continue;
        }

        if (!result.empty()) {
            result.push_back('_');
        }

        result += clean;
    }

    return result.empty() ? std::string{"signal"} : result;
}

DashboardTone inferTone(std::string_view name)
{
    static constexpr std::array<std::string_view, 15> kDanger{"error",
                                                              "fault",
                                                              "faulty",
                                                              "fail",
                                                              "failed",
                                                              "failure",
                                                              "critical",
                                                              "danger",
                                                              "dangerous",
                                                              "alarm",
                                                              "emergency",
                                                              "severe",
                                                              "overheat",
                                                              "overheating",
                                                              "shorted"};

    static constexpr std::array<std::string_view, 11> kWarning{"warning",
                                                               "warn",
                                                               "caution",
                                                               "degraded",
                                                               "derate",
                                                               "derated",
                                                               "attention",
                                                               "high",
                                                               "anomaly",
                                                               "anomalous",
                                                               "abnormal"};

    static constexpr std::array<std::string_view, 5> kInfo{
        "moderate", "medium", "info", "information", "notice"};

    static constexpr std::array<std::string_view, 9> kOk{
        "ok", "normal", "good", "healthy", "ready", "running", "engaged", "valid", "nominal"};

    static constexpr std::array<std::string_view, 9> kOff{
        "off", "inactive", "disabled", "none", "idle", "waiting", "standby", "unknown", "na"};

    std::vector<std::string> words;
    std::string joined;

    for (const std::string& word : splitWords(name)) {
        words.push_back(lowered(word));
        joined += words.back();
    }

    // First, because "NotAvailable" contains "available", which is not good news.
    if (joined.find("notavailable") != std::string::npos
        || joined.find("unavailable") != std::string::npos) {
        return DashboardTone::Off;
    }

    for (const std::string& word : words) {
        if (isOneOf(word, kDanger)) {
            return DashboardTone::Danger;
        }
    }

    for (const std::string& word : words) {
        if (isOneOf(word, kWarning)) {
            return DashboardTone::Warning;
        }
    }

    for (const std::string& word : words) {
        if (isOneOf(word, kInfo)) {
            return DashboardTone::Info;
        }
    }

    for (const std::string& word : words) {
        if (isOneOf(word, kOk)) {
            return DashboardTone::Ok;
        }
    }

    for (const std::string& word : words) {
        if (isOneOf(word, kOff)) {
            return DashboardTone::Off;
        }
    }

    return DashboardTone::Neutral;
}

int decimalsFor(double factor)
{
    const double step = std::abs(factor);

    if (!std::isfinite(step) || step <= 0.0 || step >= 1.0) {
        return 0;
    }

    // -log10(0.125) is 0.9, so one decimal; -log10(0.05) is 1.3, so two. The small allowance keeps
    // a step of exactly 0.1, whose logarithm is a hair over 1, at one.
    const int needed = static_cast<int>(std::ceil(-std::log10(step) - 1e-9));

    return std::clamp(needed, 1, 2);
}

std::uint32_t staleAfterMs(std::uint32_t cycleMs)
{
    if (cycleMs == 0) {
        return 0;
    }

    const std::uint64_t threeCycles = static_cast<std::uint64_t>(cycleMs) * 3U;

    return static_cast<std::uint32_t>(std::clamp<std::uint64_t>(threeCycles, 500U, 60'000U));
}

std::vector<DashboardState> statesOf(const DictionaryEntry& entry)
{
    std::vector<DashboardState> states;
    states.reserve(entry.values.size());

    for (const SignalValueName& value : entry.values) {
        DashboardState state;
        state.value = entry.physical(value.value);
        state.label = value.name;
        state.tone = inferTone(value.name);

        states.push_back(std::move(state));
    }

    std::sort(states.begin(), states.end(), [](const DashboardState& a, const DashboardState& b) {
        return a.value < b.value;
    });

    return states;
}

DashboardWidgetKind suggestKind(const DictionaryEntry& entry)
{
    // 1. Words for the values.
    if (!entry.values.empty()) {
        const bool zeroAndOne =
            entry.values.size() == 2
            && std::any_of(entry.values.begin(),
                           entry.values.end(),
                           [](const SignalValueName& v) { return v.value == 0; })
            && std::any_of(entry.values.begin(), entry.values.end(), [](const SignalValueName& v) {
                   return v.value == 1;
               });

        return zeroAndOne ? DashboardWidgetKind::Lamp : DashboardWidgetKind::State;
    }

    // 2. Too few bits to be anything but a switch. A two-bit status field is zero, one, and two
    // values the standard keeps for "error" and "not available".
    if (entry.bitLength <= 2) {
        return DashboardWidgetKind::Lamp;
    }

    // 3. A percentage is how full.
    if (entry.unit == "%") {
        return DashboardWidgetKind::Bar;
    }

    // 4. Counting.
    if (isCounterUnit(entry.unit)) {
        return DashboardWidgetKind::Numeric;
    }

    // 5. A dial wants a scale somebody wrote down, a span it can be read across, and a unit.
    const double span = entry.maximum - entry.minimum;

    if (entry.declaredRange && !entry.unit.empty() && span > 0.0 && span <= 100'000.0) {
        return DashboardWidgetKind::Gauge;
    }

    // 6.
    return DashboardWidgetKind::Numeric;
}

std::vector<DashboardWidgetKind> readerKinds()
{
    return {DashboardWidgetKind::Gauge,
            DashboardWidgetKind::Numeric,
            DashboardWidgetKind::Bar,
            DashboardWidgetKind::Lamp,
            DashboardWidgetKind::State,
            DashboardWidgetKind::Trend,
            DashboardWidgetKind::Image};
}

void applyEntry(DashboardWidget& widget, const DictionaryEntry& entry)
{
    // Decided before the binding is overwritten: a title is the previous binding's own if it is
    // what that binding would have produced, and it is somebody's if it is anything else.
    const bool titleIsAutomatic = widget.title.empty()
                                  || widget.title == prettyName(widget.binding.signal)
                                  || widget.title == widget.binding.signal;

    widget.binding.source = DashboardBinding::Source::Signal;
    widget.binding.message = entry.message;
    widget.binding.signal = entry.signal;
    widget.binding.variable.clear();

    if (titleIsAutomatic) {
        widget.title = prettyName(entry.signal);
    }

    const DashboardKindTraits& traits = traitsOf(widget.kind);

    if (traits.unit) {
        widget.unit = entry.unit;
    }

    if (traits.range) {
        widget.minimum = entry.minimum;
        widget.maximum = entry.maximum > entry.minimum ? entry.maximum : entry.minimum + 1.0;
    }

    if (traits.decimals) {
        widget.decimals = decimalsFor(entry.factor);
    }

    if (traits.states) {
        // A lamp that is told "0 is Off, 1 is On" has learned nothing.
        const bool saysNothing =
            widget.kind == DashboardWidgetKind::Lamp && isGenericOnOff(entry.values);

        widget.states = saysNothing ? std::vector<DashboardState>{} : statesOf(entry);
    }

    if (widget.staleMs == 0) {
        widget.staleMs = staleAfterMs(entry.cycleTimeMs);
    }
}

DashboardWidget suggestWidget(const DictionaryEntry& entry, std::optional<DashboardWidgetKind> kind)
{
    DashboardWidgetKind chosen = suggestKind(entry);

    if (kind.has_value()) {
        const std::vector<DashboardWidgetKind> readers = readerKinds();

        if (std::find(readers.begin(), readers.end(), *kind) != readers.end()) {
            chosen = *kind;
        }
    }

    DashboardWidget widget = defaultWidgetOf(chosen);
    widget.id = idFor(entry.signal);

    applyEntry(widget, entry);

    return widget;
}

DashboardWidget suggestTable(std::string_view message, std::size_t signalCount)
{
    DashboardWidget widget = defaultWidgetOf(DashboardWidgetKind::Table);
    widget.id = idFor(message) + "_table";

    widget.binding.source = DashboardBinding::Source::Message;
    widget.binding.message = std::string{message};

    // A title bar and a row to each signal, up to what a screen shows. The panel draws with its own
    // metrics; this is the size at which the first rows are seen whole.
    constexpr double kTitle = 36.0;
    constexpr double kRow = 22.0;
    constexpr std::size_t kMostRows = 14;

    widget.height = kTitle + kRow * static_cast<double>(std::min(signalCount, kMostRows));

    return widget;
}

// ---------------------------------------------------------------------------
// The dictionary
// ---------------------------------------------------------------------------

void SignalDictionary::rebuild(std::span<const std::shared_ptr<const CanDatabase>> databases)
{
    m_entries.clear();
    m_prepared.clear();
    m_byName.clear();
    m_byMessage.clear();

    for (const std::shared_ptr<const CanDatabase>& database : databases) {
        if (!database) {
            continue;
        }

        const std::string file = fileNameOf(database->sourcePath);

        for (const CanMessage& message : database->messages()) {
            for (const CanSignal& signal : message.signalList) {
                DictionaryEntry entry;
                entry.message = message.name;
                entry.signal = signal.name;
                entry.unit = signal.unit;
                entry.comment = signal.comment;
                entry.messageComment = message.comment;
                entry.transmitter = message.transmitter;
                entry.database = file;
                entry.identifier = message.identifier;
                entry.format = message.format;
                entry.cycleTimeMs = message.cycleTimeMs;
                entry.bitLength = signal.bitLength;
                entry.isSigned = signal.isSigned;
                entry.factor = signal.factor;
                entry.offset = signal.offset;
                entry.values = signal.valueNames;

                rangeOf(signal, entry.minimum, entry.maximum, entry.declaredRange);

                Prepared prepared;
                prepared.signal = lowered(entry.signal);
                prepared.message = lowered(entry.message);
                prepared.unit = lowered(entry.unit);
                prepared.comment = lowered(entry.comment + " " + entry.messageComment);

                for (const std::string& word : splitWords(entry.signal)) {
                    prepared.initials.push_back(lowerOf(word.front()));
                    prepared.words.push_back(lowered(word));
                }

                const std::size_t index = m_entries.size();

                // The first definition of a name wins a lookup, as the first database does for the
                // plot store; the others are still listed, so a person can see there are two.
                m_byName.try_emplace(entry.qualified(), index);
                m_byMessage[entry.message].push_back(index);

                m_entries.push_back(std::move(entry));
                m_prepared.push_back(std::move(prepared));
            }
        }
    }
}

const DictionaryEntry* SignalDictionary::find(std::string_view message,
                                              std::string_view signal) const noexcept
{
    try {
        std::string key;
        key.reserve(message.size() + 1 + signal.size());
        key.append(message);
        key.push_back('.');
        key.append(signal);

        const auto found = m_byName.find(key);

        return found == m_byName.end() ? nullptr : &m_entries[found->second];
    } catch (...) {
        // An allocation that failed is a lookup that found nothing.
        return nullptr;
    }
}

bool SignalDictionary::hasMessage(std::string_view message) const noexcept
{
    try {
        return m_byMessage.find(std::string{message}) != m_byMessage.end();
    } catch (...) {
        return false;
    }
}

std::vector<const DictionaryEntry*> SignalDictionary::signalsOf(std::string_view message) const
{
    std::vector<const DictionaryEntry*> result;

    const auto found = m_byMessage.find(std::string{message});

    if (found == m_byMessage.end()) {
        return result;
    }

    result.reserve(found->second.size());

    for (const std::size_t index : found->second) {
        result.push_back(&m_entries[index]);
    }

    return result;
}

std::vector<std::string> SignalDictionary::messageNames() const
{
    std::vector<std::string> names;
    names.reserve(m_byMessage.size());

    for (const auto& [name, indexes] : m_byMessage) {
        (void)indexes;
        names.push_back(name);
    }

    std::sort(names.begin(), names.end());
    return names;
}

std::vector<const DictionaryEntry*> SignalDictionary::search(std::string_view text,
                                                             std::size_t limit) const
{
    // Words, split at spaces and dots, lowercased. "EEC1.eng" is two words.
    std::vector<std::string> words;
    std::string current;

    for (const char c : text) {
        if (c == ' ' || c == '.' || c == '\t') {
            if (!current.empty()) {
                words.push_back(std::move(current));
                current.clear();
            }
            continue;
        }

        current.push_back(lowerOf(c));
    }

    if (!current.empty()) {
        words.push_back(std::move(current));
    }

    struct Scored final {
        std::size_t index;
        int score;
    };

    std::vector<Scored> scored;
    scored.reserve(m_entries.size());

    for (std::size_t i = 0; i < m_entries.size(); ++i) {
        const Prepared& p = m_prepared[i];

        int total = 1;
        bool matches = true;

        for (const std::string& word : words) {
            int best = 0;

            if (p.signal == word) {
                best = 100;
            } else if (p.signal.starts_with(word)) {
                best = 80;
            } else if (p.message.starts_with(word)) {
                best = 70;
            } else {
                // A word that begins a word inside the name: "temp" in EngineCoolantTemperature.
                const bool atWordStart =
                    std::any_of(p.words.begin(), p.words.end(), [&word](const std::string& w) {
                        return w.starts_with(word);
                    });

                if (atWordStart) {
                    best = 60;
                } else if (word.size() >= 2 && p.initials.starts_with(word)) {
                    best = 55;
                } else if (p.signal.find(word) != std::string::npos) {
                    best = 50;
                } else if (p.message.find(word) != std::string::npos) {
                    best = 40;
                } else if (p.unit == word) {
                    best = 35;
                } else if (p.comment.find(word) != std::string::npos) {
                    best = 20;
                }
            }

            if (best == 0) {
                matches = false;
                break;
            }

            total += best;
        }

        if (matches) {
            scored.push_back({i, total});
        }
    }

    // Stable: equal scores stay in the databases' order, which is the order a person knows.
    std::stable_sort(scored.begin(), scored.end(), [](const Scored& a, const Scored& b) {
        return a.score > b.score;
    });

    std::vector<const DictionaryEntry*> result;
    result.reserve(std::min(limit, scored.size()));

    for (const Scored& item : scored) {
        if (result.size() >= limit) {
            break;
        }

        result.push_back(&m_entries[item.index]);
    }

    return result;
}

BindingFinding SignalDictionary::check(const DashboardBinding& binding) const
{
    BindingFinding result;

    if (m_entries.empty()) {
        return result;
    }

    if (binding.source != DashboardBinding::Source::Signal
        && binding.source != DashboardBinding::Source::Message) {
        return result;
    }

    // Everything below is about a name that is not there, and is only worked out then.
    const auto signalNamedIn = [this](std::string_view signal) {
        // The messages that have a signal of exactly this name.
        std::vector<std::string> messages;

        for (const DictionaryEntry& entry : m_entries) {
            if (entry.signal == signal
                && std::find(messages.begin(), messages.end(), entry.message) == messages.end()) {
                messages.push_back(entry.message);
            }
        }

        return messages;
    };

    if (binding.source == DashboardBinding::Source::Message) {
        if (hasMessage(binding.message)) {
            result.status = BindingFinding::Status::Found;
        } else {
            result.status = BindingFinding::Status::UnknownMessage;
            result.suggestedMessage = nearestName(messageNames(), binding.message);
        }

        return result;
    }

    if (const DictionaryEntry* entry = find(binding.message, binding.signal); entry != nullptr) {
        result.status = BindingFinding::Status::Found;
        result.entry = entry;
        return result;
    }

    if (hasMessage(binding.message)) {
        result.status = BindingFinding::Status::UnknownSignal;

        std::vector<std::string> names;
        for (const DictionaryEntry* entry : signalsOf(binding.message)) {
            names.push_back(entry->signal);
        }

        result.suggestedSignal = nearestName(names, binding.signal);

        if (!result.suggestedSignal.empty()) {
            result.suggestedMessage = binding.message;
            return result;
        }

        // Not close to anything in this message; perhaps it is in another.
        if (const std::vector<std::string> elsewhere = signalNamedIn(binding.signal);
            elsewhere.size() == 1) {
            result.suggestedMessage = elsewhere.front();
            result.suggestedSignal = binding.signal;
        }

        return result;
    }

    result.status = BindingFinding::Status::UnknownMessage;

    // The signal is the surer clue: if exactly one message has a signal of this name, that is
    // almost certainly the message that was meant ("Engine.EngineSpeed" and EEC1).
    if (const std::vector<std::string> elsewhere = signalNamedIn(binding.signal);
        elsewhere.size() == 1) {
        result.suggestedMessage = elsewhere.front();
        result.suggestedSignal = binding.signal;
        return result;
    }

    result.suggestedMessage = nearestName(messageNames(), binding.message);

    if (!result.suggestedMessage.empty()) {
        // Spelt right in the message it was found in? Then say which signal.
        if (find(result.suggestedMessage, binding.signal) != nullptr) {
            result.suggestedSignal = binding.signal;
        }
    }

    return result;
}

std::vector<WidgetFinding> SignalDictionary::audit(const DashboardDescription& dashboard) const
{
    std::vector<WidgetFinding> findings;

    if (m_entries.empty()) {
        return findings;
    }

    for (const DashboardWidget& widget : dashboard.widgets()) {
        BindingFinding finding = check(widget.binding);

        if (finding.status == BindingFinding::Status::UnknownMessage
            || finding.status == BindingFinding::Status::UnknownSignal) {
            findings.push_back({widget.id, std::move(finding)});
        }
    }

    return findings;
}

} // namespace torquebus
