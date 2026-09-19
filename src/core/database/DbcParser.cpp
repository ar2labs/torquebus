// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "core/database/DbcParser.h"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <locale>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace torquebus {
namespace {

/// Bit 31 of the number on a `BO_` line carries the extended flag. It is not
/// part of the identifier; a database with `BO_ 2566844926` means extended
/// 0x18FEDF00, not a standard identifier that cannot exist.
constexpr std::uint32_t kExtendedFlag = 0x80000000U;

/// The name a .dbc uses where a real node name is unknown. Kept out of the
/// receiver lists, because "everyone" and "nobody in particular" are different
/// and only one of them is useful to display.
constexpr std::string_view kAnyNode = "Vector__XXX";

[[nodiscard]] constexpr bool isSpace(char c) noexcept
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

/// A .dbc identifier: letters, digits and underscore. Signal and message names
/// are drawn from this set, and so are the section keywords, which is why the
/// same function reads both.
[[nodiscard]] constexpr bool isNameChar(char c) noexcept
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

/// A cursor over one statement, with enough position tracking to say where a
/// parse went wrong.
///
/// Hand-written rather than a regex: the error messages are the point. A regex
/// that does not match tells the caller "the line is wrong somewhere", and a
/// .dbc line is long enough that this is not a useful thing to be told.
class Scanner final {
public:
    Scanner(std::string_view text, std::size_t lineNumber) noexcept
        : m_text{text}
        , m_line{lineNumber}
    { }

    [[nodiscard]] bool atEnd() noexcept
    {
        skipSpace();
        return m_position >= m_text.size();
    }

    void skipSpace() noexcept
    {
        while (m_position < m_text.size() && isSpace(m_text[m_position])) {
            ++m_position;
        }
    }

    [[nodiscard]] char peek() noexcept
    {
        skipSpace();
        return m_position < m_text.size() ? m_text[m_position] : '\0';
    }

    /// Consumes `expected` if it is next. Returns false without consuming
    /// otherwise, which is what makes the optional parts of an `SG_` line -
    /// the unit, the receiver list - straightforward.
    [[nodiscard]] bool accept(char expected) noexcept
    {
        if (peek() != expected) {
            return false;
        }
        ++m_position;
        return true;
    }

    [[nodiscard]] bool expect(char expected, std::string& error)
    {
        if (accept(expected)) {
            return true;
        }

        error = describe(std::string{"expected '"} + expected + "'");
        return false;
    }

    /// A run of name characters. Empty when the next character is not one.
    [[nodiscard]] std::string_view name() noexcept
    {
        skipSpace();
        const std::size_t start = m_position;
        while (m_position < m_text.size() && isNameChar(m_text[m_position])) {
            ++m_position;
        }
        return m_text.substr(start, m_position - start);
    }

    [[nodiscard]] bool name(std::string_view what, std::string_view& out, std::string& error)
    {
        out = name();
        if (!out.empty()) {
            return true;
        }

        error = describe(std::string{"expected "} + std::string{what});
        return false;
    }

    /// A quoted string, with `\"` and `\\` unescaped.
    [[nodiscard]] bool quoted(std::string& out, std::string& error)
    {
        if (!expect('"', error)) {
            return false;
        }

        out.clear();
        while (m_position < m_text.size()) {
            const char c = m_text[m_position++];

            if (c == '"') {
                return true;
            }

            if (c == '\\' && m_position < m_text.size()) {
                out.push_back(m_text[m_position++]);
                continue;
            }

            if (c == '\n') {
                ++m_line;
            }

            out.push_back(c);
        }

        error = describe("unterminated string");
        return false;
    }

    [[nodiscard]] bool integer(std::string_view what, std::int64_t& out, std::string& error)
    {
        skipSpace();
        const std::size_t start = m_position;

        if (m_position < m_text.size()
            && (m_text[m_position] == '-' || m_text[m_position] == '+')) {
            ++m_position;
        }
        while (m_position < m_text.size() && m_text[m_position] >= '0'
               && m_text[m_position] <= '9') {
            ++m_position;
        }

        const std::string_view digits = m_text.substr(start, m_position - start);
        if (digits.empty() || digits == "-" || digits == "+") {
            error = describe(std::string{"expected "} + std::string{what});
            return false;
        }

        const char* begin = digits.data() + (digits.front() == '+' ? 1 : 0);
        const auto parsed = std::from_chars(begin, digits.data() + digits.size(), out);
        if (parsed.ec != std::errc{}) {
            error = describe(std::string{std::string{what}} + " is out of range");
            return false;
        }

        return true;
    }

    /// A decimal number, possibly with an exponent.
    ///
    /// The span is measured here and converted through a classic-locale
    /// stream. The locale part is not decoration: a .dbc always writes "0.01"
    /// with a dot, and on a machine whose locale uses a comma the ordinary
    /// conversions read that as 0 - which would make every scaled signal on the
    /// bus decode to its offset, silently, only on that machine.
    [[nodiscard]] bool number(std::string_view what, double& out, std::string& error)
    {
        skipSpace();
        const std::size_t start = m_position;

        if (m_position < m_text.size()
            && (m_text[m_position] == '-' || m_text[m_position] == '+')) {
            ++m_position;
        }

        bool digitSeen = false;
        while (m_position < m_text.size()) {
            const char c = m_text[m_position];
            if (c >= '0' && c <= '9') {
                digitSeen = true;
                ++m_position;
            } else if (c == '.') {
                ++m_position;
            } else if ((c == 'e' || c == 'E') && digitSeen) {
                ++m_position;
                if (m_position < m_text.size()
                    && (m_text[m_position] == '-' || m_text[m_position] == '+')) {
                    ++m_position;
                }
            } else {
                break;
            }
        }

        if (!digitSeen) {
            error = describe(std::string{"expected "} + std::string{what});
            return false;
        }

        const std::string digits{m_text.substr(start, m_position - start)};
        out = parseDouble(digits);
        return true;
    }

    [[nodiscard]] std::string describe(std::string what) const
    {
        std::ostringstream message;
        message << "line " << m_line << ": " << what;

        std::string_view around = m_text.substr(std::min(m_position, m_text.size()));
        if (!around.empty()) {
            message << ", found \"" << around.substr(0, std::min<std::size_t>(24, around.size()))
                    << "\"";
        } else {
            message << ", found end of line";
        }

        return message.str();
    }

private:
    /// Locale-independent. std::stod and strtod both follow the C locale's
    /// decimal point, and a .dbc always uses a dot.
    [[nodiscard]] static double parseDouble(const std::string& text)
    {
        std::istringstream stream{text};
        stream.imbue(std::locale::classic());

        double value = 0.0;
        stream >> value;
        return value;
    }

    std::string_view m_text;
    std::size_t m_position{0};
    std::size_t m_line{1};
};

/// One logical statement of the file: a line, extended across newlines when a
/// quoted string has not been closed.
struct Statement final {
    std::string text;
    std::size_t line{1};
};

/// Removes `/* ... */` blocks, leaving newlines so line numbers survive.
///
/// Not part of the DBC format. Several generators emit them anyway - both of
/// the hand-written databases this project is tested against are full of them -
/// and a parser that chokes on a file every other tool reads is the parser that
/// is wrong.
[[nodiscard]] std::string stripBlockComments(std::string_view text)
{
    std::string out;
    out.reserve(text.size());

    bool inString = false;
    for (std::size_t i = 0; i < text.size();) {
        const char c = text[i];

        if (inString) {
            if (c == '\\' && i + 1 < text.size()) {
                out.push_back(c);
                out.push_back(text[i + 1]);
                i += 2;
                continue;
            }
            if (c == '"') {
                inString = false;
            }
            out.push_back(c);
            ++i;
            continue;
        }

        if (c == '"') {
            inString = true;
            out.push_back(c);
            ++i;
            continue;
        }

        if (c == '/' && i + 1 < text.size() && text[i + 1] == '*') {
            i += 2;
            while (i < text.size()
                   && !(text[i] == '*' && i + 1 < text.size() && text[i + 1] == '/')) {
                // Keep the newlines: an error reported after a twenty-line
                // comment block should name the real line.
                if (text[i] == '\n') {
                    out.push_back('\n');
                }
                ++i;
            }
            i = std::min(i + 2, text.size());
            continue;
        }

        out.push_back(c);
        ++i;
    }

    return out;
}

/// Splits into statements, keeping a quoted string that spans lines together.
[[nodiscard]] std::vector<Statement> splitStatements(std::string_view text)
{
    std::vector<Statement> statements;

    std::size_t lineNumber = 1;
    std::size_t start = 0;
    bool inString = false;
    std::size_t statementLine = 1;

    const auto flush = [&](std::size_t end) {
        std::string_view raw = text.substr(start, end - start);
        while (!raw.empty() && isSpace(raw.front())) {
            raw.remove_prefix(1);
        }
        while (!raw.empty() && isSpace(raw.back())) {
            raw.remove_suffix(1);
        }
        if (!raw.empty()) {
            statements.push_back(Statement{std::string{raw}, statementLine});
        }
    };

    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];

        if (inString) {
            if (c == '\\') {
                ++i;
            } else if (c == '"') {
                inString = false;
            } else if (c == '\n') {
                ++lineNumber;
            }
            continue;
        }

        if (c == '"') {
            inString = true;
            continue;
        }

        if (c == '\n') {
            flush(i);
            ++lineNumber;
            start = i + 1;
            statementLine = lineNumber;
        }
    }

    flush(text.size());
    return statements;
}

[[nodiscard]] bool parseSignal(Scanner& scanner, CanSignal& signal, std::string& error)
{
    std::string_view signalName;
    if (!scanner.name("a signal name", signalName, error)) {
        return false;
    }
    signal.name = std::string{signalName};

    // The multiplexing marker, when there is one: `M` for the switch, `m<n>`
    // for a signal carried only when the switch reads n. A message may have
    // both on one signal (`m3M`, an extended multiplexor); the switch flag is
    // what matters downstream, so both are recorded.
    if (scanner.peek() != ':') {
        const std::string_view marker = scanner.name();
        if (marker.empty()) {
            error = scanner.describe("expected ':' or a multiplexing marker");
            return false;
        }

        if (marker == "M") {
            signal.isMultiplexer = true;
        } else if (marker.front() == 'm') {
            const std::string_view digits = marker.substr(1);
            std::uint32_t selector = 0;
            const auto parsed =
                std::from_chars(digits.data(), digits.data() + digits.size(), selector);
            if (parsed.ec != std::errc{}) {
                error = scanner.describe("multiplexing marker \"" + std::string{marker}
                                         + "\" has no number after the m");
                return false;
            }
            signal.multiplexerValue = selector;
            signal.isMultiplexer =
                parsed.ptr != digits.data() + digits.size() && *parsed.ptr == 'M';
        } else {
            error = scanner.describe("unexpected \"" + std::string{marker}
                                     + "\" where a multiplexing marker was allowed");
            return false;
        }
    }

    if (!scanner.expect(':', error)) {
        return false;
    }

    std::int64_t startBit = 0;
    std::int64_t bitLength = 0;
    if (!scanner.integer("a start bit", startBit, error) || !scanner.expect('|', error)
        || !scanner.integer("a bit length", bitLength, error) || !scanner.expect('@', error)) {
        return false;
    }

    if (startBit < 0 || startBit > 63 || bitLength < 1 || bitLength > 64) {
        error = scanner.describe("signal \"" + signal.name + "\" has an impossible position: "
                                 + std::to_string(startBit) + "|" + std::to_string(bitLength));
        return false;
    }

    signal.startBit = static_cast<std::uint16_t>(startBit);
    signal.bitLength = static_cast<std::uint16_t>(bitLength);

    // @0 is Motorola, @1 is Intel. Chosen by Vector, not by anyone with a taste
    // for mnemonics.
    if (scanner.accept('0')) {
        signal.byteOrder = ByteOrder::Motorola;
    } else if (scanner.accept('1')) {
        signal.byteOrder = ByteOrder::Intel;
    } else {
        error = scanner.describe("expected @0 (Motorola) or @1 (Intel)");
        return false;
    }

    if (scanner.accept('-')) {
        signal.isSigned = true;
    } else if (scanner.accept('+')) {
        signal.isSigned = false;
    } else {
        error = scanner.describe("expected '+' or '-' after the byte order");
        return false;
    }

    if (!scanner.expect('(', error) || !scanner.number("a factor", signal.factor, error)
        || !scanner.expect(',', error) || !scanner.number("an offset", signal.offset, error)
        || !scanner.expect(')', error)) {
        return false;
    }

    if (!scanner.expect('[', error) || !scanner.number("a minimum", signal.minimum, error)
        || !scanner.expect('|', error) || !scanner.number("a maximum", signal.maximum, error)
        || !scanner.expect(']', error)) {
        return false;
    }

    if (!scanner.quoted(signal.unit, error)) {
        return false;
    }

    // Receivers, comma separated. Absent in some files, which is legal.
    while (!scanner.atEnd()) {
        const std::string_view receiver = scanner.name();
        if (receiver.empty()) {
            break;
        }
        if (receiver != kAnyNode) {
            signal.receivers.emplace_back(receiver);
        }
        if (!scanner.accept(',')) {
            break;
        }
    }

    return true;
}

/// Message identifiers a `VAL_`, `CM_` or `BA_` line refers to, resolved to the
/// message it means. Built once per parse rather than searched per line.
class MessageIndex final {
public:
    void add(std::uint32_t rawIdentifier, std::size_t index)
    {
        m_byRaw.emplace(rawIdentifier, index);
    }

    [[nodiscard]] CanMessage* find(std::vector<CanMessage>& messages,
                                   std::uint32_t rawIdentifier) const
    {
        const auto match = m_byRaw.find(rawIdentifier);
        return match == m_byRaw.end() ? nullptr : &messages[match->second];
    }

private:
    std::unordered_map<std::uint32_t, std::size_t> m_byRaw;
};

} // namespace

Result DbcParser::parse(std::string_view text, CanDatabase& database)
{
    const std::string cleaned = stripBlockComments(text);
    const std::vector<Statement> statements = splitStatements(cleaned);

    // Built into locals and only moved into `database` at the end, so a file
    // that fails halfway leaves the caller's database as it was. Reloading a
    // .dbc that someone is editing is exactly when this matters.
    std::vector<CanMessage> messages;
    MessageIndex index;
    std::string version;
    std::vector<std::string> nodes;

    CanMessage* current = nullptr;
    std::string error;

    // The `NS_` section lists the keywords the file is allowed to use, one to a
    // line and with nothing after them - so a file that declares it might use
    // `CM_` contains a line that is exactly `CM_`. Read as a section header
    // that is a comment with no text, which is how this parser failed on every
    // real file the first time it ran.
    //
    // The way out is the shape of the entries rather than a list of names: an
    // NS_ entry is a bare word alone on its line, and no real section is.
    bool inNewSymbols = false;

    for (const Statement& statement : statements) {
        Scanner scanner{statement.text, statement.line};

        const std::string_view keyword = scanner.name();
        if (keyword.empty()) {
            continue;
        }

        if (keyword == "NS_") {
            inNewSymbols = true;
            continue;
        }

        if (inNewSymbols) {
            if (scanner.atEnd()) {
                continue;
            }
            inNewSymbols = false;
        }

        if (keyword == "VERSION") {
            if (!scanner.quoted(version, error)) {
                return Result::error(ErrorCode::ParseError, error);
            }
            continue;
        }

        if (keyword == "BU_") {
            if (!scanner.accept(':')) {
                return Result::error(ErrorCode::ParseError,
                                     scanner.describe("expected ':' after BU_"));
            }
            while (!scanner.atEnd()) {
                const std::string_view node = scanner.name();
                if (node.empty()) {
                    break;
                }
                nodes.emplace_back(node);
            }
            continue;
        }

        if (keyword == "BO_") {
            std::int64_t rawIdentifier = 0;
            std::string_view messageName;
            std::int64_t length = 0;

            if (!scanner.integer("a message identifier", rawIdentifier, error)
                || !scanner.name("a message name", messageName, error)
                || !scanner.expect(':', error)
                || !scanner.integer("a payload length", length, error)) {
                return Result::error(ErrorCode::ParseError, error);
            }

            const auto raw = static_cast<std::uint32_t>(rawIdentifier);

            CanMessage message;
            message.format =
                (raw & kExtendedFlag) != 0U ? CanFrameFormat::Extended : CanFrameFormat::Standard;
            message.identifier = raw & ~kExtendedFlag;
            message.name = std::string{messageName};
            message.length = static_cast<std::uint8_t>(std::min<std::int64_t>(length, 64));

            if (!isValidIdentifier(message.identifier, message.format)) {
                return Result::error(ErrorCode::ParseError,
                                     scanner.describe("message \"" + message.name
                                                      + "\" has identifier "
                                                      + std::to_string(message.identifier)
                                                      + ", which does not fit its frame format"));
            }

            const std::string_view transmitter = scanner.name();
            if (transmitter != kAnyNode) {
                message.transmitter = std::string{transmitter};
            }

            index.add(raw, messages.size());
            messages.push_back(std::move(message));
            current = &messages.back();
            continue;
        }

        if (keyword == "SG_") {
            if (current == nullptr) {
                return Result::error(ErrorCode::ParseError,
                                     scanner.describe("SG_ outside any message"));
            }

            CanSignal signal;
            if (!parseSignal(scanner, signal, error)) {
                return Result::error(ErrorCode::ParseError, error);
            }

            current->signalList.push_back(std::move(signal));
            continue;
        }

        // Everything below refers back to a message rather than continuing one,
        // so the "current message" ends here.
        current = nullptr;

        if (keyword == "VAL_") {
            std::int64_t rawIdentifier = 0;
            std::string_view signalName;

            if (!scanner.integer("a message identifier", rawIdentifier, error)) {
                // The other form of VAL_ names an environment variable rather
                // than a message. Nothing downstream reads those.
                continue;
            }
            if (!scanner.name("a signal name", signalName, error)) {
                return Result::error(ErrorCode::ParseError, error);
            }

            CanMessage* message = index.find(messages, static_cast<std::uint32_t>(rawIdentifier));
            CanSignal* signal = nullptr;
            if (message != nullptr) {
                signal = message->findSignal(signalName);
            }

            // A value table for a message this file does not define happens in
            // merged databases. Read past it rather than failing: the table is
            // useless here, but the rest of the file is not.
            while (!scanner.atEnd() && scanner.peek() != ';') {
                std::int64_t value = 0;
                std::string valueName;
                if (!scanner.integer("a value", value, error)
                    || !scanner.quoted(valueName, error)) {
                    return Result::error(ErrorCode::ParseError, error);
                }

                if (signal != nullptr) {
                    signal->valueNames.push_back(SignalValueName{value, std::move(valueName)});
                }
            }
            continue;
        }

        if (keyword == "CM_") {
            const std::string_view target = scanner.name();

            if (target == "BO_") {
                std::int64_t rawIdentifier = 0;
                std::string comment;
                if (!scanner.integer("a message identifier", rawIdentifier, error)
                    || !scanner.quoted(comment, error)) {
                    return Result::error(ErrorCode::ParseError, error);
                }
                if (CanMessage* message =
                        index.find(messages, static_cast<std::uint32_t>(rawIdentifier))) {
                    message->comment = std::move(comment);
                }
                continue;
            }

            if (target == "SG_") {
                std::int64_t rawIdentifier = 0;
                std::string_view signalName;
                std::string comment;
                if (!scanner.integer("a message identifier", rawIdentifier, error)
                    || !scanner.name("a signal name", signalName, error)
                    || !scanner.quoted(comment, error)) {
                    return Result::error(ErrorCode::ParseError, error);
                }
                if (CanMessage* message =
                        index.find(messages, static_cast<std::uint32_t>(rawIdentifier))) {
                    if (CanSignal* found = message->findSignal(signalName)) {
                        found->comment = std::move(comment);
                    }
                }
                continue;
            }

            // CM_ BU_ <node> "..." and the bare file comment. Neither has a
            // reader yet.
            continue;
        }

        if (keyword == "BA_") {
            std::string attribute;
            if (!scanner.quoted(attribute, error)) {
                return Result::error(ErrorCode::ParseError, error);
            }

            if (attribute != "GenMsgCycleTime" || scanner.name() != "BO_") {
                continue;
            }

            std::int64_t rawIdentifier = 0;
            std::int64_t cycleTime = 0;
            if (!scanner.integer("a message identifier", rawIdentifier, error)
                || !scanner.integer("a cycle time", cycleTime, error)) {
                return Result::error(ErrorCode::ParseError, error);
            }

            if (CanMessage* message =
                    index.find(messages, static_cast<std::uint32_t>(rawIdentifier))) {
                message->cycleTimeMs = cycleTime > 0 ? static_cast<std::uint32_t>(cycleTime) : 0U;
            }
            continue;
        }

        // Sections this parser does not read: NS_, BS_, BA_DEF_, BA_DEF_DEF_,
        // EV_, SIG_GROUP_, and whatever the format grows next. Skipped rather
        // than rejected - see the note in the header.
    }

    database.clear();
    database.version = std::move(version);
    database.nodes = std::move(nodes);
    for (CanMessage& message : messages) {
        database.addMessage(std::move(message));
    }

    return Result::ok();
}

Result DbcParser::parseFile(const std::string& path, CanDatabase& database)
{
    std::ifstream file{path, std::ios::binary};
    if (!file) {
        return Result::error(ErrorCode::FileNotFound, "Cannot open \"" + path + "\"");
    }

    std::ostringstream contents;
    contents << file.rdbuf();

    Result result = parse(contents.str(), database);
    if (result.failed()) {
        // The line number alone is not enough when three databases are loaded.
        return Result::error(result.code(), path + ": " + std::string{result.message()});
    }

    database.sourcePath = path;
    return Result::ok();
}

} // namespace torquebus
